# FastStore

FastStore 是一个使用 C++ 开发、运行于 Linux 的 HTTP 文件服务器项目。

## V0 目标

V0 将实现基于 epoll 的最小 HTTP 文件服务器，并支持通过 PUT、GET、DELETE 和 HEAD 操作 `data/` 目录中的文件。

## 当前阶段

当前开发 Milestone 4-A：独立 FileService 使用 Linux directory fd 和 `*at` 系统调用安全访问 `data/`。当前尚未把 HTTP Request 映射到文件操作，也不包含 HTTP Response。

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

## M3 内存说明

M3 为验证增量 Parser，会把完整 PUT body 保存在 Connection 输入缓冲区和 HttpRequest 中，最大 body 为 64 MiB。这是临时设计；M4 文件写入阶段应改为流式消费 body，避免大文件完整驻留内存。
