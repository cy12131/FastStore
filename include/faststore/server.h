#ifndef FASTSTORE_SERVER_H
#define FASTSTORE_SERVER_H

#include "faststore/connection.h"
#include "faststore/file_service.h"
#include "faststore/router.h"
#include "faststore/thread_pool.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <unordered_map>

namespace faststore {

// Worker 只产生结果；该 move-only completion 由 Reactor 验证后应用。
struct RouteCompletion {
    int client_fd;
    std::uint64_t connection_id;
    RouteResult result;
};

// 管理监听 socket、epoll、FileService 以及所有 Connection 的生命周期。
class Server {
public:
    // 接管 FileService；所有 Connection 的上传操作通过它访问 data/。
    Server(std::uint16_t port, FileService file_service,
           std::size_t worker_count,
           bool informational_logging_enabled = true);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    // 初始化服务器资源并持续运行 epoll 事件循环。
    void run();

private:
    void initializeListeningSocket();
    void initializeEpoll();
    void acceptConnections();
    void handleClientEvent(Connection& connection, std::uint32_t events);
    bool dispatchRequest(Connection& connection);
    void publishCompletion(RouteCompletion completion);
    void notifyCompletion() noexcept;
    void handleWorkerCompletions();
    void drainCompletionEventFd() noexcept;
    bool updateClientInterest(const Connection& connection);
    void closeClient(int client_fd);

    std::uint16_t port_;       // 服务器监听的主机字节序端口。
    int listening_fd_{-1};     // 接收新连接的非阻塞 socket fd。
    int epoll_fd_{-1};         // epoll 实例的文件描述符。
    int completion_event_fd_{-1}; // worker 唤醒 Reactor 的 eventfd。
    FileService file_service_; // 唯一管理 data directory fd。
    bool informational_logging_enabled_; // 是否输出高频正常路径日志。
    std::uint64_t next_connection_id_{1};

    // 以客户端 fd 为键，按值拥有每个 Connection。
    std::unordered_map<int, Connection> connections_;

    // queue 保存真正的 RouteResult；eventfd 只负责发出“有结果”通知。
    std::queue<RouteCompletion> completions_;
    std::mutex completion_mutex_;

    // 声明在其依赖资源之后，并由析构函数首先 shutdown。
    ThreadPool thread_pool_;
};

}  // namespace faststore

#endif  // FASTSTORE_SERVER_H
