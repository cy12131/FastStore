#include "faststore/router.h"

#include <utility>

namespace faststore {
namespace {

constexpr std::string_view kFilesPrefix = "/files/";

RouteResult responseOnly(HttpResponse response) {
    return RouteResult{std::move(response), std::nullopt};
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
