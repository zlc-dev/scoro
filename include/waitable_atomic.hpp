#pragma once
#include <atomic>
#include <chrono>
#include <limits>

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

constexpr unsigned SPIN_TIMES = 1;
#endif


static_assert(SPIN_TIMES > 0, "spin times must > 0");

class waitable_atomic_int: public std::atomic_int {
public:
    waitable_atomic_int(int init = 0) noexcept
        : std::atomic_int(init) {}

    inline void wait(int expected, std::memory_order memory_order = std::memory_order_seq_cst) const noexcept {
        unsigned spins = SPIN_TIMES;
        while (load(memory_order) == expected) {
            if (--spins != 0) continue;
            wait_impl(expected);
        }
    }

    template <typename Clock = std::chrono::steady_clock, typename Rep, typename Period>
    bool wait_for(
        int expected,
        const std::chrono::duration<Rep, Period>& dur,
        std::memory_order memory_order = std::memory_order_seq_cst
    ) const noexcept {
        auto success = false;
        auto before_spin = Clock::now();
        unsigned spins = SPIN_TIMES;
        while (load(memory_order) == expected) {
            if (--spins != 0) continue;
            auto spin_dur = Clock::now() - before_spin;
            if (spin_dur >= dur)
                return false;
            success = wait_for_impl(expected, dur - spin_dur);
            if (!success)
                return false;
        }
        return true;
    }

    template <typename Clock, typename Dur>
    bool wait_until(
        int expected,
        const std::chrono::time_point<Clock, Dur>& atime,
        std::memory_order memory_order = std::memory_order_seq_cst
    ) const noexcept {
        auto success = false;
        unsigned spins = SPIN_TIMES;

        while (load(memory_order) == expected) {
            if (--spins != 0) continue;
            if (Clock::now() >= atime)
                return false;
            success = wait_until_impl(expected, atime);
            if (!success)
                return false;
        }
        return true;
    }

    inline void notify_one() const noexcept { notify_one_impl(); }
    inline void notify_all() const noexcept { notify_all_impl(); }

private:

#if defined(__linux__)

    inline static int futex_wait(const int* addr, int expected, const struct timespec* timeout) {
        return syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, timeout, nullptr, 0);
    }

    inline static int futex_wake(const int* addr, int count) {
        return syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0);
    }

    inline void wait_impl(int expected) const noexcept {
        futex_wait(reinterpret_cast<const int*>(this), expected, nullptr);
    }

    template <typename Rep, typename Period>
    bool wait_for_impl(
        int expected,
        const std::chrono::duration<Rep, Period>& dur
    ) const noexcept {
        using namespace std::chrono;
        auto s = duration_cast<seconds>(dur);
        auto ns = duration_cast<nanoseconds>(dur - s);
        struct timespec ts;
        ts.tv_sec = static_cast<time_t>(s.count());
        ts.tv_nsec = static_cast<long>(ns.count());

        int ret = futex_wait(reinterpret_cast<const int*>(this), expected, &ts);
        if (ret == 0 || errno == EAGAIN)
            return true;
        if (errno == ETIMEDOUT)
            return false;
        return true;
    }

    template <typename Clock, typename Dur>
    bool wait_until_impl(
        int expected,
        const std::chrono::time_point<Clock, Dur>& atime
    ) const noexcept {
        return wait_for_impl(expected, atime - Clock::now());
    }

    inline void notify_one_impl() const noexcept {
        futex_wake(reinterpret_cast<const int*>(this), 1);
    }
    
    inline void notify_all_impl() const noexcept {
        futex_wake(reinterpret_cast<const int*>(this), std::numeric_limits<int>::max());
    }

#elif defined(_WIN32)

    inline void wait_impl(int expected) const noexcept {
        WaitOnAddress(reinterpret_cast<const int*>(this), &expected, sizeof(std::atomic_int), INFINITE);
    }

    template <typename Rep, typename Period>
    bool wait_for_impl(
        int expected,
        const std::chrono::duration<Rep, Period>& dur
    ) const noexcept {
        using namespace std::chrono;
        auto ms = duration_cast<milliseconds>(dur).count();
        BOOL ok = WaitOnAddress(reinterpret_cast<const int*>(this), &expected, sizeof(std::atomic_int),
                                static_cast<DWORD>(ms));
        if (!ok && GetLastError() == ERROR_TIMEOUT)
            return false;
        return true;
    }

    template <typename Clock, typename Dur>
    bool wait_until_impl(
        int expected,
        const std::chrono::time_point<Clock, Dur>& atime
    ) const noexcept {
        return wait_for_impl(expected, atime - Clock::now());
    }

    inline void notify_one_impl() const noexcept {
        WakeByAddressSingle(reinterpret_cast<const int*>(this));
    }
    
    inline void notify_all_impl() const noexcept {
        WakeByAddressAll(reinterpret_cast<const int*>(this));
    }

#else

    // fallback：条件变量版本

    inline void wait_impl(int expected) const noexcept {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait(lk, [this, expected] { return load() != expected; });
    }

    template <typename Rep, typename Period>
    bool wait_for_impl(int expected,
                       const std::chrono::duration<Rep, Period>& dur) const noexcept {
        std::unique_lock<std::mutex> lk(mtx);
        return cv.wait_for(lk, dur, [this, expected] { return load() != expected; });
    }

    template <typename Clock, typename Dur>
    bool wait_until_impl(
        int expected,
        const std::chrono::time_point<Clock, Dur>& atime
    ) const noexcept {
        std::unique_lock<std::mutex> lk(mtx);
        return cv.wait_until(lk, atime, [this, expected] { return load() != expected; });
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
