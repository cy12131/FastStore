#include "faststore/http.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using faststore::HttpMethod;
using faststore::HttpParser;
using faststore::HttpRequest;
using faststore::ParseStatus;

std::vector<char> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

void append(std::vector<char>& destination, std::string_view text) {
    destination.insert(destination.end(), text.begin(), text.end());
}

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

void expectStatus(HttpParser& parser,
                  const std::vector<char>& input,
                  ParseStatus expected,
                  std::string_view message) {
    expect(parser.parse(input) == expected, message);
}

void expectError(std::string_view request) {
    HttpParser parser;
    expectStatus(parser, bytes(request), ParseStatus::kError,
                 "expected parse error");
    expect(!parser.errorMessage().empty(),
           "parse error must include a message");
}

void testNormalGet() {
    HttpParser parser;
    const auto input =
        bytes("GET /files/test.txt HTTP/1.1\r\n"
              "Host: localhost\r\n\r\n");

    expectStatus(parser, input, ParseStatus::kComplete,
                 "GET should complete");
    const HttpRequest* request = parser.request();
    expect(request != nullptr, "GET request is missing");
    expect(request->method == HttpMethod::kGet, "wrong GET method");
    expect(request->target == "/files/test.txt", "wrong GET target");
    expect(request->version == "HTTP/1.1", "wrong HTTP version");
    expect(request->content_length == 0U, "wrong GET length");
    expect(request->body.empty(), "GET body must be empty");
}

void testNormalPutAndHeaderNormalization() {
    HttpParser parser;
    const auto input =
        bytes("PUT /files/test.txt HTTP/1.1\r\n"
              "hOsT:\tlocalhost \t\r\n"
              "CONTENT-LENGTH: 5\r\n\r\n"
              "hello");

    expectStatus(parser, input, ParseStatus::kComplete,
                 "PUT should complete");
    const HttpRequest* request = parser.request();
    expect(request != nullptr, "PUT request is missing");
    expect(request->method == HttpMethod::kPut, "wrong PUT method");
    expect(request->content_length == 5U, "wrong PUT length");
    expect(request->headers.at("host") == "localhost",
           "Host normalization failed");
    expect(request->headers.at("content-length") == "5",
           "Content-Length normalization failed");
    expect(std::string(request->body.begin(), request->body.end()) == "hello",
           "wrong PUT body");
}

void testNormalDeleteAndHead() {
    HttpParser delete_parser;
    const auto delete_input =
        bytes("DELETE /files/test.txt HTTP/1.1\r\n"
              "Host: localhost\r\n\r\n");
    expectStatus(delete_parser, delete_input, ParseStatus::kComplete,
                 "DELETE should complete");
    expect(delete_parser.request()->method == HttpMethod::kDelete,
           "wrong DELETE method");

    HttpParser head_parser;
    const auto head_input =
        bytes("HEAD /files/test.txt HTTP/1.1\r\n"
              "Host: localhost\r\n\r\n");
    expectStatus(head_parser, head_input, ParseStatus::kComplete,
                 "HEAD should complete");
    expect(head_parser.request()->method == HttpMethod::kHead,
           "wrong HEAD method");
}

void testThreePartIncrementalPut() {
    HttpParser parser;
    std::vector<char> input;

    append(input, "PUT /files/a.txt HTTP/1.1\r\nHo");
    expectStatus(parser, input, ParseStatus::kIncomplete,
                 "first fragment must be incomplete");

    append(input, "st: localhost\r\nContent-Length: 5\r\n\r\nhe");
    expectStatus(parser, input, ParseStatus::kIncomplete,
                 "second fragment must be incomplete");

    append(input, "llo");
    expectStatus(parser, input, ParseStatus::kComplete,
                 "third fragment must complete");
    expect(parser.request()->body.size() == 5U,
           "fragmented body size is wrong");
}

void testSplitHeaderTerminator() {
    HttpParser parser;
    auto input =
        bytes("GET /files/a.txt HTTP/1.1\r\n"
              "Host: localhost\r\n\r");

    expectStatus(parser, input, ParseStatus::kIncomplete,
                 "split terminator must be incomplete");
    append(input, "\n");
    expectStatus(parser, input, ParseStatus::kComplete,
                 "completed terminator must parse");
}

void testRequiredAndUnsupportedFields() {
    expectError("GET / HTTP/1.1\r\n\r\n");
    expectError("PUT / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectError("POST / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectError("GET / HTTP/1.0\r\nHost: localhost\r\n\r\n");
    expectError("GET / HTTP/1.1 EXTRA\r\nHost: localhost\r\n\r\n");
    expectError("GET / HTTP/1.1\r\nBroken-Header\r\n\r\n");
    expectError("GET / HTTP/1.1\r\n: value\r\nHost: localhost\r\n\r\n");
    expectError("GET / HTTP/1.1\r\nHost: localhost\r\n"
                "Transfer-Encoding: chunked\r\n\r\n");
}

void testContentLengthValidation() {
    expectError("PUT / HTTP/1.1\r\nHost: localhost\r\n"
                "Content-Length: abc\r\n\r\n");
    expectError("PUT / HTTP/1.1\r\nHost: localhost\r\n"
                "Content-Length: -1\r\n\r\n");
    expectError("PUT / HTTP/1.1\r\nHost: localhost\r\n"
                "Content-Length: 67108865\r\n\r\n");
    expectError("PUT / HTTP/1.1\r\nHost: localhost\r\n"
                "Content-Length: 184467440737095516160\r\n\r\n");
    expectError("GET / HTTP/1.1\r\nHost: localhost\r\n"
                "Content-Length: 1\r\n\r\nx");
}

void testDuplicateContentLengthRejected() {
    expectError("PUT / HTTP/1.1\r\nHost: localhost\r\n"
                "Content-Length: 5\r\nContent-Length: 6\r\n\r\nhello");
    expectError("PUT / HTTP/1.1\r\nHost: localhost\r\n"
                "Content-Length: 5\r\ncontent-length: 5\r\n\r\nhello");
}

void testRequestLineLimit() {
    std::string request = "GET /";
    request.append(HttpParser::kMaxRequestLineSize, 'a');

    HttpParser parser;
    expectStatus(parser, bytes(request), ParseStatus::kError,
                 "oversized request line must fail");
}

void testHeaderSectionLimit() {
    std::string request =
        "GET / HTTP/1.1\r\nHost: localhost\r\nX-Large: ";
    request.append(HttpParser::kMaxHeaderSectionSize, 'a');

    HttpParser parser;
    expectStatus(parser, bytes(request), ParseStatus::kError,
                 "oversized header section must fail");
}

void testHeaderCountLimit() {
    std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\n";
    for (std::size_t index = 0; index < HttpParser::kMaxHeaderCount; ++index) {
        request += "X-" + std::to_string(index) + ": value\r\n";
    }
    request += "\r\n";

    HttpParser parser;
    expectStatus(parser, bytes(request), ParseStatus::kError,
                 "too many headers must fail");
}

void testBodyBoundaries() {
    HttpParser parser;
    auto input =
        bytes("PUT / HTTP/1.1\r\nHost: localhost\r\n"
              "Content-Length: 5\r\n\r\nhe");

    expectStatus(parser, input, ParseStatus::kIncomplete,
                 "short body must be incomplete");
    append(input, "llo");
    expectStatus(parser, input, ParseStatus::kComplete,
                 "exact body must complete");

    HttpParser extra_parser;
    const auto extra =
        bytes("PUT / HTTP/1.1\r\nHost: localhost\r\n"
              "Content-Length: 5\r\n\r\nhello!");
    expectStatus(extra_parser, extra, ParseStatus::kError,
                 "extra body bytes must fail");
}

void testZeroLengthPut() {
    HttpParser parser;
    const auto input =
        bytes("PUT /empty HTTP/1.1\r\nHost: localhost\r\n"
              "Content-Length: 0\r\n\r\n");
    expectStatus(parser, input, ParseStatus::kComplete,
                 "zero-length PUT should complete");
    expect(parser.request()->body.empty(),
           "zero-length PUT body must be empty");
}

struct TestCase {
    std::string_view name;
    void (*function)();
};

}  // namespace

int main() {
    const std::vector<TestCase> tests{
        {"normal GET", testNormalGet},
        {"normal PUT and header normalization",
         testNormalPutAndHeaderNormalization},
        {"normal DELETE and HEAD", testNormalDeleteAndHead},
        {"three-part incremental PUT", testThreePartIncrementalPut},
        {"split header terminator", testSplitHeaderTerminator},
        {"required and unsupported fields",
         testRequiredAndUnsupportedFields},
        {"Content-Length validation", testContentLengthValidation},
        {"duplicate Content-Length", testDuplicateContentLengthRejected},
        {"request-line limit", testRequestLineLimit},
        {"header-section limit", testHeaderSectionLimit},
        {"header-count limit", testHeaderCountLimit},
        {"body boundaries", testBodyBoundaries},
        {"zero-length PUT", testZeroLengthPut},
    };

    std::size_t passed = 0;
    for (const TestCase& test : tests) {
        try {
            test.function();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": "
                      << error.what() << '\n';
            return 1;
        }
    }

    std::cout << passed << " parser test groups passed\n";
    return 0;
}
