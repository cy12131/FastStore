#include "faststore/http.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using faststore::HttpMethod;
using faststore::HttpParseError;
using faststore::HttpParser;
using faststore::HttpRequest;
using faststore::ParseStatus;

std::vector<char> bytes(std::string_view text) {
    return {text.begin(), text.end()};
}

std::string_view view(const std::vector<char>& input) {
    return {input.data(), input.size()};
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
    expect(parser.parse(view(input)) == expected, message);
}

void expectError(std::string_view request) {
    HttpParser parser;
    expect(parser.parse(request) == ParseStatus::kError,
           "expected parse error");
    expect(!parser.errorMessage().empty(),
           "parse error must include a message");
}

void testPartialAndCompleteGetHeaders() {
    HttpParser parser;
    auto input = bytes("GET /files/test.txt HTTP/1.1\r\nHo");
    expectStatus(parser, input, ParseStatus::kIncomplete,
                 "partial GET headers must be incomplete");

    append(input, "st: localhost\r\n\r\n");
    expectStatus(parser, input, ParseStatus::kHeadersComplete,
                 "complete GET headers were not reported");
    const HttpRequest* request = parser.request();
    expect(request != nullptr, "GET metadata is missing");
    expect(request->method == HttpMethod::kGet, "wrong GET method");
    expect(request->target == "/files/test.txt", "wrong GET target");
    expect(request->version == "HTTP/1.1", "wrong HTTP version");
    expect(request->content_length == 0U, "wrong GET length");
    expect(parser.consumedBytes() == input.size(),
           "GET header byte count is wrong");
}

void testPutMetadataWithoutBodyCopy() {
    HttpParser parser;
    const std::string header =
        "PUT /files/test.txt HTTP/1.1\r\n"
        "hOsT:\tlocalhost \t\r\n"
        "CONTENT-LENGTH: 5\r\n\r\n";
    const auto input = bytes(header + "he");

    expectStatus(parser, input, ParseStatus::kHeadersComplete,
                 "PUT headers should complete before its body");
    const HttpRequest* request = parser.request();
    expect(request != nullptr, "PUT metadata is missing");
    expect(request->method == HttpMethod::kPut, "wrong PUT method");
    expect(request->content_length == 5U, "wrong PUT length");
    expect(request->headers.at("host") == "localhost",
           "Host normalization failed");
    expect(request->headers.at("content-length") == "5",
           "Content-Length normalization failed");
    expect(parser.consumedBytes() == header.size(),
           "parser consumed body bytes");
}

void testBodyBytesDoNotAffectParserCompletion() {
    const std::string header =
        "PUT /files/a.txt HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 5\r\n\r\n";
    for (const std::string suffix : {"", "he", "hello", "helloEXTRA"}) {
        HttpParser parser;
        expect(parser.parse(header + suffix) == ParseStatus::kHeadersComplete,
               "parser waited for or validated body bytes");
        expect(parser.consumedBytes() == header.size(),
               "parser consumed bytes beyond headers");
    }
}

void testSplitHeaderTerminator() {
    HttpParser parser;
    auto input = bytes("GET /files/a.txt HTTP/1.1\r\n"
                       "Host: localhost\r\n\r");
    expectStatus(parser, input, ParseStatus::kIncomplete,
                 "split terminator must be incomplete");
    append(input, "\nBODY");
    expectStatus(parser, input, ParseStatus::kHeadersComplete,
                 "completed terminator must parse");
    expect(parser.consumedBytes() + 4U == input.size(),
           "body after split terminator was consumed");
}

void testDeleteAndHead() {
    HttpParser delete_parser;
    expect(delete_parser.parse(
               "DELETE /files/test.txt HTTP/1.1\r\nHost: localhost\r\n\r\n") ==
               ParseStatus::kHeadersComplete,
           "DELETE headers should complete");
    expect(delete_parser.request()->method == HttpMethod::kDelete,
           "wrong DELETE method");

    HttpParser head_parser;
    expect(head_parser.parse(
               "HEAD /files/test.txt HTTP/1.1\r\nHost: localhost\r\n\r\n") ==
               ParseStatus::kHeadersComplete,
           "HEAD headers should complete");
    expect(head_parser.request()->method == HttpMethod::kHead,
           "wrong HEAD method");
}

void testRequiredAndUnsupportedFields() {
    expectError("GET / HTTP/1.1\r\n\r\n");
    expectError("PUT / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectError("GET / HTTP/1.0\r\nHost: localhost\r\n\r\n");
    expectError("GET / HTTP/1.1 EXTRA\r\nHost: localhost\r\n\r\n");
    expectError("G@T / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectError("GET / HTTP/1.1\r\nBroken-Header\r\n\r\n");
    expectError("GET / HTTP/1.1\r\n: value\r\nHost: localhost\r\n\r\n");
    expectError("GET / HTTP/1.1\r\nHost: localhost\r\n"
                "Transfer-Encoding: chunked\r\n\r\n");
}

void testUnsupportedMethodMetadata() {
    HttpParser parser;
    expect(parser.parse(
               "POST /files/a.txt HTTP/1.1\r\nHost: localhost\r\n\r\n") ==
               ParseStatus::kHeadersComplete,
           "unknown valid method should reach routing");
    expect(parser.request() != nullptr &&
               parser.request()->method == HttpMethod::kUnsupported,
           "unknown method metadata is wrong");
}

void testContentLengthValidation() {
    expectError("PUT / HTTP/1.1\r\nHost: localhost\r\n"
                "Content-Length: abc\r\n\r\n");
    expectError("PUT / HTTP/1.1\r\nHost: localhost\r\n"
                "Content-Length: -1\r\n\r\n");
    HttpParser maximum_parser;
    expect(maximum_parser.parse(
               "PUT / HTTP/1.1\r\nHost: localhost\r\n"
               "Content-Length: 1073741824\r\n\r\n") ==
               ParseStatus::kHeadersComplete,
           "exactly 1 GiB should be accepted by the protocol layer");
    expect(maximum_parser.request() != nullptr &&
               maximum_parser.request()->content_length ==
                   HttpParser::kMaxUploadBytes,
           "1 GiB Content-Length metadata is wrong");

    HttpParser too_large_parser;
    expect(too_large_parser.parse(
               "PUT / HTTP/1.1\r\nHost: localhost\r\n"
               "Content-Length: 1073741825\r\n\r\n") ==
               ParseStatus::kError,
           "oversized body should fail");
    expect(too_large_parser.error() == HttpParseError::kPayloadTooLarge,
           "oversized body should be classified for HTTP 413");
    expectError("PUT / HTTP/1.1\r\nHost: localhost\r\n"
                "Content-Length: 184467440737095516160\r\n\r\n");
    expectError("GET / HTTP/1.1\r\nHost: localhost\r\n"
                "Content-Length: 1\r\n\r\n");
}

void testDuplicateContentLengthRejected() {
    expectError("PUT / HTTP/1.1\r\nHost: localhost\r\n"
                "Content-Length: 5\r\nContent-Length: 6\r\n\r\n");
    expectError("PUT / HTTP/1.1\r\nHost: localhost\r\n"
                "Content-Length: 5\r\ncontent-length: 5\r\n\r\n");
}

void testRequestLineLimit() {
    std::string request = "GET /";
    request.append(HttpParser::kMaxRequestLineSize, 'a');
    HttpParser parser;
    expect(parser.parse(request) == ParseStatus::kError,
           "oversized request line must fail");
}

void testHeaderSectionLimit() {
    std::string request =
        "GET / HTTP/1.1\r\nHost: localhost\r\nX-Large: ";
    request.append(HttpParser::kMaxHeaderSectionSize, 'a');
    HttpParser parser;
    expect(parser.parse(request) == ParseStatus::kError,
           "oversized header section must fail");
}

void testHeaderCountLimit() {
    std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\n";
    for (std::size_t index = 0; index < HttpParser::kMaxHeaderCount; ++index) {
        request += "X-" + std::to_string(index) + ": value\r\n";
    }
    request += "\r\n";

    HttpParser parser;
    expect(parser.parse(request) == ParseStatus::kError,
           "too many headers must fail");
}

void testZeroLengthPutMetadata() {
    HttpParser parser;
    expect(parser.parse(
               "PUT /files/empty.txt HTTP/1.1\r\nHost: localhost\r\n"
               "Content-Length: 0\r\n\r\n") ==
               ParseStatus::kHeadersComplete,
           "zero-length PUT headers should complete");
    expect(parser.request()->content_length == 0U,
           "zero-length PUT metadata is wrong");
}

struct TestCase {
    std::string_view name;
    void (*function)();
};

}  // namespace

int main() {
    const std::vector<TestCase> tests{
        {"partial and complete GET headers", testPartialAndCompleteGetHeaders},
        {"PUT metadata without body copy", testPutMetadataWithoutBodyCopy},
        {"body bytes ignored by parser", testBodyBytesDoNotAffectParserCompletion},
        {"split header terminator", testSplitHeaderTerminator},
        {"DELETE and HEAD", testDeleteAndHead},
        {"required and unsupported fields", testRequiredAndUnsupportedFields},
        {"unsupported method metadata", testUnsupportedMethodMetadata},
        {"Content-Length validation", testContentLengthValidation},
        {"duplicate Content-Length", testDuplicateContentLengthRejected},
        {"request-line limit", testRequestLineLimit},
        {"header-section limit", testHeaderSectionLimit},
        {"header-count limit", testHeaderCountLimit},
        {"zero-length PUT metadata", testZeroLengthPutMetadata},
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
