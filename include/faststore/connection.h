#ifndef FASTSTORE_CONNECTION_H
#define FASTSTORE_CONNECTION_H

#include <vector>

namespace faststore {

// 保存并管理一个客户端连接在 M2 阶段需要的全部状态。
class Connection {
public:
    // 描述一次非阻塞读取结束后连接的状态。
    enum class ReadStatus {
        kOpen,        // 数据已读到 EAGAIN，连接仍然有效。
        kPeerClosed,  // recv 返回 0，对端已经关闭连接。
        kError        // recv 遇到不可恢复的错误。
    };

    // 接管 client_fd 的唯一所有权。
    explicit Connection(int client_fd) noexcept;

    // 关闭当前 Connection 独占的客户端 fd。
    ~Connection();

    // client fd 只能由一个 Connection 拥有，因此禁止复制。
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // 允许把唯一所有权转入容器；移动后源对象不再持有 fd。
    Connection(Connection&& other) noexcept;
    Connection& operator=(Connection&& other) noexcept;

    // 返回当前 Connection 拥有的客户端 fd。
    [[nodiscard]] int fd() const noexcept;

    // 返回最近一次读取后连接是否仍然有效。
    [[nodiscard]] bool isValid() const noexcept;

    // 返回累计保存的客户端输入字节，供后续 HTTP Parser 使用。
    [[nodiscard]] const std::vector<char>& inputBuffer() const noexcept;

    // 循环 recv 到 EAGAIN、EOF 或真正错误，并保存所有读取到的数据。
    ReadStatus readAvailableData();

private:
    // 关闭当前持有的 fd 并将其重置为 -1。
    void closeClientFd() noexcept;

    int client_fd_;                 // 当前 Connection 独占的客户端 fd。
    bool valid_{true};              // 连接是否仍可继续处理。
    std::vector<char> input_buffer_; // 跨多次 epoll 事件累计的输入字节。
};

}  // namespace faststore

#endif  // FASTSTORE_CONNECTION_H
