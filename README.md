# FastStore

FastStore 是一个使用 C++17 开发、运行于 Linux 的 HTTP 文件服务器项目。

## V0 目标

V0 是基于 non-blocking socket、epoll LT 和单线程事件循环的最小 HTTP 文件服务器，支持通过 PUT、GET、DELETE 和 HEAD 操作 `data/` 目录中的普通文件。

## 当前阶段

当前完成 Milestone 5-B：

- PUT 将请求体流式写入临时文件，成功后提交并返回 `201 Created`。
- GET 返回精确的 `Content-Length`，随后使用单个固定 64 KiB 用户态缓冲区流式发送文件。
- HEAD 只读取 metadata、返回与 GET 一致的长度，不打开或发送文件体。
- DELETE 删除普通文件并返回 `204 No Content`。
- 小型响应 header、metadata 和错误响应继续使用 `response_buffer` 与 offset 处理 non-blocking partial send。

## 构建

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

未指定构建类型时，项目默认使用 Debug 模式，并启用 `-Wall -Wextra -Wpedantic`。

## 运行

在 `build/` 目录中执行：

```bash
./faststore
```

服务器监听 `0.0.0.0:8080`，可使用 `Ctrl+C` 结束。

## M5-B 当前限制

每个连接只处理一个 HTTP/1.1 请求，response 发送完成后关闭；不支持 keep-alive、pipelining、chunked transfer encoding、range request、TLS 或 HTTP/2。

PUT 和 GET 都使用有界内存。GET 的文件内容不会进入 `HttpResponse::body` 或完整装入内存，而是按以下基线链路传输：

```text
read one file chunk (64 KiB) -> send with partial-send offset -> next chunk
```

当前有意不使用 `sendfile`；零拷贝留到性能优化 milestone，使普通 `read + send` 可以作为清晰的 benchmark 基线。regular-file `read()` 与 PUT 的 `write()` 仍在 Reactor 线程同步执行，因此慢速存储可能阻塞 Reactor 并延迟无关连接；ThreadPool 和异步 I/O 不属于本阶段。

GET 开始时记录 `openRead()` 得到的文件大小，并将其固定为本次响应的 `Content-Length`。外部追加的数据不会超过该边界发送；外部截断导致提前 EOF 时，本次传输失败并关闭连接。FastStore V0 不对 `data/` 内文件在活动传输期间被外部进程修改提供强一致性或快照语义。

PUT 的逻辑大小上限为 64 MiB。`Content-Length` 定义 PUT body 边界，边界之后已经读到的字节会使请求失败且不会 commit。`EAGAIN` 只表示当前 socket receive queue 暂时无数据，不能证明客户端未来不会继续发送；V0 会在当前单请求完成后关闭连接，不处理以后到达的数据。
