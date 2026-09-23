#ifndef FASTSTORE_SERVER_H
#define FASTSTORE_SERVER_H

#include "faststore/connection.h"

#include <cstdint>
#include <unordered_map>

namespace faststore {

// 管理监听 socket、epoll 实例以及所有 Connection 的生命周期。
class Server {
public:
    // 创建服务器对象并保存监听端口；此时尚未创建任何系统资源。
    // port 表示服务器要监听的 TCP 端口。
    explicit Server(std::uint16_t port);

    // 销毁所有 Connection，并关闭监听 socket 和 epoll fd。
    ~Server();

    // fd 和 Connection 具有唯一所有权，因此 Server 不可复制或移动。
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

    // 将一个客户端事件分发给对应的 Connection。
    // connection 是事件对应的状态对象，events 是 epoll 事件位集合。
    void handleClientEvent(Connection& connection, std::uint32_t events);

    // 从 epoll 删除客户端，再销毁拥有该 fd 的 Connection。
    // client_fd 是需要清理的客户端 fd。
    void closeClient(int client_fd);

    std::uint16_t port_; // 服务器监听的主机字节序端口。
    int listening_fd_{-1}; // 接收新连接的非阻塞 socket fd。
    int epoll_fd_{-1}; // epoll 实例的文件描述符。

    // 以客户端 fd 为键，按值拥有每个不可复制但可安全移动的 Connection。
    std::unordered_map<int, Connection> connections_;
};

}  // namespace faststore

#endif  // FASTSTORE_SERVER_H
