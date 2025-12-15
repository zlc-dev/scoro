#pragma once

#include <atomic>
#include <utility>
#include <cassert>

template<class T>
struct RcBox {
    std::atomic<size_t> refcnt;
    T value;

    template<class... Args>
    explicit RcBox(Args&&... args)
        : refcnt(1), value(std::forward<Args>(args)...)
    {}
};

template<class T>
class rc_ptr {
private:

    explicit rc_ptr(RcBox<T>* box) noexcept
        : m_box(box)
    {}

    template<class Tp, typename ... Args>
    friend rc_ptr<Tp> make_rc(Args&&... args);

public:
    rc_ptr() noexcept = default;

    rc_ptr(const rc_ptr& other) noexcept
        : m_box(other.m_box)
    {
        inc();
    }

    rc_ptr(rc_ptr&& other) noexcept
        : m_box(std::exchange(other.m_box, nullptr))
    {}

    rc_ptr& operator=(const rc_ptr& other) noexcept {
        if (this != &other) {
            dec();
            m_box = other.m_box;
            inc();
        }
        return *this;
    }

    rc_ptr& operator=(rc_ptr&& other) noexcept {
        if (this != &other) {
            dec();
            m_box = std::exchange(other.m_box, nullptr);
        }
        return *this;
    }

    ~rc_ptr() {
        dec();
    }

    T* get() const noexcept {
        return m_box ? &m_box->value : nullptr;
    }

    inline static rc_ptr<T> from_address(void *a) noexcept {
        assert(a);
        return rc_ptr<T>(static_cast<RcBox<T>*>(a));
    }

    void* get_address() const noexcept {
        return m_box;
    }

    T& operator*() const noexcept {
        assert(m_box);
        return m_box->value;
    }

    T* operator->() const noexcept {
        return &**this;
    }

    explicit operator bool() const noexcept {
        return m_box != nullptr;
    }

    size_t use_count() const noexcept {
        return m_box ? m_box->refcnt.load(std::memory_order_relaxed) : 0;
    }

private:
    void inc() noexcept {
        if (m_box) {
            m_box->refcnt.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void dec() noexcept {
        if (m_box && m_box->refcnt.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete m_box;
        }
        m_box = nullptr;
    }

private:
    RcBox<T>* m_box = nullptr;
};


template<class Tp, typename ... Args>
rc_ptr<Tp> make_rc(Args&&... args) {
    return rc_ptr<Tp>(
        new RcBox<Tp>(std::forward<Args>(args)...)
    );
}
