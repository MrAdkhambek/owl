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
//
// There is no cancellation, so the children that lose keep running: a timer
// that lost stays parked until it fires, a task on a pool runs to its end,
// and their results are dropped. The losers' frames and result slots
// therefore live in a shared block on the heap, held by the awaitable and by
// every child that was started, and freed by whichever of them lets go last.
// That is what keeps a loser from being destroyed while an executor still
// holds its handle. The rule it leaves the caller is the same one every
// parked coroutine already has: the executor a loser is parked on must
// outlive it.
//
// Because losers outlive the expression that built the group, when_any owns
// every child outright -- an rvalue is moved in, an lvalue is copied. A child
// that can be neither, such as async_mutex::lock_operation, does not compile
// here: wrap it in a task first.

#include <array>
#include <atomic>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
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

        // The part of a when_any that outlives the awaitable: who won, who to
        // wake, and how many parties still hold the block. The derived block
        // adds the slots and the leaves. Heap-allocated, and deleted by the
        // last holder to let go -- the awaitable in its destructor, or the
        // final child at its own final_suspend.
        struct any_state {
            std::coroutine_handle<> parent;

            // Set once, by the first child to finish; every later child
            // sees it taken and is a loser.
            std::atomic<std::size_t> winner{no_winner};

            // Two arrivals decide who continues the parent: its own check
            // after starting the children, and the winning child. Whichever
            // comes second does it -- inline for the parent's check, by
            // symmetric transfer for the child. Losers never touch this.
            std::atomic<std::size_t> pending{2};

            // The awaitable, plus every child. Each is counted before it
            // starts, so it is a holder before it can possibly finish.
            std::atomic<std::size_t> holders{1};

            virtual ~any_state() = default;

            // Called from a leaf's final_suspend. Names who runs next.
            //
            // release() may delete the block, and with it the very frame
            // this call was made from. That is legal at final_suspend -- the
            // frame is suspended, and nothing of it is touched afterwards --
            // which is why `next` is decided before the release, and the
            // caller returns it without looking at its promise again.
            [[nodiscard]] std::coroutine_handle<> child_finished(const std::size_t index) noexcept {
                std::coroutine_handle<> next = std::noop_coroutine();
                std::size_t expected = no_winner;
                if (winner.compare_exchange_strong(expected, index, std::memory_order_acq_rel)) {
                    if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1) next = parent;
                }
                release();
                return next;
            }

            // Starts every child in order; each becomes a holder before it
            // can possibly finish. There is no early stop once a child has
            // won: a win on another thread can land between two starts, and
            // stopping there would cancel whichever child came next -- only
            // sometimes, depending on timing. Returns whether the parent has
            // to wait.
            template <typename Leaves>
            bool start(const std::coroutine_handle<> waiting, Leaves& leaves) {
                parent = waiting;
                for (auto& leaf : leaves) {
                    holders.fetch_add(1, std::memory_order_relaxed);
                    leaf.start();
                }
                return pending.fetch_sub(1, std::memory_order_acq_rel) != 1;
            }

            // One holder lets go; the last one frees the block.
            void release() noexcept {
                if (holders.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
            }
        };

        // The block behind the variadic form: one slot and one leaf per
        // child, in argument order. Slots come first so their addresses
        // exist when the leaves that write into them are built.
        template <typename... Aws>
        struct any_block final : any_state {
            std::tuple<slot<await_value_t<Aws>>...> slots;
            std::array<leaf<any_state>, sizeof...(Aws)> leaves;

            template <std::size_t... Is>
            explicit any_block(std::index_sequence<Is...>, Aws&&... awaitables)
                : leaves{bound<Is>(std::forward<Aws>(awaitables))...} {
            }

        private:
            // Every child is owned: stored by value, whatever it was passed
            // as (see the header comment).
            template <std::size_t I, typename Aw>
            leaf<any_state> bound(Aw&& awaitable) {
                auto driver = run_child<any_state, std::remove_cvref_t<Aw>>(std::get<I>(slots), std::forward<Aw>(awaitable));
                driver.bind(this, I);
                return driver;
            }
        };

        // The same for the range form, over a vector of one awaitable type.
        template <typename Aw>
        struct any_range_block final : any_state {
            std::vector<slot<await_value_t<Aw>>> slots;
            std::vector<leaf<any_state>> leaves;

            explicit any_range_block(std::vector<Aw> awaitables) {
                slots.resize(awaitables.size());
                leaves.reserve(awaitables.size());
                for (std::size_t i = 0; i < awaitables.size(); ++i) {
                    leaves.push_back(run_child<any_state, Aw>(slots[i], std::move(awaitables[i])));
                    leaves.back().bind(this, i);
                }
            }
        };
    }

    // The awaitable behind the variadic when_any. On suspend it starts each
    // child until one wins; on resume it reports the winner as a variant
    // (rethrowing its error if it threw).
    template <typename... Aws>
    class [[nodiscard]] when_any_t {
        static_assert(sizeof...(Aws) > 0, "when_any needs at least one awaitable to wait for");
        static_assert((std::constructible_from<std::remove_cvref_t<Aws>, Aws&&> && ...),
                      "when_any owns its children, because a loser outlives the group: pass an rvalue to "
                      "move in, or a copyable lvalue. A non-movable awaiter (async_mutex::lock_operation, "
                      "as_result over a bare awaiter) has to be wrapped in a task first");

    public:
        // One alternative per awaitable; the winning one is the one set.
        using Variant = std::variant<await_value_t<Aws>...>;

        explicit when_any_t(Aws&&... awaitables)
            : block_(new block{std::index_sequence_for<Aws...>{}, std::forward<Aws>(awaitables)...}) {
        }

        // Movable, unlike when_all_t: nothing points into this object, only
        // out of it to the heap block, so a group can be named, moved and
        // awaited later.
        when_any_t(when_any_t&& o) noexcept : block_(std::exchange(o.block_, nullptr)) {
        }

        when_any_t(const when_any_t&) = delete;
        when_any_t& operator=(const when_any_t&) = delete;
        when_any_t& operator=(when_any_t&&) = delete;

        // Lets go of the block; the losers still running keep it alive.
        ~when_any_t() {
            if (block_) block_->release();
        }

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        bool await_suspend(const std::coroutine_handle<> parent) {
            return block_->start(parent, block_->leaves);
        }

        // Reports the winner as a variant; rethrows its error if it threw.
        Variant await_resume() {
            return winner(std::index_sequence_for<Aws...>{});
        }

    private:
        using block = detail::any_block<Aws...>;

        // Builds the variant for the winning alternative.
        template <std::size_t... Is>
        Variant winner(std::index_sequence<Is...>) {
            const std::size_t won = block_->winner.load(std::memory_order_acquire);
            std::optional<Variant> result;
            (void)((won == Is ? (result.emplace(alternative<Is>()), true) : false) || ...);
            return std::move(*result);
        }

        // Reads the winner's slot: rethrows its error, or moves its value out.
        template <std::size_t I>
        Variant alternative() {
            auto& slot = std::get<I>(block_->slots);
            if (slot.error) std::rethrow_exception(slot.error);
            return Variant{std::in_place_index<I>, slot.take()};
        }

        block* block_;
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
        static_assert(std::is_move_constructible_v<Aw>,
                      "when_any over a range moves each awaitable into its own driver; "
                      "a non-movable awaitable has to be wrapped in a task first");

    public:
        using Value = await_value_t<Aw>;

        explicit when_any_range(std::vector<Aw> awaitables) {
            if (awaitables.empty()) {
                throw std::invalid_argument("coro::when_any: the range is empty, so nothing can win");
            }
            block_ = new block{std::move(awaitables)};
        }

        when_any_range(when_any_range&& o) noexcept : block_(std::exchange(o.block_, nullptr)) {
        }

        when_any_range(const when_any_range&) = delete;
        when_any_range& operator=(const when_any_range&) = delete;
        when_any_range& operator=(when_any_range&&) = delete;

        ~when_any_range() {
            if (block_) block_->release();
        }

        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        bool await_suspend(const std::coroutine_handle<> parent) {
            return block_->start(parent, block_->leaves);
        }

        // Returns the winner as a {index, value} pair; rethrows its error if
        // the winning awaitable threw.
        std::pair<std::size_t, Value> await_resume() {
            const std::size_t won = block_->winner.load(std::memory_order_acquire);
            auto& slot = block_->slots[won];
            if (slot.error) std::rethrow_exception(slot.error);
            return {won, slot.take()};
        }

    private:
        using block = detail::any_range_block<Aw>;

        block* block_ = nullptr;
    };

    // Range overload: when_any over a vector of one awaitable type.
    //
    //   auto [index, value] = co_await when_any(std::vector{a(), b(), c()});
    template <awaitable Aw>
    [[nodiscard]] when_any_range<Aw> when_any(std::vector<Aw> awaitables) {
        return when_any_range<Aw>{std::move(awaitables)};
    }
}
