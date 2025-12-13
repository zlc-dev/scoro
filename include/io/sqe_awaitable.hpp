#pragma once

#include <cassert>
#include <coroutine>
#include <functional>
#include <liburing.h>
#include <optional>

namespace coro::io {

struct SqeResolver {
    virtual void resolve(int result) = 0;
};

struct ResumeResolver final: SqeResolver {
    friend struct SqeAwaitable;

    void resolve(int result) noexcept override {
        this->result = result;
        handle.resume();
    }

private:
    std::coroutine_handle<> handle;
    int result = 0;
};
static_assert(std::is_trivially_destructible_v<ResumeResolver>);


struct DeferredResolver final: SqeResolver {
    void resolve(int result) noexcept override {
        this->result = result;
    }

#ifndef NDEBUG
    ~DeferredResolver() {
        assert(!!result && "deferred_resolver is destructed before it's resolved");
    }
#endif

    std::optional<int> result;
};

struct CallbackResolver final: SqeResolver {
    CallbackResolver(std::function<void (int result)>&& cb): cb(std::move(cb)) {}

    void resolve(int result) noexcept override {
        this->cb(result);
        delete this;
    }

private:
    std::function<void (int result)> cb;
};

struct SqeAwaitable {
    SqeAwaitable(io_uring* ring, io_uring_sqe* sqe) noexcept: ring(ring), sqe(sqe) {}

    void set_deferred(DeferredResolver& resolver) {
        io_uring_sqe_set_data(sqe, &resolver);
    }

    void set_callback(std::function<void (int result)> cb) {
        io_uring_sqe_set_data(sqe, new CallbackResolver(std::move(cb)));
    }

    auto operator co_await() {
        struct AwaitSqe {
            ResumeResolver resolver {};
            io_uring* ring;
            io_uring_sqe* sqe;
            AwaitSqe(io_uring* ring, io_uring_sqe* sqe): ring(ring), sqe(sqe) {}

            constexpr bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> handle) noexcept {
                resolver.handle = handle;
                io_uring_sqe_set_data(sqe, &resolver);
                io_uring_submit(ring);
            }

            int await_resume() const noexcept { return resolver.result; }
        };

        return AwaitSqe(ring, sqe);
    }

private:
    io_uring* ring;
    io_uring_sqe* sqe;
};


}
