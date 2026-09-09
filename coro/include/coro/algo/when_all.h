#pragma once

// when_all -- run a set of awaitables concurrently and return their results
// once every one of them has finished. It's the generic sibling of wait_all:
// where wait_all takes task<T> specifically, when_all takes any awaitable, so
// you can mix tasks, timer awaiters, and other awaitables in one group.
//
//   auto [user, posts] = co_await when_all(fetch_user(id), fetch_posts(id));
//   // both ran in parallel; user and posts are both ready
//
// The result is a tuple, in the order given. If any child throws, when_all
// rethrows the first one it finds -- the other results are discarded. There's
// also a range overload for a dynamic vector of a single awaitable type.
//
// join is a plain alias for when_all (same for the range form).

#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "coro/algo/detail/leaf.h"
#include "coro/concepts/awaitable.h"

namespace coro {
    namespace detail {
        // Shared bookkeeping for when_all: who to wake once every child is
        // done, and how many are still pending. remaining starts at
        // children + 1 because the parent's own check counts as an arrival,
        // and the last arrival -- child or parent -- is the one that
        // continues the parent.
        //
        // The count is atomic because children finish wherever their work
        // ran: two pool workers, or a worker and the timer thread, can
        // arrive at the same instant as each other and as the parent's own
        // check. acq_rel on every decrement is what lets the arrival that
        // reaches zero see every other child's slot as written.
        struct all_state {
            std::coroutine_handle<> parent;
            std::atomic<std::size_t> remaining{0};

            void begin(const std::coroutine_handle<> waiting, const std::size_t children) noexcept {
                parent = waiting;
                remaining.store(children + 1, std::memory_order_release);
            }

            // One child is done: hand control to the parent if this was the
            // last arrival, otherwise to nothing.
            [[nodiscard]] std::coroutine_handle<> child_finished(std::size_t) noexcept {
                if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) return parent;
                return std::noop_coroutine();
            }

            // The parent's own arrival, after it has started every child.
            // True when children are still running and the parent must
            // stay suspended; false when it was the last arrival itself.
            [[nodiscard]] bool parent_waits() noexcept {
                return remaining.fetch_sub(1, std::memory_order_acq_rel) != 1;
            }
        };
    }

    // The awaitable behind the variadic when_all. On suspend it starts every
    // child; on resume it either rethrows the first child's error or returns
    // all results as a tuple, in the order given.
    template <typename... Aws>
    class [[nodiscard]] when_all_t {
    public:
        explicit when_all_t(Aws&&... awaitables)
            : when_all_t(std::index_sequence_for<Aws...>{}, std::forward<Aws>(awaitables)...) {
        }

        when_all_t(const when_all_t&) = delete;
        when_all_t& operator=(const when_all_t&) = delete;
        when_all_t(when_all_t&&) = delete;
        when_all_t& operator=(when_all_t&&) = delete;
        // Neither copyable nor movable because the parts reference each
        // other by address: leaves point into slots_ and state_.
        // Relocating the group would strand every pointer. Naming it is
        // fine -- guaranteed elision builds it in place, and it owns its
        // rvalue children (see child_storage_t) -- as long as it is then
        // awaited where it sits.

        // Ready immediately only in the degenerate case of zero awaitables.
        [[nodiscard]] bool await_ready() const noexcept {
            return sizeof...(Aws) == 0;
        }

        // Starts every child, then counts itself as an arrival; suspends until
        // the final child (or its own check) drives the remaining count to zero.
        bool await_suspend(const std::coroutine_handle<> parent) {
            state_.begin(parent, sizeof...(Aws));
            for (auto& leaf : leaves_) leaf.start();
            return state_.parent_waits();
        }

        // All children have finished: if any threw, rethrow the first error;
        // otherwise move each result out of its slot into the tuple.
        std::tuple<await_value_t<Aws>...> await_resume() {
            std::exception_ptr first;
            std::apply([&](auto&... slot) {
                ((first = first ? first : slot.error), ...);
            }, slots_);
            if (first) std::rethrow_exception(first);

            return std::apply([](auto&... slot) {
                return std::tuple<await_value_t<Aws>...>{slot.take()...};
            }, slots_);
        }

    private:
        template <std::size_t... Is>
        when_all_t(std::index_sequence<Is...>, Aws&&... awaitables)
            : leaves_{bound<Is>(std::forward<Aws>(awaitables))...} {
        }

        // Wraps one awaitable in a leaf (its single driver), storing the
        // result into the matching slot and reporting back to all_state.
        template <std::size_t I, typename Aw>
        detail::leaf<detail::all_state> bound(Aw&& awaitable) {
            auto leaf = detail::run_child<detail::all_state, detail::child_storage_t<Aw>>(
                std::get<I>(slots_), std::forward<Aw>(awaitable));
            leaf.bind(&state_, I);
            return leaf;
        }

        detail::all_state state_;
        std::tuple<detail::slot<await_value_t<Aws>>...> slots_;
        std::array<detail::leaf<detail::all_state>, sizeof...(Aws)> leaves_;
    };

    // Run every awaitable concurrently; co_await yields a tuple of results
    // (in order) once all are done, or rethrows the first error.
    //
    //   auto [user, posts] = co_await when_all(fetch_user(id), fetch_posts(id));
    template <awaitable... Aws>
    [[nodiscard]] when_all_t<Aws...> when_all(Aws&&... awaitables) {
        return when_all_t<Aws...>{std::forward<Aws>(awaitables)...};
    }

    // Range form of when_all for a dynamic vector of a single awaitable type;
    // co_await yields a vector of results in the same order. Each element
    // is moved out of the vector into its own driver, which is why the
    // element type has to be movable: there is no caller-side object left
    // to borrow from once the vector has been handed over.
    template <awaitable Aw>
    class [[nodiscard]] when_all_range {
        static_assert(std::is_move_constructible_v<Aw>,
                      "when_all over a range moves each awaitable into its own driver; "
                      "a non-movable awaitable needs the variadic form, awaited in the expression that built it");

    public:
        using Value = await_value_t<Aw>;

        explicit when_all_range(std::vector<Aw> awaitables) {
            slots_.resize(awaitables.size());
            leaves_.reserve(awaitables.size());
            for (std::size_t i = 0; i < awaitables.size(); ++i) {
                leaves_.push_back(detail::run_child<detail::all_state, Aw>(slots_[i], std::move(awaitables[i])));
                leaves_.back().bind(&state_, i);
            }
        }

        when_all_range(const when_all_range&) = delete;
        when_all_range& operator=(const when_all_range&) = delete;
        when_all_range(when_all_range&&) = delete;
        when_all_range& operator=(when_all_range&&) = delete;

        [[nodiscard]] bool await_ready() const noexcept {
            return leaves_.empty();
        }

        bool await_suspend(const std::coroutine_handle<> parent) {
            state_.begin(parent, leaves_.size());
            for (auto& leaf : leaves_) leaf.start();
            return state_.parent_waits();
        }

        std::vector<Value> await_resume() {
            for (auto& slot : slots_) if (slot.error) std::rethrow_exception(slot.error);

            std::vector<Value> results;
            results.reserve(slots_.size());
            for (auto& slot : slots_) results.push_back(slot.take());
            return results;
        }

    private:
        detail::all_state state_;
        std::vector<detail::slot<Value>> slots_;
        std::vector<detail::leaf<detail::all_state>> leaves_;
    };

    // Range overload: when_all over a vector of one awaitable type.
    //
    //   auto results = co_await when_all(std::vector{fetch(1), fetch(2), fetch(3)});
    template <awaitable Aw>
    [[nodiscard]] when_all_range<Aw> when_all(std::vector<Aw> awaitables) {
        return when_all_range<Aw>{std::move(awaitables)};
    }

    // join: an alias for when_all (variadic).
    template <awaitable... Aws>
    [[nodiscard]] when_all_t<Aws...> join(Aws&&... awaitables) {
        return when_all_t<Aws...>{std::forward<Aws>(awaitables)...};
    }

    // join: an alias for the range form of when_all.
    template <awaitable Aw>
    [[nodiscard]] when_all_range<Aw> join(std::vector<Aw> awaitables) {
        return when_all_range<Aw>{std::move(awaitables)};
    }
}
