#ifndef FASTSTORE_ROUTER_H
#define FASTSTORE_ROUTER_H

#include "faststore/file_service.h"
#include "faststore/http.h"
#include "faststore/http_response.h"

#include <optional>
#include <string>
#include <string_view>

namespace faststore {

// 解析 V0 唯一支持的 /files/{single-basename} target，不做 URL decoding。
[[nodiscard]] std::optional<std::string> extractFileName(
    std::string_view target);

// 将存储层错误集中转换为不泄露内部细节的 HTTP response。
[[nodiscard]] HttpResponse responseForFileError(FileError error);

// 同步执行 M5-A 方法对应的最小文件操作并生成 response。
[[nodiscard]] HttpResponse routeRequest(FileService& file_service,
                                        const HttpRequest& request);

}  // namespace faststore

#endif  // FASTSTORE_ROUTER_H
