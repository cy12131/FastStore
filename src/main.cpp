#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <system_error>

namespace {

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

int main() {
    const std::filesystem::path data_directory{FASTSTORE_DATA_DIRECTORY};

    std::cout << "FastStore V0 starting\n";
    std::cout << "Data directory: " << data_directory << '\n';

    std::error_code error;
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

    std::cout << "FastStore initialization complete\n";
    return EXIT_SUCCESS;
}
