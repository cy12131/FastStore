#ifndef FASTSTORE_CONNECTION_H
#define FASTSTORE_CONNECTION_H

#include "faststore/file_service.h"
#include "faststore/http.h"
#include "faststore/http_response.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace faststore {

struct RouteResult;

// 保存单个客户端的 socket、请求、上传和 response 发送状态。
class Connection {
public:
    enum class ReadStatus {
        kOpen,
        kPeerClosed,
        kError
    };

    enum class WriteStatus {
        kPending,
        kComplete,
        kError
    };

    enum class State {
        kReadingHeaders,
        kRequestReady,
        kReceivingBody,
        kWritingResponse,
        kWritingFileBody,
        kResponseComplete,
        kError
    };

    static constexpr std::size_t kReadChunkSize = 16U * 1024U;
    static constexpr std::size_t kFileChunkSize = 64U * 1024U;
    static constexpr std::size_t kMaxBufferedInput =
        HttpParser::kMaxHeaderSectionSize + kReadChunkSize;

    explicit Connection(int client_fd,
                        bool informational_logging_enabled = true) noexcept;
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] bool wantsRead() const noexcept;
    [[nodiscard]] bool wantsWrite() const noexcept;
    [[nodiscard]] std::size_t readableSize() const noexcept;
    [[nodiscard]] std::size_t peakReadableBytes() const noexcept;
    [[nodiscard]] std::size_t receivedBodyBytes() const noexcept;
    [[nodiscard]] std::size_t responseOffset() const noexcept;
    [[nodiscard]] std::size_t responseSize() const noexcept;
    [[nodiscard]] std::size_t fileBufferCapacity() const noexcept;
    [[nodiscard]] std::size_t fileChunkValidSize() const noexcept;
    [[nodiscard]] std::size_t fileChunkSendOffset() const noexcept;
    [[nodiscard]] std::uint64_t expectedFileBytes() const noexcept;
    [[nodiscard]] std::uint64_t sentFileBytes() const noexcept;
    [[nodiscard]] bool hasOpenReadFile() const noexcept;
    [[nodiscard]] const HttpRequest* request() const noexcept;
    [[nodiscard]] const std::string& errorMessage() const noexcept;

    // 每次 recv 后立即解析/消费输入；PUT body 直接写入临时文件。
    ReadStatus readAvailableData(FileService& file_service);

    // 依次发送 response headers 与可选文件体，并为 EPOLLOUT 保留 partial-send 状态。
    WriteStatus writeAvailableData();

private:
    [[nodiscard]] std::string_view readableData() const noexcept;
    bool appendInput(const char* data, std::size_t size);
    void consume(std::size_t size) noexcept;
    void compactIfNeeded();
    bool processBufferedInput(FileService& file_service);
    bool finalizeAtReadBoundary(FileService& file_service, bool peer_eof);
    void prepareResponse(HttpResponse response);
    void prepareRouteResult(RouteResult result);
    WriteStatus writeFileBody();
    bool loadNextFileChunk();
    void prepareBadRequest();
    void failTransport(std::string message);
    void closeClientFd() noexcept;

    int client_fd_;
    bool informational_logging_enabled_;
    bool valid_{true};
    State state_{State::kReadingHeaders};
    std::vector<char> input_buffer_;
    std::size_t read_offset_{0};
    std::size_t peak_readable_bytes_{0};
    HttpParser http_parser_;
    std::optional<UploadFile> upload_;
    std::string final_filename_;
    std::size_t expected_body_bytes_{0};
    std::size_t received_body_bytes_{0};
    std::string response_buffer_;
    std::size_t response_offset_{0};
    std::optional<ReadFile> read_file_;
    std::vector<char> file_buffer_;
    std::size_t file_chunk_valid_size_{0};
    std::size_t file_chunk_send_offset_{0};
    std::uint64_t expected_file_bytes_{0};
    std::uint64_t loaded_file_bytes_{0};
    std::uint64_t sent_file_bytes_{0};
    std::string error_message_;
};

}  // namespace faststore

#endif  // FASTSTORE_CONNECTION_H
