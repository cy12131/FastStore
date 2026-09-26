# FastStore

FastStore 是一个使用 C++17 开发、运行于 Linux 的 HTTP 文件服务器项目。

## V0 目标

V0 是基于 non-blocking socket、epoll LT 和单线程事件循环的最小 HTTP 文件服务器，支持通过 PUT、GET、DELETE 和 HEAD 操作 `data/` 目录中的普通文件。

## 当前阶段

当前完成 Milestone 6-C：在不改变单 Reactor、HTTP 或文件处理行为的前提下增加显式 benchmark quiet logging 控制，并建立最终 Release baseline。M6-A 健壮性验证和 M6-B benchmark 基础设施继续保留。

当前接口行为：

- PUT 将请求体流式写入临时文件，成功后提交并返回 `201 Created`。
- GET 返回精确的 `Content-Length`，随后使用单个固定 64 KiB 用户态缓冲区流式发送文件。
- HEAD 只读取 metadata、返回与 GET 一致的长度，不打开或发送文件体。
- DELETE 删除普通文件并返回 `204 No Content`。
- 每个连接只处理一个 HTTP/1.1 请求，response 发送完成后关闭。

## 构建与测试

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
ctest --output-on-failure
```

未指定构建类型时，项目默认使用 Debug 模式，并启用 `-Wall -Wextra -Wpedantic`。

## 运行

在 `build/` 目录中执行：

```bash
./faststore
```

服务器监听 `0.0.0.0:8080`，可使用 `Ctrl+C` 结束。

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

准备数据并显式启动 Server。普通执行仍保留全部信息日志；只有精确设置 `FASTSTORE_QUIET=1` 才抑制 accept、recv、正常 response 完成和正常 close 日志，错误日志始终保留。最终正式基线使用独立 Release 构建与显式 quiet 模式：

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

PUT 的逻辑大小上限为 64 MiB。`Content-Length` 定义 PUT body 边界，commit 前已经读取到的额外字节会使请求失败且不会写入最终文件。`EAGAIN` 只表示当前 socket receive queue 暂时无数据，不能证明客户端未来不会继续发送。

当前不支持 keep-alive、pipelining、chunked transfer encoding、range request、TLS、HTTP/2、认证、ThreadPool 或异步磁盘 I/O。
