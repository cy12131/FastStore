# FastStore

FastStore 是一个使用 C++ 开发、运行于 Linux 的 HTTP 文件服务器项目。

## V0 目标

V0 将实现基于 epoll 的最小 HTTP 文件服务器，并支持通过 PUT、GET、DELETE 和 HEAD 操作 `data/` 目录中的文件。

## 当前阶段

当前开发 Milestone 4-B：PUT request body 通过有界 Connection buffer 流式写入临时文件，完成后再提交到 `data/`。当前不包含 HTTP Response、通用 Router 或 keep-alive。

## 构建

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

未指定构建类型时，项目默认使用 Debug 模式。

## 运行

在 `build/` 目录中执行：

```bash
./faststore
```

程序启动后持续运行，可使用 `Ctrl+C` 结束。

## M4-B 当前限制

PUT body 的逻辑大小上限仍为 64 MiB，但 body 不再完整保存在 Connection 或 HttpRequest 中。Connection 使用固定大小的网络读取块并及时消费已写入磁盘的数据。

M4-B 在 Reactor 线程同步执行普通文件 `write()`。慢速存储可能延迟其他连接的处理；这个限制有意保留到 ThreadPool milestone 解决。

当前每个连接只处理一个请求，不支持 HTTP Response、keep-alive、pipelining 或 chunked transfer encoding。

`Content-Length` 定义 PUT body 的文件边界，边界之后的字节永远不会写入文件。如果 commit 前已经读到 extra bytes，请求会失败且不会 commit。`EAGAIN` 只表示当前 socket receive queue 暂时无数据，不能证明客户端未来不会继续发送；V0 会在当前单个请求完成后关闭连接，不处理以后到达的数据。
