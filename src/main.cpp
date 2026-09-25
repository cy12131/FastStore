#include "faststore/server.h"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace {

// 输出 data 目录初始化错误并返回进程失败状态。
// path 是发生错误的目录，message 描述操作，error 保存系统错误码。
int report_data_directory_error(const std::filesystem::path& path,
                                std::string_view message,
                                const std::error_code& error = {}) {
    std::cerr << "FastStore initialization failed: " << message
              << ": " << path;
    if (error) {
        std::cerr << " (" << error.message() << ')';
    }
    std::cerr << '\n';
    return EXIT_FAILURE;
}

}  // namespace

// 程序入口：准备 data 目录，然后创建并启动 TCP Server。
int main() {
    // port 是 M1 阶段固定使用的 TCP 监听端口。
    constexpr std::uint16_t port = 8080;

    // data_directory 是构建时配置的文件数据目录绝对路径。
    const std::filesystem::path data_directory{FASTSTORE_DATA_DIRECTORY};

    std::cout << "FastStore starting\n";
    std::cout << "Data directory: " << data_directory << '\n';

    // error 接收 filesystem API 的错误，避免异常中断初始化流程。
    std::error_code error;

    // exists 表示配置的数据路径当前是否已经存在。
    const bool exists = std::filesystem::exists(data_directory, error);
    if (error) {
        return report_data_directory_error(
            data_directory, "unable to inspect data directory", error);
    }

    if (!exists) {
        if (!std::filesystem::create_directories(data_directory, error)) {
            return report_data_directory_error(
                data_directory, "unable to create data directory", error);
        }
    }

    if (!std::filesystem::is_directory(data_directory, error)) {
        return report_data_directory_error(
            data_directory, "data path is not a directory", error);
    }

    auto file_service_result = faststore::openFileService(data_directory);
    if (!file_service_result.ok()) {
        std::cerr << "FastStore initialization failed: unable to open data "
                     "directory safely (FileError="
                  << static_cast<int>(file_service_result.error()) << ")\n";
        return EXIT_FAILURE;
    }

    try {
        // Server 拥有 FileService、监听资源和所有客户端连接。
        faststore::Server server{port, file_service_result.takeValue()};
        server.run();
    } catch (const std::exception& error) {
        // error 保存 Server 初始化或事件循环抛出的异常信息。
        std::cerr << "FastStore server failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
