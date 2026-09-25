#ifndef FASTSTORE_ROUTER_H
#define FASTSTORE_ROUTER_H

#include "faststore/file_service.h"
#include "faststore/http.h"
#include "faststore/http_response.h"

#include <optional>
#include <string>
#include <string_view>

namespace faststore {

// Router 同步决策的结果。只有成功 GET 携带独占 ReadFile。
struct RouteResult {
    HttpResponse response;
    std::optional<ReadFile> read_file;
};

// 解析 V0 唯一支持的 /files/{single-basename} target，不做 URL decoding。
[[nodiscard]] std::optional<std::string> extractFileName(
    std::string_view target);

// 将存储层错误集中转换为不泄露内部细节的 HTTP response。
[[nodiscard]] HttpResponse responseForFileError(FileError error);

// 同步执行方法对应的最小文件操作；GET 只打开文件，不读取文件体。
[[nodiscard]] RouteResult routeRequest(FileService& file_service,
                                       const HttpRequest& request);

}  // namespace faststore

#endif  // FASTSTORE_ROUTER_H
