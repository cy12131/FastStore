#ifndef FASTSTORE_HTTP_RESPONSE_H
#define FASTSTORE_HTTP_RESPONSE_H

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace faststore {

enum class HttpStatus : unsigned int {
    kOk = 200,
    kCreated = 201,
    kNoContent = 204,
    kBadRequest = 400,
    kNotFound = 404,
    kMethodNotAllowed = 405,
    kPayloadTooLarge = 413,
    kInternalServerError = 500,
    kNotImplemented = 501
};

// 小型 HTTP response；GET 仅在此保存 headers，文件体由 Connection 流式发送。
struct HttpResponse {
    HttpStatus status{HttpStatus::kInternalServerError};
    std::uint64_t content_length{0};
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

[[nodiscard]] std::string_view reasonPhrase(HttpStatus status) noexcept;
[[nodiscard]] HttpResponse makeResponse(
    HttpStatus status, std::uint64_t content_length = 0);
[[nodiscard]] std::string serializeHttpResponse(const HttpResponse& response);

}  // namespace faststore

#endif  // FASTSTORE_HTTP_RESPONSE_H
