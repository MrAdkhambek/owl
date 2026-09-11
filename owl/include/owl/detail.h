#pragma once

#include <memory>
#include <netinet/in.h>
#include <new>
#include <sys/socket.h>

#include <h2o.h>

#include "coro/task.h"
#include "owl/coro/loop_scheduler.h"
#include "owl/core/drivers.h"
#include "owl/core/metrics.h"
#include "owl/http/detail/finish.h"
#include "owl/http/request.h"
#include "owl/routing/router.h"

#if defined(OWL_ENABLE_POSTGRESQL) || defined(OWL_ENABLE_SQLITE) || defined(OWL_ENABLE_REDIS)
#include <optional>
#endif
#ifdef OWL_ENABLE_POSTGRESQL
#include <sql/psql/psql.h>
#endif
#ifdef OWL_ENABLE_SQLITE
#include <sql/sqlite/sqlite.h>
#endif
#ifdef OWL_ENABLE_REDIS
#include <coro/io/reactor_ref.h>
#include <redis/client.h>
#include <redis/config.h>
#endif

namespace owl::detail {
    // The h2o global config, disposed with it. Disposing it is what runs
    // the Dispatcher's on_dispose and so lets go of the app state.
    struct GlobalConf final {
        h2o_globalconf_t conf{};

        GlobalConf() {
            h2o_config_init(&conf);
        }

        ~GlobalConf() {
            h2o_config_dispose(&conf);
        }

        GlobalConf(const GlobalConf&) = delete;
        GlobalConf& operator=(const GlobalConf&) = delete;
    };

    // One worker: its context, the loop that context runs on, and the
    // listener once one is opened. It is torn down in the reverse of how it
    // was built, and it has to die before the config its context points at.
    struct Worker final {
        h2o_context_t ctx{};
        h2o_accept_ctx_t accept_ctx{};
        h2o_socket_t* listener = nullptr;

        explicit Worker(h2o_globalconf_t* const conf) {
            h2o_context_init(&ctx, h2o_evloop_create(), conf);
            accept_ctx.ctx = &ctx;
            accept_ctx.hosts = conf->hosts;
        }

        ~Worker() {
            h2o_loop_t* const loop = ctx.loop;
            if (listener != nullptr) {
                h2o_socket_close(listener);
                // A closed evloop socket is freed on the loop's next pass.
                h2o_evloop_run(loop, 0);
            }
            h2o_context_dispose(&ctx);
            h2o_evloop_destroy(loop);
        }

        Worker(const Worker&) = delete;
        Worker& operator=(const Worker&) = delete;
    };

    // The driver configs the builder collected, as one value that travels
    // through Server, the Dispatcher and on_context_init unchanged; the
    // macros stay only where a driver type is named. Empty when no driver
    // is enabled.
    struct DriverConfigs final {
#ifdef OWL_ENABLE_POSTGRESQL
        std::optional<sql::psql::config> psql;
#endif
#ifdef OWL_ENABLE_SQLITE
        std::optional<sql::sqlite::config> sqlite;
#endif
#ifdef OWL_ENABLE_REDIS
        std::optional<redis::config> redis;
#endif
    };

    template <typename S>
    struct Dispatcher {
        h2o_handler_t super;
        const Router<S>* router;
        const MiddlewareChain<S>* server_layers;
        std::shared_ptr<S> state;
        DriverConfigs drivers;
    };

    // Runs the request through its chains and the handler, then sends what
    // came back. The exchange's clock starts before the try, so a handler
    // that throws is still charged the time it spent unwinding; a throw
    // from anywhere in the run is answered with the 500 floor.
    template <typename S>
    static coro::task<> run_handler(
        const Handler<S>* const handler,
        Request* const request,
        MatchedChains<S>* const chains,
        const MiddlewareChain<S>* const server_layers,
        const Context<S>* const context
    ) {
        const Exchange exchange{};
        try {
            const Next<S> next{chains->splice(server_layers), handler, context};
            auto response = co_await next(*request);
            exchange.matched(*request, response.status());
            co_await std::move(response).send(request->raw());
        } catch (...) {
            exchange.matched(*request, 500);
            send_error_floor(request->raw(), 500);
        }
    }

    // One request's dispatch, in the request's pool so that both die
    // together however far the handler got: the chains the match collects,
    // which Next views in place, and the task that runs them. The chains
    // live here rather than in the coroutine frame so they are neither
    // zeroed nor copied on the way in (see MatchedChains).
    template <typename S>
    struct Job final {
        MatchedChains<S> chains;
        coro::task<> work;

        [[nodiscard]] static Job* in(h2o_mem_pool_t* const pool) {
            void* const memory = h2o_mem_alloc_shared(pool, sizeof(Job), [](void* const object) {
                std::destroy_at(static_cast<Job*>(object));
            });
            // Default-initialised, not value-initialised: braces here would
            // be the memset MatchedChains exists to avoid.
            return ::new(memory) Job;
        }

        void launch(
            const Handler<S>* const handler,
            Request* const request,
            const MiddlewareChain<S>* const server_layers,
            const Context<S>* const context
        ) {
            // The frame must outlive this call: a handler that parks on I/O
            // is resumed later, and the pool's dispose is what frees it.
            work = run_handler(handler, request, &chains, server_layers, context);
            work.start();
        }
    };

    static void on_accept(h2o_socket_t* const listener, const char* const err) {
        if (err != nullptr) return;
        auto* const worker = static_cast<Worker*>(listener->data);
        h2o_socket_t* const sock = h2o_evloop_socket_accept(listener);
        if (sock == nullptr) return;
        h2o_accept(&worker->accept_ctx, sock);
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////
    //
    ////////////////////////////////////////////////////////////////////////////////////////////////
    template <typename S>
    static void on_context_init(h2o_handler_t* handler, h2o_context_t* ctx) {
        const auto* const dispatcher = reinterpret_cast<Dispatcher<S>*>(handler);

        // The scheduler, its hop and the reactor are born with the Context so
        // extraction hands handlers a working loop from the first request on;
        // the hop is registered on this context's queue, which is what makes
        // post() safe from the pool threads.
        auto* const context = new Context<S>{dispatcher->state, ctx->loop};
        h2o_multithread_register_receiver(ctx->queue, &context->hop, &on_loop_hop);

        // Each driver holds a handle to a member of this Context, so
        // they are built now rather than with it; how is drivers.h's.
#ifdef OWL_ENABLE_POSTGRESQL
        wire<sql::pool<sql::psql>>(*context, dispatcher->drivers);
#endif
#ifdef OWL_ENABLE_SQLITE
        wire<sql::pool<sql::sqlite>>(*context, dispatcher->drivers);
#endif
#ifdef OWL_ENABLE_REDIS
        wire<redis::client>(*context, dispatcher->drivers);
#endif
        h2o_context_set_handler_context(ctx, handler, context);
    }

    template <typename S>
    static void on_context_dispose(h2o_handler_t* h, h2o_context_t* ctx) {
        auto* const context = static_cast<Context<S>*>(h2o_context_get_handler_context(ctx, h));
        if (context == nullptr) return;
        // The receiver is linked into this context's queue; unlink it before
        // the Context that owns it dies.
        h2o_multithread_unregister_receiver(ctx->queue, &context->hop);
        delete context;
    }

    // h2o owns the handler's memory (h2o_create_handler allocates it and
    // h2o_config_dispose frees it after this hook), so only the C++ members
    // are torn down here.
    template <typename S>
    static void on_dispose(h2o_handler_t* h) {
        std::destroy_at(reinterpret_cast<Dispatcher<S>*>(h));
    }

    template <typename S>
    static int on_req(h2o_handler_t* const self, h2o_req_t* const req) noexcept {
        const auto* const context = static_cast<Context<S>*>(h2o_context_get_handler_context(req->conn->ctx, self));
        const auto* const dispatcher = reinterpret_cast<Dispatcher<S>*>(self);

        try {
            auto* const request = Request::from(req);
            auto* const job = Job<S>::in(&req->pool);
            const auto* const handler = dispatcher->router->match(
                request->method(),
                request->path(),
                *request,
                &job->chains
            );

            if (handler == nullptr) {
                const auto allowed = dispatcher->router->allowed_methods(request->path());
                const auto status = allowed.empty() ? 404 : 405;
                Exchange::unmatched(to_string(request->method()), request->route_pattern(), status);
                allowed.empty() ? send_not_found(req) : send_not_allowed(req, allowed.to_allow_header());
                return 0;
            }

            job->launch(handler, request, dispatcher->server_layers, context);
        } catch (...) {
            // The exchange never produced a handler response -- Request
            // construction, match, or launch failed -- so it is counted under
            // the unmatched route rather than a real one. The raw h2o method
            // token is used because Request::from may itself be what threw.
            Exchange::unmatched(std::string_view{req->method.base, req->method.len}, "unmatched", 500);
            send_error_floor(req, 500);
        }
        return 0;
    }

    ////////////////////////////////////////////////////////////////////////////////////////////////
    //
    ////////////////////////////////////////////////////////////////////////////////////////////////
    [[nodiscard]] static std::uint16_t port_of(const int fd) noexcept {
        sockaddr_in actual{};
        socklen_t actual_len = sizeof(actual);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &actual_len) != 0) return 0;
        return ntohs(actual.sin_port);
    }

    // The one place a Dispatcher is built, for Server and for tests that
    // wire a worker by hand. h2o hands back zeroed memory of the right size,
    // so the C++ members are constructed in place and on_dispose destroys
    // them. Must run before h2o_context_init: that sizes each context's
    // per-handler slot array from the handlers registered so far.
    template <typename S>
    [[nodiscard]] static Dispatcher<S>* make_dispatcher(
        h2o_pathconf_t* const pathconf,
        const Router<S>* const router,
        const MiddlewareChain<S>* const server_layers,
        std::shared_ptr<S> state,
        DriverConfigs drivers
    ) {
        auto* const dispatcher = reinterpret_cast<Dispatcher<S>*>(h2o_create_handler(pathconf, sizeof(Dispatcher<S>)));

        //
        dispatcher->super.on_req = &on_req<S>;
        dispatcher->super.dispose = &on_dispose<S>;
        dispatcher->super.on_context_init = &on_context_init<S>;
        dispatcher->super.on_context_dispose = &on_context_dispose<S>;

        //
        dispatcher->router = router;
        dispatcher->server_layers = server_layers;
        std::construct_at(&dispatcher->state, std::move(state));
        std::construct_at(&dispatcher->drivers, std::move(drivers));

        return dispatcher;
    }
}
