#pragma once

#include <cstdint>
#include <csignal>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <h2o.h>

#include "owl/coro/loop_scheduler.h"
#include "owl/http/detail/finish.h"
#include "owl/http/request.h"
#include "owl/routing/router.h"

namespace owl {
    struct Config {
        std::string address = "127.0.0.1";
        std::uint16_t port = 8080;
        int backlog = 1024;
        unsigned threads = 1;
    };

    namespace detail {
        struct Worker {
            h2o_context_t ctx{};
            h2o_accept_ctx_t accept_ctx{};
            h2o_socket_t* listener = nullptr;
            h2o_multithread_receiver_t hop{};
        };

        struct Dispatcher {
            h2o_handler_t super;
            const Router<>* router;
            const MiddlewareChain* server_layers;
        };

        struct SendJob {
            coro::task<> work;

            static void dispose(void* const memory) {
                std::destroy_at(static_cast<SendJob*>(memory));
            }
        };

        inline void launch_handler(
            const Handler* const handler,
            Request* const request,
            h2o_req_t* const req,
            MatchedChains chains,
            const MiddlewareChain* const server_layers
        ) {
            auto* const job = std::construct_at(static_cast<SendJob*>(h2o_mem_alloc_shared(&req->pool, sizeof(SendJob), &SendJob::dispose)));
            job->work = [](
                const Handler* const h,
                Request* const r,
                MatchedChains ch,
                const MiddlewareChain* const front
            ) -> coro::task<> {
                    try {
                        Terminal term{h};
                        const auto span = ch.splice(front);
                        const Next next{span.data, span.count, &term};
                        auto response = co_await next(*r);
                        co_await std::move(response).send(r->raw());
                    } catch (...) {
                        send_error_floor(r->raw(), 500);
                    }
                }(handler, request, std::move(chains), server_layers);
            job->work.start();
        }

        inline void on_accept(h2o_socket_t* const listener, const char* const err) {
            if (err != nullptr) return;
            auto* const worker = static_cast<Worker*>(listener->data);
            h2o_socket_t* const sock = h2o_evloop_socket_accept(listener);
            if (sock == nullptr) return;
            h2o_accept(&worker->accept_ctx, sock);
        }

        inline int on_req(h2o_handler_t* const self, h2o_req_t* const req) noexcept {
            const auto* const dispatcher = reinterpret_cast<Dispatcher*>(self);
            try {
                auto* const request = Request::from(req);

                MatchedChains chains{};
                const auto* const handler = dispatcher->router->match(request->method(), request->path(), *request, &chains);
                if (handler == nullptr) {
                    const auto allowed = dispatcher->router->allowed_methods(request->path());
                    allowed.empty() ? send_not_found(req) : send_not_allowed(req, allowed.to_allow_header());
                    return 0;
                }

                const MiddlewareChain* const front = dispatcher->server_layers != nullptr && !dispatcher->server_layers->empty()
                                                         ? dispatcher->server_layers
                                                         : nullptr;
                launch_handler(handler, request, req, std::move(chains), front);
            } catch (...) {
                send_error_floor(req, 500);
            }
            return 0;
        }
    }

    class Server final {
    public:
        class Builder {
        public:
            Builder router(Router<> router) && {
                router_ = std::make_unique<Router<>>(std::move(router));
                return std::move(*this);
            }

            template <typename F>
            Builder layer(F mw) && {
                layers_.emplace_back(wrap_layer<void>(std::move(mw)));
                return std::move(*this);
            }

            Builder config(Config config) && {
                config_ = std::move(config);
                return std::move(*this);
            }

            Builder thread(const unsigned n) && {
                config_.threads = n;
                return std::move(*this);
            }

            Server build() && {
                if (!router_) throw std::invalid_argument("Server: router is required");
                return Server{std::move(router_), config_, std::move(layers_)};
            }

        private:
            std::unique_ptr<Router<>> router_;
            Config config_{};
            MiddlewareChain layers_{};
        };

        [[nodiscard]] static Builder builder() {
            return Builder{};
        }

        [[nodiscard]] std::uint16_t port() const noexcept {
            return bound_port_;
        }

        void start() const {
            std::vector<std::thread> threads;
            threads.reserve(workers_.size() - 1);
            for (std::size_t i = 1; i < workers_.size(); ++i) {
                threads.emplace_back([this, i] {
                    serve(*workers_[i]);
                });
            }
            serve(*workers_.front());
            for (auto& thread : threads) thread.join();
        }

    private:
        Server(std::unique_ptr<Router<>> router, Config config, MiddlewareChain layers)
            : router_(std::move(router)),
              config_(std::move(config)),
              layers_(std::move(layers)) {
            std::signal(SIGPIPE, SIG_IGN);
            h2o_config_init(&globalconf_);
            h2o_hostconf_t* const host = h2o_config_register_host(&globalconf_, h2o_iovec_init(H2O_STRLIT("default")), 65535);
            h2o_pathconf_t* const pathconf = h2o_config_register_path(host, "/", 0);

            auto* const dispatcher = reinterpret_cast<detail::Dispatcher*>(h2o_create_handler(pathconf, sizeof(detail::Dispatcher)));
            dispatcher->super.on_req = &detail::on_req;
            dispatcher->router = router_.get();
            dispatcher->server_layers = &layers_;

            const unsigned count = config_.threads == 0 ? 1 : config_.threads;
            workers_.reserve(count);
            for (unsigned i = 0; i < count; ++i) {
                auto worker = std::make_unique<detail::Worker>();
                h2o_context_init(&worker->ctx, h2o_evloop_create(), &globalconf_);
                worker->accept_ctx.ctx = &worker->ctx;
                worker->accept_ctx.hosts = globalconf_.hosts;
                workers_.push_back(std::move(worker));
            }
            open_listeners();
        }

        static void serve(detail::Worker& worker) {
            h2o_multithread_register_receiver(worker.ctx.queue, &worker.hop, &detail::on_loop_hop);
            loop_scheduler io{worker.ctx.loop, &worker.hop};
            loop_scheduler::enter(io);
            for (;;) h2o_evloop_run(worker.ctx.loop, INT32_MAX);
        }

        void open_listeners() {
#ifndef SO_REUSEPORT
            if (workers_.size() > 1) {
                throw std::runtime_error("owl::Server: this platform has no SO_REUSEPORT, so threads must be 1");
            }
#endif
            for (std::size_t i = 0; i < workers_.size(); ++i) {
                const int fd = open_socket(i == 0 ? config_.port : bound_port_);
                if (i == 0) bound_port_ = port_of(fd);
                auto& worker = *workers_[i];
                worker.listener = h2o_evloop_socket_create(worker.ctx.loop, fd, H2O_SOCKET_FLAG_DONT_READ);
                worker.listener->data = &worker;
                h2o_socket_read_start(worker.listener, &detail::on_accept);
            }
        }

        [[nodiscard]] int open_socket(const std::uint16_t port) const {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) throw std::runtime_error("owl::Server: socket() failed");

            constexpr auto one = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
            if (workers_.size() > 1 && ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) != 0) {
                ::close(fd);
                throw std::runtime_error("owl::Server: SO_REUSEPORT rejected; threads must be 1");
            }
#endif
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (::inet_pton(AF_INET, config_.address.c_str(), &addr.sin_addr) != 1) {
                ::close(fd);
                throw std::invalid_argument("owl::Server: bad listen address: " + config_.address);
            }
            if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, config_.backlog) != 0) {
                ::close(fd);
                throw std::runtime_error("owl::Server: cannot bind " + config_.address + ":" + std::to_string(port));
            }
            return fd;
        }

        [[nodiscard]] static std::uint16_t port_of(const int fd) noexcept {
            sockaddr_in actual{};
            socklen_t actual_len = sizeof(actual);
            if (::getsockname(fd, reinterpret_cast<sockaddr*>(&actual), &actual_len) != 0) return 0;
            return ntohs(actual.sin_port);
        }

        std::unique_ptr<Router<>> router_;
        Config config_{};
        MiddlewareChain layers_{};
        h2o_globalconf_t globalconf_{};
        std::vector<std::unique_ptr<detail::Worker>> workers_;
        std::uint16_t bound_port_ = 0;
    };
}
