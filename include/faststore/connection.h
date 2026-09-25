#ifndef FASTSTORE_CONNECTION_H
#define FASTSTORE_CONNECTION_H

#include "faststore/file_service.h"
#include "faststore/http.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace faststore {

// 保存单个客户端的 socket、解析、缓冲区和流式上传状态。
class Connection {
public:
    // 描述一次非阻塞读取结束后连接的状态。
    enum class ReadStatus {
        kOpen,        // 数据已读到 EAGAIN，连接仍然有效。
        kPeerClosed,  // recv 返回 0，对端已经关闭连接。
        kError        // 读取、协议或文件操作发生不可恢复错误。
    };

    // 单请求连接在 M4-B 中使用的明确状态机。
    enum class State {
        kReadingHeaders,
        kReceivingBody,
        kRequestComplete,
        kError
    };

    static constexpr std::size_t kReadChunkSize = 16U * 1024U;
    static constexpr std::size_t kMaxBufferedInput =
        HttpParser::kMaxHeaderSectionSize + kReadChunkSize;

    // 接管 client_fd 的唯一所有权。
    explicit Connection(int client_fd) noexcept;
    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    [[nodiscard]] int fd() const noexcept;
    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] std::size_t readableSize() const noexcept;
    [[nodiscard]] std::size_t peakReadableBytes() const noexcept;
    [[nodiscard]] std::size_t receivedBodyBytes() const noexcept;
    [[nodiscard]] const HttpRequest* request() const noexcept;
    [[nodiscard]] const std::string& errorMessage() const noexcept;

    // 每次 recv 后立即解析/消费输入；PUT body 直接写入临时文件。
    ReadStatus readAvailableData(FileService& file_service);

private:
    [[nodiscard]] std::string_view readableData() const noexcept;
    bool appendInput(const char* data, std::size_t size);
    void consume(std::size_t size) noexcept;
    void compactIfNeeded();
    bool processBufferedInput(FileService& file_service);
    bool finalizeAtReadBoundary(bool peer_eof);
    bool fail(std::string message);
    void closeClientFd() noexcept;

    int client_fd_;                   // 当前 Connection 独占的客户端 fd。
    bool valid_{true};                 // 连接是否仍可继续处理。
    State state_{State::kReadingHeaders};
    std::vector<char> input_buffer_;   // 包含已消费前缀和当前可读区域。
    std::size_t read_offset_{0};       // 当前可读区域的起始位置。
    std::size_t peak_readable_bytes_{0};
    HttpParser http_parser_;           // 仅解析 request line 和 headers。
    std::optional<UploadFile> upload_; // PUT 期间拥有临时文件。
    std::string final_filename_;       // PUT 提交使用的已验证 basename。
    std::size_t expected_body_bytes_{0};
    std::size_t received_body_bytes_{0};
    std::string error_message_;
};

}  // namespace faststore

#endif  // FASTSTORE_CONNECTION_H
