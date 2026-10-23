#include "faststore/connection.h"
#include "faststore/file_service.h"
#include "faststore/http_response.h"
#include "faststore/router.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
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
using faststore::HttpStatus;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

class TemporaryTree {
public:
    TemporaryTree() {
        std::string pattern = "/tmp/faststore-response-XXXXXX";
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
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                         0, descriptors) == -1) {
            throw std::runtime_error("socketpair failed");
        }
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
            throw std::runtime_error("test request send failed");
        }
    }

    void shutdownWrite() {
        if (::shutdown(peer_fd_, SHUT_WR) == -1) {
            throw std::runtime_error("test shutdown failed");
        }
    }

    std::string drainAvailable() {
        std::string output;
        std::array<char, 4096> buffer{};
        while (true) {
            const ssize_t count =
                ::recv(peer_fd_, buffer.data(), buffer.size(), 0);
            if (count > 0) {
                output.append(buffer.data(), static_cast<std::size_t>(count));
                continue;
            }
            if (count == -1 && errno == EINTR) {
                continue;
            }
            if (count == -1 &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return output;
            }
            if (count == 0) {
                return output;
            }
            throw std::runtime_error("test response recv failed");
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

    Connection::ReadStatus readRequest(std::string_view request,
                                       bool peer_half_close = false) {
        sockets_.sendAll(request);
        if (peer_half_close) {
            sockets_.shutdownWrite();
        }
        const Connection::ReadStatus status =
            connection_.readAvailableData(service_);
        if (connection_.state() == Connection::State::kRequestReady) {
            auto parsed = connection_.beginWorkerDispatch();
            expect(parsed.has_value(), "request was not available for dispatch");
            expect(connection_.state() ==
                       Connection::State::kWaitingForWorker,
                   "dispatch did not enter worker-waiting state");
            expect(connection_.applyRouteResult(
                       faststore::routeRequest(service_, parsed.value())),
                   "route result was not accepted");
        }
        return status;
    }

    std::string sendPreparedResponse() {
        expect(connection_.state() == Connection::State::kWritingResponse,
               "response was not prepared");
        const auto status = connection_.writeAvailableData();
        expect(status == Connection::WriteStatus::kComplete,
               "small response did not complete");
        expect(connection_.state() == Connection::State::kResponseComplete,
               "response completion state is wrong");
        return sockets_.drainAvailable();
    }

private:
    TemporaryTree tree_;
    FileService service_;
    SocketPair sockets_;
    Connection connection_;
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

void expectStatusLine(const std::string& response,
                      std::string_view status_line) {
    expect(response.rfind(status_line, 0) == 0U, "wrong response status");
    expect(response.find("\r\nContent-Length: ") != std::string::npos,
           "response is missing Content-Length");
    expect(response.find("\r\nConnection: close\r\n\r\n") !=
               std::string::npos,
           "response is missing close framing");
}

void testResponseSerialization() {
    const std::string serialized = faststore::serializeHttpResponse(
        faststore::makeResponse(HttpStatus::kCreated));
    expect(serialized ==
               "HTTP/1.1 201 Created\r\n"
               "Content-Length: 0\r\n"
               "Connection: close\r\n\r\n",
           "response serialization is not exact CRLF HTTP/1.1");
}

void testFileListingResponse() {
    Harness empty;
    expect(empty.readRequest(
               "GET /files HTTP/1.1\r\nHost: localhost\r\n\r\n") ==
               Connection::ReadStatus::kOpen,
           "empty listing request failed");
    const std::string empty_response = empty.sendPreparedResponse();
    const std::string empty_body = "{\"files\":[]}";
    expectStatusLine(empty_response, "HTTP/1.1 200 OK\r\n");
    expect(empty_response.find("Content-Type: application/json\r\n") !=
               std::string::npos,
           "listing response has the wrong Content-Type");
    expect(empty_response.find("Content-Length: " +
                               std::to_string(empty_body.size()) + "\r\n") !=
               std::string::npos,
           "empty listing response has the wrong Content-Length");
    expect(empty_response.substr(empty_response.find("\r\n\r\n") + 4U) ==
               empty_body,
           "empty listing response has the wrong body");

    Harness populated;
    writeFile(populated.data() / "z.txt", "1234");
    writeFile(populated.data() / "a\"quote\n.txt", "xy");
    writeFile(populated.data() / ".faststore-upload-hidden", "partial");
    expect(std::filesystem::create_directory(populated.data() / "directory"),
           "unable to create listing directory fixture");
    expect(::symlink((populated.data() / "z.txt").c_str(),
                     (populated.data() / "symlink").c_str()) == 0,
           "unable to create listing symlink fixture");
    expect(::mkfifo((populated.data() / "fifo").c_str(), 0600) == 0,
           "unable to create listing FIFO fixture");

    expect(populated.readRequest(
               "GET /files HTTP/1.1\r\nHost: localhost\r\n\r\n") ==
               Connection::ReadStatus::kOpen,
           "populated listing request failed");
    const std::string response = populated.sendPreparedResponse();
    const std::string body =
        "{\"files\":[{\"name\":\"a\\\"quote\\n.txt\",\"size\":2},"
        "{\"name\":\"z.txt\",\"size\":4}]}";
    expectStatusLine(response, "HTTP/1.1 200 OK\r\n");
    expect(response.find("Content-Type: application/json\r\n") !=
               std::string::npos,
           "listing response has the wrong Content-Type");
    expect(response.find("Content-Length: " + std::to_string(body.size()) +
                         "\r\n") != std::string::npos,
           "listing response has the wrong Content-Length");
    expect(response.substr(response.find("\r\n\r\n") + 4U) == body,
           "listing JSON escaping, filtering, ordering, or sizes are wrong");
    expect(!populated.connection().hasOpenReadFile(),
           "listing incorrectly entered the streaming file pipeline");
}

void testPutCreated() {
    Harness harness;
    expect(harness.readRequest(
               "PUT /files/put.txt HTTP/1.1\r\nHost: localhost\r\n"
               "Content-Length: 5\r\n\r\nhello") ==
               Connection::ReadStatus::kOpen,
           "PUT request read failed");
    expect(readFile(harness.data() / "put.txt") == "hello",
           "PUT did not commit content");
    const std::string response = harness.sendPreparedResponse();
    expectStatusLine(response, "HTTP/1.1 201 Created\r\n");
}

void testHeadExistingAndMissing() {
    Harness existing;
    writeFile(existing.data() / "head.txt", "hello");
    expect(existing.readRequest(
               "HEAD /files/head.txt HTTP/1.1\r\nHost: localhost\r\n\r\n") ==
               Connection::ReadStatus::kOpen,
           "HEAD existing read failed");
    const std::string ok_response = existing.sendPreparedResponse();
    expectStatusLine(ok_response, "HTTP/1.1 200 OK\r\n");
    expect(ok_response.find("Content-Length: 5\r\n") != std::string::npos,
           "HEAD returned wrong file length");
    const std::size_t header_end = ok_response.find("\r\n\r\n");
    expect(header_end != std::string::npos &&
               header_end + 4U == ok_response.size(),
           "HEAD sent a response body");

    Harness missing;
    expect(missing.readRequest(
               "HEAD /files/missing.txt HTTP/1.1\r\nHost: localhost\r\n\r\n") ==
               Connection::ReadStatus::kOpen,
           "HEAD missing read failed");
    expectStatusLine(missing.sendPreparedResponse(),
                     "HTTP/1.1 404 Not Found\r\n");
}

void testDeleteExistingAndMissing() {
    Harness existing;
    writeFile(existing.data() / "delete.txt", "content");
    expect(existing.readRequest(
               "DELETE /files/delete.txt HTTP/1.1\r\n"
               "Host: localhost\r\n\r\n") == Connection::ReadStatus::kOpen,
           "DELETE existing read failed");
    expect(!std::filesystem::exists(existing.data() / "delete.txt"),
           "DELETE did not remove file");
    expectStatusLine(existing.sendPreparedResponse(),
                     "HTTP/1.1 204 No Content\r\n");

    Harness missing;
    expect(missing.readRequest(
               "DELETE /files/missing.txt HTTP/1.1\r\n"
               "Host: localhost\r\n\r\n") == Connection::ReadStatus::kOpen,
           "DELETE missing read failed");
    expectStatusLine(missing.sendPreparedResponse(),
                     "HTTP/1.1 404 Not Found\r\n");
}

void testRoutingErrors() {
    Harness invalid;
    invalid.readRequest(
        "HEAD /files/a/b HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatusLine(invalid.sendPreparedResponse(),
                     "HTTP/1.1 400 Bad Request\r\n");

    Harness unsupported;
    unsupported.readRequest(
        "POST /files/a.txt HTTP/1.1\r\nHost: localhost\r\n\r\n");
    const std::string method_response = unsupported.sendPreparedResponse();
    expectStatusLine(method_response,
                     "HTTP/1.1 405 Method Not Allowed\r\n");
    expect(method_response.find("Allow: GET, PUT, DELETE, HEAD\r\n") !=
               std::string::npos,
           "405 response is missing Allow");

    Harness malformed;
    malformed.readRequest("BROKEN\r\n\r\n");
    expectStatusLine(malformed.sendPreparedResponse(),
                     "HTTP/1.1 400 Bad Request\r\n");

    Harness too_large;
    too_large.readRequest(
        "PUT /files/large HTTP/1.1\r\nHost: localhost\r\n"
        "Content-Length: 1073741825\r\n\r\n");
    expectStatusLine(too_large.sendPreparedResponse(),
                     "HTTP/1.1 413 Payload Too Large\r\n");

}

void testNotRegularIsHiddenAsNotFound() {
    Harness harness;
    expect(std::filesystem::create_directory(harness.data() / "directory"),
           "unable to create non-regular target");
    harness.readRequest(
        "HEAD /files/directory HTTP/1.1\r\nHost: localhost\r\n\r\n");
    expectStatusLine(harness.sendPreparedResponse(),
                     "HTTP/1.1 404 Not Found\r\n");
}

void testPeerHalfCloseStillGetsResponse() {
    Harness harness;
    writeFile(harness.data() / "half.txt", "x");
    expect(harness.readRequest(
               "HEAD /files/half.txt HTTP/1.1\r\nHost: localhost\r\n\r\n",
               true) == Connection::ReadStatus::kPeerClosed,
           "peer half-close was not observed");
    expect(harness.connection().state() == Connection::State::kWritingResponse,
           "peer half-close discarded pending response");
    expectStatusLine(harness.sendPreparedResponse(),
                     "HTTP/1.1 200 OK\r\n");
}

void testSendEagainPreservesOffset() {
    Harness harness;
    harness.readRequest("BROKEN\r\n\r\n");
    expect(harness.connection().state() == Connection::State::kWritingResponse,
           "malformed request did not prepare response");

    constexpr int small_buffer = 1024;
    expect(::setsockopt(harness.connection().fd(), SOL_SOCKET, SO_SNDBUF,
                        &small_buffer, sizeof(small_buffer)) == 0,
           "unable to reduce test send buffer");

    const std::array<char, 4096> filler{};
    std::size_t filler_bytes = 0;
    while (true) {
        const ssize_t sent = ::send(
            harness.connection().fd(), filler.data(), filler.size(),
            MSG_NOSIGNAL);
        if (sent > 0) {
            filler_bytes += static_cast<std::size_t>(sent);
            continue;
        }
        if (sent == -1 && errno == EINTR) {
            continue;
        }
        expect(sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK),
               "test failed to create send pressure");
        break;
    }
    expect(filler_bytes > 0U, "send pressure filler was not written");

    expect(harness.connection().writeAvailableData() ==
               Connection::WriteStatus::kPending,
           "full send buffer did not produce EAGAIN");
    const std::size_t saved_offset = harness.connection().responseOffset();
    expect(saved_offset < harness.connection().responseSize(),
           "pending send lost its remaining response");

    const std::string first_drain = harness.sockets().drainAvailable();
    expect(first_drain.size() >= filler_bytes,
           "did not drain all send-pressure bytes");
    std::string response = first_drain.substr(filler_bytes);

    Connection::WriteStatus status = Connection::WriteStatus::kPending;
    for (int attempt = 0;
         attempt < 8 && status == Connection::WriteStatus::kPending;
         ++attempt) {
        status = harness.connection().writeAvailableData();
        expect(harness.connection().responseOffset() >= saved_offset,
               "partial send offset moved backwards");
        response += harness.sockets().drainAvailable();
    }

    expect(status == Connection::WriteStatus::kComplete,
           "response did not resume after EAGAIN");
    expectStatusLine(response, "HTTP/1.1 400 Bad Request\r\n");
}

struct TestCase {
    std::string_view name;
    void (*function)();
};

}  // namespace

int main() {
    const std::vector<TestCase> tests{
        {"response serialization", testResponseSerialization},
        {"file listing response", testFileListingResponse},
        {"PUT created", testPutCreated},
        {"HEAD existing and missing", testHeadExistingAndMissing},
        {"DELETE existing and missing", testDeleteExistingAndMissing},
        {"routing errors", testRoutingErrors},
        {"non-regular hidden as not found", testNotRegularIsHiddenAsNotFound},
        {"peer half-close response", testPeerHalfCloseStillGetsResponse},
        {"send EAGAIN preserves offset", testSendEagainPreservesOffset},
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

    std::cout << passed << " HTTP response test groups passed\n";
    return 0;
}
