#pragma once

#include <cstdint>
#include <csignal>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <h2o.h>

#include "owl/routing/router.h"
#include "owl/detail.h"

#ifdef OWL_ENABLE_POSTGRESQL
#include <optional>

#include <sql/psql.h>
#endif
#ifdef OWL_ENABLE_SQLITE
#include <memory>

#include <sql/sqlite.h>
#include <sql/sqlite_handle.h>
#endif

namespace owl {
    struct Config {
        std::string address = "127.0.0.1";
        std::uint16_t port = 8080;
        int backlog = 1024;
        unsigned threads = 1;
    };


    template <typename S>
    class Server final {
    public:
        class Builder {
        public:
            // Throws rather than replacing: a silently swapped router hides
            // registration the second router's handlers never see.
            template <typename Self>
            [[nodiscard]] auto&& router(this Self&& self, Router<S> router) {
                if (self.router_) throw std::invalid_argument("Server: router already set");
                self.router_ = std::make_unique<Router<S>>(std::move(router));
                return std::forward<Self>(self);
            }

            template <typename Self, typename F>
            [[nodiscard]] auto&& layer(this Self&& self, F mw) {
                self.layers_.emplace_back(wrap_layer<S>(std::move(mw)));
                return std::forward<Self>(self);
            }

            // config() replaces wholesale; a second call resets every field
            // not repeated. thread() is the sanctioned post-config tweak.
            template <typename Self>
            [[nodiscard]] auto&& config(this Self&& self, Config config) {
                if (self.config_) throw std::invalid_argument("Server: config already set");
                self.config_ = std::make_unique<Config>(std::move(config));
                return std::forward<Self>(self);
            }

            template <typename Self>
            [[nodiscard]] auto&& thread(this Self&& self, const unsigned n) {
                if (!self.config_) self.config_ = std::make_unique<Config>();
                self.config_->threads = n;
                return std::forward<Self>(self);
            }

#ifdef OWL_ENABLE_POSTGRESQL
            // This worker shard's postgres connections. A method called
            // without its macro does not exist, so the misuse is a compile
            // error, not a build_with check.
            template <typename Self>
            [[nodiscard]] auto&& with_psql(this Self&& self, sql::psql::config cfg) {
                self.psql_config_ = std::move(cfg);
                return std::forward<Self>(self);
            }
#endif
#ifdef OWL_ENABLE_SQLITE
            // The process-wide sqlite pool; build_with constructs it.
            template <typename Self>
            [[nodiscard]] auto&& with_sqlite(this Self&& self, sql::sqlite::config cfg) {
                self.sqlite_config_ = std::move(cfg);
                return std::forward<Self>(self);
            }
#endif

            // The state is the type: build_with is the only way to finish a
            // server, so a server that never got its state is a compile error
            // rather than a null read per request.
            template <typename Self>
            [[nodiscard]] Server build_with(this Self&& self, std::shared_ptr<S> state) {
                if (!state) throw std::invalid_argument("Server: state is required");
                if (!self.router_) throw std::invalid_argument("Server: router is required");
                if (!self.config_) throw std::invalid_argument("Server: config is required");
#ifdef OWL_ENABLE_SQLITE
                std::shared_ptr<sql::pool<sql::sqlite>> sqlite_pool;
                if (self.sqlite_config_) sqlite_pool = std::make_shared<sql::pool<sql::sqlite>>(*self.sqlite_config_);
#endif
                return Server{std::move(self.router_), std::move(self.config_), std::move(self.layers_), std::move(state)
#ifdef OWL_ENABLE_POSTGRESQL
                                  ,
                              std::move(self.psql_config_)
#endif
#ifdef OWL_ENABLE_SQLITE
                              ,
                              std::move(sqlite_pool)
#endif
                };
            }

        private:
            std::unique_ptr<Router<S>> router_;
            std::unique_ptr<Config> config_;
            MiddlewareChain<S> layers_{};
#ifdef OWL_ENABLE_POSTGRESQL
            std::optional<sql::psql::config> psql_config_;
#endif
#ifdef OWL_ENABLE_SQLITE
            std::optional<sql::sqlite::config> sqlite_config_;
#endif
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
        Server(std::unique_ptr<Router<S>> router, std::unique_ptr<Config> config, MiddlewareChain<S> layers, std::shared_ptr<S> state
#ifdef OWL_ENABLE_POSTGRESQL
               ,
               std::optional<sql::psql::config> psql_config
#endif
#ifdef OWL_ENABLE_SQLITE
               ,
               std::shared_ptr<sql::pool<sql::sqlite>> sqlite_pool
#endif
               )
            : router_(std::move(router)),
              config_(std::move(config)),
              layers_(std::move(layers)),
              state_(std::move(state)) {
            ////////////////////////////////////////////////////////////////////////////////////////////////
            //
            ////////////////////////////////////////////////////////////////////////////////////////////////
            std::signal(SIGPIPE, SIG_IGN);
            h2o_config_init(&globalconf_);
            const unsigned count = config_->threads == 0 ? 1 : config_->threads;

            ////////////////////////////////////////////////////////////////////////////////////////////////
            //
            ////////////////////////////////////////////////////////////////////////////////////////////////
            h2o_hostconf_t* const hostconf = h2o_config_register_host(&globalconf_, h2o_iovec_init(H2O_STRLIT("default")), 65535);
            h2o_pathconf_t* const pathconf = h2o_config_register_path(hostconf, "/", 0);

            ////////////////////////////////////////////////////////////////////////////////////////////////
            // create single universal handler
            ////////////////////////////////////////////////////////////////////////////////////////////////
            auto* const dispatcher = reinterpret_cast<detail::Dispatcher<S>*>(h2o_create_handler(pathconf, sizeof(detail::Dispatcher<S>)));
            dispatcher->super.on_req = &detail::on_req<S>;
            dispatcher->super.dispose = &detail::on_dispose<S>;
            dispatcher->super.on_context_init = &detail::on_context_init<S>;
            dispatcher->super.on_context_dispose = &detail::on_context_dispose<S>;

            dispatcher->router = router_.get();
            dispatcher->server_layers = &layers_;
            std::construct_at(&dispatcher->state, state_);
#ifdef OWL_ENABLE_POSTGRESQL
            if (psql_config) std::construct_at(&dispatcher->psql_config, std::move(*psql_config));
#endif
#ifdef OWL_ENABLE_SQLITE
            if (sqlite_pool) std::construct_at(&dispatcher->sqlite, std::move(sqlite_pool));
#endif

            ////////////////////////////////////////////////////////////////////////////////////////////////
            // init workers
            ////////////////////////////////////////////////////////////////////////////////////////////////
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
            for (;;) h2o_evloop_run(worker.ctx.loop, INT32_MAX);
            h2o_context_dispose(&worker.ctx);
        }

        void open_listeners() {
#ifndef SO_REUSEPORT
            if (workers_.size() > 1) {
                throw std::runtime_error("owl::Server: this platform has no SO_REUSEPORT, so threads must be 1");
            }
#endif
            for (std::size_t i = 0; i < workers_.size(); ++i) {
                const int fd = open_socket(i == 0 ? config_->port : bound_port_, workers_.size());
                if (i == 0) bound_port_ = detail::port_of(fd);

                auto& worker = *workers_[i];
                worker.listener = h2o_evloop_socket_create(worker.ctx.loop, fd, H2O_SOCKET_FLAG_DONT_READ);
                worker.listener->data = &worker;
                h2o_socket_read_start(worker.listener, &detail::on_accept);
            }
        }

        [[nodiscard]] int open_socket(const std::uint16_t port, std::size_t workers_size) const {
            const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) throw std::runtime_error("owl::Server: socket() failed");

            constexpr auto reuse = 1;
            ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
            if (workers_.size() > 1 && ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) != 0) {
                ::close(fd);
                throw std::runtime_error("owl::Server: SO_REUSEPORT rejected; threads must be 1");
            }
#endif
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(port);
            if (::inet_pton(AF_INET, config_->address.c_str(), &addr.sin_addr) != 1) {
                ::close(fd);
                throw std::invalid_argument("owl::Server: bad listen address: " + config_->address);
            }
            if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, config_->backlog) != 0) {
                ::close(fd);
                throw std::runtime_error("owl::Server: cannot bind " + config_->address + ":" + std::to_string(port));
            }
            return fd;
        }


        std::unique_ptr<Router<S>> router_;
        std::unique_ptr<Config> config_;
        MiddlewareChain<S> layers_{};
        std::shared_ptr<S> state_;
        h2o_globalconf_t globalconf_{};
        std::vector<std::unique_ptr<detail::Worker>> workers_;
        std::uint16_t bound_port_ = 0;
    };
}
