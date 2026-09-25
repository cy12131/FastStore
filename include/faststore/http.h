#ifndef FASTSTORE_HTTP_H
#define FASTSTORE_HTTP_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace faststore {

// V0 支持的 HTTP 请求方法。
enum class HttpMethod {
    kGet,
    kPut,
    kDelete,
    kHead,
    kUnsupported
};

// 将方法枚举转换为日志使用的稳定文本。
[[nodiscard]] std::string_view toString(HttpMethod method) noexcept;

// HTTP/1.1 请求的最小元数据模型；request body 不保存在这里。
struct HttpRequest {
    HttpMethod method{HttpMethod::kGet}; // 已验证的请求方法。
    std::string target;                  // 原始 request-target。
    std::string version;                 // 当前只可能是 HTTP/1.1。

    // Header name 统一保存为 ASCII 小写，value 已去除两端空格和 tab。
    std::unordered_map<std::string, std::string> headers;

    std::size_t content_length{0}; // 已验证的十进制 Content-Length。
};

// Parser 只区分 header 数据不足、header 完整和请求非法。
enum class ParseStatus {
    kIncomplete,
    kHeadersComplete,
    kError
};

enum class HttpParseError {
    kNone,
    kBadRequest,
    kPayloadTooLarge
};

// 针对单个 HTTP/1.1 请求 header 的有状态增量解析器。
class HttpParser {
public:
    static constexpr std::size_t kMaxRequestLineSize = 8U * 1024U;
    static constexpr std::size_t kMaxHeaderSectionSize = 32U * 1024U;
    static constexpr std::size_t kMaxHeaderCount = 100U;
    static constexpr std::size_t kMaxBodySize = 64U * 1024U * 1024U;

    // 解析 Connection 当前尚未消费的输入，header 完整后立即返回。
    ParseStatus parse(std::string_view input);

    // 仅在状态为 kHeadersComplete 时返回请求元数据。
    [[nodiscard]] const HttpRequest* request() const noexcept;

    // 返回最近一次解析错误类型和说明。
    [[nodiscard]] HttpParseError error() const noexcept;
    [[nodiscard]] const std::string& errorMessage() const noexcept;

    // 返回完整 header section 占用的输入字节数。
    [[nodiscard]] std::size_t consumedBytes() const noexcept;

private:
    // 解析已经完整到达的 request line 和 header section。
    ParseStatus parseHeaders(std::string_view data,
                             std::size_t request_line_end,
                             std::size_t header_terminator,
                             std::size_t header_size);

    // 记录不可恢复的错误并统一切换到 kError。
    ParseStatus fail(
        std::string message,
        HttpParseError error = HttpParseError::kBadRequest);

    ParseStatus status_{ParseStatus::kIncomplete}; // 当前解析状态。
    std::size_t consumed_bytes_{0};                // Header section 字节数。
    std::optional<HttpRequest> request_;            // 已解析的请求元数据。
    HttpParseError error_{HttpParseError::kNone};
    std::string error_message_;                    // 最近一次错误说明。
};

}  // namespace faststore

#endif  // FASTSTORE_HTTP_H
