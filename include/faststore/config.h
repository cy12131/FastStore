#ifndef FASTSTORE_CONFIG_H
#define FASTSTORE_CONFIG_H

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace faststore {

struct ServerConfig {
    std::uint16_t port{8080};
    std::filesystem::path data_directory{"data"};
    std::size_t worker_count{4};
    bool informational_logging_enabled{true};
};

// 只读取并验证环境；目录创建和其他启动副作用仍由 main 负责。
[[nodiscard]] ServerConfig loadServerConfig();

}  // namespace faststore

#endif  // FASTSTORE_CONFIG_H
