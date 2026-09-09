#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>

#include "owl/coro/loop_scheduler.h"

#ifdef OWL_ENABLE_POSTGRESQL
#include <sql/pool.h>
#include <sql/psql.h>

#include "owl/coro/loop_reactor.h"
#endif
#ifdef OWL_ENABLE_SQLITE
#include <sql/pool.h>
#include <sql/sqlite.h>
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

        explicit Context(std::shared_ptr<S> s) noexcept : state(std::move(s)) {
        }

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;

        std::shared_ptr<S> state;

        // The worker's cross-thread wakeup for loop_scheduler::post(): the
        // dispatcher registers it on this context's h2o queue when it wires
        // the worker up, below.
        h2o_multithread_receiver_t hop{};

        // The worker's scheduler, wired by the dispatcher at context init and
        // handed to handlers through extraction. Null until then: extraction
        // kicks 500 rather than handing out a scheduler that would swallow
        // posts into the void.
        owl::loop_scheduler loop;

#ifdef OWL_ENABLE_POSTGRESQL
        // The psql pool holds a reactor_ref to this reactor, so the reactor
        // is declared first and destroyed last; a Context is neither
        // copyable nor movable, so the address is stable. The optional is
        // empty until on_context_init wires it, and stays empty when the
        // builder never asked for the driver -- the extractor kicks 500.
        owl::loop_reactor reactor;
        std::optional<sql::pool<sql::psql>> psql;
#endif
#ifdef OWL_ENABLE_SQLITE
        // Per worker like the psql pool: the sqlite connection threads post
        // their completions back through this worker's loop_scheduler.
        std::optional<sql::pool<sql::sqlite>> sqlite;
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
