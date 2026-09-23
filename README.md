# FastStore

FastStore 是一个使用 C++ 开发、运行于 Linux 的 HTTP 文件服务器项目。

## V0 目标

V0 将实现基于 epoll 的最小 HTTP 文件服务器，并支持通过 PUT、GET、DELETE 和 HEAD 操作 `data/` 目录中的文件。

## 当前阶段

当前开发 Milestone 2：Server 负责 epoll 事件分发，每个 Connection 独立拥有客户端 fd 并累计保存收到的数据。当前不包含 HTTP 解析或文件传输功能。

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
