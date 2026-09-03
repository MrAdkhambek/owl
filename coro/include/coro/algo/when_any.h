#pragma once

// when_any -- run a set of awaitables concurrently and return as soon as the
// first one of them finishes. The generic counterpart of wait_all's
// "all-of" idea: it's a race, so you get whichever awaitable wins first.
// Because it takes any awaitable (not just task<T>), you can mix tasks,
// timer awaiters, and other awaitables in one group.
//
//   auto result = co_await when_any(fetch_fast(), fetch_slow());
//   // returns the moment whichever of the two finishes first
//
//   auto [index, value] = co_await when_any(std::vector{a(), b(), c()});
//   // index is the position of the winner; value is its result
//
// The winner is reported as a variant (which alternative won) for the
// variadic form, or as a {index, value} pair for the range form. If the winner
// threw, when_any rethrows that error.
//
// A void task still occupies an alternative in the variant (a void_value
// token), so you can always tell which awaitable won.

#include <coroutine>
#include <cstddef>
#include <exception>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "coro/algo/detail/leaf.h"
#include "coro/concepts/awaitable.h"

namespace coro {
    namespace detail {
        // Sentinel for "no winner yet"; only meaningful as long as there are
        // still children running.
        inline constexpr std::size_t no_winner = static_cast<std::size_t>(-1);

        // Shared bookkeeping for when_any: who to wake, which child won (if
        // any), and whether we're still in the starting-up phase.
        struct any_state {
            std::coroutine_handle<> parent;
            std::size_t winner = no_winner;
            bool starting = true;

            // A child has finished. If one already won, ignore this one. If we
            //'re still starting, remember the winner but don't wake yet (the
            // starter will). Otherwise we're done -- hand control to the parent.
            [[nodiscard]] std::coroutine_handle<> child_finished(const std::size_t index) noexcept {
                if (winner != no_winner) return std::noop_coroutine();
                winner = index;
                if (starting) return std::noop_coroutine();
                return parent;
            }
        };
    }

    // The awaitable behind the variadic when_any. On suspend it starts each
    // child until one wins; on resume it reports the winner as a variant
    // (rethrowing its error if it threw).
    template <typename... Aws>
    class [[nodiscard]] when_any_t {
        static_assert(sizeof...(Aws) > 0, "when_any needs at least one awaitable to wait for");

    public:
        // One alternative per awaitable; the winning one is the one set.
        using Variant = std::variant<await_value_t<Aws>...>;

        explicit when_any_t(Aws&&... awaitables)
            : when_any_t(std::index_sequence_for<Aws...>{}, std::forward<Aws>(awaitables)...) {
        }

        when_any_t(const when_any_t&) = delete;
        when_any_t& operator=(const when_any_t&) = delete;
        when_any_t(when_any_t&&) = delete;
        when_any_t& operator=(when_any_t&&) = delete;
        // Neither copyable nor movable, as with when_all: leaves, slots
        // and state_ reference each other by address, so the group is
        // awaited where it was created.

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        // Starts each child in turn until one wins (or they're all running);
        // suspends if none has won yet, resumes if one did.
        bool await_suspend(const std::coroutine_handle<> parent) {
            state_.parent = parent;
            state_.starting = true;
            for (auto& leaf : leaves_) {
                leaf.start();
                if (state_.winner != detail::no_winner) break;
            }
            state_.starting = false;
            return state_.winner == detail::no_winner;
        }

        // Reports the winner as a variant; rethrows its error if it threw.
        Variant await_resume() {
            return winner(std::index_sequence_for<Aws...>{});
        }

    private:
        template <std::size_t... Is>
        when_any_t(std::index_sequence<Is...>, Aws&&... awaitables)
            : leaves_{bound<Is>(std::forward<Aws>(awaitables))...} {
        }

        // Wraps one awaitable in a leaf (its single driver), storing the
        // result into the matching slot and reporting back to any_state.
        template <std::size_t I, typename Aw>
        detail::leaf<detail::any_state> bound(Aw&& awaitable) {
            auto leaf = detail::run_child<detail::any_state>(std::get<I>(slots_), std::forward<Aw>(awaitable));
            leaf.bind(&state_, I);
            return leaf;
        }

        // Builds the variant for the winning alternative.
        template <std::size_t... Is>
        Variant winner(std::index_sequence<Is...>) {
            std::optional<Variant> result;
            (void)((state_.winner == Is ? (result.emplace(alternative<Is>()), true) : false) || ...);
            return std::move(*result);
        }

        // Reads the winner's slot: rethrows its error, or moves its value out.
        template <std::size_t I>
        Variant alternative() {
            auto& slot = std::get<I>(slots_);
            if (slot.error) std::rethrow_exception(slot.error);
            return Variant{std::in_place_index<I>, std::move(*slot.value)};
        }

        detail::any_state state_;
        std::tuple<detail::slot<await_value_t<Aws>>...> slots_;
        std::array<detail::leaf<detail::any_state>, sizeof...(Aws)> leaves_;
    };

    // Run every awaitable concurrently; co_await yields a variant holding the
    // first one to finish (or rethrows its error).
    //
    //   auto result = co_await when_any(fetch_fast(), fetch_slow());
    template <awaitable... Aws>
    [[nodiscard]] when_any_t<Aws...> when_any(Aws&&... awaitables) {
        return when_any_t<Aws...>{std::forward<Aws>(awaitables)...};
    }

    // Range form of when_any for a dynamic vector of a single awaitable type;
    // co_await yields a {index, value} pair naming the winner.
    template <awaitable Aw>
    class [[nodiscard]] when_any_range {
    public:
        using Value = await_value_t<Aw>;

        explicit when_any_range(std::vector<Aw> awaitables)
            : awaitables_(std::move(awaitables)) {
            if (awaitables_.empty()) {
                throw std::invalid_argument("coro::when_any: the range is empty, so nothing can win");
            }

            slots_.resize(awaitables_.size());
            leaves_.reserve(awaitables_.size());
            for (std::size_t i = 0; i < awaitables_.size(); ++i) {
                leaves_.push_back(detail::run_child<detail::any_state>(
                    slots_[i], std::move(awaitables_[i])));
                leaves_.back().bind(&state_, i);
            }
        }

        when_any_range(const when_any_range&) = delete;
        when_any_range& operator=(const when_any_range&) = delete;
        when_any_range(when_any_range&&) = delete;
        when_any_range& operator=(when_any_range&&) = delete;

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        bool await_suspend(const std::coroutine_handle<> parent) {
            state_.parent = parent;
            state_.starting = true;
            for (auto& leaf : leaves_) {
                leaf.start();
                if (state_.winner != detail::no_winner) break;
            }
            state_.starting = false;
            return state_.winner == detail::no_winner;
        }

        // Returns the winner as a {index, value} pair; rethrows its error if
        // the winning awaitable threw.
        std::pair<std::size_t, Value> await_resume() {
            auto& slot = slots_[state_.winner];
            if (slot.error) std::rethrow_exception(slot.error);
            return {state_.winner, std::move(*slot.value)};
        }

    private:
        detail::any_state state_;
        std::vector<Aw> awaitables_;
        std::vector<detail::slot<Value>> slots_;
        std::vector<detail::leaf<detail::any_state>> leaves_;
    };

    // Range overload: when_any over a vector of one awaitable type.
    //
    //   auto [index, value] = co_await when_any(std::vector{a(), b(), c()});
    template <awaitable Aw>
    [[nodiscard]] when_any_range<Aw> when_any(std::vector<Aw> awaitables) {
        return when_any_range<Aw>{std::move(awaitables)};
    }
}
