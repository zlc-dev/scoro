#pragma once

#include "container/queue.hpp"
#include "ptr/intrusive_ptr.hpp"
#include <atomic>
#include <coroutine>
#include <mutex>
#include <optional>
#include <print>
namespace coro {

struct CountingSemaphore {
public:

    struct AcquireAwaitable {
    public:
        AcquireAwaitable(CountingSemaphore& sem): m_semaphore(sem) {}

        bool await_ready() const noexcept {
            return false;
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            return m_semaphore.acquire_helper(handle);
        }

        void await_resume() const noexcept {}

    private:
        CountingSemaphore& m_semaphore;
    };

    struct Counter {
        int count;
        unsigned waiters;
    };

    explicit CountingSemaphore(int init)
    : m_count(Counter { init, 0 }), m_waiters() 
    {}

    CountingSemaphore(): CountingSemaphore(0) {} 

    ~CountingSemaphore() {
        while(auto waiter = m_waiters.try_dequeue( std::lock_guard { m_mutex } )) {
            waiter->destroy();
        }
    }

    void release(int count = 1) {
        Counter new_counter, counter = m_count.load(std::memory_order_relaxed);
        unsigned need_wake;
        do {
            new_counter = counter;
            new_counter.count += count;
            need_wake = count > 0 ? 
                std::min(static_cast<unsigned>(count), counter.waiters) : 0;
            new_counter.waiters -= need_wake;
        } while (!m_count.compare_exchange_weak(
            counter, new_counter,
            std::memory_order_acq_rel, std::memory_order_relaxed
        ));
        while(need_wake-- > 0) {
            if (auto waiter = m_waiters.wait_dequeue(std::unique_lock { m_mutex })) {
                waiter->resume();
            } else {
                return;
            }
        }
    }

    [[nodiscard]] AcquireAwaitable acquire() {
        return AcquireAwaitable { *this };
    }

    bool try_acquire() {
        Counter new_counter, counter = m_count.load(std::memory_order_relaxed);
        bool success;
        do {
            new_counter = counter;
            if ((success = (new_counter.count > 0)))
                new_counter.count--;
        } while(!m_count.compare_exchange_weak(
            counter, new_counter,
            std::memory_order_acq_rel, std::memory_order_relaxed
        ));
        return success;
    }

private:
    
    bool acquire_helper(std::coroutine_handle<> handle) {
        Counter new_counter, counter = m_count.load(std::memory_order_relaxed);
        bool need_wait;
        do {
            new_counter = counter;
            if ((need_wait = (--new_counter.count < 0)))
                new_counter.waiters += 1;
        } while(!m_count.compare_exchange_weak(
            counter, new_counter,
            std::memory_order_acq_rel, std::memory_order_relaxed
        ));
        if (need_wait) {
            m_waiters.enqueue(handle);
            return true;
        }
        return false;
    }

private:
    std::atomic<Counter> m_count;
    std::mutex m_mutex;
    nct::AtomicQueue<std::coroutine_handle<>> m_waiters;
};

struct Mutex {
public:

    Mutex(): m_sem(1) {}

    auto lock() {
        return m_sem.acquire();
    }

    auto try_lock() {
        return m_sem.try_acquire();
    }

    auto unlock() {
        return m_sem.release();
    }

private:
    CountingSemaphore m_sem;
};

}
