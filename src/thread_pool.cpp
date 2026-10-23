#include "faststore/thread_pool.h"

#include <iostream>
#include <stdexcept>
#include <utility>

namespace faststore {

ThreadPool::ThreadPool(std::size_t worker_count) {
    if (worker_count == 0U) {
        throw std::invalid_argument(
            "ThreadPool requires at least one worker");
    }

    workers_.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    } catch (...) {
        // thread 创建到一半失败时，先让已经启动的 worker 退出，再 join。
        // 否则 vector 析构 joinable thread 会调用 std::terminate。
        shutdown();
        throw;
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

void ThreadPool::shutdown() noexcept {
    {
        std::lock_guard<std::mutex> lock{mutex_};
        // stopping_ 禁止新的任务进入，但 worker 仍会排空已有 queue。
        stopping_ = true;
    }
    condition_.notify_all();

    // join 保证所有已排队任务完成，并且没有 worker 再访问成员后，
    // ThreadPool 才开始销毁 queue、mutex 和 condition variable。
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::submit(std::function<void()> task) {
    if (!task) {
        throw std::invalid_argument("ThreadPool cannot submit an empty task");
    }

    {
        std::lock_guard<std::mutex> lock{mutex_};
        if (stopping_) {
            throw std::runtime_error("ThreadPool is stopping");
        }
        tasks_.push(std::move(task));
    }

    // queue 更新完成后再唤醒一个 worker；无需在持锁时 notify。
    condition_.notify_one();
}

std::size_t ThreadPool::workerCount() const noexcept {
    // workers_ 只在构造期间改变，构造完成后大小固定，无需加锁。
    return workers_.size();
}

void ThreadPool::workerLoop() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock{mutex_};
            // wait 可能发生 spurious wakeup，predicate 每次重新检查共享状态。
            condition_.wait(lock, [this] {
                return stopping_ || !tasks_.empty();
            });

            // graceful shutdown：停止后仍先处理 queue，只有 queue 空才退出。
            if (stopping_ && tasks_.empty()) {
                return;
            }

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        // task 必须在锁外执行，否则慢任务会阻止 submit 和其他 worker 取任务。
        // 单个 task 的异常不能逃出 thread entry，否则会终止整个进程。
        try {
            task();
        } catch (const std::exception& error) {
            std::cerr << "ThreadPool task failed: " << error.what() << '\n';
        } catch (...) {
            std::cerr << "ThreadPool task failed: unknown exception\n";
        }
    }
}

}  // namespace faststore
