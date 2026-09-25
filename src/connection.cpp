#include "faststore/connection.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <utility>

namespace faststore {
namespace {

constexpr std::string_view kFilesPrefix = "/files/";

// M4-B 只接受 /files/{single-basename}，不进行 URL decoding。
std::optional<std::string> extractPutFilename(std::string_view target) {
    if (target.size() <= kFilesPrefix.size() ||
        target.substr(0, kFilesPrefix.size()) != kFilesPrefix) {
        return std::nullopt;
    }

    const std::string_view filename = target.substr(kFilesPrefix.size());
    if (validateFilename(filename) != FileError::kNone) {
        return std::nullopt;
    }
    return std::string{filename};
}

}  // namespace

Connection::Connection(int client_fd) noexcept : client_fd_(client_fd) {}

Connection::Connection(Connection&& other) noexcept
    : client_fd_(std::exchange(other.client_fd_, -1)),
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
      error_message_(std::move(other.error_message_)) {}

Connection& Connection::operator=(Connection&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    closeClientFd();
    client_fd_ = std::exchange(other.client_fd_, -1);
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

bool Connection::isValid() const noexcept {
    return valid_;
}

Connection::State Connection::state() const noexcept {
    return state_;
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

const HttpRequest* Connection::request() const noexcept {
    return http_parser_.request();
}

const std::string& Connection::errorMessage() const noexcept {
    return error_message_;
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
        return fail("connection input buffer limit exceeded");
    }

    input_buffer_.insert(input_buffer_.end(), data, data + size);
    peak_readable_bytes_ =
        std::max(peak_readable_bytes_, readableSize());
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
    if (state_ == State::kError) {
        return false;
    }
    if (state_ == State::kRequestComplete) {
        return readableSize() == 0U ||
               fail("extra bytes after the single supported request");
    }

    if (state_ == State::kReadingHeaders) {
        const ParseStatus parse_status = http_parser_.parse(readableData());
        if (parse_status == ParseStatus::kIncomplete) {
            return true;
        }
        if (parse_status == ParseStatus::kError) {
            return fail(http_parser_.errorMessage());
        }

        const HttpRequest* parsed_request = http_parser_.request();
        if (parsed_request == nullptr) {
            return fail("HTTP parser completed without request metadata");
        }

        consume(http_parser_.consumedBytes());
        if (parsed_request->method != HttpMethod::kPut) {
            if (readableSize() != 0U) {
                return fail("extra bytes after bodyless request");
            }
            state_ = State::kRequestComplete;
            return true;
        }

        const auto filename = extractPutFilename(parsed_request->target);
        if (!filename.has_value()) {
            return fail("invalid PUT target");
        }

        auto upload_result = file_service.createTemp();
        if (!upload_result.ok()) {
            return fail("unable to create temporary upload file");
        }

        upload_.emplace(upload_result.takeValue());
        final_filename_ = filename.value();
        expected_body_bytes_ = parsed_request->content_length;
        state_ = State::kReceivingBody;
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
            return fail("unable to write upload body");
        }
        consume(body_bytes);
        received_body_bytes_ += body_bytes;
    }

    if (received_body_bytes_ == expected_body_bytes_ &&
        readableSize() != 0U) {
        return fail("bytes received after Content-Length boundary");
    }
    return true;
}

bool Connection::finalizeAtReadBoundary(bool peer_eof) {
    if (state_ == State::kReadingHeaders) {
        if (peer_eof) {
            return fail("unexpected EOF while reading HTTP headers");
        }
        return true;
    }
    if (state_ != State::kReceivingBody) {
        return state_ != State::kError;
    }

    if (received_body_bytes_ != expected_body_bytes_) {
        if (peer_eof) {
            return fail("unexpected EOF while receiving PUT body");
        }
        return true;
    }
    if (readableSize() != 0U) {
        return fail("bytes received after Content-Length boundary");
    }

    // Commit only after recv has reached EAGAIN/EOF, so bytes already read
    // beyond Content-Length are rejected first. EAGAIN only means the receive
    // queue is temporarily empty; it cannot prove that the peer will not send
    // more bytes later. V0 completes and closes this one-request connection.
    const FileResult<void> commit_result = upload_->commit(final_filename_);
    if (!commit_result.ok()) {
        return fail("unable to commit upload");
    }

    upload_.reset();
    state_ = State::kRequestComplete;
    return true;
}

bool Connection::fail(std::string message) {
    state_ = State::kError;
    valid_ = false;
    error_message_ = std::move(message);
    return false;
}

Connection::ReadStatus Connection::readAvailableData(
    FileService& file_service) {
    std::array<char, kReadChunkSize> read_buffer{};

    while (true) {
        const ssize_t bytes_received =
            ::recv(client_fd_, read_buffer.data(), read_buffer.size(), 0);

        if (bytes_received > 0) {
            const auto chunk_size = static_cast<std::size_t>(bytes_received);
            if (!appendInput(read_buffer.data(), chunk_size) ||
                !processBufferedInput(file_service)) {
                return ReadStatus::kError;
            }

            std::cout << "Received " << bytes_received
                      << " bytes from fd=" << client_fd_
                      << ", unread buffer=" << readableSize()
                      << " bytes" << std::endl;
            continue;
        }

        if (bytes_received == 0) {
            if (!finalizeAtReadBoundary(true)) {
                return ReadStatus::kError;
            }
            valid_ = false;
            return ReadStatus::kPeerClosed;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (!finalizeAtReadBoundary(false)) {
                return ReadStatus::kError;
            }
            return ReadStatus::kOpen;
        }

        const int error = errno;
        fail(std::string{"recv failed: "} + std::strerror(error));
        return ReadStatus::kError;
    }
}

}  // namespace faststore
