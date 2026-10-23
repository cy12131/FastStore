#include "faststore/config.h"
#include "faststore/server.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
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
    try {
        // Config 只解析并校验；从这里开始才允许产生目录和网络副作用。
        const faststore::ServerConfig config = faststore::loadServerConfig();

        if (config.informational_logging_enabled) {
            std::cout << "FastStore starting: port=" << config.port
                      << " data=" << config.data_directory
                      << " workers=" << config.worker_count
                      << " quiet=false\n";
        }

        // error 接收 filesystem API 的错误，避免异常中断初始化流程。
        std::error_code error;
        const bool exists =
            std::filesystem::exists(config.data_directory, error);
        if (error) {
            return report_data_directory_error(
                config.data_directory,
                "unable to inspect data directory", error);
        }

        if (!exists &&
            !std::filesystem::create_directories(
                config.data_directory, error)) {
            return report_data_directory_error(
                config.data_directory,
                "unable to create data directory", error);
        }

        if (!std::filesystem::is_directory(
                config.data_directory, error)) {
            return report_data_directory_error(
                config.data_directory,
                "data path is not a directory", error);
        }

        auto file_service_result =
            faststore::openFileService(config.data_directory);
        if (!file_service_result.ok()) {
            std::cerr << "FastStore initialization failed: unable to open "
                         "data directory safely (FileError="
                      << static_cast<int>(file_service_result.error())
                      << ")\n";
            return EXIT_FAILURE;
        }

        // Server 不读取环境变量，只接收已经验证的配置值。
        faststore::Server server{
            config.port, file_service_result.takeValue(),
            config.worker_count, config.informational_logging_enabled};
        server.run();
    } catch (const std::exception& error) {
        // 配置错误在创建目录、Server、listener socket 前到达这里。
        std::cerr << "FastStore startup failed: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
