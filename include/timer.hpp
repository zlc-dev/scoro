#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <print>
#include <queue>
#include <vector>

using callback_t = void(*)(void* arg);

template<typename Clock>
class Timer {
public:
    struct Task {
        std::chrono::time_point<Clock> expire_time;
        callback_t callback;
        void* arg;
        callback_t cancel;
    };

    Timer() = default;
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    void add_task(const std::chrono::time_point<Clock>& expire_time, callback_t cb, void* arg = nullptr, callback_t cancel = nullptr) {
        std::lock_guard<std::mutex> _locker {m_mutex};
        m_tasks.push(Task{expire_time, cb, arg, cancel});
        m_cv.notify_one();
    }

    template<typename Rep, typename Period>
    void add_task_after(const std::chrono::duration<Rep, Period>& delay, callback_t cb, void* arg = nullptr, callback_t cancel = nullptr) {
        add_task(Clock::now() + delay, cb, arg, cancel);
    }

    void run() {
        std::unique_lock<std::mutex> locker { m_mutex };
        while (!m_stop.load(std::memory_order_acquire)) {
            while (!m_tasks.empty() && m_tasks.top().expire_time <= Clock::now()) {
                Task task = m_tasks.top();
                m_tasks.pop();
                locker.unlock();
                task.callback(task.arg);
                locker.lock();
            }

            if (m_tasks.empty()) {
                m_cv.wait(locker);
            } else if (m_tasks.top().expire_time > Clock::now()) {
                m_cv.wait_until(locker, m_tasks.top().expire_time);
            }
        }
    }

    void stop() {
        std::lock_guard<std::mutex> _locker { m_mutex };
        while (!m_tasks.empty()) {
            if(m_tasks.top().cancel) m_tasks.top().cancel(m_tasks.top().arg);
            m_tasks.pop();
        }
        m_stop.store(true, std::memory_order_release);
        m_cv.notify_all();
    }

private:

    struct TaskCompare {
        bool operator()(const Task& lhs, const Task& rhs) const noexcept {
            return lhs.expire_time > rhs.expire_time;
        }
    };

    std::priority_queue<Task, std::vector<Task>, TaskCompare> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic_bool m_stop;
};


template<typename Clock>
class TimerRunner {
public:
    TimerRunner(): m_timer(), m_runner([this](){ m_timer.run(); }) {}
    
    ~TimerRunner() {
        stop();
        if (m_runner.joinable())
            m_runner.join();
    }

    void add_task(const std::chrono::time_point<Clock>& expire_time, callback_t cb, void* arg = nullptr, callback_t cancel = nullptr) {
        m_timer.add_task(expire_time, cb, arg, cancel);
    }

    template<typename Rep, typename Period>
    void add_task_after(const std::chrono::duration<Rep, Period>& delay, callback_t cb, void* arg = nullptr, callback_t cancel = nullptr) {
        add_task(Clock::now() + delay, cb, arg, cancel);
    }

    void stop() {
        m_timer.stop();
    }

private:
    Timer<Clock> m_timer;
    std::thread m_runner;
};

template<typename Clock>
inline TimerRunner<Clock>& get_running_timer() {
    static TimerRunner<Clock> timer_runner {};
    return timer_runner;
}
