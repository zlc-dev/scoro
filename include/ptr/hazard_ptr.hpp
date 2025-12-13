#pragma once

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

class HazardPointer {
private:
    std::atomic<void*> m_ptr;
};


class Record {
public:

private:
    std::atomic<std::thread::id> m_owner;
    std::atomic<Record*> m_next;
    HazardPointer m_hp[10];
};

inline static Record *global {};
inline thread_local Record *hps;
inline thread_local std::vector<void*> rlist {};


