#include "faststore/thread_pool.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using faststore::ThreadPool;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

void testWorkerCount() {
    ThreadPool pool{4};
    expect(pool.workerCount() == 4U, "worker count is not fixed at four");
}

void testBasicExecution() {
    std::atomic<std::size_t> counter{0};
    {
        ThreadPool pool{4};
        for (std::size_t index = 0; index < 1000U; ++index) {
            pool.submit([&counter] {
                counter.fetch_add(1U, std::memory_order_relaxed);
            });
        }
    }
    expect(counter.load(std::memory_order_relaxed) == 1000U,
           "not every submitted task executed");
}

void testMultipleWorkers() {
    constexpr std::size_t worker_count = 4U;
    std::mutex mutex;
    std::condition_variable condition;
    std::set<std::thread::id> worker_ids;
    std::size_t ready_workers = 0;
    bool release_workers = false;

    bool all_workers_started = false;
    {
        ThreadPool pool{worker_count};
        for (std::size_t index = 0; index < worker_count; ++index) {
            pool.submit([&] {
                std::unique_lock<std::mutex> lock{mutex};
                worker_ids.insert(std::this_thread::get_id());
                ++ready_workers;
                condition.notify_all();
                condition.wait(lock, [&release_workers] {
                    return release_workers;
                });
            });
        }

        {
            std::unique_lock<std::mutex> lock{mutex};
            all_workers_started = condition.wait_for(
                lock, std::chrono::seconds{5}, [&ready_workers] {
                    return ready_workers == worker_count;
                });
            release_workers = true;
        }
        condition.notify_all();
    }

    expect(all_workers_started, "all workers did not begin executing tasks");
    expect(worker_ids.size() == worker_count,
           "tasks did not execute on four distinct worker threads");
    expect(worker_ids.count(std::this_thread::get_id()) == 0U,
           "a task unexpectedly executed on the submitting thread");
}

void testGracefulShutdown() {
    std::atomic<std::size_t> completed{0};
    {
        ThreadPool pool{3};
        for (std::size_t index = 0; index < 60U; ++index) {
            pool.submit([&completed] {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
                completed.fetch_add(1U, std::memory_order_relaxed);
            });
        }
    }
    expect(completed.load(std::memory_order_relaxed) == 60U,
           "destructor returned before queued tasks completed");
}

void testExplicitShutdownIsIdempotent() {
    std::atomic<bool> completed{false};
    ThreadPool pool{1};
    pool.submit([&completed] {
        completed.store(true, std::memory_order_relaxed);
    });
    pool.shutdown();
    pool.shutdown();
    expect(completed.load(std::memory_order_relaxed),
           "explicit shutdown did not drain queued work");

    bool rejected = false;
    try {
        pool.submit([] {});
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "submit after shutdown was accepted");
}

void testTaskExceptionDoesNotStopWorker() {
    std::atomic<bool> later_task_ran{false};
    {
        ThreadPool pool{1};
        pool.submit([] { throw std::runtime_error("expected test error"); });
        pool.submit([&later_task_ran] {
            later_task_ran.store(true, std::memory_order_relaxed);
        });
    }
    expect(later_task_ran.load(std::memory_order_relaxed),
           "task exception prevented a later task from running");
}

void testZeroWorkersRejected() {
    bool threw = false;
    try {
        ThreadPool pool{0};
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "zero-worker ThreadPool was accepted");
}

void testEmptyTaskRejected() {
    ThreadPool pool{1};
    bool threw = false;
    try {
        pool.submit({});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "empty task was accepted");
}

struct TestCase {
    std::string_view name;
    void (*function)();
};

}  // namespace

int main() {
    const std::vector<TestCase> tests{
        {"worker count", testWorkerCount},
        {"basic execution", testBasicExecution},
        {"multiple workers", testMultipleWorkers},
        {"graceful shutdown", testGracefulShutdown},
        {"explicit idempotent shutdown", testExplicitShutdownIsIdempotent},
        {"task exception", testTaskExceptionDoesNotStopWorker},
        {"zero workers", testZeroWorkersRejected},
        {"empty task", testEmptyTaskRejected},
    };

    std::size_t passed = 0;
    for (const TestCase& test : tests) {
        try {
            test.function();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": "
                      << error.what() << '\n';
            return 1;
        }
    }

    std::cout << passed << " thread pool test groups passed\n";
    return 0;
}
