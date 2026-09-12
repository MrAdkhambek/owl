#pragma once

#include <memory>
#include <stdexcept>
#include <type_traits>

#include "owl/coro/loop_reactor.h"
#include "owl/coro/loop_scheduler.h"

#if defined(OWL_ENABLE_POSTGRESQL) || defined(OWL_ENABLE_SQLITE)
#include <sql/pool.h>
#endif
#ifdef OWL_ENABLE_POSTGRESQL
#include <sql/psql/psql.h>
#endif
#ifdef OWL_ENABLE_SQLITE
#include <sql/sqlite/sqlite.h>
#endif
#ifdef OWL_ENABLE_REDIS
#include <redis/client.h>
#endif

namespace owl {
    // Per-worker server state handed to extractors and the handler. Passed by
    // const ref through the chain; not stored on Request so HTTP stays HTTP.
    // There is no stateless server: a shared_ptr<void> would erase the one
    // thing the type system proves about handlers, so void is refused. A
    // Context is constructed once per worker and borrowed everywhere else,
    // so copying is deleted rather than silently allowed.
    template <typename S>
    struct Context final {
        static_assert(!std::is_void_v<S>, "Context requires a state type");

        Context() = default;

        // A worker's Context: the scheduler and the reactor are bound to the
        // loop from birth, so neither is assigned into afterwards and
        // neither needs to be movable.
        Context(
            std::shared_ptr<S> s,
            h2o_loop_t* const h2o_loop
        ) noexcept
            : state(std::move(s)),
              loop(h2o_loop, &hop),
              reactor(h2o_loop) {
        }

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;

        const std::shared_ptr<S> state;

        // The worker's scheduler, wired by the dispatcher at context init and
        // handed to handlers through extraction. Null until then: extraction
        // kicks 500 rather than handing out a scheduler that would swallow
        // posts into the void.
        const owl::loop_scheduler loop;

        // The worker's fd reactor on the same loop, for whatever on this
        // worker parks on a descriptor -- the psql pool today. Declared
        // before the pools so it outlives them.
        const owl::loop_reactor reactor;

        // The worker's cross-thread wakeup for loop_scheduler::post(): the
        // dispatcher registers it on this context's h2o queue when it wires
        // the worker up, below.
        h2o_multithread_receiver_t hop{};

        // The worker's receiver for WebSocket sends and closes posted from
        // other threads. mutable because it is h2o's to write into from any
        // thread; handlers see the Context as const, and run_handler hands
        // this to the engine through that const view.
        mutable h2o_multithread_receiver_t ws_hop{};

#ifdef OWL_ENABLE_POSTGRESQL
        // Null until on_context_init wires it, and null for good when the
        // builder never asked for the driver -- the extractor kicks 500. A
        // unique_ptr because the Context is the one owner and the pool is
        // built only once the Context, and the reactor it points at, exist;
        // handlers borrow it by const&, never share it. The pool holds a
        // reactor_ref to the member above, so it is declared after it and
        // dies before it.
        std::unique_ptr<sql::pool<sql::psql>> psql;
#endif
#ifdef OWL_ENABLE_SQLITE
        // Per worker like the psql pool: the sqlite connection threads post
        // their completions back through this worker's loop_scheduler.
        std::unique_ptr<sql::pool<sql::sqlite>> sqlite;
#endif
#ifdef OWL_ENABLE_REDIS
        // The worker's Redis client: one multiplexed connection, plus one
        // per live subscription, on this worker's reactor. Wired like the
        // pools above; null for good when the builder never asked.
        std::unique_ptr<redis::client> redis;
#endif
    };

    template <typename T>
    class State final {
        static_assert(!std::is_void_v<T>, "State requires a state type");

    public:
        State() = default;

        explicit State(std::shared_ptr<T> p) noexcept : ptr_(std::move(p)) {
        }

        T& get() const {
            if (!ptr_) throw std::runtime_error("owl::State: empty");
            return *ptr_;
        }

        T& operator*() const {
            return get();
        }

        T* operator->() const {
            return &get();
        }

        explicit operator bool() const noexcept {
            return static_cast<bool>(ptr_);
        }

        [[nodiscard]] std::shared_ptr<T> shared() const noexcept {
            return ptr_;
        }

        [[nodiscard]] T* get_if() const noexcept {
            return ptr_.get();
        }

    private:
        std::shared_ptr<T> ptr_;
    };
}
