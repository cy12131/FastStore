#include "faststore/connection.h"

#include "faststore/router.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <utility>

namespace faststore {

Connection::Connection(
    int client_fd, bool informational_logging_enabled) noexcept
    : Connection(client_fd, 0, informational_logging_enabled) {}

Connection::Connection(int client_fd, std::uint64_t connection_id,
                       bool informational_logging_enabled) noexcept
    : client_fd_(client_fd),
      connection_id_(connection_id),
      informational_logging_enabled_(informational_logging_enabled) {}

Connection::Connection(Connection&& other) noexcept
    : client_fd_(std::exchange(other.client_fd_, -1)),
      connection_id_(std::exchange(other.connection_id_, 0)),
      informational_logging_enabled_(other.informational_logging_enabled_),
      valid_(std::exchange(other.valid_, false)),
      state_(std::exchange(other.state_, State::kError)),
      input_buffer_(std::move(other.input_buffer_)),
      read_offset_(std::exchange(other.read_offset_, 0)),
      peak_readable_bytes_(std::exchange(other.peak_readable_bytes_, 0)),
      http_parser_(std::move(other.http_parser_)),
      upload_(std::move(other.upload_)),
      final_filename_(std::move(other.final_filename_)),
      expected_body_bytes_(std::exchange(other.expected_body_bytes_, 0)),
      received_body_bytes_(std::exchange(other.received_body_bytes_, 0)),
      response_buffer_(std::move(other.response_buffer_)),
      response_offset_(std::exchange(other.response_offset_, 0)),
      read_file_(std::move(other.read_file_)),
      file_buffer_(std::move(other.file_buffer_)),
      file_chunk_valid_size_(
          std::exchange(other.file_chunk_valid_size_, 0)),
      file_chunk_send_offset_(
          std::exchange(other.file_chunk_send_offset_, 0)),
      expected_file_bytes_(std::exchange(other.expected_file_bytes_, 0)),
      loaded_file_bytes_(std::exchange(other.loaded_file_bytes_, 0)),
      sent_file_bytes_(std::exchange(other.sent_file_bytes_, 0)),
      error_message_(std::move(other.error_message_)) {}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    closeClientFd();
    client_fd_ = std::exchange(other.client_fd_, -1);
    connection_id_ = std::exchange(other.connection_id_, 0);
    informational_logging_enabled_ = other.informational_logging_enabled_;
    valid_ = std::exchange(other.valid_, false);
    state_ = std::exchange(other.state_, State::kError);
    input_buffer_ = std::move(other.input_buffer_);
    read_offset_ = std::exchange(other.read_offset_, 0);
    peak_readable_bytes_ = std::exchange(other.peak_readable_bytes_, 0);
    http_parser_ = std::move(other.http_parser_);
    upload_ = std::move(other.upload_);
    final_filename_ = std::move(other.final_filename_);
    expected_body_bytes_ = std::exchange(other.expected_body_bytes_, 0);
    received_body_bytes_ = std::exchange(other.received_body_bytes_, 0);
    response_buffer_ = std::move(other.response_buffer_);
    response_offset_ = std::exchange(other.response_offset_, 0);
    read_file_ = std::move(other.read_file_);
    file_buffer_ = std::move(other.file_buffer_);
    file_chunk_valid_size_ =
        std::exchange(other.file_chunk_valid_size_, 0);
    file_chunk_send_offset_ =
        std::exchange(other.file_chunk_send_offset_, 0);
    expected_file_bytes_ = std::exchange(other.expected_file_bytes_, 0);
    loaded_file_bytes_ = std::exchange(other.loaded_file_bytes_, 0);
    sent_file_bytes_ = std::exchange(other.sent_file_bytes_, 0);
    error_message_ = std::move(other.error_message_);
    return *this;
}

Connection::~Connection() {
    closeClientFd();
}

void Connection::closeClientFd() noexcept {
    if (client_fd_ < 0) {
        return;
    }

    const int fd_to_close = client_fd_;
    client_fd_ = -1;
    if (::close(fd_to_close) == -1) {
        const int error = errno;
        std::cerr << "Failed to close client fd=" << fd_to_close
                  << ": " << std::strerror(error) << std::endl;
    }
}

int Connection::fd() const noexcept {
    return client_fd_;
}

std::uint64_t Connection::id() const noexcept {
    return connection_id_;
}

bool Connection::isValid() const noexcept {
    return valid_;
}

Connection::State Connection::state() const noexcept {
    return state_;
}

bool Connection::wantsRead() const noexcept {
    return state_ == State::kReadingHeaders ||
           state_ == State::kRequestReady ||
           state_ == State::kReceivingBody;
}

bool Connection::wantsWrite() const noexcept {
    return state_ == State::kWritingResponse ||
           state_ == State::kWritingFileBody;
}

std::size_t Connection::readableSize() const noexcept {
    return input_buffer_.size() - read_offset_;
}

std::size_t Connection::peakReadableBytes() const noexcept {
    return peak_readable_bytes_;
}

std::size_t Connection::receivedBodyBytes() const noexcept {
    return received_body_bytes_;
}

std::size_t Connection::responseOffset() const noexcept {
    return response_offset_;
}

std::size_t Connection::responseSize() const noexcept {
    return response_buffer_.size();
}

std::size_t Connection::fileBufferCapacity() const noexcept {
    return file_buffer_.size();
}

std::size_t Connection::fileChunkValidSize() const noexcept {
    return file_chunk_valid_size_;
}

std::size_t Connection::fileChunkSendOffset() const noexcept {
    return file_chunk_send_offset_;
}

std::uint64_t Connection::expectedFileBytes() const noexcept {
    return expected_file_bytes_;
}

std::uint64_t Connection::sentFileBytes() const noexcept {
    return sent_file_bytes_;
}

bool Connection::hasOpenReadFile() const noexcept {
    return read_file_.has_value();
}

const HttpRequest* Connection::request() const noexcept {
    return http_parser_.request();
}

const std::string& Connection::errorMessage() const noexcept {
    return error_message_;
}

std::optional<HttpRequest> Connection::beginWorkerDispatch() {
    if (state_ != State::kRequestReady || http_parser_.request() == nullptr) {
        return std::nullopt;
    }

    // 先进入 waiting；即使 request copy 抛异常，Reactor 也能应用 500，
    // 不会留下可被重复 dispatch 的 kRequestReady。
    state_ = State::kWaitingForWorker;
    return *http_parser_.request();
}

bool Connection::applyRouteResult(RouteResult result) {
    if (state_ != State::kWaitingForWorker) {
        return false;
    }
    prepareRouteResult(std::move(result));
    return true;
}

std::string_view Connection::readableData() const noexcept {
    const std::size_t size = readableSize();
    if (size == 0U) {
        return {};
    }
    return {input_buffer_.data() + read_offset_, size};
}

bool Connection::appendInput(const char* data, std::size_t size) {
    if (input_buffer_.size() + size > kMaxBufferedInput && read_offset_ > 0U) {
        input_buffer_.erase(
            input_buffer_.begin(),
            input_buffer_.begin() +
                static_cast<std::vector<char>::difference_type>(read_offset_));
        read_offset_ = 0;
    }

    if (input_buffer_.size() > kMaxBufferedInput ||
        size > kMaxBufferedInput - input_buffer_.size()) {
        prepareBadRequest();
        return false;
    }

    input_buffer_.insert(input_buffer_.end(), data, data + size);
    peak_readable_bytes_ = std::max(peak_readable_bytes_, readableSize());
    return true;
}

void Connection::consume(std::size_t size) noexcept {
    read_offset_ += size;
    compactIfNeeded();
}

void Connection::compactIfNeeded() {
    if (read_offset_ == input_buffer_.size()) {
        input_buffer_.clear();
        read_offset_ = 0;
        return;
    }

    if (read_offset_ >= kReadChunkSize &&
        read_offset_ >= input_buffer_.size() / 2U) {
        input_buffer_.erase(
            input_buffer_.begin(),
            input_buffer_.begin() +
                static_cast<std::vector<char>::difference_type>(read_offset_));
        read_offset_ = 0;
    }
}

bool Connection::processBufferedInput(FileService& file_service) {
    if (!wantsRead()) {
        return state_ != State::kError;
    }

    if (state_ == State::kReadingHeaders) {
        const ParseStatus parse_status = http_parser_.parse(readableData());
        if (parse_status == ParseStatus::kIncomplete) {
            return true;
        }
        if (parse_status == ParseStatus::kError) {
            if (http_parser_.error() == HttpParseError::kPayloadTooLarge) {
                prepareResponse(makeResponse(HttpStatus::kPayloadTooLarge));
            } else {
                prepareBadRequest();
            }
            return true;
        }

        const HttpRequest* parsed_request = http_parser_.request();
        if (parsed_request == nullptr) {
            prepareResponse(makeResponse(HttpStatus::kInternalServerError));
            return true;
        }

        consume(http_parser_.consumedBytes());
        if (parsed_request->method == HttpMethod::kUnsupported) {
            // The method is syntactically valid but unsupported. Respond 405
            // immediately and close after sending; an unsupported body is not
            // consumed or interpreted as another request.
            prepareRouteResult(routeRequest(file_service, *parsed_request));
            return true;
        }
        if (parsed_request->method != HttpMethod::kPut) {
            if (readableSize() != 0U) {
                prepareBadRequest();
                return true;
            }
            state_ = State::kRequestReady;
            return true;
        }

        const auto filename = extractFileName(parsed_request->target);
        if (!filename.has_value()) {
            prepareBadRequest();
            return true;
        }

        auto upload_result = file_service.createTemp();
        if (!upload_result.ok()) {
            prepareResponse(responseForFileError(upload_result.error()));
            return true;
        }

        upload_.emplace(upload_result.takeValue());
        final_filename_ = filename.value();
        expected_body_bytes_ = parsed_request->content_length;
        state_ = State::kReceivingBody;
    }

    if (state_ == State::kRequestReady) {
        if (readableSize() != 0U) {
            prepareBadRequest();
        }
        return true;
    }
    if (state_ != State::kReceivingBody) {
        return true;
    }

    const std::size_t remaining =
        expected_body_bytes_ - received_body_bytes_;
    const std::size_t body_bytes = std::min(readableSize(), remaining);
    if (body_bytes > 0U) {
        const FileResult<void> write_result =
            upload_->writeAll(readableData().substr(0, body_bytes));
        if (!write_result.ok()) {
            prepareResponse(responseForFileError(write_result.error()));
            return true;
        }
        consume(body_bytes);
        received_body_bytes_ += body_bytes;
    }

    if (received_body_bytes_ == expected_body_bytes_ &&
        readableSize() != 0U) {
        prepareBadRequest();
    }
    return true;
}

bool Connection::finalizeAtReadBoundary(FileService&,
                                        bool peer_eof) {
    if (state_ == State::kWritingResponse ||
        state_ == State::kWritingFileBody ||
        state_ == State::kResponseComplete) {
        return true;
    }
    if (state_ == State::kReadingHeaders) {
        if (peer_eof) {
            failTransport("unexpected EOF while reading HTTP headers");
            return false;
        }
        return true;
    }
    if (state_ == State::kRequestReady) {
        return true;
    }
    if (state_ != State::kReceivingBody) {
        return state_ != State::kError;
    }

    if (received_body_bytes_ != expected_body_bytes_) {
        if (peer_eof) {
            failTransport("unexpected EOF while receiving PUT body");
            return false;
        }
        return true;
    }
    if (readableSize() != 0U) {
        prepareBadRequest();
        return true;
    }

    // Bytes already read beyond Content-Length are rejected before commit.
    // EAGAIN only means the receive queue is temporarily empty; V0 completes
    // this one request and closes after sending its response.
    const FileResult<void> commit_result = upload_->commit(final_filename_);
    if (!commit_result.ok()) {
        prepareResponse(responseForFileError(commit_result.error()));
        return true;
    }

    upload_.reset();
    // PUT body streaming 与 commit 仍在 Reactor；成功后直接准备原有 201。
    prepareResponse(makeResponse(HttpStatus::kCreated));
    return true;
}

void Connection::prepareResponse(HttpResponse response) {
    upload_.reset();
    read_file_.reset();
    file_buffer_.clear();
    file_chunk_valid_size_ = 0;
    file_chunk_send_offset_ = 0;
    expected_file_bytes_ = 0;
    loaded_file_bytes_ = 0;
    sent_file_bytes_ = 0;
    input_buffer_.clear();
    read_offset_ = 0;
    response_buffer_ = serializeHttpResponse(response);
    response_offset_ = 0;
    state_ = State::kWritingResponse;
}

void Connection::prepareRouteResult(RouteResult result) {
    prepareResponse(std::move(result.response));
    if (!result.read_file.has_value()) {
        return;
    }

    read_file_ = std::move(result.read_file);
    expected_file_bytes_ = read_file_->size();
    if (expected_file_bytes_ > 0U) {
        file_buffer_.resize(kFileChunkSize);
    }
}

void Connection::prepareBadRequest() {
    prepareResponse(makeResponse(HttpStatus::kBadRequest));
}

void Connection::failTransport(std::string message) {
    state_ = State::kError;
    valid_ = false;
    upload_.reset();
    read_file_.reset();
    file_buffer_.clear();
    error_message_ = std::move(message);
}

Connection::ReadStatus Connection::readAvailableData(
    FileService& file_service) {
    std::array<char, kReadChunkSize> read_buffer{};

    while (true) {
        const ssize_t bytes_received =
            ::recv(client_fd_, read_buffer.data(), read_buffer.size(), 0);

        if (bytes_received > 0) {
            const auto chunk_size = static_cast<std::size_t>(bytes_received);
            if (!appendInput(read_buffer.data(), chunk_size)) {
                return wantsWrite() ? ReadStatus::kOpen : ReadStatus::kError;
            }
            if (!processBufferedInput(file_service)) {
                return ReadStatus::kError;
            }

            if (informational_logging_enabled_) {
                std::cout << "Received " << bytes_received
                          << " bytes from fd=" << client_fd_
                          << ", unread buffer=" << readableSize()
                          << " bytes" << std::endl;
            }
            if (wantsWrite()) {
                return ReadStatus::kOpen;
            }
            continue;
        }

        if (bytes_received == 0) {
            if (!finalizeAtReadBoundary(file_service, true)) {
                return ReadStatus::kError;
            }
            return ReadStatus::kPeerClosed;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!finalizeAtReadBoundary(file_service, false)) {
                return ReadStatus::kError;
            }
            return ReadStatus::kOpen;
        }

        const int error = errno;
        failTransport(std::string{"recv failed: "} + std::strerror(error));
        return ReadStatus::kError;
    }
}

Connection::WriteStatus Connection::writeAvailableData() {
    if (!wantsWrite()) {
        failTransport("send requested without pending output");
        return WriteStatus::kError;
    }

    if (state_ == State::kWritingResponse) {
        while (response_offset_ < response_buffer_.size()) {
            const ssize_t sent = ::send(
                client_fd_, response_buffer_.data() + response_offset_,
                response_buffer_.size() - response_offset_, MSG_NOSIGNAL);
            if (sent > 0) {
                response_offset_ += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent == -1 && errno == EINTR) {
                continue;
            }
            if (sent == -1 &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return WriteStatus::kPending;
            }

            const int error = sent == 0 ? EIO : errno;
            failTransport(std::string{"send failed: "} +
                          std::strerror(error));
            return WriteStatus::kError;
        }

        if (!read_file_.has_value() || expected_file_bytes_ == 0U) {
            read_file_.reset();
            file_buffer_.clear();
            state_ = State::kResponseComplete;
            return WriteStatus::kComplete;
        }
        state_ = State::kWritingFileBody;
    }

    return writeFileBody();
}

Connection::WriteStatus Connection::writeFileBody() {
    while (sent_file_bytes_ < expected_file_bytes_) {
        if (file_chunk_send_offset_ == file_chunk_valid_size_) {
            if (!loadNextFileChunk()) {
                return WriteStatus::kError;
            }
        }

        const ssize_t sent = ::send(
            client_fd_, file_buffer_.data() + file_chunk_send_offset_,
            file_chunk_valid_size_ - file_chunk_send_offset_, MSG_NOSIGNAL);
        if (sent > 0) {
            const auto sent_size = static_cast<std::size_t>(sent);
            file_chunk_send_offset_ += sent_size;
            sent_file_bytes_ += static_cast<std::uint64_t>(sent_size);
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return WriteStatus::kPending;
        }

        const int error = sent == 0 ? EIO : errno;
        failTransport(std::string{"file body send failed: "} +
                      std::strerror(error));
        return WriteStatus::kError;
    }

    read_file_.reset();
    file_buffer_.clear();
    file_chunk_valid_size_ = 0;
    file_chunk_send_offset_ = 0;
    state_ = State::kResponseComplete;
    return WriteStatus::kComplete;
}

bool Connection::loadNextFileChunk() {
    if (!read_file_.has_value() ||
        file_chunk_send_offset_ != file_chunk_valid_size_) {
        failTransport("invalid file streaming state");
        return false;
    }

    const std::uint64_t remaining =
        expected_file_bytes_ - loaded_file_bytes_;
    const std::size_t to_read = static_cast<std::size_t>(
        std::min<std::uint64_t>(file_buffer_.size(), remaining));
    if (to_read == 0U) {
        failTransport("file body ended before Content-Length was sent");
        return false;
    }

    while (true) {
        const ssize_t count =
            ::read(read_file_->fd(), file_buffer_.data(), to_read);
        if (count > 0) {
            file_chunk_valid_size_ = static_cast<std::size_t>(count);
            file_chunk_send_offset_ = 0;
            loaded_file_bytes_ += static_cast<std::uint64_t>(count);
            return true;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        if (count == 0) {
            failTransport("file was truncated during streaming GET");
            return false;
        }

        const int error = errno;
        failTransport(std::string{"file read failed: "} +
                      std::strerror(error));
        return false;
    }
}

}  // namespace faststore
