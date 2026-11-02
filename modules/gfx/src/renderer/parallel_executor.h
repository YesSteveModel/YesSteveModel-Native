#pragma once

#include <cstddef>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "cpu.h"

#ifdef YSM_ANDROID
#define YSM_EXECUTOR_NO_SPIN
#endif

namespace ysm::gfx::renderer {
// NOT thread-safe
class ParallelExecutor
{
public:
    static constexpr size_t kMinimumThreadCount = 2;
    static constexpr size_t kMaximumThreadCount = 8;

    using InitTask = std::function<void(size_t, size_t)>;
    using ActiveScope = std::unique_ptr<ParallelExecutor, void(*)(ParallelExecutor*)>;

    ParallelExecutor() : ParallelExecutor(0) {}

    explicit ParallelExecutor(const InitTask& initTask) : ParallelExecutor(0, initTask) {}

    explicit ParallelExecutor(const size_t size, const InitTask& initTask = nullptr)
    {
        const auto thread_count = DetermineThreadCount(size);
        threads_.reserve(thread_count - 1);
        for (size_t i = 1; i < thread_count; ++i)
        {
            threads_.emplace_back(&ParallelExecutor::Worker, this, i,
                                  thread_count, initTask);
        }
    }

    ParallelExecutor(const ParallelExecutor&) = delete;
    ParallelExecutor &operator=(const ParallelExecutor&) = delete;
    ParallelExecutor(ParallelExecutor&&) = delete;
    ParallelExecutor &operator=(ParallelExecutor&&) = delete;

    ~ParallelExecutor()
    {
        if (!threads_.empty())
        {
            workers_available_.store(false, std::memory_order_release);
#ifdef YSM_EXECUTOR_NO_SPIN
            task_seq_.fetch_add(1, std::memory_order_release);
            NotifyAll(task_seq_);
#else
            auto scope = BeginActiveScope();
#endif
            for (auto &thread : threads_)
            {
                thread.join();
            }
            threads_.clear();
        }
    }

    size_t GetThreadCount() const noexcept
    {
        return threads_.size() + 1;
    }

    static constexpr ActiveScope NopScope() noexcept {
        return ActiveScope{nullptr, [](auto){}};
    }

    ActiveScope BeginActiveScope() noexcept {
#ifdef YSM_EXECUTOR_NO_SPIN
        return {nullptr, [](auto) {}};
#else
        next_seq_.store(task_seq_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        NotifyAll(next_seq_);
        return {this, [](auto self) { self->EndActiveScope(); }};
#endif
    }

    void EndActiveScope() noexcept {
#ifndef YSM_EXECUTOR_NO_SPIN
        next_seq_.store(task_seq_.load(std::memory_order_relaxed), std::memory_order_release);
#endif
    }

    void Execute(const auto& taskFunc)
    {
        auto threadCount = GetThreadCount();

        // 派发任务
        Task task{
            [](auto idx, auto workers, auto ctx) { (*static_cast<decltype(&taskFunc)>(ctx))(idx, workers); },
            &taskFunc
        };
        currentTask = &task;
        running_workers_.fetch_add(static_cast<int32_t>(threadCount - 1), std::memory_order_relaxed);
        task_seq_.fetch_add(1, std::memory_order_release);              // sfence: 保护 currentTask 和 runningWorkers 的写入

#ifdef YSM_EXECUTOR_NO_SPIN
        NotifyAll(task_seq_);
#endif

        taskFunc(0, threadCount);

        // 等待其他 worker
#ifdef YSM_EXECUTOR_NO_SPIN
        for (auto v = threadCount; v != 0; v = running_workers_.load(std::memory_order_acquire))
        {
            WaitFor(running_workers_, v);
        }
#else
#ifndef ANDROID
        for (auto v = threadCount; v != 0; v = running_workers_.load(std::memory_order_acquire))
        {
            ysm_pause;
        }
#else
        for (volatile auto v = threadCount;;) {
            asm volatile(
                "ldaxr %0, [%1]"
                : "=r" (v)
                : "r" (&running_workers_)
                : "memory", "cc"
            );
            if (v > 0) {
                __wfe();
            } else {
                break;
            }
        }
#endif
#endif

        // 清理任务
        currentTask = nullptr;
    }

    static ParallelExecutor& Get() noexcept;

private:
    static size_t DetermineThreadCount(const size_t size) noexcept
    {
        return size < kMinimumThreadCount ? kMinimumThreadCount :
               size > kMaximumThreadCount ? kMaximumThreadCount :
               size;
    }

    void Worker(const size_t idx, const size_t workers, InitTask initTask)
    {
        if (initTask)
        {
            initTask(idx, workers);
            initTask = nullptr;
        }

        size_t localSeq = 0;
        while (workers_available_.load(std::memory_order_acquire))
        {
#ifdef YSM_EXECUTOR_NO_SPIN
            WaitFor(task_seq_, localSeq);
            auto id = task_seq_.load(std::memory_order_acquire);
#else /* YSM_EXECUTOR_NO_SPIN */
#ifndef YSM_ANDROID
            ysm_pause;
#endif /* YSM_ANDROID */
            WaitFor(next_seq_, localSeq);
#ifdef YSM_ANDROID
            volatile size_t id;
            {
                asm volatile(
                        "ldaxr %0, [%1]"
                        : "=r" (id)
                        : "r" (&task_seq_)
                        : "cc", "memory"
                    );
                if (id <= localSeq) {
                    __wfe();
                    continue;
                }
            }
#else /* YSM_ANDROID */
            auto id = task_seq_.load(std::memory_order_acquire);
#endif /* YSM_ANDROID */
#endif /* YSM_EXECUTOR_NO_SPIN */
            if (id > localSeq) [[likely]]
            {
                if (auto task = currentTask; task) [[likely]]
                {
                    localSeq = id;
                    task->func(idx, workers, task->ctx);
                    if (running_workers_.fetch_sub(1, std::memory_order_release) == 1)
                    {
#ifdef YSM_EXECUTOR_NO_SPIN
                        NotifyOne(running_workers_);
#endif
                    }
                }
            }
        }
    }

    static void NotifyOne(std::atomic_size_t &value) noexcept;
    static void NotifyAll(std::atomic_size_t &value) noexcept;
    static void WaitFor(std::atomic_size_t &value, size_t v) noexcept;

    struct Task {
        void(*func)(size_t, size_t, const void*);
        const void* ctx;
    };

    std::vector<std::thread> threads_;
    std::atomic_bool workers_available_ = true;

    Task* currentTask = nullptr;
    alignas(64) std::atomic_size_t task_seq_ = 0;
#ifndef YSM_EXECUTOR_NO_SPIN
    alignas(64) std::atomic_size_t next_seq_ = 0;
#endif
    alignas(64) std::atomic_size_t running_workers_ = 0;
};
}

#ifdef YSM_EXECUTOR_NO_SPIN
#undef YSM_EXECUTOR_NO_SPIN
#endif
