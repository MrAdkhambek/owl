#pragma once

// The half of an awaiter that as_result and as_tuple both pass straight
// through: readiness and suspension. Only await_resume differs between
// them, which is what each derived class supplies.
//
// Neither copyable nor movable: it stores the child awaiter by value, and
// a suspended coroutine holds a pointer into it.

#include <coroutine>
#include <utility>

#include "coro/concepts/awaitable.h"

namespace coro::detail {
    // The awaiter the adapter will hold: get_awaiter normalises the
    // three spellings of "awaitable" (member operator co_await, free
    // one, or an awaiter already) before anything is stored.
    template <typename Aw>
    using awaiter_storage_t = decltype(get_awaiter(std::declval<Aw>()));

    template <typename Aw>
    class awaiter_adapter {
    public:
        explicit awaiter_adapter(Aw&& awaitable) : awaiter_(get_awaiter(std::forward<Aw>(awaitable))) {
        }

        awaiter_adapter(const awaiter_adapter&) = delete;
        awaiter_adapter& operator=(const awaiter_adapter&) = delete;
        awaiter_adapter(awaiter_adapter&&) = delete;
        awaiter_adapter& operator=(awaiter_adapter&&) = delete;

        [[nodiscard]] bool await_ready() {
            return awaiter_.await_ready();
        }

        decltype(auto) await_suspend(const std::coroutine_handle<> waiting) {
            return awaiter_.await_suspend(waiting);
        }

    protected:
        awaiter_storage_t<Aw> awaiter_;
    };
}
