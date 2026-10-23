#include "faststore/connection.h"
#include "faststore/file_service.h"
#include "faststore/router.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using faststore::Connection;
using faststore::FileService;

// 把断言失败转换为带说明的测试异常。
void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

// 为单个测试提供自动清理的独立 data 目录。
class TemporaryTree {
public:
    TemporaryTree() {
        std::string pattern = "/tmp/faststore-get-streaming-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        root_ = created;
        data_ = root_ / "data";
        expect(std::filesystem::create_directory(data_),
               "unable to create data directory");
    }

    ~TemporaryTree() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    TemporaryTree(const TemporaryTree&) = delete;
    TemporaryTree& operator=(const TemporaryTree&) = delete;

    [[nodiscard]] const std::filesystem::path& data() const noexcept {
        return data_;
    }

private:
    std::filesystem::path root_;
    std::filesystem::path data_;
};

// 模拟 Server 端与客户端端的 non-blocking 字节流。
class SocketPair {
public:
    SocketPair() {
        int descriptors[2] = {-1, -1};
        expect(::socketpair(AF_UNIX,
                            SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                            descriptors) == 0,
               "socketpair failed");
        server_fd_ = descriptors[0];
        peer_fd_ = descriptors[1];
    }

    ~SocketPair() {
        if (server_fd_ >= 0) {
            ::close(server_fd_);
        }
        if (peer_fd_ >= 0) {
            ::close(peer_fd_);
        }
    }

    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;

    int releaseServer() noexcept {
        const int fd = server_fd_;
        server_fd_ = -1;
        return fd;
    }

    [[nodiscard]] int peerFd() const noexcept {
        return peer_fd_;
    }

    // 发送短测试请求；请求规模不会触发发送端背压。
    void sendAll(std::string_view data) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const ssize_t sent = ::send(peer_fd_, data.data() + offset,
                                        data.size() - offset, MSG_NOSIGNAL);
            if (sent > 0) {
                offset += static_cast<std::size_t>(sent);
            } else if (sent == -1 && errno == EINTR) {
                continue;
            } else {
                throw std::runtime_error("request send failed");
            }
        }
    }

    // 读取当前已到达的所有响应字节，遇到 EAGAIN 返回。
    std::string drainAvailable() {
        std::string output;
        std::array<char, 16U * 1024U> buffer{};
        while (peer_fd_ >= 0) {
            const ssize_t count =
                ::recv(peer_fd_, buffer.data(), buffer.size(), 0);
            if (count > 0) {
                output.append(buffer.data(), static_cast<std::size_t>(count));
            } else if (count == -1 && errno == EINTR) {
                continue;
            } else if (count == -1 &&
                       (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            } else if (count == 0) {
                break;
            } else {
                throw std::runtime_error("response recv failed");
            }
        }
        return output;
    }

    // 模拟客户端下载中途断开。
    void closePeer() noexcept {
        if (peer_fd_ >= 0) {
            ::close(peer_fd_);
            peer_fd_ = -1;
        }
    }

private:
    int server_fd_{-1};
    int peer_fd_{-1};
};

FileService makeService(const std::filesystem::path& data_path) {
    auto result = faststore::openFileService(data_path);
    expect(result.ok(), "FileService initialization failed");
    return result.takeValue();
}

// 汇集单个连接测试所需的文件服务、socketpair 与 Connection。
class Harness {
public:
    Harness()
        : service_(makeService(tree_.data())),
          connection_(sockets_.releaseServer()) {}

    [[nodiscard]] Connection& connection() noexcept {
        return connection_;
    }

    [[nodiscard]] SocketPair& sockets() noexcept {
        return sockets_;
    }

    [[nodiscard]] const std::filesystem::path& data() const noexcept {
        return tree_.data();
    }

    void prepare(std::string_view request) {
        sockets_.sendAll(request);
        expect(connection_.readAvailableData(service_) ==
                   Connection::ReadStatus::kOpen,
               "request read failed");
        if (connection_.state() == Connection::State::kRequestReady) {
            auto parsed = connection_.beginWorkerDispatch();
            expect(parsed.has_value(), "request was not available for dispatch");
            expect(connection_.applyRouteResult(
                       faststore::routeRequest(service_, parsed.value())),
                   "route result was not accepted");
        }
        expect(connection_.wantsWrite(), "request did not prepare output");
    }

private:
    TemporaryTree tree_;
    FileService service_;
    SocketPair sockets_;
    Connection connection_;
};

// 生成不依赖大块内存的确定性文件内容。
char patternByte(std::uint64_t offset) {
    return static_cast<char>((offset * 131U + 17U) % 251U);
}

void writePatternFile(const std::filesystem::path& path, std::uint64_t size) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    std::array<char, 8192> chunk{};
    std::uint64_t written = 0;
    while (written < size) {
        const std::size_t count = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk.size(), size - written));
        for (std::size_t index = 0; index < count; ++index) {
            chunk[index] = patternByte(written + index);
        }
        stream.write(chunk.data(), static_cast<std::streamsize>(count));
        written += count;
    }
    expect(static_cast<bool>(stream), "unable to write pattern file");
}

void writeTextFile(const std::filesystem::path& path,
                   std::string_view content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    expect(static_cast<bool>(stream), "unable to write text file");
}

// 驱动 Connection，模拟 Server 在每次可写后收到 EPOLLOUT。
template <typename Consumer>
Connection::WriteStatus driveOutput(Harness& harness, Consumer consume) {
    Connection::WriteStatus status = Connection::WriteStatus::kPending;
    for (int attempt = 0; attempt < 10000; ++attempt) {
        status = harness.connection().writeAvailableData();
        consume(harness.sockets().drainAvailable());
        if (status != Connection::WriteStatus::kPending) {
            return status;
        }
    }
    throw std::runtime_error("output did not finish");
}

std::string collectOutput(Harness& harness) {
    std::string wire;
    const auto status = driveOutput(
        harness, [&wire](std::string part) { wire += std::move(part); });
    expect(status == Connection::WriteStatus::kComplete,
           "response did not complete");
    return wire;
}

std::pair<std::string_view, std::string_view> splitResponse(
    const std::string& wire) {
    const std::size_t boundary = wire.find("\r\n\r\n");
    expect(boundary != std::string::npos, "response header is incomplete");
    return {std::string_view{wire}.substr(0, boundary + 4U),
            std::string_view{wire}.substr(boundary + 4U)};
}

void expectStatusAndLength(std::string_view headers,
                           std::string_view status,
                           std::uint64_t length) {
    if (headers.rfind(status, 0) != 0U) {
        throw std::runtime_error("wrong response status: " +
                                 std::string{headers});
    }
    const std::string length_header =
        "Content-Length: " + std::to_string(length) + "\r\n";
    expect(headers.find(length_header) != std::string_view::npos,
           "wrong Content-Length");
    expect(headers.find("Connection: close\r\n") != std::string_view::npos,
           "response does not close connection");
}

void testEmptyAndSmallGet() {
    Harness empty;
    writeTextFile(empty.data() / "empty", "");
    empty.prepare("GET /files/empty HTTP/1.1\r\nHost: x\r\n\r\n");
    expect(empty.connection().fileBufferCapacity() == 0U,
           "empty GET allocated a file chunk");
    const std::string empty_wire = collectOutput(empty);
    const auto empty_parts = splitResponse(empty_wire);
    expectStatusAndLength(empty_parts.first, "HTTP/1.1 200 OK\r\n", 0);
    expect(empty_parts.second.empty(), "empty GET sent a body");

    Harness small;
    writeTextFile(small.data() / "small", std::string_view{"hello\0world", 11});
    small.prepare("GET /files/small HTTP/1.1\r\nHost: x\r\n\r\n");
    const std::string wire = collectOutput(small);
    const auto parts = splitResponse(wire);
    expectStatusAndLength(parts.first, "HTTP/1.1 200 OK\r\n", 11);
    expect(parts.second == std::string_view{"hello\0world", 11},
           "small GET body differs");
}

void testMultiChunkGet() {
    constexpr std::uint64_t size =
        3U * Connection::kFileChunkSize + 197U;
    Harness harness;
    writePatternFile(harness.data() / "multi", size);
    harness.prepare("GET /files/multi HTTP/1.1\r\nHost: x\r\n\r\n");
    const std::string wire = collectOutput(harness);
    const auto parts = splitResponse(wire);
    expectStatusAndLength(parts.first, "HTTP/1.1 200 OK\r\n", size);
    expect(parts.second.size() == size, "multi-chunk size differs");
    for (std::uint64_t index = 0; index < size; ++index) {
        expect(parts.second[static_cast<std::size_t>(index)] ==
                   patternByte(index),
               "multi-chunk bytes differ");
    }
}

void testErrorsAndHead() {
    Harness missing;
    missing.prepare("GET /files/missing HTTP/1.1\r\nHost: x\r\n\r\n");
    const std::string missing_wire = collectOutput(missing);
    expectStatusAndLength(splitResponse(missing_wire).first,
                          "HTTP/1.1 404 Not Found\r\n", 0);

    Harness invalid;
    invalid.prepare("GET /files/a/b HTTP/1.1\r\nHost: x\r\n\r\n");
    const std::string invalid_wire = collectOutput(invalid);
    expectStatusAndLength(splitResponse(invalid_wire).first,
                          "HTTP/1.1 400 Bad Request\r\n", 0);

    Harness non_regular;
    expect(std::filesystem::create_directory(non_regular.data() / "dir"),
           "unable to create directory target");
    non_regular.prepare("GET /files/dir HTTP/1.1\r\nHost: x\r\n\r\n");
    const std::string non_regular_wire = collectOutput(non_regular);
    expectStatusAndLength(splitResponse(non_regular_wire).first,
                          "HTTP/1.1 404 Not Found\r\n", 0);

    Harness head;
    writeTextFile(head.data() / "head", "12345");
    head.prepare("HEAD /files/head HTTP/1.1\r\nHost: x\r\n\r\n");
    const std::string head_wire = collectOutput(head);
    const auto head_parts = splitResponse(head_wire);
    expectStatusAndLength(head_parts.first, "HTTP/1.1 200 OK\r\n", 5);
    expect(head_parts.second.empty(), "HEAD unexpectedly sent a body");
    expect(!head.connection().hasOpenReadFile(), "HEAD opened a ReadFile");
}

// 多 MiB 测试逐段校验，不构造同尺寸 expected response。
void testLargeBoundedStreaming() {
    constexpr std::uint64_t size = 8U * 1024U * 1024U + 321U;
    Harness harness;
    writePatternFile(harness.data() / "large", size);
    harness.prepare("GET /files/large HTTP/1.1\r\nHost: x\r\n\r\n");
    expect(harness.connection().fileBufferCapacity() ==
               Connection::kFileChunkSize,
           "file buffer is not fixed at one chunk");

    std::string pending_header;
    bool header_complete = false;
    std::uint64_t body_offset = 0;
    auto consume = [&](std::string part) {
        std::string_view body = part;
        if (!header_complete) {
            pending_header += part;
            const std::size_t boundary = pending_header.find("\r\n\r\n");
            if (boundary == std::string::npos) {
                return;
            }
            expectStatusAndLength(
                std::string_view{pending_header}.substr(0, boundary + 4U),
                "HTTP/1.1 200 OK\r\n", size);
            body = std::string_view{pending_header}.substr(boundary + 4U);
            header_complete = true;
        }
        for (char byte : body) {
            expect(byte == patternByte(body_offset),
                   "large streaming byte differs");
            ++body_offset;
        }
        if (header_complete) {
            pending_header.clear();
        }
    };

    expect(driveOutput(harness, consume) ==
               Connection::WriteStatus::kComplete,
           "large GET did not complete");
    expect(header_complete && body_offset == size,
           "large GET body length differs");
    expect(harness.connection().sentFileBytes() == size,
           "large GET sent counter differs");
}

void testPartialSendPreservesChunk() {
    constexpr std::uint64_t size = 1024U * 1024U + 73U;
    Harness harness;
    writePatternFile(harness.data() / "pressure", size);
    constexpr int small_buffer = 1024;
    expect(::setsockopt(harness.connection().fd(), SOL_SOCKET, SO_SNDBUF,
                        &small_buffer, sizeof(small_buffer)) == 0,
           "unable to reduce send buffer");
    harness.prepare("GET /files/pressure HTTP/1.1\r\nHost: x\r\n\r\n");

    expect(harness.connection().writeAvailableData() ==
               Connection::WriteStatus::kPending,
           "send pressure did not produce EAGAIN");
    expect(harness.connection().state() ==
               Connection::State::kWritingFileBody,
           "GET did not reach file-body phase");
    const std::size_t saved_offset =
        harness.connection().fileChunkSendOffset();
    expect(saved_offset > 0U &&
               saved_offset < harness.connection().fileChunkValidSize(),
           "partial chunk offset was not preserved");

    std::string wire = harness.sockets().drainAvailable();
    expect(driveOutput(harness, [&wire](std::string part) {
               wire += std::move(part);
           }) == Connection::WriteStatus::kComplete,
           "GET did not resume after EAGAIN");
    const auto parts = splitResponse(wire);
    expect(parts.second.size() == size, "resumed body size differs");
    for (std::uint64_t index = 0; index < size; ++index) {
        expect(parts.second[static_cast<std::size_t>(index)] ==
                   patternByte(index),
               "resumed body has duplicate, missing, or reordered bytes");
    }
}

void testExternalGrowthAndTruncate() {
    Harness growth;
    writeTextFile(growth.data() / "grow", "original");
    growth.prepare("GET /files/grow HTTP/1.1\r\nHost: x\r\n\r\n");
    {
        std::ofstream stream(growth.data() / "grow",
                             std::ios::binary | std::ios::app);
        stream << "-extra";
    }
    const std::string growth_wire = collectOutput(growth);
    const auto growth_parts = splitResponse(growth_wire);
    expectStatusAndLength(growth_parts.first, "HTTP/1.1 200 OK\r\n", 8);
    expect(growth_parts.second == "original",
           "GET exceeded its initial Content-Length after growth");

    Harness truncated;
    writePatternFile(truncated.data() / "truncated", 128U * 1024U);
    truncated.prepare(
        "GET /files/truncated HTTP/1.1\r\nHost: x\r\n\r\n");
    std::filesystem::resize_file(truncated.data() / "truncated", 0);
    expect(truncated.connection().writeAvailableData() ==
               Connection::WriteStatus::kError,
           "early EOF was not treated as streaming failure");
    const std::string wire = truncated.sockets().drainAvailable();
    expect(wire.rfind("HTTP/1.1 200 OK\r\n", 0) == 0U,
           "truncate test did not send original headers");
    expect(truncated.connection().sentFileBytes() == 0U,
           "truncate test sent unexpected file bytes");
    expect(!truncated.connection().hasOpenReadFile(),
           "truncate failure leaked ReadFile ownership");
}

void testClientDisconnectCleanup() {
    Harness harness;
    writePatternFile(harness.data() / "disconnect", 1024U * 1024U);
    harness.prepare(
        "GET /files/disconnect HTTP/1.1\r\nHost: x\r\n\r\n");
    expect(harness.connection().hasOpenReadFile(),
           "GET did not retain ReadFile");
    harness.sockets().closePeer();
    expect(harness.connection().writeAvailableData() ==
               Connection::WriteStatus::kError,
           "client disconnect was not a send failure");
    expect(!harness.connection().hasOpenReadFile(),
           "client disconnect leaked ReadFile");
    expect(!harness.connection().isValid(),
           "failed connection remained valid");
}

struct TestCase {
    std::string_view name;
    void (*function)();
};

}  // namespace

int main() {
    using namespace std::string_view_literals;
    const std::vector<TestCase> tests{
        {"empty and small GET", testEmptyAndSmallGet},
        {"multi-chunk GET", testMultiChunkGet},
        {"errors and HEAD", testErrorsAndHead},
        {"large bounded streaming", testLargeBoundedStreaming},
        {"partial send preserves chunk", testPartialSendPreservesChunk},
        {"external growth and truncate", testExternalGrowthAndTruncate},
        {"client disconnect cleanup", testClientDisconnectCleanup},
    };

    std::size_t passed = 0;
    for (const TestCase& test : tests) {
        try {
            test.function();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what()
                      << '\n';
            return 1;
        }
    }
    std::cout << passed << " GET streaming test groups passed\n";
    return 0;
}
