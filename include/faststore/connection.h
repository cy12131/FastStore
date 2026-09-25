#ifndef FASTSTORE_CONNECTION_H
#define FASTSTORE_CONNECTION_H

#include "faststore/file_service.h"
#include "faststore/http.h"
#include "faststore/http_response.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace faststore {

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
        kResponseComplete,
        kError
    };

    static constexpr std::size_t kReadChunkSize = 16U * 1024U;
    static constexpr std::size_t kMaxBufferedInput =
        HttpParser::kMaxHeaderSectionSize + kReadChunkSize;

    explicit Connection(int client_fd) noexcept;
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
    [[nodiscard]] const HttpRequest* request() const noexcept;
    [[nodiscard]] const std::string& errorMessage() const noexcept;

    // 每次 recv 后立即解析/消费输入；PUT body 直接写入临时文件。
    ReadStatus readAvailableData(FileService& file_service);

    // 循环 non-blocking send，保留 partial-send offset 等待下次 EPOLLOUT。
    WriteStatus writeAvailableData();

private:
    [[nodiscard]] std::string_view readableData() const noexcept;
    bool appendInput(const char* data, std::size_t size);
    void consume(std::size_t size) noexcept;
    void compactIfNeeded();
    bool processBufferedInput(FileService& file_service);
    bool finalizeAtReadBoundary(FileService& file_service, bool peer_eof);
    void prepareResponse(HttpResponse response);
    void prepareBadRequest();
    void failTransport(std::string message);
    void closeClientFd() noexcept;

    int client_fd_;
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
    std::string error_message_;
};

}  // namespace faststore

#endif  // FASTSTORE_CONNECTION_H
