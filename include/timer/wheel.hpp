#ifndef SCORO_TIMER_WHEEL_HPP
#define SCORO_TIMER_WHEEL_HPP
#include "ptr/intrusive_ptr.hpp"
#include "ptr/rc_ptr.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <vector>
#pragma once

namespace coro {

using callback_t = void(*)(void* arg);

struct Task {
    enum State {
        ePending,
        eCanceled,
        eConsumed
    };

    size_t              execute_time;
    void*               arg;
    callback_t          callback;
    callback_t          cancel;
    std::atomic<State>  state;
};

struct TaskHandle {
    rc_ptr<Task> task;
};

template<typename Wheel, size_t N>
class TimeWheel {
public:
    inline static constexpr size_t Dur = N * Wheel::Dur;

    size_t add_task(size_t execute_time, callback_t cb, void* arg = nullptr, callback_t cancel = nullptr) {
    
    }

    void step() {

    }

private:
    Wheel wheel;
    std::array<std::vector<rc_ptr<Task>>, N> m_tasks;
};

template<size_t N>
class TimeWheel<void, N> {
public:
    inline static constexpr size_t Dur = N;

    size_t add_task(size_t execute_time, callback_t cb, void* arg = nullptr, callback_t cancel = nullptr) {

    }

    void step() {

    }

private:
    size_t m_pointer { 0 };
    std::array<std::vector<Task>, N> m_tasks;
};

}

#endif
