#ifndef FASTSTORE_THREAD_POOL_H
#define FASTSTORE_THREAD_POOL_H

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace faststore {

// 拥有固定数量 worker 的最小线程池。任务只通过 submit() 进入内部队列，
// queue、停止状态和 worker thread 都不向外暴露。
class ThreadPool {
public:
    explicit ThreadPool(std::size_t worker_count);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    void submit(std::function<void()> task);
    void shutdown() noexcept;

    [[nodiscard]] std::size_t workerCount() const noexcept;

private:
    void workerLoop();

    // workers_ 由 ThreadPool 唯一拥有，析构前必须全部 join。
    std::vector<std::thread> workers_;

    // mutex_ 同时保护 tasks_ 和 stopping_，使提交与停止形成明确顺序。
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool stopping_{false};
};

}  // namespace faststore

#endif  // FASTSTORE_THREAD_POOL_H
