#pragma once

#include <atomic>
#include <cassert>
#include <optional>
#include <mutex>
#include <type_traits>
#include <utility>

namespace nct {

template <typename T>
class AtomicQueue {
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

    std::optional<T> try_dequeue() noexcept(std::is_nothrow_move_constructible_v<T>) {
        std::lock_guard<std::mutex> _locker { m_mutex };
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

    std::optional<T> wait_dequeue() noexcept(std::is_nothrow_move_constructible_v<T>) {
        std::unique_lock<std::mutex> locker { m_mutex };
        Node *head = m_head.load(std::memory_order_acquire);
        Node *tail = m_tail.load(std::memory_order_acquire);
        Node *next = head->next.load(std::memory_order_relaxed);
        while (next == nullptr) {
            if (tail == nullptr) return std::nullopt;
            if (head == tail) {
                locker.unlock();
                m_tail.wait(tail);
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
    std::mutex m_mutex;
    std::atomic<Node*> m_tail;
    std::atomic<Node*> m_head;
};

}
