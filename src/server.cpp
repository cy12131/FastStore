#include "faststore/server.h"

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace faststore {
namespace {

// kListenBacklog 是内核监听队列允许使用的最大积压连接数。
constexpr int kListenBacklog = SOMAXCONN;

// kMaxEvents 是一次 epoll_wait 最多取出的就绪事件数量。
constexpr std::size_t kMaxEvents = 64;

// kReadBufferSize 是每次 recv 使用的栈缓冲区容量。
constexpr std::size_t kReadBufferSize = 8192;

// 根据当前 errno 抛出 system_error；operation 表示失败的系统调用。
[[noreturn]] void throwSystemError(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

// 关闭一个有效 fd，并在失败时记录错误。
// fd 是待关闭的文件描述符，description 用于标识日志中的资源类型。
void closeFileDescriptor(int fd, const char* description) noexcept {
    if (fd < 0) {
        return;
    }

    if (::close(fd) == -1) {
        // error 保存 close 失败时的 errno，防止后续输出覆盖它。
        const int error = errno;
        std::cerr << "Failed to close " << description << " fd=" << fd
                  << ": " << std::strerror(error) << std::endl;
    }
}

}  // namespace

// 保存监听端口；socket 和 epoll 资源延迟到 run() 中创建。
Server::Server(std::uint16_t port) : port_(port) {}

// 释放仍由 Server 持有的全部文件描述符。
Server::~Server() {
    // client_fd 依次表示析构时仍处于打开状态的客户端连接。
    for (const int client_fd : client_fds_) {
        closeFileDescriptor(client_fd, "client");
    }
    client_fds_.clear();

    closeFileDescriptor(listening_fd_, "listening socket");
    closeFileDescriptor(epoll_fd_, "epoll");
}

// 初始化网络资源并持续分发 epoll 返回的就绪事件。
void Server::run() {
    initializeListeningSocket();
    initializeEpoll();

    std::cout << "Listening on 0.0.0.0:" << port_ << std::endl;

    // ready_events 保存本轮 epoll_wait 返回的就绪事件。
    std::array<epoll_event, kMaxEvents> ready_events{};
    while (true) {
        // event_count 是本轮实际返回的事件数量。
        const int event_count = ::epoll_wait(
            epoll_fd_, ready_events.data(),
            static_cast<int>(ready_events.size()), -1);

        if (event_count == -1) {
            if (errno == EINTR) {
                continue;
            }
            throwSystemError("epoll_wait");
        }

        // index 是当前正在分发的就绪事件下标。
        for (int index = 0; index < event_count; ++index) {
            // event 引用当前就绪事件，避免复制 epoll_event。
            const epoll_event& event =
                ready_events[static_cast<std::size_t>(index)];

            // fd 是当前事件关联的监听 socket 或客户端 socket。
            const int fd = event.data.fd;

            if (fd == listening_fd_) {
                if ((event.events & EPOLLIN) != 0U) {
                    acceptConnections();
                }
                if ((event.events & (EPOLLERR | EPOLLHUP)) != 0U) {
                    throw std::runtime_error(
                        "listening socket reported an error");
                }
                continue;
            }

            if (client_fds_.find(fd) != client_fds_.end()) {
                handleClientEvent(fd, event.events);
            }
        }
    }
}

// 创建非阻塞 IPv4 TCP socket，设置地址复用并绑定到指定端口。
void Server::initializeListeningSocket() {
    listening_fd_ = ::socket(
        AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listening_fd_ == -1) {
        throwSystemError("socket");
    }

    // enable 用于开启 SO_REUSEADDR。
    constexpr int enable = 1;
    if (::setsockopt(listening_fd_, SOL_SOCKET, SO_REUSEADDR,
                     &enable, sizeof(enable)) == -1) {
        throwSystemError("setsockopt(SO_REUSEADDR)");
    }

    // address 描述监听地址 0.0.0.0 和配置的 TCP 端口。
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port_);

    if (::bind(listening_fd_, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) == -1) {
        throwSystemError("bind");
    }

    if (::listen(listening_fd_, kListenBacklog) == -1) {
        throwSystemError("listen");
    }
}

// 创建 close-on-exec 的 epoll 实例，并监听新连接事件。
void Server::initializeEpoll() {
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ == -1) {
        throwSystemError("epoll_create1");
    }

    // event 描述监听 socket 需要关注的 LT 可读事件。
    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = listening_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listening_fd_, &event) == -1) {
        throwSystemError("epoll_ctl(ADD listening socket)");
    }
}

// 循环接收监听队列中的连接，直到非阻塞 accept4 返回 EAGAIN。
void Server::acceptConnections() {
    while (true) {
        // peer_address 接收客户端的 IPv4 地址和源端口。
        sockaddr_in peer_address{};

        // peer_address_length 既提供缓冲区大小，也接收实际地址长度。
        socklen_t peer_address_length = sizeof(peer_address);

        // client_fd 是本次 accept4 创建的非阻塞客户端 socket。
        const int client_fd = ::accept4(
            listening_fd_, reinterpret_cast<sockaddr*>(&peer_address),
            &peer_address_length, SOCK_NONBLOCK | SOCK_CLOEXEC);

        if (client_fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }

            // error 保存 accept4 失败时的 errno。
            const int error = errno;
            std::cerr << "accept4 failed: " << std::strerror(error)
                      << std::endl;
            return;
        }

        try {
            client_fds_.insert(client_fd);
        } catch (...) {
            closeFileDescriptor(client_fd, "untracked client");
            throw;
        }

        // event 描述客户端需要关注的可读和对端半关闭事件。
        epoll_event event{};
        event.events = EPOLLIN | EPOLLRDHUP;
        event.data.fd = client_fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) == -1) {
            // error 保存客户端注册失败时的 errno。
            const int error = errno;
            std::cerr << "Failed to register client fd=" << client_fd
                      << " with epoll: " << std::strerror(error) << std::endl;
            client_fds_.erase(client_fd);
            closeFileDescriptor(client_fd, "unregistered client");
            continue;
        }

        // peer_ip 保存转换为文本形式的客户端 IPv4 地址。
        std::array<char, INET_ADDRSTRLEN> peer_ip{};

        // address 指向转换成功的地址文本；失败时为 nullptr。
        const char* address = ::inet_ntop(AF_INET, &peer_address.sin_addr,
                                          peer_ip.data(), peer_ip.size());
        std::cout << "Accepted connection: fd=" << client_fd;
        if (address != nullptr) {
            std::cout << " peer=" << address << ':'
                      << ntohs(peer_address.sin_port);
        }
        std::cout << std::endl;
    }
}

// 处理客户端可读、错误、挂断和半关闭事件。
void Server::handleClientEvent(int client_fd, std::uint32_t events) {
    if ((events & EPOLLIN) != 0U && !receiveClientData(client_fd)) {
        closeClient(client_fd);
        return;
    }

    if ((events & EPOLLERR) != 0U) {
        // socket_error 接收内核记录的异步 socket 错误码。
        int socket_error = 0;

        // error_length 提供并接收 SO_ERROR 值的长度。
        socklen_t error_length = sizeof(socket_error);
        if (::getsockopt(client_fd, SOL_SOCKET, SO_ERROR,
                         &socket_error, &error_length) == 0 &&
            socket_error != 0) {
            std::cerr << "Socket error on fd=" << client_fd << ": "
                      << std::strerror(socket_error) << std::endl;
        } else {
            std::cerr << "Socket error on fd=" << client_fd << std::endl;
        }
    }

    if ((events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) {
        closeClient(client_fd);
    }
}

// 将客户端当前可读的数据全部读取；数据内容在 M1 阶段直接丢弃。
bool Server::receiveClientData(int client_fd) {
    // buffer 是每次 recv 临时存放数据的固定大小缓冲区。
    std::array<char, kReadBufferSize> buffer{};

    while (true) {
        // bytes_received 是本次 recv 实际读取的字节数。
        const ssize_t bytes_received =
            ::recv(client_fd, buffer.data(), buffer.size(), 0);

        if (bytes_received > 0) {
            std::cout << "Received " << bytes_received
                      << " bytes from fd=" << client_fd << std::endl;
            continue;
        }

        if (bytes_received == 0) {
            return false;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }

        // error 保存 recv 失败时的 errno。
        const int error = errno;
        std::cerr << "recv failed for fd=" << client_fd << ": "
                  << std::strerror(error) << std::endl;
        return false;
    }
}

// 统一注销并关闭一个客户端 fd；重复清理会被安全忽略。
void Server::closeClient(int client_fd) {
    if (client_fds_.erase(client_fd) == 0U) {
        return;
    }

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr) == -1 &&
        errno != ENOENT && errno != EBADF) {
        // error 保存 epoll 删除失败时的 errno。
        const int error = errno;
        std::cerr << "Failed to remove client fd=" << client_fd
                  << " from epoll: " << std::strerror(error) << std::endl;
    }

    closeFileDescriptor(client_fd, "client");
    std::cout << "Connection closed: fd=" << client_fd << std::endl;
}

}  // namespace faststore
