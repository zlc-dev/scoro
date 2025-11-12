#pragma once

#include <atomic>
#include <coroutine>
#include <exception>
#include <iostream>
#include <print>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>

namespace coro {

class LooperScheduler {
public:

    LooperScheduler()
        : m_tasks{}, m_mutex{}, m_cv{}, 
        m_stop{ false }, m_worker([this] { run(); }) 
    {}

    ~LooperScheduler() {
        stop();
        if (m_worker.joinable()) m_worker.join();
    }

    void submit(std::coroutine_handle<> handle) {
        std::lock_guard<std::mutex> _locker { m_mutex };
        if (m_stop.load(std::memory_order_acquire))
            return;
        m_tasks.push(handle);
        m_cv.notify_one();
    }

    void stop() {
        std::lock_guard<std::mutex> _locker { m_mutex };
        m_stop.store(true, std::memory_order_release);
        while (!m_tasks.empty()) {
            m_tasks.front().destroy();
            m_tasks.pop();
        }
        m_cv.notify_all();
    }

private:

    void run() {
        std::unique_lock<std::mutex> locker { m_mutex };
        while (!m_stop.load(std::memory_order_acquire)) {
            while (!m_tasks.empty()) {
                std::coroutine_handle<> task = m_tasks.front();
                m_tasks.pop();
                locker.unlock();
                try {
                    task.resume();
                } catch (const std::exception& e) {
                    std::println(std::cerr, "unhandled exception: {}", e.what());
                } catch (...) {
                    std::println(std::cerr, "unhandled unknown exception");
                }
                locker.lock();
            }
            m_cv.wait(locker);
        }
    }

private:
    std::queue<std::coroutine_handle<>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic_bool m_stop;
    std::thread m_worker;
};

template<typename Scheduler>
class SchedulerWrapper: public Scheduler {
private:
    
    struct SchedAwaiter {
        bool await_ready() {
            return false;
        }

        void await_suspend(std::coroutine_handle<> handle) {
            if (!handle.done())
                m_scheduler.submit(handle);
        }

        void await_resume() {}

        SchedulerWrapper<Scheduler>& m_scheduler;
    };

public:
    using Scheduler::Scheduler;

    SchedAwaiter sched() {
        return SchedAwaiter { *this };
    }

};


inline SchedulerWrapper<LooperScheduler>& get_global_scheduler() {
    static SchedulerWrapper<LooperScheduler> instance {};
    return instance;
}


} // namespace coro
