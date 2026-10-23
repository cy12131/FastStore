#include "faststore/http.h"

#include <limits>
#include <utility>

namespace faststore {
namespace {

constexpr std::string_view kCrlf = "\r\n";
constexpr std::string_view kHeaderTerminator = "\r\n\r\n";

// HTTP field-name 只允许 RFC token 使用的 ASCII 字符。
bool isTokenCharacter(char character) noexcept {
    const unsigned char value = static_cast<unsigned char>(character);
    if ((value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') ||
        (value >= '0' && value <= '9')) {
        return true;
    }

    switch (value) {
        case '!':
        case '#':
        case '$':
        case '%':
        case '&':
        case '\'':
        case '*':
        case '+':
        case '-':
        case '.':
        case '^':
        case '_':
        case '`':
        case '|':
        case '~':
            return true;
        default:
            return false;
    }
}

// Header name 在验证 token 语法后统一转为 ASCII 小写。
std::optional<std::string> normalizeHeaderName(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }

    std::string normalized;
    normalized.reserve(name.size());
    for (const char character : name) {
        if (!isTokenCharacter(character)) {
            return std::nullopt;
        }

        if (character >= 'A' && character <= 'Z') {
            normalized.push_back(
                static_cast<char>(character - 'A' + 'a'));
        } else {
            normalized.push_back(character);
        }
    }
    return normalized;
}

// 去除 header value 两端的 SP 和 HTAB。
std::string_view trimOptionalWhitespace(std::string_view value) noexcept {
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

// 拒绝 value 中除 HTAB 外的控制字符。
bool isValidHeaderValue(std::string_view value) noexcept {
    for (const char character : value) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if ((byte < 0x20U && byte != '\t') || byte == 0x7FU) {
            return false;
        }
    }
    return true;
}

// 严格解析不带符号的十进制 Content-Length，并检测 size_t 溢出。
bool parseContentLength(std::string_view text,
                        std::size_t& result,
                        std::string& error) {
    if (text.empty()) {
        error = "Content-Length is empty";
        return false;
    }

    std::size_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            error = "Content-Length must be a non-negative decimal integer";
            return false;
        }

        const std::size_t digit =
            static_cast<std::size_t>(character - '0');
        if (value >
            (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            error = "Content-Length overflows size_t";
            return false;
        }
        value = value * 10U + digit;
    }

    result = value;
    return true;
}

HttpMethod parseMethod(std::string_view method) noexcept {
    if (method == "GET") {
        return HttpMethod::kGet;
    }
    if (method == "PUT") {
        return HttpMethod::kPut;
    }
    if (method == "DELETE") {
        return HttpMethod::kDelete;
    }
    if (method == "HEAD") {
        return HttpMethod::kHead;
    }
    return HttpMethod::kUnsupported;
}

}  // namespace

std::string_view toString(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::kGet:
            return "GET";
        case HttpMethod::kPut:
            return "PUT";
        case HttpMethod::kDelete:
            return "DELETE";
        case HttpMethod::kHead:
            return "HEAD";
        case HttpMethod::kUnsupported:
            return "UNSUPPORTED";
    }
    return "UNKNOWN";
}

ParseStatus HttpParser::parse(std::string_view input) {
    if (status_ != ParseStatus::kIncomplete) {
        return status_;
    }
    if (input.empty()) {
        return status_;
    }

    const std::size_t request_line_end = input.find(kCrlf);
    if (request_line_end == std::string_view::npos) {
        if (input.size() > kMaxRequestLineSize) {
            return fail("request line exceeds 8 KiB");
        }
    } else if (request_line_end > kMaxRequestLineSize) {
        return fail("request line exceeds 8 KiB");
    }

    const std::size_t header_terminator = input.find(kHeaderTerminator);
    if (header_terminator == std::string_view::npos) {
        if (input.size() > kMaxHeaderSectionSize) {
            return fail("header section exceeds 32 KiB");
        }
        return status_;
    }

    const std::size_t header_size =
        header_terminator + kHeaderTerminator.size();
    if (header_size > kMaxHeaderSectionSize) {
        return fail("header section exceeds 32 KiB");
    }
    if (request_line_end == std::string_view::npos ||
        request_line_end > header_terminator) {
        return fail("request line is not terminated with CRLF");
    }

    return parseHeaders(input, request_line_end,
                        header_terminator, header_size);
}

const HttpRequest* HttpParser::request() const noexcept {
    if (status_ != ParseStatus::kHeadersComplete || !request_.has_value()) {
        return nullptr;
    }
    return &request_.value();
}

HttpParseError HttpParser::error() const noexcept {
    return error_;
}

const std::string& HttpParser::errorMessage() const noexcept {
    return error_message_;
}

std::size_t HttpParser::consumedBytes() const noexcept {
    return consumed_bytes_;
}

ParseStatus HttpParser::parseHeaders(std::string_view data,
                                     std::size_t request_line_end,
                                     std::size_t header_terminator,
                                     std::size_t header_size) {
    const std::string_view request_line = data.substr(0, request_line_end);

    const std::size_t first_space = request_line.find(' ');
    if (first_space == std::string_view::npos || first_space == 0U) {
        return fail("malformed request line");
    }

    const std::size_t second_space =
        request_line.find(' ', first_space + 1U);
    if (second_space == std::string_view::npos ||
        second_space == first_space + 1U ||
        second_space + 1U >= request_line.size() ||
        request_line.find(' ', second_space + 1U) !=
            std::string_view::npos) {
        return fail("malformed request line");
    }

    const std::string_view method_text = request_line.substr(0, first_space);
    for (const char character : method_text) {
        if (!isTokenCharacter(character)) {
            return fail("HTTP method is not a valid token");
        }
    }
    const HttpMethod method = parseMethod(method_text);

    const std::string_view target =
        request_line.substr(first_space + 1U,
                            second_space - first_space - 1U);
    const std::string_view version = request_line.substr(second_space + 1U);
    if (target.empty()) {
        return fail("request target is empty");
    }
    if (version != "HTTP/1.1") {
        return fail("only HTTP/1.1 is supported");
    }

    HttpRequest request;
    request.method = method;
    request.target.assign(target);
    request.version.assign(version);

    std::size_t cursor = request_line_end + kCrlf.size();
    std::size_t header_count = 0;
    while (cursor < header_terminator) {
        const std::size_t line_end = data.find(kCrlf, cursor);
        if (line_end == std::string_view::npos ||
            line_end > header_terminator || line_end == cursor) {
            return fail("malformed header line");
        }

        const std::string_view line = data.substr(cursor, line_end - cursor);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0U) {
            return fail("header name is empty or missing a colon");
        }

        const std::optional<std::string> normalized_name =
            normalizeHeaderName(line.substr(0, colon));
        if (!normalized_name.has_value()) {
            return fail("header name is not a valid token");
        }

        const std::string_view value =
            trimOptionalWhitespace(line.substr(colon + 1U));
        if (!isValidHeaderValue(value)) {
            return fail("header value contains a control character");
        }

        ++header_count;
        if (header_count > kMaxHeaderCount) {
            return fail("header count exceeds 100");
        }

        const auto insertion = request.headers.emplace(
            normalized_name.value(), std::string{value});
        if (!insertion.second) {
            return fail("duplicate headers are not supported");
        }

        cursor = line_end + kCrlf.size();
    }

    const auto host = request.headers.find("host");
    if (host == request.headers.end() || host->second.empty()) {
        return fail("HTTP/1.1 requires a non-empty Host header");
    }
    if (request.headers.find("transfer-encoding") != request.headers.end()) {
        return fail("Transfer-Encoding is not supported");
    }

    const auto content_length = request.headers.find("content-length");
    const bool has_content_length = content_length != request.headers.end();
    std::size_t parsed_content_length = 0;
    if (has_content_length) {
        std::string error;
        if (!parseContentLength(content_length->second,
                                parsed_content_length, error)) {
            return fail(std::move(error));
        }
        if (parsed_content_length > kMaxUploadBytes) {
            return fail("Content-Length exceeds 1 GiB",
                        HttpParseError::kPayloadTooLarge);
        }
    }

    if (request.method == HttpMethod::kPut) {
        if (!has_content_length) {
            return fail("PUT requires Content-Length");
        }
    } else if (request.method != HttpMethod::kUnsupported &&
               has_content_length && parsed_content_length != 0U) {
        return fail("this method does not accept a request body");
    }

    request.content_length = parsed_content_length;
    request_ = std::move(request);
    consumed_bytes_ = header_size;
    status_ = ParseStatus::kHeadersComplete;
    return status_;
}

ParseStatus HttpParser::fail(std::string message,
                             HttpParseError error) {
    status_ = ParseStatus::kError;
    error_ = error;
    error_message_ = std::move(message);
    request_.reset();
    return status_;
}

}  // namespace faststore
