#include "faststore/file_service.h"
#include "faststore/http.h"
#include "faststore/server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using faststore::FileService;
using faststore::HttpParser;
using faststore::Server;

// 将测试条件失败转换为包含上下文的异常。
void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

// 为真实 Server 集成测试提供自动清理的隔离 data 目录。
class TemporaryTree {
public:
    TemporaryTree() {
        std::string pattern = "/tmp/faststore-robustness-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        root_ = created;
        data_ = root_ / "data";
        expect(std::filesystem::create_directory(data_),
               "unable to create temporary data directory");
    }

    ~TemporaryTree() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    TemporaryTree(const TemporaryTree&) = delete;
    TemporaryTree& operator=(const TemporaryTree&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

    [[nodiscard]] const std::filesystem::path& data() const noexcept {
        return data_;
    }

private:
    std::filesystem::path root_;
    std::filesystem::path data_;
};

// 写入精确的二进制测试内容。
void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    expect(static_cast<bool>(stream), "unable to write test file");
}

// 读取小型验证文件的完整内容。
std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
}

// 分块创建多 MiB 文件，避免构造另一份同尺寸 expected buffer。
void writeLargeFile(const std::filesystem::path& path, std::size_t size) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    const std::array<char, 64U * 1024U> chunk{};
    std::size_t written = 0;
    while (written < size) {
        const std::size_t count = std::min(chunk.size(), size - written);
        stream.write(chunk.data(), static_cast<std::streamsize>(count));
        written += count;
    }
    expect(static_cast<bool>(stream), "unable to write large test file");
}

// 统计尚未提交的 FastStore 上传临时文件。
std::size_t countTemporaryFiles(const std::filesystem::path& data_path) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(data_path)) {
        if (entry.path().filename().string().rfind(
                ".faststore-upload-", 0) == 0U) {
            ++count;
        }
    }
    return count;
}

// 在短暂异步清理窗口内等待条件成立。
void waitFor(const std::function<bool()>& predicate,
             std::string_view failure_message) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (predicate()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(false, failure_message);
}

// 临时绑定 port 0，让内核选择一个 loopback 测试端口。
std::uint16_t chooseLoopbackPort() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    expect(fd >= 0, "unable to create port-selection socket");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
        0) {
        ::close(fd);
        throw std::runtime_error("unable to bind port-selection socket");
    }

    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(fd);
        throw std::runtime_error("getsockname failed");
    }
    const std::uint16_t port = ntohs(address.sin_port);
    ::close(fd);
    return port;
}

// 连接测试 Server，并为异常测试设置有限的发送/接收等待时间。
int connectTo(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }

    timeval timeout{};
    timeout.tv_sec = 3;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout)) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout)) != 0) {
        ::close(fd);
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// 将完整测试请求写入 TCP；服务器提前关闭时返回 false。
bool sendAll(int fd, std::string_view data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ssize_t sent = ::send(fd, data.data() + offset,
                                    data.size() - offset, MSG_NOSIGNAL);
        if (sent > 0) {
            offset += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

// 读取直至 one-request-per-connection Server 关闭连接。
std::string readToClose(int fd) {
    std::string response;
    std::array<char, 16U * 1024U> buffer{};
    while (true) {
        const ssize_t count = ::recv(fd, buffer.data(), buffer.size(), 0);
        if (count > 0) {
            response.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            return response;
        }
        if (errno == EINTR) {
            continue;
        }
        throw std::runtime_error("response read failed or timed out");
    }
}

// 对单个请求执行可选 write-half close，并收集完整响应。
std::string performRequest(std::uint16_t port,
                           std::string_view request,
                           bool half_close = false) {
    const int fd = connectTo(port);
    expect(fd >= 0, "unable to connect to test server");
    if (!sendAll(fd, request)) {
        ::close(fd);
        throw std::runtime_error("request send failed");
    }
    if (half_close && ::shutdown(fd, SHUT_WR) != 0) {
        ::close(fd);
        throw std::runtime_error("request half-close failed");
    }

    try {
        std::string response = readToClose(fd);
        ::close(fd);
        return response;
    } catch (...) {
        ::close(fd);
        throw;
    }
}

// 建立连接、发送可选数据后立即断开，不等待响应。
void sendAndDrop(std::uint16_t port,
                 std::string_view request,
                 bool reset = false) {
    const int fd = connectTo(port);
    expect(fd >= 0, "unable to connect before drop");
    if (!request.empty()) {
        expect(sendAll(fd, request), "unable to send request before drop");
    }
    if (reset) {
        linger option{};
        option.l_onoff = 1;
        option.l_linger = 0;
        expect(::setsockopt(fd, SOL_SOCKET, SO_LINGER, &option,
                            sizeof(option)) == 0,
               "unable to configure reset close");
    }
    ::close(fd);
}

// 验证响应起始状态行。
void expectStatus(std::string_view response, std::string_view status_line) {
    if (response.rfind(status_line, 0) != 0U) {
        throw std::runtime_error("unexpected HTTP status; expected " +
                                 std::string{status_line});
    }
}

// 统计子 Server 进程当前打开的 fd 数量。
std::size_t processFdCount(pid_t pid) {
    std::error_code error;
    const std::filesystem::path fd_path =
        std::filesystem::path{"/proc"} / std::to_string(pid) / "fd";
    std::size_t count = 0;
    for (std::filesystem::directory_iterator iterator(fd_path, error), end;
         !error && iterator != end; iterator.increment(error)) {
        ++count;
    }
    expect(!error, "unable to inspect server fd count");
    return count;
}

// 在子进程运行真实 epoll Server，并在析构时可靠终止和回收。
class ServerProcess {
public:
    explicit ServerProcess(const std::filesystem::path& data_path)
        : port_(chooseLoopbackPort()) {
        pid_ = ::fork();
        expect(pid_ >= 0, "fork failed");
        if (pid_ == 0) {
            const int null_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (null_fd >= 0) {
                ::dup2(null_fd, STDOUT_FILENO);
                ::dup2(null_fd, STDERR_FILENO);
                ::close(null_fd);
            }
            auto service_result = faststore::openFileService(data_path);
            if (!service_result.ok()) {
                ::_exit(2);
            }
            try {
                Server server{port_, service_result.takeValue()};
                server.run();
            } catch (...) {
                ::_exit(3);
            }
            ::_exit(0);
        }

        for (int attempt = 0; attempt < 200; ++attempt) {
            const int probe = connectTo(port_);
            if (probe >= 0) {
                ::close(probe);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                baseline_fd_count_ = processFdCount(pid_);
                return;
            }
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                throw std::runtime_error("test server exited during startup");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        stop();
        throw std::runtime_error("test server did not start");
    }

    ~ServerProcess() {
        stop();
    }

    ServerProcess(const ServerProcess&) = delete;
    ServerProcess& operator=(const ServerProcess&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }

    [[nodiscard]] pid_t pid() const noexcept {
        return pid_;
    }

    [[nodiscard]] std::size_t baselineFdCount() const noexcept {
        return baseline_fd_count_;
    }

    void expectAlive() const {
        expect(pid_ > 0 && ::kill(pid_, 0) == 0,
               "server process is no longer alive");
    }

private:
    void stop() noexcept {
        if (pid_ <= 0) {
            return;
        }
        ::kill(pid_, SIGTERM);
        while (::waitpid(pid_, nullptr, 0) == -1 && errno == EINTR) {
        }
        pid_ = -1;
    }

    std::uint16_t port_{0};
    pid_t pid_{-1};
    std::size_t baseline_fd_count_{0};
};

std::string headRequest(std::string_view filename) {
    return "HEAD /files/" + std::string{filename} +
           " HTTP/1.1\r\nHost: localhost\r\n\r\n";
}

std::string putHeader(std::string_view filename, std::size_t size) {
    return "PUT /files/" + std::string{filename} +
           " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
           std::to_string(size) + "\r\n\r\n";
}

// 每个重要失败后用已知文件 HEAD 验证事件循环仍可服务。
void expectServerSurvives(const ServerProcess& server) {
    server.expectAlive();
    expectStatus(performRequest(server.port(), headRequest("known.txt")),
                 "HTTP/1.1 200 OK\r\n");
}

void testMalformedHttpAndUnsupportedMethod(const ServerProcess& server) {
    std::string oversized_line = "GET /";
    oversized_line.append(HttpParser::kMaxRequestLineSize, 'a');

    std::string oversized_headers =
        "GET /files/known.txt HTTP/1.1\r\nHost: localhost\r\nX-Large: ";
    oversized_headers.append(HttpParser::kMaxHeaderSectionSize, 'a');

    std::string too_many_headers =
        "GET /files/known.txt HTTP/1.1\r\nHost: localhost\r\n";
    for (std::size_t index = 0; index < HttpParser::kMaxHeaderCount; ++index) {
        too_many_headers +=
            "X-" + std::to_string(index) + ": value\r\n";
    }
    too_many_headers += "\r\n";

    const std::vector<std::pair<std::string, std::string_view>> cases{
        {"GET  HTTP/1.1\r\nHost: localhost\r\n\r\n",
         "HTTP/1.1 400 Bad Request\r\n"},
        {"GET /files/known.txt HTTP/1.0\r\nHost: localhost\r\n\r\n",
         "HTTP/1.1 400 Bad Request\r\n"},
        {"G@T /files/known.txt HTTP/1.1\r\nHost: localhost\r\n\r\n",
         "HTTP/1.1 400 Bad Request\r\n"},
        {"GET /files/known.txt HTTP/1.1\r\n\r\n",
         "HTTP/1.1 400 Bad Request\r\n"},
        {"GET /files/known.txt HTTP/1.1\r\nBroken\r\n\r\n",
         "HTTP/1.1 400 Bad Request\r\n"},
        {"PUT /files/a HTTP/1.1\r\nHost: x\r\nContent-Length: no\r\n\r\n",
         "HTTP/1.1 400 Bad Request\r\n"},
        {"PUT /files/a HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n",
         "HTTP/1.1 400 Bad Request\r\n"},
        {"PUT /files/a HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
         "HTTP/1.1 400 Bad Request\r\n"},
        {std::move(oversized_line), "HTTP/1.1 400 Bad Request\r\n"},
        {std::move(oversized_headers), "HTTP/1.1 400 Bad Request\r\n"},
        {std::move(too_many_headers), "HTTP/1.1 400 Bad Request\r\n"},
        {"PUT /files/large HTTP/1.1\r\nHost: x\r\nContent-Length: 67108865\r\n\r\n",
         "HTTP/1.1 413 Payload Too Large\r\n"},
        {"POST /files/known.txt HTTP/1.1\r\nHost: localhost\r\n\r\n",
         "HTTP/1.1 405 Method Not Allowed\r\n"},
    };

    for (const auto& test_case : cases) {
        expectStatus(performRequest(server.port(), test_case.first),
                     test_case.second);
        expectServerSurvives(server);
    }
}

void testRejectedTargets(const ServerProcess& server,
                         const TemporaryTree& tree) {
    const std::vector<std::string_view> targets{
        "/files/",          "/files/..",       "/files/../name",
        "/files/a/b",       "/abc",            "/files/a..b",
        "/files/a\\b",      "/files/.faststore-upload-user",
    };
    for (const std::string_view target : targets) {
        const std::string request = "HEAD " + std::string{target} +
                                    " HTTP/1.1\r\nHost: x\r\n\r\n";
        expectStatus(performRequest(server.port(), request),
                     "HTTP/1.1 400 Bad Request\r\n");
        expectServerSurvives(server);
    }
    expect(!std::filesystem::exists(tree.root() / "name"),
           "path traversal created a file outside data");
    expect(readFile(tree.data() / "known.txt") == "known-content",
           "target rejection modified known file");
}

void testIncompletePutAndExtraBytes(const ServerProcess& server,
                                    const TemporaryTree& tree) {
    sendAndDrop(server.port(), putHeader("headers-only", 10U));
    waitFor([&] {
        return countTemporaryFiles(tree.data()) == 0U;
    }, "headers-only disconnect leaked upload temp");
    expect(!std::filesystem::exists(tree.data() / "headers-only"),
           "headers-only disconnect committed a target");
    expectServerSurvives(server);

    sendAndDrop(server.port(), putHeader("partial", 20U) + "abc");
    waitFor([&] {
        return countTemporaryFiles(tree.data()) == 0U;
    }, "partial disconnect leaked upload temp");
    expect(!std::filesystem::exists(tree.data() / "partial"),
           "partial disconnect committed a target");
    expectServerSurvives(server);

    const int chunked_fd = connectTo(server.port());
    expect(chunked_fd >= 0, "unable to connect chunked incomplete PUT");
    expect(sendAll(chunked_fd, putHeader("several", 50U) + "abc"),
           "unable to send first incomplete chunk");
    expect(sendAll(chunked_fd, "defgh"),
           "unable to send second incomplete chunk");
    expect(sendAll(chunked_fd, "ijkl"),
           "unable to send third incomplete chunk");
    ::close(chunked_fd);
    waitFor([&] {
        return countTemporaryFiles(tree.data()) == 0U;
    }, "multi-chunk disconnect leaked upload temp");
    expect(!std::filesystem::exists(tree.data() / "several"),
           "multi-chunk disconnect committed a target");
    expectServerSurvives(server);

    const int half_fd = connectTo(server.port());
    expect(half_fd >= 0, "unable to connect half-close PUT");
    expect(sendAll(half_fd, putHeader("half-put", 20U) + "short"),
           "unable to send half-close PUT");
    expect(::shutdown(half_fd, SHUT_WR) == 0,
           "unable to half-close incomplete PUT");
    expect(readToClose(half_fd).empty(),
           "incomplete PUT unexpectedly generated a response");
    ::close(half_fd);
    waitFor([&] {
        return countTemporaryFiles(tree.data()) == 0U;
    }, "half-close leaked upload temp");
    expect(!std::filesystem::exists(tree.data() / "half-put"),
           "half-close committed an incomplete target");
    expectServerSurvives(server);

    writeFile(tree.data() / "extra.txt", "ORIGINAL");
    expectStatus(performRequest(
                     server.port(), putHeader("extra.txt", 3U) + "abcXYZ"),
                 "HTTP/1.1 400 Bad Request\r\n");
    expect(readFile(tree.data() / "extra.txt") == "ORIGINAL",
           "bytes beyond Content-Length modified the committed file");
    expect(countTemporaryFiles(tree.data()) == 0U,
           "extra-byte rejection leaked upload temp");
    expectServerSurvives(server);
}

void testHalfCloseAndFileFailures(const ServerProcess& server,
                                  const TemporaryTree& tree) {
    expectStatus(performRequest(server.port(), headRequest("known.txt"), true),
                 "HTTP/1.1 200 OK\r\n");

    const int incomplete_fd = connectTo(server.port());
    expect(incomplete_fd >= 0, "unable to connect incomplete header test");
    expect(sendAll(incomplete_fd, "HEAD /files/known.txt HTTP/1.1\r\nHost:"),
           "unable to send incomplete headers");
    expect(::shutdown(incomplete_fd, SHUT_WR) == 0,
           "unable to half-close incomplete headers");
    expect(readToClose(incomplete_fd).empty(),
           "incomplete headers unexpectedly generated a response");
    ::close(incomplete_fd);
    expectServerSurvives(server);

    expectStatus(performRequest(server.port(), headRequest("missing")),
                 "HTTP/1.1 404 Not Found\r\n");
    expectStatus(performRequest(server.port(), headRequest("directory")),
                 "HTTP/1.1 404 Not Found\r\n");
    expectStatus(performRequest(server.port(), headRequest("link")),
                 "HTTP/1.1 404 Not Found\r\n");
    expectStatus(performRequest(server.port(), headRequest("pipe")),
                 "HTTP/1.1 404 Not Found\r\n");
    expect(readFile(tree.root() / "outside.txt") == "outside",
           "symlink target was modified");
    expectServerSurvives(server);
}

void testGetDisconnectChurnAndResources(const ServerProcess& server,
                                        const TemporaryTree& tree) {
    const int get_fd = connectTo(server.port());
    expect(get_fd >= 0, "unable to connect GET disconnect test");
    expect(sendAll(get_fd,
                   "GET /files/large.bin HTTP/1.1\r\nHost: x\r\n\r\n"),
           "unable to send large GET");
    std::array<char, 4096> prefix{};
    expect(::recv(get_fd, prefix.data(), prefix.size(), 0) > 0,
           "large GET produced no response bytes");
    linger reset_option{};
    reset_option.l_onoff = 1;
    reset_option.l_linger = 0;
    expect(::setsockopt(get_fd, SOL_SOCKET, SO_LINGER, &reset_option,
                        sizeof(reset_option)) == 0,
           "unable to configure GET reset");
    ::close(get_fd);
    expectServerSurvives(server);

    constexpr int cycles = 75;
    for (int index = 0; index < cycles; ++index) {
        sendAndDrop(server.port(), "", index % 2 == 0);
        sendAndDrop(server.port(), headRequest("known.txt"), true);
        sendAndDrop(server.port(), "BROKEN\r\n\r\n", true);
    }
    expectServerSurvives(server);

    waitFor([&] {
        return processFdCount(server.pid()) <= server.baselineFdCount();
    }, "server fd count did not return to baseline after churn");
    expect(countTemporaryFiles(tree.data()) == 0U,
           "connection churn leaked upload temp files");
}

void testCommittedFileSurvivesClientDrop(const ServerProcess& server,
                                         const TemporaryTree& tree) {
    const int put_fd = connectTo(server.port());
    expect(put_fd >= 0, "unable to connect committed PUT test");
    expect(sendAll(put_fd, putHeader("committed.txt", 7U) + "CONTENT"),
           "unable to send committed PUT");
    waitFor([&] {
        return std::filesystem::exists(tree.data() / "committed.txt");
    }, "complete PUT was not committed");
    linger reset_option{};
    reset_option.l_onoff = 1;
    reset_option.l_linger = 0;
    expect(::setsockopt(put_fd, SOL_SOCKET, SO_LINGER, &reset_option,
                        sizeof(reset_option)) == 0,
           "unable to configure post-commit reset");
    ::close(put_fd);
    expect(readFile(tree.data() / "committed.txt") == "CONTENT",
           "client cleanup damaged committed file");
    expect(countTemporaryFiles(tree.data()) == 0U,
           "committed PUT retained a temporary file");

    const std::string downloaded = performRequest(
        server.port(), "GET /files/committed.txt HTTP/1.1\r\nHost: x\r\n\r\n");
    expectStatus(downloaded, "HTTP/1.1 200 OK\r\n");
    expect(downloaded.size() >= 7U &&
               downloaded.substr(downloaded.size() - 7U) == "CONTENT",
           "subsequent GET returned wrong committed content");
    expectStatus(performRequest(
                     server.port(),
                     "DELETE /files/committed.txt HTTP/1.1\r\nHost: x\r\n\r\n"),
                 "HTTP/1.1 204 No Content\r\n");
    expectServerSurvives(server);
}

struct TestCase {
    std::string_view name;
    std::function<void()> function;
};

}  // namespace

int main() {
    try {
        TemporaryTree tree;
        writeFile(tree.data() / "known.txt", "known-content");
        writeLargeFile(tree.data() / "large.bin", 4U * 1024U * 1024U);
        expect(std::filesystem::create_directory(tree.data() / "directory"),
               "unable to create directory target");
        writeFile(tree.root() / "outside.txt", "outside");
        expect(::symlink((tree.root() / "outside.txt").c_str(),
                         (tree.data() / "link").c_str()) == 0,
               "unable to create symlink target");
        expect(::mkfifo((tree.data() / "pipe").c_str(), 0600) == 0,
               "unable to create FIFO target");

        ServerProcess server{tree.data()};
        const std::vector<TestCase> tests{
            {"malformed HTTP and unsupported method",
             [&] { testMalformedHttpAndUnsupportedMethod(server); }},
            {"rejected targets",
             [&] { testRejectedTargets(server, tree); }},
            {"incomplete PUT and extra bytes",
             [&] { testIncompletePutAndExtraBytes(server, tree); }},
            {"half-close and FileService failures",
             [&] { testHalfCloseAndFileFailures(server, tree); }},
            {"GET disconnect, churn, and resources",
             [&] { testGetDisconnectChurnAndResources(server, tree); }},
            {"committed file survives client drop",
             [&] { testCommittedFileSurvivesClientDrop(server, tree); }},
        };

        std::size_t passed = 0;
        for (const TestCase& test : tests) {
            test.function();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        }
        std::cout << passed << " robustness test groups passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] robustness test: " << error.what() << '\n';
        return 1;
    }
}
