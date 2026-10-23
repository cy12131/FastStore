#include "faststore/server.h"

#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace faststore {
namespace {

// kListenBacklog 是内核监听队列允许使用的最大积压连接数。
constexpr int kListenBacklog = SOMAXCONN;

// kMaxEvents 是一次 epoll_wait 最多取出的就绪事件数量。
constexpr std::size_t kMaxEvents = 64;

// 根据当前 errno 抛出 system_error；operation 表示失败的系统调用。
[[noreturn]] void throwSystemError(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

// 关闭 Server 自身拥有的监听或 epoll fd。
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
Server::Server(std::uint16_t port, FileService file_service,
               std::size_t worker_count,
               bool informational_logging_enabled)
    : port_(port),
      file_service_(std::move(file_service)),
      informational_logging_enabled_(informational_logging_enabled),
      thread_pool_(worker_count) {}

// 先停止 worker，再销毁 Connection 和 Server 系统资源。
Server::~Server() {
    // Worker task 仍可能使用 FileService、completion queue 和 eventfd。
    // 先 drain/join 所有 worker，之后才销毁这些依赖资源。
    thread_pool_.shutdown();

    // 清空容器会调用每个 Connection 的析构函数并关闭对应客户端 fd。
    connections_.clear();

    closeFileDescriptor(listening_fd_, "listening socket");
    closeFileDescriptor(completion_event_fd_, "completion eventfd");
    closeFileDescriptor(epoll_fd_, "epoll");
}

// 初始化网络资源并持续分发 epoll 返回的就绪事件。
void Server::run() {
    initializeListeningSocket();
    initializeEpoll();

    if (informational_logging_enabled_) {
        std::cout << "Listening on 0.0.0.0:" << port_ << std::endl;
    }

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

            if (fd == completion_event_fd_) {
                if ((event.events & EPOLLIN) != 0U) {
                    handleWorkerCompletions();
                }
                if ((event.events & (EPOLLERR | EPOLLHUP)) != 0U) {
                    throw std::runtime_error(
                        "completion eventfd reported an error");
                }
                continue;
            }

            // connection_it 将客户端 fd 映射到唯一的 Connection。
            const auto connection_it = connections_.find(fd);
            if (connection_it != connections_.end()) {
                handleClientEvent(connection_it->second, event.events);
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

    completion_event_fd_ =
        ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (completion_event_fd_ == -1) {
        throwSystemError("eventfd");
    }

    event = {};
    event.events = EPOLLIN;
    event.data.fd = completion_event_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD,
                    completion_event_fd_, &event) == -1) {
        throwSystemError("epoll_ctl(ADD completion eventfd)");
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

        // connection 立即接管 client_fd；异常退出时也会自动关闭它。
        const std::uint64_t connection_id = next_connection_id_++;
        Connection connection{client_fd, connection_id,
                              informational_logging_enabled_};

        // connection_inserted 表示唯一所有权是否已移动到 map 中。
        const bool connection_inserted =
            connections_.try_emplace(client_fd, std::move(connection)).second;
        if (!connection_inserted) {
            throw std::logic_error("accepted duplicate client fd");
        }

        // 从此处开始，client_fd 由容器内的 Connection 唯一拥有。
        epoll_event event{};
        event.events = EPOLLIN | EPOLLRDHUP;
        event.data.fd = client_fd;
        if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &event) == -1) {
            // error 保存客户端注册失败时的 errno。
            const int error = errno;
            std::cerr << "Failed to register client fd=" << client_fd
                      << " with epoll: " << std::strerror(error) << std::endl;

            // erase 销毁 Connection，由其析构函数关闭 client_fd。
            connections_.erase(client_fd);
            continue;
        }

        // peer_ip 保存转换为文本形式的客户端 IPv4 地址。
        std::array<char, INET_ADDRSTRLEN> peer_ip{};

        // address 指向转换成功的地址文本；失败时为 nullptr。
        const char* address = ::inet_ntop(AF_INET, &peer_address.sin_addr,
                                          peer_ip.data(), peer_ip.size());
        if (informational_logging_enabled_) {
            std::cout << "Accepted connection: fd=" << client_fd;
            if (address != nullptr) {
                std::cout << " peer=" << address << ':'
                          << ntohs(peer_address.sin_port);
            }
            std::cout << std::endl;
        }
    }
}

// 分发 EPOLLIN/EPOLLOUT；只有 Server 修改 epoll interest。
void Server::handleClientEvent(Connection& connection, std::uint32_t events) {
    const int client_fd = connection.fd();

    if ((events & EPOLLERR) != 0U) {
        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);
        if (::getsockopt(client_fd, SOL_SOCKET, SO_ERROR,
                         &socket_error, &error_length) == 0 &&
            socket_error != 0) {
            std::cerr << "Socket error on fd=" << client_fd << ": "
                      << std::strerror(socket_error) << std::endl;
        } else {
            std::cerr << "Socket error on fd=" << client_fd << std::endl;
        }
        closeClient(client_fd);
        return;
    }

    if (connection.wantsRead() &&
        (events & (EPOLLIN | EPOLLRDHUP)) != 0U) {
        const Connection::ReadStatus read_status =
            connection.readAvailableData(file_service_);

        if (read_status == Connection::ReadStatus::kError ||
            connection.state() == Connection::State::kError) {
            std::cerr << "Connection processing error: fd=" << client_fd
                      << ": " << connection.errorMessage() << std::endl;
            closeClient(client_fd);
            return;
        }

        if (connection.wantsWrite()) {
            if (!updateClientInterest(connection)) {
                closeClient(client_fd);
                return;
            }
        } else if (connection.state() == Connection::State::kRequestReady) {
            if (!dispatchRequest(connection) ||
                !updateClientInterest(connection)) {
                closeClient(client_fd);
                return;
            }
        } else if (read_status == Connection::ReadStatus::kPeerClosed) {
            closeClient(client_fd);
            return;
        }
    }

    if (connection.state() == Connection::State::kWaitingForWorker) {
        // waiting 状态不监听 EPOLLOUT；EPOLLHUP/ERR 仍可关闭断开的 peer。
        if ((events & EPOLLHUP) != 0U) {
            closeClient(client_fd);
        }
        return;
    }

    if (connection.wantsWrite()) {
        // EPOLLRDHUP is only a peer write-half close. A response may still be
        // sent; EPOLLHUP/EPOLLERR represent a fatal path handled separately.
        if ((events & EPOLLHUP) != 0U) {
            closeClient(client_fd);
            return;
        }
        if ((events & EPOLLOUT) != 0U) {
            const Connection::WriteStatus write_status =
                connection.writeAvailableData();
            if (write_status == Connection::WriteStatus::kError) {
                std::cerr << "Response send error: fd=" << client_fd
                          << ": " << connection.errorMessage() << std::endl;
                closeClient(client_fd);
                return;
            }
            if (write_status == Connection::WriteStatus::kComplete) {
                if (informational_logging_enabled_) {
                    std::cout << "Response sent: fd=" << client_fd
                              << std::endl;
                }
                closeClient(client_fd);
            }
        }
        return;
    }

    if (connection.state() == Connection::State::kResponseComplete ||
        (events & (EPOLLHUP | EPOLLRDHUP)) != 0U) {
        closeClient(client_fd);
    }
}

bool Server::dispatchRequest(Connection& connection) {
    const int client_fd = connection.fd();
    const std::uint64_t connection_id = connection.id();

    try {
        std::optional<HttpRequest> request = connection.beginWorkerDispatch();
        if (!request.has_value()) {
            std::cerr << "Connection was not ready for worker dispatch: fd="
                      << client_fd << std::endl;
            return false;
        }

        // Worker 捕获独立 request、fd 和 generation，不捕获 Connection、
        // map iterator 或 client object。Server 的生命周期由 shutdown/join
        // 保证覆盖 task 对 FileService/completion resources 的访问。
        thread_pool_.submit(
            [this, request = std::move(request.value()),
             client_fd, connection_id]() mutable {
                RouteResult result{
                    makeResponse(HttpStatus::kInternalServerError),
                    std::nullopt};
                try {
                    result = routeRequest(file_service_, request);
                } catch (const std::exception& error) {
                    std::cerr << "Worker routing failed: "
                              << error.what() << std::endl;
                } catch (...) {
                    std::cerr << "Worker routing failed: unknown exception"
                              << std::endl;
                }

                publishCompletion(RouteCompletion{
                    client_fd, connection_id, std::move(result)});
            });
        return true;
    } catch (const std::exception& error) {
        std::cerr << "Unable to dispatch request for fd=" << client_fd
                  << ": " << error.what() << std::endl;
    } catch (...) {
        std::cerr << "Unable to dispatch request for fd=" << client_fd
                  << ": unknown exception" << std::endl;
    }

    // beginWorkerDispatch() 先进入 waiting，因此 submit/copy 失败也能由
    // Reactor 生成 500，不会让连接永久停在等待状态。
    return connection.applyRouteResult(RouteResult{
        makeResponse(HttpStatus::kInternalServerError), std::nullopt});
}

void Server::publishCompletion(RouteCompletion completion) {
    {
        std::lock_guard<std::mutex> lock{completion_mutex_};
        completions_.push(std::move(completion));
    }
    notifyCompletion();
}

void Server::notifyCompletion() noexcept {
    const std::uint64_t value = 1;
    while (true) {
        const ssize_t written = ::write(
            completion_event_fd_, &value, sizeof(value));
        if (written == static_cast<ssize_t>(sizeof(value))) {
            return;
        }
        if (written == -1 && errno == EINTR) {
            continue;
        }
        if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // counter 已饱和意味着 Reactor 已有未处理通知，无需重复唤醒。
            return;
        }
        const int error = written == -1 ? errno : EIO;
        std::cerr << "Failed to notify completion eventfd: "
                  << std::strerror(error) << std::endl;
        return;
    }
}

void Server::drainCompletionEventFd() noexcept {
    std::uint64_t value = 0;
    while (true) {
        const ssize_t count = ::read(
            completion_event_fd_, &value, sizeof(value));
        if (count == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        const int error = count == -1 ? errno : EIO;
        std::cerr << "Failed to drain completion eventfd: "
                  << std::strerror(error) << std::endl;
        return;
    }
}

void Server::handleWorkerCompletions() {
    drainCompletionEventFd();

    std::queue<RouteCompletion> local;
    {
        std::lock_guard<std::mutex> lock{completion_mutex_};
        local.swap(completions_);
    }

    while (!local.empty()) {
        RouteCompletion completion = std::move(local.front());
        local.pop();

        const auto connection_it = connections_.find(completion.client_fd);
        if (connection_it == connections_.end() ||
            connection_it->second.id() != completion.connection_id) {
            // fd 可能已关闭并被 Linux 复用；generation 不匹配的结果过期。
            continue;
        }

        Connection& connection = connection_it->second;
        if (!connection.applyRouteResult(std::move(completion.result))) {
            std::cerr << "Completion arrived for non-waiting connection: fd="
                      << connection.fd() << std::endl;
            closeClient(connection.fd());
            continue;
        }
        if (!updateClientInterest(connection)) {
            closeClient(connection.fd());
        }
    }
}

// 在读取和响应发送阶段之间切换 LT epoll interest。
bool Server::updateClientInterest(const Connection& connection) {
    epoll_event event{};
    event.events = EPOLLRDHUP;
    if (connection.wantsWrite()) {
        event.events |= EPOLLOUT;
    } else if (connection.wantsRead()) {
        event.events |= EPOLLIN;
    }
    event.data.fd = connection.fd();
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD,
                    connection.fd(), &event) == -1) {
        const int error = errno;
        std::cerr << "Failed to update epoll interest for fd="
                  << connection.fd() << ": " << std::strerror(error)
                  << std::endl;
        return false;
    }
    return true;
}

// 先从 epoll 删除客户端，再销毁拥有该 fd 的 Connection。
void Server::closeClient(int client_fd) {
    // connection_it 在 erase 前定位 Connection；erase 后不可再解引用。
    const auto connection_it = connections_.find(client_fd);
    if (connection_it == connections_.end()) {
        return;
    }

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr) == -1 &&
        errno != ENOENT && errno != EBADF) {
        // error 保存 epoll 删除失败时的 errno。
        const int error = errno;
        std::cerr << "Failed to remove client fd=" << client_fd
                  << " from epoll: " << std::strerror(error) << std::endl;
    }

    // 销毁 Connection；其析构函数在这里关闭 client_fd。
    connections_.erase(connection_it);
    if (informational_logging_enabled_) {
        std::cout << "Connection closed: fd=" << client_fd << std::endl;
    }
}

}  // namespace faststore
