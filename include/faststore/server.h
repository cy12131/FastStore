#ifndef FASTSTORE_SERVER_H
#define FASTSTORE_SERVER_H

#include <cstdint>
#include <unordered_set>

namespace faststore {

// 管理 M1 阶段的监听 socket、epoll 实例和所有客户端 fd。
class Server {
public:
    // 创建服务器对象并保存监听端口；此时尚未创建任何系统资源。
    // port 表示服务器要监听的 TCP 端口。
    explicit Server(std::uint16_t port);

    // 关闭仍由服务器持有的客户端、监听 socket 和 epoll fd。
    ~Server();

    // fd 具有唯一所有权，因此服务器对象不可复制或移动。
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;

    // 初始化服务器资源并持续运行 epoll 事件循环。
    void run();

private:
    // 创建、配置、绑定并启动非阻塞 IPv4 监听 socket。
    void initializeListeningSocket();

    // 创建 epoll 实例并注册监听 socket 的可读事件。
    void initializeEpoll();

    // 循环接收所有待处理连接，直到 accept4 返回 EAGAIN。
    void acceptConnections();

    // 处理一个客户端的 epoll 事件。
    // client_fd 是产生事件的客户端 fd，events 是 epoll 返回的事件位集合。
    void handleClientEvent(int client_fd, std::uint32_t events);

    // 循环读取客户端数据，直到读空、断开或发生错误。
    // client_fd 是待读取的客户端 fd；返回 true 表示连接仍可保留。
    bool receiveClientData(int client_fd);

    // 将客户端从 epoll 和 fd 集合移除，然后关闭其 fd。
    // client_fd 是需要清理的客户端 fd。
    void closeClient(int client_fd);

    std::uint16_t port_;                 // 服务器监听的主机字节序端口。
    int listening_fd_{-1};               // 接收新连接的非阻塞 socket fd。
    int epoll_fd_{-1};                   // epoll 实例的文件描述符。
    std::unordered_set<int> client_fds_; // 当前由服务器持有的客户端 fd。
};

}  // namespace faststore

#endif  // FASTSTORE_SERVER_H
