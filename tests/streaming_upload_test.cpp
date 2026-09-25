#include "faststore/connection.h"
#include "faststore/file_service.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using faststore::Connection;
using faststore::FileService;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

class TemporaryTree {
public:
    TemporaryTree() {
        std::string pattern = "/tmp/faststore-streaming-XXXXXX";
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        const char* created = ::mkdtemp(writable.data());
        if (created == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        root_ = created;
        data_ = root_ / "data";
        if (!std::filesystem::create_directory(data_)) {
            throw std::runtime_error("unable to create test data directory");
        }
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

class SocketPair {
public:
    SocketPair() {
        int descriptors[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC,
                         0, descriptors) == -1) {
            throw std::runtime_error("socketpair failed");
        }
        server_fd_ = descriptors[0];
        peer_fd_ = descriptors[1];

        const int flags = ::fcntl(server_fd_, F_GETFL, 0);
        if (flags == -1 ||
            ::fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK) == -1) {
            closeAll();
            throw std::runtime_error("unable to make test socket non-blocking");
        }
    }

    ~SocketPair() {
        closeAll();
    }

    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;

    int releaseServer() noexcept {
        const int fd = server_fd_;
        server_fd_ = -1;
        return fd;
    }

    void sendAll(std::string_view data) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const ssize_t sent = ::send(peer_fd_, data.data() + offset,
                                        data.size() - offset, MSG_NOSIGNAL);
            if (sent > 0) {
                offset += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent == -1 && errno == EINTR) {
                continue;
            }
            throw std::runtime_error("test send failed");
        }
    }

    void closePeer() noexcept {
        if (peer_fd_ >= 0) {
            ::close(peer_fd_);
            peer_fd_ = -1;
        }
    }

private:
    void closeAll() noexcept {
        if (server_fd_ >= 0) {
            ::close(server_fd_);
            server_fd_ = -1;
        }
        closePeer();
    }

    int server_fd_{-1};
    int peer_fd_{-1};
};

FileService makeService(const std::filesystem::path& data_path) {
    auto result = faststore::openFileService(data_path);
    expect(result.ok(), "FileService initialization failed");
    return result.takeValue();
}

class Harness {
public:
    Harness()
        : service_(makeService(tree_.data())),
          connection_(std::in_place, sockets_.releaseServer()) {}

    void send(std::string_view data) {
        sockets_.sendAll(data);
    }

    Connection::ReadStatus pump() {
        return connection_->readAvailableData(service_);
    }

    void disconnect() noexcept {
        sockets_.closePeer();
    }

    [[nodiscard]] Connection& connection() noexcept {
        return connection_.value();
    }

    void destroyConnection() noexcept {
        connection_.reset();
    }

    [[nodiscard]] const std::filesystem::path& data() const noexcept {
        return tree_.data();
    }

private:
    TemporaryTree tree_;
    FileService service_;
    SocketPair sockets_;
    std::optional<Connection> connection_;
};

void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    expect(static_cast<bool>(stream), "unable to write test file");
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
}

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

std::filesystem::path findTemporaryFile(
    const std::filesystem::path& data_path) {
    for (const auto& entry : std::filesystem::directory_iterator(data_path)) {
        if (entry.path().filename().string().rfind(
                ".faststore-upload-", 0) == 0U) {
            return entry.path();
        }
    }
    throw std::runtime_error("temporary upload file not found");
}

std::string putHeader(std::string_view filename, std::size_t size) {
    return "PUT /files/" + std::string{filename} +
           " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
           std::to_string(size) + "\r\n\r\n";
}

void testHeaderAndPartialBody() {
    Harness harness;
    harness.send(putHeader("a.txt", 10U) + "abc");
    expect(harness.pump() == Connection::ReadStatus::kOpen,
           "partial upload should remain open");
    expect(harness.connection().state() ==
               Connection::State::kReceivingBody,
           "partial upload has wrong state");
    expect(harness.connection().receivedBodyBytes() == 3U,
           "partial body byte count is wrong");
    expect(harness.connection().readableSize() == 0U,
           "partial body was not consumed");
    expect(readFile(findTemporaryFile(harness.data())) == "abc",
           "partial body was not written to temp file");
}

void testMultiChunkUpload() {
    Harness harness;
    harness.send(putHeader("multi.txt", 10U) + "abc");
    expect(harness.pump() == Connection::ReadStatus::kOpen,
           "first chunk failed");
    harness.send("def");
    expect(harness.pump() == Connection::ReadStatus::kOpen,
           "second chunk failed");
    harness.send("ghij");
    expect(harness.pump() == Connection::ReadStatus::kOpen,
           "final chunk failed");

    expect(harness.connection().state() ==
               Connection::State::kRequestComplete,
           "multi-chunk upload did not complete");
    expect(readFile(harness.data() / "multi.txt") == "abcdefghij",
           "multi-chunk content is wrong");
    expect(countTemporaryFiles(harness.data()) == 0U,
           "multi-chunk upload leaked temp file");
}

void testHeaderAndEntireBody() {
    Harness harness;
    harness.send(putHeader("entire.txt", 10U) + "abcdefghij");
    expect(harness.pump() == Connection::ReadStatus::kOpen,
           "single-chunk upload failed");
    expect(harness.connection().state() ==
               Connection::State::kRequestComplete,
           "single-chunk upload did not complete");
    expect(readFile(harness.data() / "entire.txt") == "abcdefghij",
           "single-chunk content is wrong");
}

void testExtraBytesPreventCommit() {
    Harness harness;
    const auto data_path = harness.data();
    writeFile(data_path / "extra.txt", "ORIGINAL");
    harness.send(putHeader("extra.txt", 3U) + "abcXYZ");
    expect(harness.pump() == Connection::ReadStatus::kError,
           "already-read extra bytes should fail the request");
    expect(harness.connection().state() == Connection::State::kError,
           "extra bytes did not set error state");
    expect(readFile(data_path / "extra.txt") == "ORIGINAL",
           "extra bytes request modified final file");
    expect(countTemporaryFiles(data_path) == 1U,
           "failed upload temp should remain owned by Connection");
    harness.destroyConnection();
    expect(countTemporaryFiles(data_path) == 0U,
           "failed upload temp was not cleaned by Connection RAII");
    expect(readFile(data_path / "extra.txt") == "ORIGINAL",
           "cleanup modified the original target");
}

void testDisconnectMidUpload() {
    Harness harness;
    const auto data_path = harness.data();
    harness.send(putHeader("partial.txt", 100U) +
                 std::string(30U, 'p'));
    expect(harness.pump() == Connection::ReadStatus::kOpen,
           "partial upload read failed");
    harness.disconnect();
    expect(harness.pump() == Connection::ReadStatus::kError,
           "unexpected EOF should fail upload");
    expect(!std::filesystem::exists(data_path / "partial.txt"),
           "disconnect committed a partial final file");
    expect(countTemporaryFiles(data_path) == 1U,
           "Connection should still own partial temp file");
    harness.destroyConnection();
    expect(countTemporaryFiles(data_path) == 0U,
           "disconnect temp was not cleaned by Connection RAII");
}

void testZeroLengthBody() {
    Harness harness;
    harness.send(putHeader("empty.txt", 0U));
    expect(harness.pump() == Connection::ReadStatus::kOpen,
           "zero body request failed");
    expect(harness.connection().state() ==
               Connection::State::kRequestComplete,
           "zero body request did not complete");
    expect(std::filesystem::file_size(harness.data() / "empty.txt") == 0U,
           "zero body request did not create an empty file");
}

void testZeroLengthExtraRejected() {
    Harness harness;
    harness.send(putHeader("zero-extra.txt", 0U) + "X");
    expect(harness.pump() == Connection::ReadStatus::kError,
           "extra byte after zero-length body should fail");
    expect(!std::filesystem::exists(harness.data() / "zero-extra.txt"),
           "zero-length request committed before extra-byte check");
}

void testBodylessRequestExtraRejected() {
    Harness harness;
    harness.send("GET /files/a.txt HTTP/1.1\r\nHost: localhost\r\n\r\nX");
    expect(harness.pump() == Connection::ReadStatus::kError,
           "extra byte after bodyless request should fail");
    expect(harness.connection().state() == Connection::State::kError,
           "bodyless extra byte did not set error state");
}

void testInvalidTarget() {
    Harness harness;
    harness.send("PUT /files/a/b HTTP/1.1\r\nHost: localhost\r\n"
                 "Content-Length: 3\r\n\r\nabc");
    expect(harness.pump() == Connection::ReadStatus::kError,
           "invalid target should fail");
    expect(harness.connection().state() == Connection::State::kError,
           "invalid target did not set error state");
    expect(countTemporaryFiles(harness.data()) == 0U,
           "invalid target created a temp file");
}

void testLargeBoundedStreamingUpload() {
    constexpr std::size_t total_size = 8U * 1024U * 1024U;
    Harness harness;
    harness.send(putHeader("large.bin", total_size));
    expect(harness.pump() == Connection::ReadStatus::kOpen,
           "large upload header failed");

    const std::string chunk(Connection::kReadChunkSize, 'L');
    std::size_t sent = 0;
    while (sent < total_size) {
        const std::size_t current =
            std::min(chunk.size(), total_size - sent);
        harness.send(std::string_view{chunk.data(), current});
        expect(harness.pump() == Connection::ReadStatus::kOpen,
               "large upload chunk failed");
        sent += current;
    }

    expect(harness.connection().state() ==
               Connection::State::kRequestComplete,
           "large upload did not complete");
    expect(std::filesystem::file_size(harness.data() / "large.bin") ==
               total_size,
           "large upload size is wrong");
    expect(harness.connection().peakReadableBytes() <=
               Connection::kMaxBufferedInput,
           "large upload buffer exceeded its fixed bound");
    expect(harness.connection().peakReadableBytes() < 128U * 1024U,
           "large upload retained too much unread input");
    expect(countTemporaryFiles(harness.data()) == 0U,
           "large upload leaked temp file");
}

struct TestCase {
    std::string_view name;
    void (*function)();
};

}  // namespace

int main() {
    const std::vector<TestCase> tests{
        {"header and partial body", testHeaderAndPartialBody},
        {"multi-chunk upload", testMultiChunkUpload},
        {"header and entire body", testHeaderAndEntireBody},
        {"already-read extra bytes prevent commit",
         testExtraBytesPreventCommit},
        {"disconnect mid-upload", testDisconnectMidUpload},
        {"zero-length body", testZeroLengthBody},
        {"zero-length extra rejected", testZeroLengthExtraRejected},
        {"bodyless request extra rejected", testBodylessRequestExtraRejected},
        {"invalid target", testInvalidTarget},
        {"large bounded streaming upload", testLargeBoundedStreamingUpload},
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

    std::cout << passed << " streaming upload test groups passed\n";
    return 0;
}
