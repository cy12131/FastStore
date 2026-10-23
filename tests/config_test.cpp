#include "faststore/config.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using faststore::ServerConfig;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

// 每个测试保存变量原值，析构时精确恢复，避免依赖执行顺序或污染用户环境。
class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(std::string name,
                              std::optional<std::string> value)
        : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str())) {
            previous_value_ = previous;
        }
        set(value);
    }

    ~ScopedEnvironmentVariable() {
        if (previous_value_.has_value()) {
            ::setenv(name_.c_str(), previous_value_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator=(
        const ScopedEnvironmentVariable&) = delete;

private:
    void set(const std::optional<std::string>& value) {
        const int result = value.has_value()
                               ? ::setenv(name_.c_str(), value->c_str(), 1)
                               : ::unsetenv(name_.c_str());
        if (result != 0) {
            throw std::runtime_error("unable to update test environment");
        }
    }

    std::string name_;
    std::optional<std::string> previous_value_;
};

class CleanConfigEnvironment {
public:
    CleanConfigEnvironment()
        : port_("FASTSTORE_PORT", std::nullopt),
          data_("FASTSTORE_DATA_DIR", std::nullopt),
          workers_("FASTSTORE_WORKERS", std::nullopt),
          quiet_("FASTSTORE_QUIET", std::nullopt) {}

private:
    ScopedEnvironmentVariable port_;
    ScopedEnvironmentVariable data_;
    ScopedEnvironmentVariable workers_;
    ScopedEnvironmentVariable quiet_;
};

void expectInvalid(std::string_view variable,
                   std::string_view value,
                   std::string_view expected_message) {
    CleanConfigEnvironment clean;
    ScopedEnvironmentVariable setting{
        std::string{variable}, std::string{value}};
    try {
        static_cast<void>(faststore::loadServerConfig());
    } catch (const std::invalid_argument& error) {
        expect(std::string_view{error.what()}.find(expected_message) !=
                   std::string_view::npos,
               "configuration error message lacks variable context");
        return;
    }
    throw std::runtime_error("invalid configuration was accepted");
}

void testDefaults() {
    CleanConfigEnvironment clean;
    const ServerConfig config = faststore::loadServerConfig();
    expect(config.port == 8080U, "default port is wrong");
    expect(config.data_directory ==
               std::filesystem::path{FASTSTORE_DATA_DIRECTORY},
           "default data directory changed");
    expect(config.worker_count == 4U, "default worker count is wrong");
    expect(config.informational_logging_enabled,
           "informational logging should default to enabled");
}

void testValidOverrides() {
    CleanConfigEnvironment clean;
    ScopedEnvironmentVariable port{"FASTSTORE_PORT", "9090"};
    ScopedEnvironmentVariable data{
        "FASTSTORE_DATA_DIR", "/tmp/faststore-config-test"};
    ScopedEnvironmentVariable workers{"FASTSTORE_WORKERS", "2"};
    ScopedEnvironmentVariable quiet{"FASTSTORE_QUIET", "1"};

    const ServerConfig config = faststore::loadServerConfig();
    expect(config.port == 9090U, "port override was ignored");
    expect(config.data_directory == "/tmp/faststore-config-test",
           "data directory override was ignored");
    expect(config.worker_count == 2U, "worker override was ignored");
    expect(!config.informational_logging_enabled,
           "quiet=1 did not disable informational logging");
}

void testQuietZero() {
    CleanConfigEnvironment clean;
    ScopedEnvironmentVariable quiet{"FASTSTORE_QUIET", "0"};
    expect(faststore::loadServerConfig().informational_logging_enabled,
           "quiet=0 did not enable informational logging");
}

void testValidBoundaries() {
    CleanConfigEnvironment clean;
    ScopedEnvironmentVariable port{"FASTSTORE_PORT", "65535"};
    ScopedEnvironmentVariable workers{"FASTSTORE_WORKERS", "256"};
    const ServerConfig maximum = faststore::loadServerConfig();
    expect(maximum.port == 65535U, "maximum port was rejected");
    expect(maximum.worker_count == 256U,
           "maximum worker count was rejected");
}

void testInvalidPorts() {
    for (const std::string_view value :
         {"", "0", "65536", "70000", "999999999999999999999999",
          "abc", "8080abc", "-1",
          " 8080", "8080 "}) {
        expectInvalid("FASTSTORE_PORT", value, "FASTSTORE_PORT");
    }
}

void testInvalidWorkers() {
    for (const std::string_view value :
         {"", "0", "257", "999999", "999999999999999999999999",
          "abc", "4abc", "-1"}) {
        expectInvalid("FASTSTORE_WORKERS", value, "FASTSTORE_WORKERS");
    }
}

void testInvalidQuiet() {
    for (const std::string_view value :
         {"", "2", "true", "yes", "abc"}) {
        expectInvalid("FASTSTORE_QUIET", value, "FASTSTORE_QUIET");
    }
}

void testEmptyDataDirectory() {
    expectInvalid("FASTSTORE_DATA_DIR", "", "FASTSTORE_DATA_DIR");
}

struct TestCase {
    std::string_view name;
    void (*function)();
};

}  // namespace

int main() {
    const std::vector<TestCase> tests{
        {"defaults", testDefaults},
        {"valid overrides", testValidOverrides},
        {"quiet zero", testQuietZero},
        {"valid boundaries", testValidBoundaries},
        {"invalid ports", testInvalidPorts},
        {"invalid workers", testInvalidWorkers},
        {"invalid quiet", testInvalidQuiet},
        {"empty data directory", testEmptyDataDirectory},
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

    std::cout << passed << " config test groups passed\n";
    return 0;
}
