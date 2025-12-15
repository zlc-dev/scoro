#pragma once

#include "waitable_atomic.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <optional>
#include <type_traits>
#include <utility>

namespace nct {

template <typename T>
class AtomicQueue {
private:
    struct FakeLocker {
        void lock() {}
        void unlock() {}
    };
public:
    AtomicQueue() {
        auto *stub = new Node();
        m_head.store(stub, std::memory_order_relaxed);
        m_tail.store(stub, std::memory_order_relaxed);
    }

    ~AtomicQueue() {
        stop();
        while (try_dequeue()) { /* drain */ }
        Node *stub = m_head.exchange(nullptr);
        delete stub;
    }

    AtomicQueue(const AtomicQueue&) = delete;
    AtomicQueue(AtomicQueue&&) = delete;
    AtomicQueue& operator=(const AtomicQueue&) = delete;
    AtomicQueue& operator=(AtomicQueue&&) = delete;

    template <typename... Args>
    bool enqueue(Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) {
        Node *n = new(std::nothrow) Node(std::in_place_type<T>, std::forward<Args>(args)...);
        if (!n) { 
            return false; 
        }
        Node *tail = m_tail.load(std::memory_order_relaxed);
        do {
            if (tail == nullptr) {
                n->value.~T();
                delete n;
                return false;
            }  
        } while(!m_tail.compare_exchange_weak(
            tail, n, 
            std::memory_order_release, std::memory_order_relaxed
        ));
        tail->next.store(n, std::memory_order_release);
        m_tail.notify_one();
        return true;
    }

    template<typename Locker>
    std::optional<T> try_dequeue(Locker locker) noexcept(std::is_nothrow_move_constructible_v<T>) {
        Node *head = m_head.load(std::memory_order_acquire);
        Node *next = head->next.load(std::memory_order_relaxed);
        if (next == nullptr)
            return std::nullopt;
        m_head.store(next, std::memory_order_release);
        delete head;
        std::optional<T> result(std::move(next->value));
        next->value.~T();
        return result;
    }

    std::optional<T> try_dequeue() noexcept(std::is_nothrow_move_constructible_v<T>) {
        return try_dequeue(FakeLocker {});
    }

    template<typename Locker>
    std::optional<T> wait_dequeue(Locker locker) noexcept(std::is_nothrow_move_constructible_v<T>) {
        return wait_dequeue_helper<false>(std::chrono::time_point<std::chrono::steady_clock>{}, std::move(locker));
    }

    std::optional<T> wait_dequeue() noexcept(std::is_nothrow_move_constructible_v<T>) {
        return wait_dequeue(FakeLocker {});
    }

    template <typename Clock, typename Dur, typename Locker>
    std::optional<T> wait_dequeue_until(
        const std::chrono::time_point<Clock, Dur>& atime,
        Locker locker
    ) noexcept(std::is_nothrow_move_constructible_v<T>) 
    {
        return wait_dequeue_helper<true>(atime, std::move(locker));
    }

    template <typename Clock, typename Dur>
    std::optional<T> wait_dequeue_until(
        const std::chrono::time_point<Clock, Dur>& atime
    ) noexcept(std::is_nothrow_move_constructible_v<T>) 
    {
        return wait_dequeue_until(atime, FakeLocker {});
    }

    template <typename Clock = std::chrono::steady_clock, typename Rep, typename Period, typename Locker>
    std::optional<T> wait_dequeue_for(
        const std::chrono::duration<Rep, Period>& dur,
        Locker locker
    ) noexcept(std::is_nothrow_move_constructible_v<T>) 
    {
        return wait_dequeue_until(std::move(locker), Clock::now() + dur);
    }

    template <typename Clock = std::chrono::steady_clock, typename Rep, typename Period>
    std::optional<T> wait_dequeue_for(
        const std::chrono::duration<Rep, Period>& dur
    ) noexcept(std::is_nothrow_move_constructible_v<T>) 
    {
        return wait_dequeue_for(dur, FakeLocker {});
    }

    /**
    * @brief check empty
    * 
    * @return false - Guaranteed non-empty
    * @return true  - Possibly empty (may include false positives)
    * 
    * @note Thread-safe but may spuriously report empty on non-empty queues.
    *       Never incorrectly reports non-empty on actually empty queues.
    */
    bool maybe_empty() const noexcept {
        Node *head = m_head.load(std::memory_order_relaxed);
        return !head->next;
    }

    void stop() noexcept {
        m_tail.store(nullptr, std::memory_order_release);
        m_tail.notify_all();
    }

private:

    template<bool Timeout, typename Clock, typename Dur,  typename Locker>
    std::optional<T> wait_dequeue_helper(
        const std::chrono::time_point<Clock, Dur>& atime,
        Locker locker
    ) noexcept(std::is_nothrow_move_constructible_v<T>) {
        Node *head = m_head.load(std::memory_order_acquire);
        Node *tail = m_tail.load(std::memory_order_acquire);
        Node *next = head->next.load(std::memory_order_relaxed);
        while (next == nullptr) {
            if (tail == nullptr) return std::nullopt;
            if (head == tail) {
                locker.unlock();
                if constexpr (Timeout) {
                    bool success = m_tail.wait_until(tail, atime);
                    if (!success) return std::nullopt;
                } else {
                    m_tail.wait(tail);
                }
                locker.lock();
                head = m_head.load(std::memory_order_acquire);
                next = head->next.load(std::memory_order_relaxed);
            }
            tail = m_tail.load(std::memory_order_acquire);
        }
        m_head.store(next, std::memory_order_release);
        delete head;
        std::optional<T> result(std::move(next->value));
        next->value.~T();
        return result;
    }

    struct Node {
        std::atomic<Node*> next;
        union {
            T value;
        };

        Node() noexcept : next(nullptr) {}

        template<typename... Args>
        explicit Node(std::in_place_type_t<T>, Args&&... args) noexcept(std::is_nothrow_constructible_v<T, Args...>) 
            : next(nullptr), value{ std::forward<Args>(args)... } {}

        ~Node() {}
    };

private:
    waitable_atomic<Node*> m_tail;
    std::atomic<Node*> m_head;
};

}
