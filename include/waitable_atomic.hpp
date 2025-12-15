#pragma once
#include "detail/meta.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sys/types.h>
#include <type_traits>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>

constexpr unsigned SPIN_TIMES = 1000;

#elif defined(_WIN32)
#pragma comment(lib, "Synchronization.lib")
#include <windows.h>
#include <synchapi.h>

constexpr unsigned SPIN_TIMES = 1000;

#else
#include <condition_variable>
#include <mutex>

constexpr unsigned SPIN_TIMES = 0;
#endif

namespace detail {

template<typename T>
requires (std::is_trivial_v<T> && sizeof(T) <= 8)
class waitable_atomic_base {
public:
    waitable_atomic_base(T init = 0) noexcept
        : m_value(init) {}

    waitable_atomic_base(waitable_atomic_base&&) = delete;

    inline void wait(T expected, std::memory_order memory_order = std::memory_order_seq_cst) const noexcept {
        unsigned spins = SPIN_TIMES;
        while (spins-- > 0) {
            if (load(memory_order) != expected) 
                return;
        }
        while (load(memory_order) == expected) {
            wait_impl(expected);
        }
    }

    template <typename Clock, typename Dur>
    bool wait_until(
        T expected,
        const std::chrono::time_point<Clock, Dur>& atime,
        std::memory_order memory_order = std::memory_order_seq_cst
    ) const noexcept {
        unsigned spins = SPIN_TIMES;
        while (spins-- > 0) {
            if (load(memory_order) != expected) 
                return true;
        }
        while (load(memory_order) == expected) {
            if (Clock::now() >= atime)
                return false;
            wait_until_impl(expected, atime);
        }
        return load(memory_order) != expected;
    }

    template <typename Clock = std::chrono::steady_clock, typename Rep, typename Period>
    bool wait_for(
        T expected,
        const std::chrono::duration<Rep, Period>& dur,
        std::memory_order memory_order = std::memory_order_seq_cst
    ) const noexcept {
        return wait_until(expected, Clock::now() + dur, memory_order);
    }

    inline void notify_one() const noexcept { notify_one_impl(); }
    inline void notify_all() const noexcept { notify_all_impl(); }

    T load(std::memory_order order = std::memory_order_seq_cst) const noexcept 
    {
        return std::atomic_ref { m_value }.load(order);
    }

    void store(T desired, std::memory_order order = std::memory_order_seq_cst) noexcept
    {
        std::atomic_ref { m_value }.store(desired, order);
    }

    T exchange(T value, std::memory_order order = std::memory_order_seq_cst) noexcept {

    }

    bool compare_exchange_weak(
        T& expected,
        T desired,
        std::memory_order success,
        std::memory_order failure
    ) noexcept
    {
        return std::atomic_ref { m_value }.compare_exchange_weak(
            expected, desired, success, failure);
    }

    bool compare_exchange_strong(
        T& expected,
        T desired,
        std::memory_order success,
        std::memory_order failure
    ) noexcept
    {
        return std::atomic_ref { m_value }.compare_exchange_strong(
            expected, desired, success, failure);
    }

private:

    inline static constexpr size_t get_alignment() {
        if constexpr (sizeof(T) <= 1) return 1;
        else if constexpr (sizeof(T) <= 2) return 2;
        else if constexpr (sizeof(T) <= 4) return 4;
        else if constexpr (sizeof(T) <= 8) return 8;
        else static_assert(detail::meta::dependent_false<T>, "wrong size of T");
    }

protected:

    alignas(std::max(get_alignment(), alignof(T))) T m_value;

private:

#if defined(__linux__)

    inline static constexpr uint32_t 
    FUTEX2_SIZE_FLAG = sizeof(T) == 1 ? FUTEX2_SIZE_U8 : 
        sizeof(T) <= 2 ? FUTEX2_SIZE_U16 :
        sizeof(T) <= 4 ? FUTEX2_SIZE_U32 :
        sizeof(T) <= 8 ? FUTEX2_SIZE_U64 : -1;

    static_assert(FUTEX2_SIZE_FLAG <= FUTEX2_SIZE_U64, "wrong futex2 size");

    inline static int futex_wait(const T* addr, T expected, const struct timespec* timeout) {
        struct futex_waitv w = {
            .val   = 0,
            .uaddr = reinterpret_cast<size_t>(addr),
            .flags = FUTEX2_SIZE_FLAG | FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
        };
        std::memcpy(&w.val, &expected, sizeof(expected));
        return syscall(SYS_futex_waitv, &w, 1, 0, timeout, CLOCK_MONOTONIC);
    }

    inline static int futex_wake(const T* addr, int count) {
        return syscall(SYS_futex_wake,
            addr,
            FUTEX2_SIZE_FLAG | FUTEX_PRIVATE_FLAG,
            count
        );
    }

    inline void wait_impl(T expected) const noexcept {
        futex_wait(&m_value, expected, nullptr);
    }

    template <typename Rep, typename Period>
    bool wait_for_impl(
        T expected,
        const std::chrono::duration<Rep, Period>& dur
    ) const noexcept {
        using namespace std::chrono;
        auto s = duration_cast<seconds>(dur);
        auto ns = duration_cast<nanoseconds>(dur - s);

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);

        ts.tv_sec += static_cast<time_t>(s.count());
        ts.tv_nsec += static_cast<long>(ns.count());
        if (ts.tv_nsec >= 1'000'000'000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1'000'000'000;
        }

        int ret = 0;
        do {
            ret = futex_wait(&m_value, expected, &ts);
        } while(ret < 0 && errno == EINTR);

        if (ret == 0 || errno == EAGAIN)
            return true;
        if (ret < 0 && errno == ETIMEDOUT)
            return false;
        return true;
    }

    template <typename Clock, typename Dur>
    bool wait_until_impl(
        T expected,
        const std::chrono::time_point<Clock, Dur>& atime
    ) const noexcept {
        return wait_for_impl(expected, atime - Clock::now());
    }

    inline void notify_one_impl() const noexcept {
        futex_wake(&m_value, 1);
    }
    
    inline void notify_all_impl() const noexcept {
        futex_wake(&m_value, std::numeric_limits<int>::max());
    }

#elif defined(_WIN32)

    inline void wait_impl(T expected) const noexcept {
        WaitOnAddress(reinterpret_cast<const T*>(&m_value), &expected, sizeof(std::atomic<T>), INFINITE);
    }

    template <typename Rep, typename Period>
    bool wait_for_impl(
        T expected,
        const std::chrono::duration<Rep, Period>& dur
    ) const noexcept {
        using namespace std::chrono;
        auto ms = duration_cast<milliseconds>(dur).count();
        BOOL ok = WaitOnAddress(&m_value, &expected, sizeof(std::atomic<T>),
                                static_cast<DWORD>(ms));
        if (!ok && GetLastError() == ERROR_TIMEOUT)
            return false;
        return true;
    }

    template <typename Clock, typename Dur>
    bool wait_until_impl(
        T expected,
        const std::chrono::time_point<Clock, Dur>& atime
    ) const noexcept {
        return wait_for_impl(expected, atime - Clock::now());
    }

    inline void notify_one_impl() const noexcept {
        WakeByAddressSingle(&m_value);
    }
    
    inline void notify_all_impl() const noexcept {
        WakeByAddressAll(&m_value);
    }

#else

    // fallback：条件变量版本

    inline void wait_impl(T expected) const noexcept {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [this, expected] { return std::atomic_ref{ m_value }.load() != expected; });
    }

    template <typename Rep, typename Period>
    bool wait_for_impl(T expected,
                       const std::chrono::duration<Rep, Period>& dur) const noexcept {
        std::unique_lock<std::mutex> lk(mtx);
        return cv.wait_for(lk, dur, [this, expected] { return std::atomic_ref{ m_value }.load() != expected; });
    }

    template <typename Clock, typename Dur>
    bool wait_until_impl(
        T expected,
        const std::chrono::time_point<Clock, Dur>& atime
    ) const noexcept {
        std::unique_lock<std::mutex> lk(mtx);
        return cv.wait_until(lk, atime, [this, expected] { return std::atomic_ref{ m_value }.load() != expected; });
    }

    inline void notify_one_impl() const noexcept {
        std::lock_guard<std::mutex> _lk(mtx);
        cv.notify_one();
    }
    
    inline void notify_all_impl() const noexcept {
        std::lock_guard<std::mutex> _lk(mtx);
        cv.notify_all();
    }
    
    mutable std::mutex mtx;
    mutable std::condition_variable cv;
#endif
    
};

}

template<typename T>
class waitable_atomic: public detail::waitable_atomic_base<T> {
    using detail::waitable_atomic_base<T>::waitable_atomic_base;
};

template<typename T>
requires std::is_integral_v<T>
class waitable_atomic<T>: public detail::waitable_atomic_base<T> {
    using detail::waitable_atomic_base<T>::waitable_atomic_base;

    T fetch_add(T value, std::memory_order order = std::memory_order_seq_cst) {
        return std::atomic_ref { detail::waitable_atomic_base<T>::m_value }.fetch_add(value, order);
    }
    
    T fetch_sub(T value, std::memory_order order = std::memory_order_seq_cst) {
        return std::atomic_ref { detail::waitable_atomic_base<T>::m_value }.fetch_sub(value, order);
    }

    T fetch_and(T value, std::memory_order order = std::memory_order_seq_cst) {
        return std::atomic_ref { detail::waitable_atomic_base<T>::m_value }.fetch_and(value, order);
    }

    T fetch_or(T value, std::memory_order order = std::memory_order_seq_cst) {
        return std::atomic_ref { detail::waitable_atomic_base<T>::m_value }.fetch_or(value, order);
    }
    
    T fetch_xor(T value, std::memory_order order = std::memory_order_seq_cst) {
        return std::atomic_ref { detail::waitable_atomic_base<T>::m_value }.fetch_xor(value, order);
    }
};

