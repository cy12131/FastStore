#ifndef FASTSTORE_HTTP_H
#define FASTSTORE_HTTP_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace faststore {

// V0 支持的 HTTP 请求方法。
enum class HttpMethod {
    kGet,
    kPut,
    kDelete,
    kHead
};

// 将方法枚举转换为日志使用的稳定文本。
[[nodiscard]] std::string_view toString(HttpMethod method) noexcept;

// HTTP/1.1 请求的最小数据模型。
struct HttpRequest {
    HttpMethod method{HttpMethod::kGet}; // 已验证的请求方法。
    std::string target;                  // 原始 request-target。
    std::string version;                 // 当前只可能是 HTTP/1.1。

    // Header name 统一保存为 ASCII 小写，value 已去除两端空格和 tab。
    std::unordered_map<std::string, std::string> headers;

    std::size_t content_length{0}; // 已验证的十进制 Content-Length。
    std::vector<char> body;        // 完整请求体；M3 暂时整体保存在内存。
};

// 明确区分数据不足、请求完整和请求非法。
enum class ParseStatus {
    kIncomplete,
    kComplete,
    kError
};

// 针对单个 HTTP/1.1 请求的有状态增量解析器。
class HttpParser {
public:
    static constexpr std::size_t kMaxRequestLineSize = 8U * 1024U;
    static constexpr std::size_t kMaxHeaderSectionSize = 32U * 1024U;
    static constexpr std::size_t kMaxHeaderCount = 100U;
    static constexpr std::size_t kMaxBodySize = 64U * 1024U * 1024U;

    // Connection 输入缓冲区的绝对上限，避免在解析前无限增长。
    static constexpr std::size_t kMaxBufferedRequestSize =
        kMaxHeaderSectionSize + kMaxBodySize;

    // 解析 Connection 当前累计的全部输入；数据不足时保留内部进度。
    ParseStatus parse(const std::vector<char>& input);

    // 仅在状态为 kComplete 时返回已完成请求，否则返回 nullptr。
    [[nodiscard]] const HttpRequest* request() const noexcept;

    // 返回最近一次解析错误；非错误状态下为空。
    [[nodiscard]] const std::string& errorMessage() const noexcept;

    // 返回完整请求占用的输入字节数。
    [[nodiscard]] std::size_t consumedBytes() const noexcept;

private:
    // 解析已经完整到达的 request line 和 header section。
    ParseStatus parseHeaders(std::string_view data,
                             std::size_t request_line_end,
                             std::size_t header_terminator,
                             std::size_t header_size);

    // 记录不可恢复的错误并统一切换到 kError。
    ParseStatus fail(std::string message);

    ParseStatus status_{ParseStatus::kIncomplete}; // 当前解析状态。
    bool headers_parsed_{false};                   // Header 是否已成功解析。
    std::size_t header_size_{0};                   // Body 在输入中的起始偏移。
    std::size_t expected_request_size_{0};         // Header 与 body 的总长度。
    std::size_t consumed_bytes_{0};                // 完整请求消耗的字节数。
    std::optional<HttpRequest> request_;            // 正在构造或已完成的请求。
    std::string error_message_;                    // 最近一次错误说明。
};

}  // namespace faststore

#endif  // FASTSTORE_HTTP_H
