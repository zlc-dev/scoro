#pragma once

#include <atomic>
#include <cassert>
#include <memory>
#include <utility>

template<typename T>
class intrusive_ptr {
public:

    intrusive_ptr() = default;

    intrusive_ptr(T* ptr): m_ptr(ptr) {
        if(m_ptr) intrusive_ptr_add_ref(m_ptr);
    }

    intrusive_ptr(const intrusive_ptr& oth): intrusive_ptr(oth.m_ptr) {}

    template<typename U>
    intrusive_ptr(const intrusive_ptr<U>& oth): intrusive_ptr(static_cast<T*>(oth.get())) {}

    intrusive_ptr(intrusive_ptr&& oth): m_ptr(std::exchange(oth.m_ptr, nullptr)) {}
    
    template<typename U>
    intrusive_ptr(intrusive_ptr<U>&& oth): m_ptr(static_cast<T*>(oth.get())) {
        oth.reset();
    }
 
    ~intrusive_ptr() {
        if(m_ptr) intrusive_ptr_release(m_ptr);
        m_ptr = nullptr;
    }
    intrusive_ptr & operator=(T* rhs)
    {
        intrusive_ptr(rhs).swap(*this);
        return *this;
    }

    void reset() {
        intrusive_ptr().swap( *this );
    }

    void reset( T * rhs ) {
        intrusive_ptr(rhs).swap( *this );
    }

    T* get() const {
        return m_ptr;
    }

    const T& operator*() const noexcept {
        assert(m_ptr != nullptr);
        return *m_ptr;
    }

    const T* operator->() const noexcept {
        assert(m_ptr != nullptr);
        return m_ptr;
    }

    T& operator*() noexcept {
        assert(m_ptr != nullptr);
        return *m_ptr;
    }

    T* operator->() noexcept {
        assert(m_ptr != nullptr);
        return m_ptr;
    }

    void swap(intrusive_ptr& rhs)
    {
        T * tmp = m_ptr;
        m_ptr = rhs.m_ptr;
        rhs.m_ptr = tmp;
    }

private:
    T* m_ptr { nullptr };
};

template<class T, class U> inline bool operator==(intrusive_ptr<T> const & a, intrusive_ptr<U> const & b)
{
    return a.get() == b.get();
}

template<class T, class U> inline bool operator!=(intrusive_ptr<T> const & a, intrusive_ptr<U> const & b)
{
    return a.get() != b.get();
}

template<class T, class U> inline bool operator==(intrusive_ptr<T> const & a, U * b)
{
    return a.get() == b;
}

template<class T, class U> inline bool operator!=(intrusive_ptr<T> const & a, U * b)
{
    return a.get() != b;
}

template<class T, class U> inline bool operator==(T * a, intrusive_ptr<U> const & b)
{
    return a == b.get();
}

template<class T, class U> inline bool operator!=(T * a, intrusive_ptr<U> const & b)
{
    return a != b.get();
}


template<class T> T * get_pointer(intrusive_ptr<T> const & p)
{
    return p.get();
}

template<class T, class U> intrusive_ptr<T> static_pointer_cast(intrusive_ptr<U> const & p)
{
    return static_cast<T *>(p.get());
}

template<class T, class U> intrusive_ptr<T> const_pointer_cast(intrusive_ptr<U> const & p)
{
    return const_cast<T *>(p.get());
}

template<class T, class U> intrusive_ptr<T> dynamic_pointer_cast(intrusive_ptr<U> const & p)
{
    return dynamic_cast<T *>(p.get());
}

template<typename T, typename... Args>
intrusive_ptr<T> make_intrusive(Args&&... args) {
    return intrusive_ptr<T> { new T { std::forward<Args>(args)... } };
}

template<class T, class Counter = unsigned long, typename Deleter = std::default_delete<T>>
class intrusive_ref_counter
{
protected:
    Counter m_ref_count;

public:
    intrusive_ref_counter() : m_ref_count(0) {}

    friend inline void intrusive_ptr_add_ref(T* p) noexcept
    {
        ++p->m_ref_count;
    }

    friend inline void intrusive_ptr_release(T* p) noexcept
    {
        if(--p->m_ref_count == 0)
            Deleter()(p);
    }
};

template<typename T, typename Counter = std::atomic_ulong, typename Deleter = std::default_delete<T>>
class intrusive_ref_counter_mt
{
protected:
    Counter m_ref_count;

public:
    intrusive_ref_counter_mt() : m_ref_count(0) {}

    friend inline void intrusive_ptr_add_ref(T* p) noexcept
    {
        std::atomic_fetch_add_explicit(&p->m_ref_count, 1, std::memory_order_relaxed);
    }

    friend inline void intrusive_ptr_release(T* p) noexcept
    {
        if(std::atomic_fetch_sub_explicit(&p->m_ref_count, 1, std::memory_order_acq_rel) == 1) {
            Deleter()(p);
        }
    }
};

