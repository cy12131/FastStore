# FastStore Project Final Report

## 1. 项目定位与范围

FastStore 是一个面向完整 C++ Linux 工程实践的小型 HTTP 文件服务器，不是生产级对象存储或调度算法研究平台。项目从 non-blocking socket 和 epoll LT 起步，逐步补齐 HTTP parser、安全文件访问、streaming I/O、Reactor/Worker 分工、配置、测试、性能基线及 systemd 部署。

最终交付包含 Linux C++17 Server、Windows x64 Qt Client C0 交付包、HTTP 文件 CRUD 与列表、固定 ThreadPool、Debug/Release 测试、benchmark 工具、环境变量配置、systemd unit 和安装脚本。单次 PUT 协议上限为 1 GiB，body 始终按 chunk streaming。

## 2. 里程碑总结

| Milestone | Outcome |
|---|---|
| M0 | CMake/C++17 项目骨架与工程约束 |
| M1 | non-blocking TCP listener、epoll LT Reactor |
| M2 | Connection 状态与资源生命周期 |
| M3 | 增量 HTTP/1.1 request header parser |
| M4 | 基于 directory fd 的安全 FileService |
| M5 | streaming PUT、routing/response、streaming GET |
| M6 | robustness、资源观察和性能基线 |
| Client C0 | Windows Qt 列表、上传、下载、删除工作流 |
| M7 | 固定 ThreadPool 与 Reactor/Worker integration |
| M8 | 严格环境变量 Config 与 startup engineering |
| M9 | Release 安装、专用用户、持久目录、systemd 部署 |
| M10 | 最终构建、协议、稳定性、部署、benchmark 与文档验收 |

## 3. 最终架构与设计决定

```text
TCP client -> non-blocking socket + epoll LT -> single Reactor
                                                   |
                                      immutable HttpRequest
                                                   v
                                            fixed ThreadPool
                                         Router / FileService
                                                   |
                                      RouteCompletion queue
                                                   v
                                      eventfd wakes Reactor
```

- Reactor 独占 socket、epoll 和 Connection 状态；worker 不跨线程操作连接。
- completion 携带 fd 和稳定 connection id，避免 fd 复用后错误应用旧结果。
- eventfd 只负责唤醒；move-only 结果存放在受 mutex 保护的 queue。
- PUT 在 Reactor 持续 recv 并按 chunk 写临时文件，成功后 commit；失败或断连不暴露半文件。
- GET 在 worker 完成 open/metadata，Reactor 使用固定 64 KiB buffer 处理 read、partial send 和 EPOLLOUT。
- FileService 以稳定 directory fd 配合 `openat` 等系统调用约束访问范围。
- Config 只解析和校验环境；目录创建、文件服务初始化和 bind 仍属于 startup。

## 4. HTTP 行为

| Method | Target | Success | Failure examples |
|---|---|---:|---|
| GET | `/files` | 200 JSON list | 400 invalid target |
| PUT | `/files/{name}` | 201 | 400 invalid name, 413 over limit |
| GET | `/files/{name}` | 200 | 404 missing |
| HEAD | `/files/{name}` | 200 | 404 missing |
| DELETE | `/files/{name}` | 204 | 404 missing |
| unsupported | valid target | - | 405 |

每个 TCP connection 只处理一个 HTTP/1.1 request，response 后关闭。最大允许 `Content-Length` 为 1073741824 bytes；等于 1 GiB 由 parser 接受，大于 1 GiB 返回 413。提升上限没有改变 streaming 架构，也不会分配 1 GiB request buffer。

## 5. 测试策略与最终结果

自动化测试覆盖 request/header/Content-Length、FileService、routing/response、streaming PUT/GET、partial I/O、disconnect、malformed request、ThreadPool 生命周期及 Config 严格校验。

2026-10-24 最终门禁：

| Gate | Result |
|---|---|
| Debug build and CTest | PASS，8/8 |
| Release build and CTest | PASS，8/8 |
| `git diff --check` | PASS |
| 旧 64 MiB upload limit 扫描 | PASS，无残留 |
| 1 GiB exact parser boundary | PASS |
| 1 GiB + 1 byte service response | PASS，413 |

没有真实写入 1 GiB。实机使用 256 MiB 文件完成 PUT、GET、SHA-256 和 DELETE：上传与下载均为 268435456 bytes，SHA-256 均为 `a6d72ac7690f53be6ae46ba88506bd97302a093f7108472bd9efc3cefda06484`。

systemd 实机验证：

- 普通 PUT/GET/HEAD/DELETE：201/200/200/204；
- 空文件 PUT/GET/HEAD/DELETE：通过，Content-Length 和下载大小均为 0；
- invalid path 400、missing 404、unsupported POST 405；
- 64 个并发 GET 全部 200；
- incomplete PUT 主动断连后服务健康，最终目标文件不可见；
- 正常 restart 后已上传数据仍可读取；
- SIGKILL 后 systemd 自动更换 PID，restart counter 增加，数据仍可读取。

## 6. Final Release Benchmark

结果目录为 `benchmarks/results/m10-final-release-20261023T185345Z/`。运行目标是 Release、4 workers、`FASTSTORE_QUIET=1`、localhost、warm cache。覆盖 `head`、`get-4k`、`get-8m`、`put-64k` 及并发 1/4/8/16/32/64/128。交付确认参数为 2 秒预热、5 秒测量、1 次重复；28/28 measurement 成功，`failures.csv` 仅有表头，56 份 client raw output 和 28 份 CPU/RSS sample 均保留。

c=16 代表数据：

| Workload | req/s | MiB/s | mean latency | P99 | avg CPU | peak RSS KiB |
|---|---:|---:|---:|---:|---:|---:|
| HEAD | 10348.34 | - | 1.50 ms | 3.40 ms | 45.40% | 4616 |
| GET 4 KiB | 12154.13 | 48.17 | 1.19 ms | 2.336 ms | 95.20% | 5504 |
| GET 8 MiB | 271.97 | 2181.12 | 58.11 ms | 100.266 ms | 83.79% | 5816 |
| PUT 64 KiB | 1915.33 | - | 12.84 ms | 118.868 ms | 35.57% | 5024 |

与 M6-B quiet 正式基线相同 c=16 的 req/s 相比，HEAD、GET 4 KiB、GET 8 MiB 分别约 +7.3%、+10.0%、+25.2%，PUT 约 -9.4%。最终轮更短且仅重复一次，差异只用于发现明显回退，不应解释为确定的性能提升；长期趋势仍以 M6-B 的 5 秒预热、20 秒测量、3 次重复结果为准。

## 7. Linux 部署与运维

| Item | Value |
|---|---|
| Executable | `/opt/faststore/bin/faststore` |
| Service account | `faststore:faststore` |
| Persistent data | `/var/lib/faststore/data` |
| Unit | `/etc/systemd/system/faststore.service` |
| Port / workers | 8080 / 4 |
| Restart policy | `on-failure`, 2 seconds |
| Logging | stdout/stderr to journald |

安装流程先完成 Release build 和 CTest，再执行 `sudo deploy/install.sh`。脚本创建账户与目录、安装 root-owned executable 和 unit、执行 daemon-reload，但不会自动 start、enable 或删除数据。

```bash
sudo systemctl start faststore
sudo systemctl stop faststore
sudo systemctl restart faststore
sudo systemctl enable faststore
systemctl status faststore
journalctl -u faststore -n 100 --no-pager
journalctl -u faststore -f
```

最终服务为 active、enabled，ExecStart 指向 `/opt/faststore/bin/faststore`，数据目录为 `/var/lib/faststore/data`，监听 `0.0.0.0:8080`。部署 binary 与 Release build 的 SHA-256 一致。journald 已验证 startup、request、正常 restart、SIGKILL failure 和自动恢复。

## 8. Windows Qt Client

Client C0 的 Windows x64 交付包位于 `data/FastStoreClient_Windows_x64.zip`。客户端通过公开 HTTP API 完成列表、上传、下载和删除，不共享服务端 filesystem 或线程状态。公网部署需替换实际 Server 地址并限制 8080/TCP 来源范围。

## 9. 已知限制

- 单连接单 request，无 keep-alive、pipelining、chunked transfer、range、TLS、HTTP/2 或认证；
- regular-file read/write 是同步 I/O，慢盘可能阻塞 Reactor；
- 固定 ThreadPool task queue 无容量上限，没有 backpressure；
- 无动态 worker、priority、work stealing、任务取消或研究型调度；
- 应用层没有 SIGTERM graceful drain；
- GET 不提供外部文件变化的快照一致性；
- request target 不做 URL decode，文件列表假定文件名可表示为 UTF-8；
- 无磁盘配额、用户隔离、访问控制、审计或在线升级；
- benchmark 是单机 warm-cache 基线，不代表公网生产容量。

## 10. 工程经验

核心收获是建立清晰职责边界与可验证交付：Reactor 独占连接状态可降低并发推理复杂度；completion 必须防御 fd reuse；大文件能力首先是有界内存和失败清理问题；部署必须共同验证 binary、identity、permissions、persistence、restart 和 logging；benchmark 必须保留环境、原始输出、失败记录和资源采样。

## 11. Git 交付建议

当前工作区跨越 Client C0、M7、M8、M9、1 GiB 调整和 M10 文档，且核心文件存在阶段重叠。建议按语义审阅并分组暂存：

1. Client C0 与文件列表 API；
2. M7 ThreadPool 和 Reactor/Worker integration；
3. M8 Config/startup engineering；
4. M9 systemd unit 与 installer；
5. 1 GiB upload limit 与边界测试；
6. M10 README 和 final report。

`CMakeLists.txt`、`README.md`、`src/main.cpp` 和 HTTP/Server 测试应按 hunk 检查。用户数据、handoff 包、Windows 二进制交付物是否纳入版本控制需由交付策略决定。本次 M10 不执行 commit 或 push。
