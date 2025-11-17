#pragma once

#include "waitable_atomic.hpp"
namespace coro {

struct Semaphore {
public:



private:
    std::atomic_int m_count;
};

}
