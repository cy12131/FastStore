#ifndef FASTSTORE_SERVER_H
#define FASTSTORE_SERVER_H

#include "faststore/connection.h"
#include "faststore/file_service.h"

#include <cstdint>
#include <unordered_map>

namespace faststore {

// 管理监听 socket、epoll、FileService 以及所有 Connection 的生命周期。
class Server {
public:
    // 接管 FileService；所有 Connection 的上传操作通过它访问 data/。
    Server(std::uint16_t port, FileService file_service);
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
    void closeClient(int client_fd);

    std::uint16_t port_;       // 服务器监听的主机字节序端口。
    int listening_fd_{-1};     // 接收新连接的非阻塞 socket fd。
    int epoll_fd_{-1};         // epoll 实例的文件描述符。
    FileService file_service_; // 唯一管理 data directory fd。

    // 以客户端 fd 为键，按值拥有每个 Connection。
    std::unordered_map<int, Connection> connections_;
};

}  // namespace faststore

#endif  // FASTSTORE_SERVER_H
