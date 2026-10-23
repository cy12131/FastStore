#include "faststore/router.h"

#include <string>
#include <utility>

namespace faststore {
namespace {

constexpr std::string_view kFilesPrefix = "/files/";
constexpr std::string_view kFilesCollection = "/files";

RouteResult responseOnly(HttpResponse response) {
    return RouteResult{std::move(response), std::nullopt};
}

void appendJsonString(std::string_view value, std::string& output) {
    constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char byte : value) {
        switch (byte) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (byte < 0x20U) {
                    output += "\\u00";
                    output.push_back(hex[byte >> 4U]);
                    output.push_back(hex[byte & 0x0FU]);
                } else {
                    output.push_back(static_cast<char>(byte));
                }
                break;
        }
    }
    output.push_back('"');
}

HttpResponse makeFileListResponse(const std::vector<FileEntry>& entries) {
    std::string body{"{\"files\":["};
    bool first = true;
    for (const FileEntry& entry : entries) {
        if (!first) {
            body.push_back(',');
        }
        first = false;
        body += "{\"name\":";
        appendJsonString(entry.name, body);
        body += ",\"size\":";
        body += std::to_string(entry.size);
        body.push_back('}');
    }
    body += "]}";

    HttpResponse response = makeResponse(HttpStatus::kOk, body.size());
    response.headers.emplace_back("Content-Type", "application/json");
    response.body = std::move(body);
    return response;
}

}  // namespace

std::optional<std::string> extractFileName(std::string_view target) {
    if (target.size() <= kFilesPrefix.size() ||
        target.substr(0, kFilesPrefix.size()) != kFilesPrefix) {
        return std::nullopt;
    }

    const std::string_view filename = target.substr(kFilesPrefix.size());
    if (validateFilename(filename) != FileError::kNone) {
        return std::nullopt;
    }
    return std::string{filename};
}

HttpResponse responseForFileError(FileError error) {
    switch (error) {
        case FileError::kInvalidName:
            return makeResponse(HttpStatus::kBadRequest);
        case FileError::kNotFound:
        case FileError::kNotRegularFile:
            return makeResponse(HttpStatus::kNotFound);
        case FileError::kTooLarge:
            return makeResponse(HttpStatus::kPayloadTooLarge);
        case FileError::kNone:
        case FileError::kAlreadyExists:
        case FileError::kPermissionDenied:
        case FileError::kInvalidState:
        case FileError::kIoError:
            return makeResponse(HttpStatus::kInternalServerError);
    }
    return makeResponse(HttpStatus::kInternalServerError);
}

RouteResult routeRequest(FileService& file_service,
                         const HttpRequest& request) {
    if (request.target == kFilesCollection &&
        request.method == HttpMethod::kGet) {
        auto listed = file_service.listFiles();
        if (!listed.ok()) {
            return responseOnly(
                makeResponse(HttpStatus::kInternalServerError));
        }
        return responseOnly(makeFileListResponse(listed.value()));
    }

    const auto filename = extractFileName(request.target);
    if (!filename.has_value()) {
        return responseOnly(makeResponse(HttpStatus::kBadRequest));
    }

    switch (request.method) {
        case HttpMethod::kPut:
            // Streaming PUT 已由 Connection 成功 commit，Router 不重复写文件。
            return responseOnly(makeResponse(HttpStatus::kCreated));
        case HttpMethod::kGet: {
            auto opened = file_service.openRead(filename.value());
            if (!opened.ok()) {
                return responseOnly(responseForFileError(opened.error()));
            }

            ReadFile file = opened.takeValue();
            RouteResult result{
                makeResponse(HttpStatus::kOk, file.size()), std::nullopt};
            result.read_file.emplace(std::move(file));
            return result;
        }
        case HttpMethod::kHead: {
            const auto metadata = file_service.metadata(filename.value());
            if (!metadata.ok()) {
                return responseOnly(responseForFileError(metadata.error()));
            }
            return responseOnly(
                makeResponse(HttpStatus::kOk, metadata.value().size));
        }
        case HttpMethod::kDelete: {
            const auto removed = file_service.remove(filename.value());
            if (!removed.ok()) {
                return responseOnly(responseForFileError(removed.error()));
            }
            return responseOnly(makeResponse(HttpStatus::kNoContent));
        }
        case HttpMethod::kUnsupported: {
            HttpResponse response =
                makeResponse(HttpStatus::kMethodNotAllowed);
            response.headers.emplace_back(
                "Allow", "GET, PUT, DELETE, HEAD");
            return responseOnly(std::move(response));
        }
    }
    return responseOnly(makeResponse(HttpStatus::kInternalServerError));
}

}  // namespace faststore
