#pragma once

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <print>

template<typename T, typename Allocator = std::allocator<T>>
class AtomicQueue {
public:
    struct Node {
        alignas(alignof(T)) std::byte payload[sizeof(T)];
        Node* next { nullptr };
        std::atomic_int dec_count { 0 };
    };

    struct NodePtr {
        Node* node;
        int ref_count;
    };

    AtomicQueue() : m_head(NodePtr { new Node(), 0 }), m_tail(m_head.load().node) {}

    template<typename... Args>
    void push(Args&&... args) {
        Node* new_node = new Node();
        new (new_node->payload) T ( std::forward<Args>(args)... );
        Node* tail = m_tail.load(std::memory_order_acquire);
        for(;;) {
            if(m_tail.compare_exchange_strong(
                tail, new_node,
                std::memory_order_acq_rel, std::memory_order_relaxed
            ))
            {
                tail->next = new_node;
                break;
            }
        }
    }

    std::optional<T> pop() {
        NodePtr head = m_head.load(std::memory_order_acquire);
        
        std::optional<T> ret {};
        for(;;) {
            if(!m_head.compare_exchange_strong(
                head, NodePtr { head.node, head.ref_count + 1 },
                std::memory_order_acq_rel, std::memory_order_acquire
            )) {
                continue;
            }

            if(head.node->next == nullptr) {
                if (head.node->dec_count.fetch_sub(1) == 1) {
                    delete head.node;
                }
                break;
            }

            head.ref_count++;

            if(m_head.compare_exchange_strong(
                head, NodePtr { head.node->next, 1 },
                std::memory_order_acq_rel, std::memory_order_acquire
            )) {
                ret.emplace(std::move(*reinterpret_cast<T*>(head.node->next->payload)));
                reinterpret_cast<T*>(head.node->next->payload)->~T();
                if (head.node->next->dec_count.fetch_sub(1) == 1) {
                    delete head.node->next;
                }
                int increase_count = head.ref_count - 1;
                if (head.node->dec_count.fetch_add(increase_count) == -increase_count) {
                    delete head.node;
                }
                break;
            } else {
                if (head.node->dec_count.fetch_sub(1) == 1) {
                    delete head.node;
                }
            }
        }
        return std::move(ret);
    }

private:
    std::atomic<NodePtr> m_head;
    std::atomic<Node*> m_tail;
};

