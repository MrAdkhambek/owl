#pragma once

#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>

#include <h2o.h>

#include "coro/task.h"
#include "owl/coro/loop_scheduler.h"
#include "owl/http/detail/finish.h"
#include "owl/http/request.h"
#include "owl/routing/router.h"

#ifdef OWL_ENABLE_PROMETHEUS
#include <chrono>
#include <prometheus/http.h>
#endif
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
    struct Worker {
        h2o_context_t ctx{};
        h2o_accept_ctx_t accept_ctx{};
        h2o_socket_t* listener = nullptr;
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

    struct SendJob {
        coro::task<> work;

        static void dispose(void* const memory) {
            std::destroy_at(static_cast<SendJob*>(memory));
        }
    };

    template <typename S>
    static void launch_handler(
        const Handler<S>* const handler,
        Request* const request,
        h2o_req_t* const req,
        MatchedChains<S> chains,
        const MiddlewareChain<S>* const server_layers,
        const Context<S>* const context
    ) {
        auto* const job = std::construct_at(static_cast<SendJob*>(h2o_mem_alloc_shared(&req->pool, sizeof(SendJob), &SendJob::dispose)));
        job->work = [](
            const Handler<S>* const h,
            Request* const r,
            MatchedChains<S> ch,
            const MiddlewareChain<S>* const front,
            const Context<S>* const ctx
        ) -> coro::task<> {
                // The clock starts before try: a handler that throws must
                // still be recorded with the time it spent unwinding.
#ifdef OWL_ENABLE_PROMETHEUS
                const auto start = std::chrono::steady_clock::now();
#endif
                try {
                    Terminal<S> term{h};
                    const auto span = ch.splice(front);
                    const Next<S> next{span.data, span.count, &term, ctx};
                    auto response = co_await next(*r);
#ifdef OWL_ENABLE_PROMETHEUS
                    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                    owl::prometheus::record_request(to_string(r->method()), r->route_pattern(), response.status(), elapsed);
#endif
                    co_await std::move(response).send(r->raw());
                } catch (...) {
#ifdef OWL_ENABLE_PROMETHEUS
                    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                    owl::prometheus::record_request(to_string(r->method()), r->route_pattern(), 500, elapsed);
#endif
                    send_error_floor(r->raw(), 500);
                }
            }(handler, request, std::move(chains), server_layers, context);
        job->work.start();
    }

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

        // The pools are built in place: neither copyable nor movable, and
        // each holds a handle to this Context's own reactor or scheduler.
#ifdef OWL_ENABLE_POSTGRESQL
        if (dispatcher->drivers.psql) context->psql.emplace(*dispatcher->drivers.psql, sql::reactor_ref{context->reactor});
#endif
#ifdef OWL_ENABLE_SQLITE
        if (dispatcher->drivers.sqlite) context->sqlite.emplace(*dispatcher->drivers.sqlite, sql::scheduler_ref{context->loop});
#endif
#ifdef OWL_ENABLE_REDIS
        if (dispatcher->drivers.redis) context->redis.emplace(*dispatcher->drivers.redis, coro::reactor_ref{context->reactor});
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

            MatchedChains<S> chains{};
            const auto* const handler = dispatcher->router->match(
                request->method(),
                request->path(),
                *request,
                &chains
            );

            if (handler == nullptr) {
                const auto allowed = dispatcher->router->allowed_methods(request->path());
#ifdef OWL_ENABLE_PROMETHEUS
                const auto status = allowed.empty() ? 404 : 405;
                owl::prometheus::record_unmatched(to_string(request->method()), request->route_pattern(), status);
#endif
                allowed.empty() ? send_not_found(req) : send_not_allowed(req, allowed.to_allow_header());
                return 0;
            }

            const MiddlewareChain<S>* const front = dispatcher->server_layers != nullptr && !dispatcher->server_layers->empty()
                                                        ? dispatcher->server_layers
                                                        : nullptr;
            launch_handler(handler, request, req, std::move(chains), front, context);
        } catch (...) {
#ifdef OWL_ENABLE_PROMETHEUS
            // The exchange never produced a handler response -- Request
            // construction, match, or launch failed -- so it is counted under
            // the unmatched route rather than a real one. The raw h2o method
            // token is used because Request::from may itself be what threw.
            owl::prometheus::record_unmatched(std::string_view{req->method.base, req->method.len}, "unmatched", 500);
#endif
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
