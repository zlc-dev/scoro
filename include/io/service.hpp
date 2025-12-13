#pragma once

#include "coro.hpp"
#include "io/sqe_awaitable.hpp"
#include <cstdio>
#include <liburing.h>
#include <print>
#include <stdexcept>

namespace coro::io {

class Service {
public:

    Service(int entries = 64, uint32_t flags = 0, uint32_t wq_fd = 0) {
        io_uring_params p = {
            .flags = flags,
            .wq_fd = wq_fd,
        };

        io_uring_queue_init_params(entries, &m_ring, &p);        
    }

    ~Service() noexcept {
        io_uring_queue_exit(&m_ring);
    }

    /** Nop
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    SqeAwaitable nop(uint8_t iflags = 0) {
        auto* sqe = io_uring_get_sqe_safe();
        io_uring_prep_nop(sqe);
        return await_work(sqe, iflags);
    }


    /** Read from a file descriptor at a given offset asynchronously
     * @see pread(2)
     * @see io_uring_enter(2) IORING_OP_READ
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    SqeAwaitable read(
        int fd,
        void* buf,
        unsigned nbytes,
        off_t offset,
        uint8_t iflags = 0
    ) {
        auto* sqe = io_uring_get_sqe_safe();
        io_uring_prep_read(sqe, fd, buf, nbytes, offset);
        return await_work(sqe, iflags);
    }

    /** Write to a file descriptor at a given offset asynchronously
     * @see pwrite(2)
     * @see io_uring_enter(2) IORING_OP_WRITE
     * @param iflags IOSQE_* flags
     * @return a task object for awaiting
     */
    SqeAwaitable write(
        int fd,
        const void* buf,
        unsigned nbytes,
        off_t offset,
        uint8_t iflags = 0
    ) {
        auto* sqe = io_uring_get_sqe_safe();
        io_uring_prep_write(sqe, fd, buf, nbytes, offset);
        return await_work(sqe, iflags);
    }

    io_uring_sqe* io_uring_get_sqe_safe() {
        auto* sqe = io_uring_get_sqe(&m_ring);
        if (sqe) [[likely]] {
            return sqe;
        } else {
            io_uring_cq_advance(&m_ring, m_cqe_count);
            m_cqe_count = 0;
            io_uring_submit(&m_ring);
            sqe = io_uring_get_sqe(&m_ring);
            if (sqe) [[likely]] return sqe;
            throw std::runtime_error("no mem");
        }
    }

    template <typename T, bool Nothrow>
    T run(const Future<T, Nothrow>& future) noexcept(Nothrow) {
        while (!future.done()) {
            io_uring_submit_and_wait(&m_ring, 1);

            io_uring_cqe *cqe;
            unsigned head;

            io_uring_for_each_cqe(&m_ring, head, cqe) {
                ++m_cqe_count;
                auto coro = static_cast<SqeResolver *>(io_uring_cqe_get_data(cqe));
                if (coro) coro->resolve(cqe->res);
            }

            io_uring_cq_advance(&m_ring, m_cqe_count);
            m_cqe_count = 0;
        }
    }

private:
    SqeAwaitable await_work(
        io_uring_sqe* sqe,
        uint8_t iflags
    ) noexcept {
        io_uring_sqe_set_flags(sqe, iflags);
        return SqeAwaitable(&m_ring, sqe);
    }

private:
    io_uring m_ring;
    unsigned m_cqe_count = 0;
};

}

