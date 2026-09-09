#pragma once

// The contract for "somewhere a coroutine can run". Deliberately
// minimal: a scheduler is anything whose schedule() can be awaited,
// and nothing more. How the transfer happens -- thread pool, event
// loop, immediate resume -- is the implementation's business, which is
// why generic code can constrain on the concept without buying any
// particular executor.

#include <coroutine>

namespace coro {
    // A type whose schedule() is (by convention) an awaitable. Awaiting
    // it transfers the coroutine to wherever the scheduler runs work;
    // what that means concretely is left entirely open.
    template <typename S>
    concept scheduler = requires(S& instance) {
        { instance.schedule() };
    };

    // A scheduler that can also resume a bare coroutine handle from
    // ordinary code -- the delivery half of an executor. Needed by
    // anything that wakes parked coroutines itself (a reactor, a timer
    // thread) rather than through a co_await. Const for the same reason
    // io_reactor is: posting is using the scheduler, not changing it.
    template <typename S>
    concept postable = scheduler<S> && requires(const S& instance, std::coroutine_handle<> continuation) {
        { instance.post(continuation) };
    };
}
