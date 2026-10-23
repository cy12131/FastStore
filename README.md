# FastStore

FastStore 是一个使用 C++17 开发、运行于 Linux 的 HTTP 文件服务器项目。

## Project Overview

FastStore 是一个用于练习完整 C++ Linux 工程流程的小型 HTTP 文件服务器。它以 non-blocking socket、epoll LT、单 Reactor 和固定 ThreadPool 为核心，支持上传、下载、查询、删除和列出数据目录中的普通文件，并提供配置、测试、性能基线和 systemd 部署链路。

## 当前阶段

M0～M10 已完成。当前仓库包含 Linux Server、Windows Qt Client 交付包、自动化测试、benchmark 工具、systemd unit 与安装脚本；最终验证记录见 `docs/PROJECT_FINAL_REPORT.md`。

## Features

当前接口行为：

- PUT 将请求体流式写入临时文件，成功后提交并返回 `201 Created`。
- `GET /files` 返回按文件名排序的普通文件 JSON 列表。
- `GET /files/{name}` 返回精确的 `Content-Length`，随后使用单个固定 64 KiB 用户态缓冲区流式发送文件。
- HEAD 只读取 metadata、返回与 GET 一致的长度，不打开或发送文件体。
- DELETE 删除普通文件并返回 `204 No Content`。
- 每个连接只处理一个 HTTP/1.1 请求，response 发送完成后关闭。

## Architecture

```text
client socket
    |
    v
epoll LT Reactor -- recv / HTTP parse / streaming PUT / socket send
    |                         ^
    | complete request        | eventfd + completion queue
    v                         |
fixed ThreadPool ------ Router / FileService metadata operations
                              |
                              v
                    configured data directory
```

Reactor 独占 socket、epoll 和 Connection 状态；worker 不持有 Connection 指针，也不直接操作 socket。worker 完成路由后通过受 mutex 保护的 completion queue 和 eventfd 唤醒 Reactor，Reactor 使用 fd、connection id 和状态三重检查丢弃过期结果。PUT body 与 GET file body 都按 chunk 流式处理，不把完整文件放入内存。

## Project Structure

| Path | Purpose |
|---|---|
| `include/faststore/` | Server、Connection、HTTP、FileService、Config、ThreadPool 公共接口 |
| `src/` | Linux Server 实现与程序入口 |
| `tests/` | parser、文件服务、streaming、robustness、线程池和配置测试 |
| `benchmarks/` | wrk/hey workload、采样、解析、汇总和历史原始结果 |
| `deploy/` | systemd unit 与幂等安装脚本 |
| `data/` | 开发环境默认数据目录及 Windows Client 交付包 |
| `docs/` | 最终项目总结与验收记录 |

## HTTP API

| Method | Target | Success | Description |
|---|---|---:|---|
| `GET` | `/files` | 200 | 返回按文件名排序的 JSON 文件列表 |
| `PUT` | `/files/{name}` | 201 | 按 Content-Length 流式上传并原子提交 |
| `GET` | `/files/{name}` | 200 | 返回文件，body 以 64 KiB buffer 流式发送 |
| `HEAD` | `/files/{name}` | 200 | 返回与 GET 一致的 metadata，不发送 body |
| `DELETE` | `/files/{name}` | 204 | 删除普通文件 |

非法 target 返回 400，不存在的文件返回 404，不支持的方法返回 405；上传声明长度超过 1 GiB 返回 413。文件名不允许路径分隔符或目录穿越。

## Build and Test

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j
ctest --test-dir build-debug --output-on-failure

cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure
```

未指定构建类型时，项目默认使用 Debug 模式，并启用 `-Wall -Wextra -Wpedantic`。

## 运行

开发运行示例：

```bash
./build-debug/faststore
```

服务器监听 0.0.0.0:8080，可使用 Ctrl+C 结束。

## Windows Qt Client

Client C0 提供 Windows x64 Qt 客户端交付包 `data/FastStoreClient_Windows_x64.zip`，覆盖服务连接、文件列表、上传、下载和删除的基础工作流。客户端通过公开 HTTP API 与 Server 通信，不参与服务端文件路径或线程模型。公网使用前需按实际部署修改 Server 地址，并只开放必要的 8080/TCP 访问范围。

## Configuration

Config 模块只读取和校验环境变量。目录创建、FileService 打开、socket bind 和 Server 创建仍由后续启动流程负责。未设置变量时使用默认值；显式非法值会让程序在创建 Server 和 listener 前失败，不会静默回退。

| Environment Variable | Default | Meaning | Valid Values |
|---|---|---|---|
| FASTSTORE_PORT | 8080 | TCP 监听端口 | 十进制整数 1..65535 |
| FASTSTORE_DATA_DIR | 构建时配置的项目 data/ 目录 | 文件存储目录 | 非空路径 |
| FASTSTORE_WORKERS | 4 | 固定 ThreadPool worker 数量 | 十进制整数 1..256 |
| FASTSTORE_QUIET | 未设置，等价于 0 | informational logging 开关 | 0 开启，1 关闭 |

整数值采用严格全字符串解析，不接受空值、符号、前后空白或 trailing characters。Linux 自定义启动示例：

~~~bash
FASTSTORE_PORT=9090 \
FASTSTORE_DATA_DIR=/srv/faststore/data \
FASTSTORE_WORKERS=4 \
FASTSTORE_QUIET=1 \
./faststore
~~~

## Deployment

M9 使用独立 Release runtime、持久数据目录和 systemd unit：

| Purpose | Path |
|---|---|
| Release binary | /opt/faststore/bin/faststore |
| Persistent data | /var/lib/faststore/data |
| Installed unit | /etc/systemd/system/faststore.service |
| Unit source-of-truth | deploy/faststore.service |

先构建并验证 Release，再运行安装脚本：

~~~bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure
sudo deploy/install.sh
~~~

安装脚本创建专用的 faststore system user，以 root:root、0755 安装二进制，并让 faststore:faststore 以 0750 权限拥有持久数据目录。脚本只安装文件并执行 daemon-reload，不会自动启动、enable 或删除数据。

### systemd Operations

~~~bash
sudo systemctl start faststore
sudo systemctl stop faststore
sudo systemctl restart faststore
sudo systemctl status faststore
sudo systemctl enable faststore
systemctl is-active faststore
systemctl is-enabled faststore
~~~

unit 通过 M8 环境接口配置端口 8080、数据目录 /var/lib/faststore/data、4 个固定 worker 和 FASTSTORE_QUIET=0。服务使用 Restart=on-failure 与 2 秒重启间隔；管理员执行 systemctl stop 不会触发失败重启。数据位于 /var/lib，升级二进制或重启服务不会清除已上传文件。

### Logging

stdout/stderr 由 journald 收集，不创建独立日志文件：

~~~bash
journalctl -u faststore -n 50 --no-pager
journalctl -u faststore -f
~~~

## M6-C 本地性能基线

主基线在 Linux Server 本机通过 `127.0.0.1` 运行，避免把公网和 Windows 客户端差异混入服务器 baseline。安装工具：

```bash
sudo apt install wrk hey sysstat
```

`wrk` 是 GET 和 PUT 的主负载生成器，并使用 `--latency` 和 Lua histogram 获取客户端观察到的 P50/P95/P99。Debian wrk 4.1.0 无法正确完成带非零 Content-Length 的 HEAD response，因此 HEAD 使用支持 HTTP/1.1 HEAD 和延迟分位数的 `hey`；不使用会发送 HTTP/1.0 的 ApacheBench。CPU/RSS 由 runner 直接采样 `/proc`，`pidstat` 只作为可选的人工交叉检查。

四种固定 workload：

| workload | 请求 | 目的 |
|---|---|---|
| `head` | HEAD 已存在的 4 KiB 文件 | parser、routing、metadata 与小响应 |
| `get-4k` | GET 4 KiB | 小文件读取与响应 |
| `get-8m` | GET 8 MiB | streaming GET 与同步磁盘读取 |
| `put-64k` | PUT 64 KiB | streaming PUT、临时文件和 commit |

默认参数集中在 `benchmarks/config.sh`：

```text
concurrency = 1, 4, 8, 16, 32, 64, 128
warm-up = 5 seconds
measurement = 20 seconds
repeat = 3
wrk threads = min(concurrency, 4)
```

准备数据并显式启动 Server。普通执行仍保留全部信息日志；设置 FASTSTORE_QUIET=1 会抑制启动、accept、recv、正常 response 完成和正常 close 日志，错误日志始终保留；除 0、1 外的显式值会启动失败。最终正式基线使用独立 Release 构建与显式 quiet 模式：

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
ctest --test-dir build-release --output-on-failure

benchmarks/prepare_data.sh
FASTSTORE_QUIET=1 ./build-release/faststore >server.log 2>&1 &
server_pid=$!
BENCH_SERVER_PID=$server_pid BENCH_LOGGING_MODE=quiet \
BENCH_RESULT_PREFIX=final-release- benchmarks/run_baseline.sh
```

`FASTSTORE_QUIET` 只在进程启动时读取一次，热路径只检查内部 `bool`。runner 不会替 Server 开启 quiet；当 `BENCH_LOGGING_MODE=quiet` 时会检查目标进程环境中确有 `FASTSTORE_QUIET=1`，并把 `logging_mode=quiet` 写入 metadata。

runner 不会自行启动、停止或查杀 Server。未设置 `BENCH_SERVER_PID` 时，只有系统中恰好存在一个名为 `faststore` 的进程才会继续，并验证 `/proc/<pid>/exe`。短时基础设施 smoke test 可通过环境变量覆盖，例如：

```bash
BENCH_SERVER_PID=$server_pid \
BENCH_CONCURRENCIES='1 4' WARMUP_SECONDS=1 \
MEASURE_SECONDS=2 REPEATS=1 benchmarks/run_baseline.sh
```

固定 GET 文件和 PUT payload 由 `prepare_data.sh` 确定性生成，不提交 Git。PUT Lua 使用 `run timestamp + concurrency + repeat + thread id + request sequence` 生成目标名，避免并发请求集中覆盖一个文件；每个 warm-up/measurement 后只清理专用 `faststore-bench-put-*` namespace。runner 同时保留 2 GiB 最小空闲磁盘保护，清理失败会将 run 标记失败。

每次正式运行写入 `benchmarks/results/<UTC timestamp>/`：

```text
environment.txt
raw/                    原始 client 输出，包含每个 repeat 和 warm-up
samples/                每秒 CPU/RSS 样本
raw_results.csv         全部成功 repeat，不只保留平均值
failures.csv            失败阶段、原因和 raw 文件路径
summary.csv
summary.md
```

`raw_results.csv` 包含 timestamp、workload、concurrency、repeat、duration、requests/sec、raw Transfer/sec、bytes/sec、GET MiB/sec、P50/P95/P99、平均 CPU、平均/峰值 RSS。HEAD 的工具会把 Content-Length 当作逻辑 data size，因此其 transfer 字段有意留空；PUT 的 wrk Transfer/sec 原值会保留，但不把响应方向流量标成上传 MiB/sec。

基线定义为 warm-cache steady-state：允许 Linux 正常使用 page cache，不调用 `drop_caches`，也不要求 root 清缓存。RSS 是进程驻留集趋势，不等价于精确 heap allocation。runner 会从 Server 可执行文件旁的 CMake cache 记录 build type；不同 build type 的差异只能主要解释为构建配置变化，不能归因于 Reactor 或调度变化。

本机保留三组互不覆盖的结果：

- `benchmarks/results/20260925T130412Z/`：Debug 探索基线。
- `benchmarks/results/release-20260925T142624Z/`：Release logging-enabled 对照基线。
- `benchmarks/results/final-release-20260926T045638Z/`：Release quiet 正式基线。

quiet 正式基线完成 84/84 个 measurement、0 failure。完整运行的 Server 输出为 138,775 bytes，较 logging-enabled 的约 783 MiB 降低约 99.98%；剩余输出是客户端在 benchmark 时间边界断开时产生的 EOF、Broken pipe 和 connection reset 等错误诊断。PUT c=4 仍有明显双峰，三次为 3924.00、3856.04、2121.25 req/s，CV 为 30.96%，原始 repeat 均保留且没有择优重跑。PUT 阶段的 RSS 还受固定 workload 顺序及前序 GET 后 allocator 驻留影响，不应单独归因于日志模式。生成结果目录仍由 `.gitignore` 排除，不纳入源码提交。

wrk/hey 失败、非成功 HTTP 状态、socket error、Server 退出、资源采样失败、磁盘保护触发或 PUT 清理失败都会保留 raw output、写入 `failures.csv`，且不会生成看似正常的成功行。

## M10 Final Release Validation

最终验收运行保存在 `benchmarks/results/m10-final-release-20261023T185345Z/`，包含 56 份 client raw output、28 份 CPU/RSS sample、CSV 和 Markdown 汇总，`failures.csv` 只有表头。目标为独立 Release binary，4 workers、quiet 模式、warm-cache；覆盖 4 类 workload 和全部 1/4/8/16/32/64/128 并发档位。为控制交付验收耗时，本轮每档预热 2 秒、测量 5 秒、重复 1 次，因此与 M6-B 的 5/20/3 正式基线只作趋势比较，不把短测波动解释为架构收益。

代表性 c=16 结果：

| workload | req/s | throughput | mean latency | P99 | CPU | peak RSS |
|---|---:|---:|---:|---:|---:|---:|
| HEAD | 10348.34 | - | 1.50 ms | 3.40 ms | 45.40% | 4616 KiB |
| GET 4 KiB | 12154.13 | 48.17 MiB/s | 1.19 ms | 2.336 ms | 95.20% | 5504 KiB |
| GET 8 MiB | 271.97 | 2181.12 MiB/s | 58.11 ms | 100.266 ms | 83.79% | 5816 KiB |
| PUT 64 KiB | 1915.33 | response throughput 不代表上传吞吐 | 12.84 ms | 118.868 ms | 35.57% | 5024 KiB |

相同 c=16 下，M6-B quiet 基线的 req/s 分别为 9645.45、11045.57、217.23、2113.47；本轮前三项分别约 +7.3%、+10.0%、+25.2%，PUT 约 -9.4%。由于本轮时长和重复数更少，这些差异仅说明当前版本未出现明显整体性能回退；判断稳定性仍以保留的长时 M6-B 结果为准。

## Linux 资源观察

M6-A 不引入监控系统。需要手工观察连接失败后的资源回收时，可以使用：

```bash
pidof faststore
ls /proc/<pid>/fd | wc -l
grep VmRSS /proc/<pid>/status
```

临时连接关闭后，fd 数量应回落到接近启动基线。RSS 可能受 allocator 和 page cache 影响而波动，不要求逐次完全回落，但不应随重复失败场景近似线性增长。

## Windows 公网手工验证

生产源码和自动化测试不包含公网 IP；将下列 `<server-ip>` 替换为实际服务器地址。正常 PUT、GET 和失败后的存活检查可以使用 Windows 自带的 `curl.exe`：

```powershell
curl.exe -T .\source.bin http://<server-ip>:8080/files/manual.bin
curl.exe -o .\downloaded.bin http://<server-ip>:8080/files/manual.bin
curl.exe -I http://<server-ip>:8080/files/manual.bin
Get-FileHash .\source.bin -Algorithm SHA256
Get-FileHash .\downloaded.bin -Algorithm SHA256
```

可用 Python socket 制造 incomplete PUT：

```python
import socket
s = socket.create_connection(("<server-ip>", 8080))
s.sendall(b"PUT /files/incomplete.bin HTTP/1.1\r\nHost: test\r\nContent-Length: 100\r\n\r\npartial")
s.close()
```

可在较大 GET 尚未完成时中止 `curl.exe`，随后再次执行 HEAD 或小文件 PUT/GET，确认 Server 仍能正常服务。公网测试前应确认云防火墙和系统防火墙仅按需要开放端口。

## 当前限制

PUT 和 GET 都使用有界内存。GET 文件内容不会进入 `HttpResponse::body`，而是按以下基线链路传输：

```text
read one file chunk (64 KiB) -> send with partial-send offset -> next chunk
```

当前有意不使用 `sendfile`；普通 `read + send` 保留为后续性能测试的基线。regular-file `read()` 与 PUT `write()` 仍在 Reactor 线程同步执行，因此慢速存储可能阻塞 Reactor 并延迟无关连接。

GET 开始时固定本次响应的 `Content-Length`。外部追加的数据不会超过该边界发送；外部截断导致提前 EOF 时，本次传输失败并关闭连接。V0 不对活动传输期间被外部进程修改的文件提供强一致性或快照语义。

PUT 的逻辑大小上限为 1 GiB（1073741824 bytes）。Content-Length 等于 1 GiB 时协议层接受请求，大于 1 GiB 时返回 413 Payload Too Large。上传仍按现有 chunk 流式写入临时文件，不会把完整 request body 缓存在内存中；commit 前已经读取到的额外字节会使请求失败且不会写入最终文件。EAGAIN 只表示当前 socket receive queue 暂时无数据，不能证明客户端未来不会继续发送。

当前不支持 keep-alive、pipelining、chunked transfer encoding、range request、TLS、HTTP/2、认证或异步磁盘 I/O。M7-B 仍采用一个 Reactor 加简单固定线程池：M8 默认配置为 4 个 worker，并允许在启动时通过严格校验的 FASTSTORE_WORKERS 设置 1..256；task queue 没有容量上限，也不支持运行期动态扩缩、work stealing、priority、CPU/IO 分类、lock-free queue 或研究型调度算法。进程依赖 systemd 管理生命周期，尚未实现应用层优雅 SIGTERM drain。

V0 将文件名视为原始字节，请求 target 不做 URL decoding。`GET /files` 的 JSON 响应假定已存储的文件名是有效 UTF-8；非 UTF-8 文件名在 V0 中无法为 JSON 客户端可靠表示。

## Threading Model

### M7-A Fixed ThreadPool

ThreadPool 是独立的固定 worker 基础模块。构造参数固定 worker 数量，0 会抛出 std::invalid_argument；submit() 拒绝空任务，并在停止状态抛出 std::runtime_error。mutex 同时保护 task queue 与 stopping_，worker 通过带 predicate 的 condition-variable wait 应对 spurious wakeup，并在锁外执行任务。

析构和显式 shutdown() 都采用 graceful shutdown：停止接受新任务，唤醒 worker，继续执行所有已入队任务，queue 清空后退出并 join 全部 worker。任务异常会在 worker 内捕获并记录到 stderr，不会杀死 worker 或进程。

### M7-B Reactor + Worker integration

Server 拥有固定 ThreadPool、completion queue、queue mutex 和 non-blocking eventfd。请求解析完成后，Reactor 把 Connection 切换到 kWaitingForWorker，task 只捕获独立的 HttpRequest、client fd 和稳定 connection_id；worker 不捕获 Connection 指针/引用或 map iterator，也不调用 socket、epoll 或 Connection 状态修改方法。worker 执行 Router/FileService 后把 move-only RouteResult 推入 queue，再写 eventfd 唤醒 epoll。

Reactor drain eventfd 后短暂持锁，把当前 completions 移入本地 queue。应用结果前同时验证 fd 仍存在、connection_id 仍匹配且 Connection 仍在 waiting 状态，因此已关闭连接或被 Linux 复用 fd 的陈旧结果会被丢弃。Connection::applyRouteResult() 只在 Reactor 调用，之后沿用已有 response header 与 streaming GET 状态机。

Router 没有共享可变状态。FileService 只共享生命周期稳定、只读使用的数据目录 fd；metadata/open/remove/create 的每次调用都使用局部状态和线程安全的系统调用。目录列表使用 openat(directory_fd_, ".") 创建独立 open-file description，避免普通 dup 共享目录 offset 导致并发 readdir 相互干扰。

Streaming PUT 的 recv、临时文件写入和 commit 仍留在 Reactor，成功后直接生成原有 201 Created。GET 的业务 open 在 worker，但文件 chunk read、partial send 和 EPOLLOUT streaming 仍由 Reactor 执行。Server 析构先停止并 join worker，再关闭 Connection、completion eventfd、监听 socket 和 epoll，保证 task 捕获的 Server 资源仍存活。

固定线程池用于学习 Reactor/Worker 的职责分离，并不表示该小型项目需要复杂调度。本阶段有意不实现动态扩缩、priority、work stealing、CPU/IO 分类、lock-free queue、future/promise 或任务取消；任务也不得从 worker 内销毁其所属 ThreadPool。
