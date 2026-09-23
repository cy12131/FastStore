#include "faststore/connection.h"

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <utility>

namespace faststore {
namespace {

// kReadBufferSize 是每次 recv 使用的临时栈缓冲区容量。
constexpr std::size_t kReadBufferSize = 8192;

}  // namespace

// 保存由 accept4 创建的客户端 fd，并从此承担其关闭责任。
Connection::Connection(int client_fd) noexcept : client_fd_(client_fd) {}

// 从 other 接管 fd、有效状态和输入缓冲区。
Connection::Connection(Connection&& other) noexcept
    : client_fd_(std::exchange(other.client_fd_, -1)),
      valid_(std::exchange(other.valid_, false)),
      input_buffer_(std::move(other.input_buffer_)) {}

// 先释放当前资源，再从 other 接管连接状态。
Connection& Connection::operator=(Connection&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    closeClientFd();
    client_fd_ = std::exchange(other.client_fd_, -1);
    valid_ = std::exchange(other.valid_, false);
    input_buffer_ = std::move(other.input_buffer_);
    return *this;
}

// 释放 Connection 唯一拥有的客户端 fd。
Connection::~Connection() {
    closeClientFd();
}

// 关闭当前 fd，并确保析构或再次赋值时不会重复关闭。
void Connection::closeClientFd() noexcept {
    if (client_fd_ < 0) {
        return;
    }

    // fd_to_close 保存关闭前的 fd；成员立即重置以解除所有权。
    const int fd_to_close = client_fd_;
    client_fd_ = -1;

    if (::close(fd_to_close) == -1) {
        // error 保存 close 失败时的 errno，防止日志操作覆盖它。
        const int error = errno;
        std::cerr << "Failed to close client fd=" << fd_to_close
                  << ": " << std::strerror(error) << std::endl;
    }
}

// 返回用于 epoll 查找和系统调用的客户端 fd。
int Connection::fd() const noexcept {
    return client_fd_;
}

// 返回 Connection 记录的有效状态。
bool Connection::isValid() const noexcept {
    return valid_;
}

// 返回不会转移所有权的只读输入缓冲区引用。
const std::vector<char>& Connection::inputBuffer() const noexcept {
    return input_buffer_;
}

// 读取当前所有可用字节，并将其追加到持久输入缓冲区。
Connection::ReadStatus Connection::readAvailableData() {
    // read_buffer 临时保存一次 recv 返回的数据。
    std::array<char, kReadBufferSize> read_buffer{};

    while (true) {
        // bytes_received 是本次 recv 实际读取的字节数。
        const ssize_t bytes_received =
            ::recv(client_fd_, read_buffer.data(), read_buffer.size(), 0);

        if (bytes_received > 0) {
            // chunk_size 是转换为容器索引类型后的本次数据长度。
            const auto chunk_size = static_cast<std::size_t>(bytes_received);
            input_buffer_.insert(input_buffer_.end(), read_buffer.data(),
                                 read_buffer.data() + chunk_size);

            std::cout << "Received " << bytes_received
                      << " bytes from fd=" << client_fd_
                      << ", connection buffer=" << input_buffer_.size()
                      << " bytes" << std::endl;
            continue;
        }

        if (bytes_received == 0) {
            valid_ = false;
            return ReadStatus::kPeerClosed;
        }

        if (errno == EINTR) {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ReadStatus::kOpen;
        }

        // error 保存 recv 失败时的 errno。
        const int error = errno;
        valid_ = false;
        std::cerr << "recv failed for fd=" << client_fd_ << ": "
                  << std::strerror(error) << std::endl;
        return ReadStatus::kError;
    }
}

}  // namespace faststore
