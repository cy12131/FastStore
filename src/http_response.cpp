#include "faststore/http_response.h"

#include <string_view>

namespace faststore {

std::string_view reasonPhrase(HttpStatus status) noexcept {
    switch (status) {
        case HttpStatus::kOk:
            return "OK";
        case HttpStatus::kCreated:
            return "Created";
        case HttpStatus::kNoContent:
            return "No Content";
        case HttpStatus::kBadRequest:
            return "Bad Request";
        case HttpStatus::kNotFound:
            return "Not Found";
        case HttpStatus::kMethodNotAllowed:
            return "Method Not Allowed";
        case HttpStatus::kPayloadTooLarge:
            return "Payload Too Large";
        case HttpStatus::kInternalServerError:
            return "Internal Server Error";
        case HttpStatus::kNotImplemented:
            return "Not Implemented";
    }
    return "Internal Server Error";
}

HttpResponse makeResponse(HttpStatus status, std::uint64_t content_length) {
    HttpResponse response;
    response.status = status;
    response.content_length = content_length;
    return response;
}

std::string serializeHttpResponse(const HttpResponse& response) {
    std::string serialized =
        "HTTP/1.1 " +
        std::to_string(static_cast<unsigned int>(response.status)) + " " +
        std::string{reasonPhrase(response.status)} + "\r\n";

    for (const auto& header : response.headers) {
        serialized += header.first;
        serialized += ": ";
        serialized += header.second;
        serialized += "\r\n";
    }

    serialized += "Content-Length: ";
    serialized += std::to_string(response.content_length);
    serialized += "\r\nConnection: close\r\n\r\n";
    serialized += response.body;
    return serialized;
}

}  // namespace faststore
