#include "faststore/config.h"

#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace faststore {
namespace {

std::size_t parseUnsignedInteger(std::string_view value,
                                 std::size_t minimum,
                                 std::size_t maximum,
                                 std::string_view variable) {
    std::size_t parsed = 0;
    // from_chars 不做隐式 trim；ptr 必须到达末尾，防止 "4abc" 被部分接受。
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (value.empty() || result.ec != std::errc{} ||
        result.ptr != value.data() + value.size() ||
        parsed < minimum || parsed > maximum) {
        throw std::invalid_argument(
            "Invalid " + std::string{variable} +
            ": expected integer in range " + std::to_string(minimum) +
            ".." + std::to_string(maximum));
    }
    return parsed;
}

}  // namespace

ServerConfig loadServerConfig() {
    ServerConfig config;
    // 保持现有二进制的构建时默认目录；环境变量只在显式设置时覆盖。
    config.data_directory = FASTSTORE_DATA_DIRECTORY;

    // 未设置沿用默认值；显式设置则必须合法，绝不静默回退。
    if (const char* value = std::getenv("FASTSTORE_PORT")) {
        config.port = static_cast<std::uint16_t>(parseUnsignedInteger(
            value, 1U, 65535U, "FASTSTORE_PORT"));
    }

    if (const char* value = std::getenv("FASTSTORE_DATA_DIR")) {
        if (*value == '\0') {
            throw std::invalid_argument(
                "FASTSTORE_DATA_DIR must not be empty");
        }
        config.data_directory = value;
    }

    if (const char* value = std::getenv("FASTSTORE_WORKERS")) {
        config.worker_count = parseUnsignedInteger(
            value, 1U, 256U, "FASTSTORE_WORKERS");
    }

    if (const char* value = std::getenv("FASTSTORE_QUIET")) {
        const std::string_view quiet{value};
        if (quiet == "0") {
            config.informational_logging_enabled = true;
        } else if (quiet == "1") {
            config.informational_logging_enabled = false;
        } else {
            throw std::invalid_argument(
                "Invalid FASTSTORE_QUIET: expected 0 or 1");
        }
    }

    return config;
}

}  // namespace faststore
