#pragma once

#ifndef OWL_ENABLE_POSTGRESQL
#error "sql/psql.h requires OWL_ENABLE_POSTGRESQL (cmake -DOWN_ENABLE_POSTGRESQL=ON, link owl::sql_psql)"
#endif

#include <chrono>
#include <coroutine>
#include <cstdlib>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <libpq-fe.h>

#include <coro/task.h>

#include "sql/bind.h"
#include "sql/dialect.h"
#include "sql/error.h"
#include "sql/reactor.h"
#include "sql/resume.h"
#include "sql/result.h"
#include "sql/detail/table.h"

namespace sql {
    namespace detail {
        // Decodes bytea text output -- PQunescapeBytea accepts both hex
        // (\x48656c6c6f) and the legacy escape form, so a server with
        // bytea_output = escape still round-trips.
        [[nodiscard]] inline std::string decode_bytea(const std::string_view text) {
            std::size_t len = 0;
            unsigned char* const bin = PQunescapeBytea(reinterpret_cast<const unsigned char*>(text.data()), &len);
            if (bin == nullptr) return {};
            std::string out{reinterpret_cast<const char*>(bin), len};
            PQfreemem(bin);
            return out;
        }
    }

    class psql final {
    public:
        static constexpr sql::dialect dialect = sql::dialect::postgres;
        static constexpr bool owns_thread = false;
        // The pool's conditional_t needs a nameable type even when unused.
        using reactor_type = std::monostate;

        struct config final {
            std::string dsn;
            std::size_t size = 4;
        };

        [[nodiscard]] static std::size_t pool_size(const config& cfg) {
            return cfg.size;
        }

        class connection final {
        public:
            connection(const config cfg, const sql::any_reactor& reactor) : cfg_(cfg), reactor_(&reactor) {
            }

            connection(const connection&) = delete;
            connection& operator=(const connection&) = delete;

            ~connection() {
                disconnect();
            }

            [[nodiscard]] bool healthy() const {
                return conn_ == nullptr || PQstatus(conn_) != CONNECTION_BAD;
            }

            // The async connect state machine. libpq documents that the
            // socket may change between PQconnectPoll calls (multi-host
            // DSNs, address-family fallback), so each wait re-reads
            // PQsocket and re-wraps: release the old fd, wrap the new.
            // After PGRES_POLLING_OK the last wrap stays for the
            // connection's life.
            [[nodiscard]] coro::task<std::expected<void, sql::error>> connect() {
                if (conn_ != nullptr) co_return std::expected<void, sql::error>{};
                if (cfg_.dsn.empty()) {
                    co_return std::unexpected(sql::error{.message = "sql: empty postgres dsn"});
                }

                conn_ = PQconnectStart(cfg_.dsn.c_str());
                if (conn_ == nullptr || PQstatus(conn_) == CONNECTION_BAD) {
                    const auto e = last_error();
                    disconnect();
                    co_return std::unexpected(e);
                }
                if (PQsetnonblocking(conn_, 1) == -1) {
                    // Everything below assumes non-blocking.
                    const auto e = last_error();
                    disconnect();
                    co_return std::unexpected(e);
                }

                int wrapped_fd = -1;
                for (;;) {
                    const auto rc = PQconnectPoll(conn_);
                    if (rc == PGRES_POLLING_OK) break;
                    if (rc == PGRES_POLLING_FAILED) {
                        const auto e = last_error();
                        disconnect();
                        co_return std::unexpected(e);
                    }
                    const auto interest = rc == PGRES_POLLING_READING ? coro::interest::read : coro::interest::write;
                    const int fd = PQsocket(conn_);
                    if (fd != wrapped_fd) {
                        if (wrapped_fd != -1) reactor_->release(wrapped_fd);
                        wrapped_fd = fd;
                    }
                    const auto s = co_await reactor_->wait(fd, interest, std::chrono::milliseconds{-1});
                    if (s != coro::wait_status::ready) {
                        disconnect();
                        co_return std::unexpected(sql::error{.message = "sql: postgres connect interrupted"});
                    }
                }
                co_return std::expected<void, sql::error>{};
            }

            // The whole loop, not just the read half: send (flush loop),
            // read (consume until not busy), then drain PQgetResult until
            // nullptr, re-checking PQisBusy before EACH call -- stopping at
            // the first result leaves the connection unusable for the next
            // checkout. Binds go out as text with unspecified oids (the
            // server infers); no user-facing binary protocol in v1, so
            // blob binds are sent verbatim and bytea columns are unreliable
            // as binds -- document, do not crash.
            [[nodiscard]] coro::task<std::expected<result, sql::error>>
            run(const std::string_view sql_text, const std::span<const bind_value> binds, const sql::resumer* /*unused: same loop*/) {
                if (conn_ == nullptr) {
                    if (auto opened = co_await connect(); !opened) co_return std::unexpected(opened.error());
                }

                const std::string command{sql_text};
                std::vector<const char*> values;
                std::vector<std::string> numbers;
                values.reserve(binds.size());
                numbers.reserve(binds.size());
                for (const auto& b : binds) {
                    std::visit(
                        [&](const auto& v) {
                            using V = std::decay_t<decltype(v)>;
                            if constexpr (std::is_same_v<V, std::nullptr_t>) {
                                values.push_back(nullptr);
                            } else if constexpr (std::is_same_v<V, std::int64_t>) {
                                numbers.emplace_back(std::to_string(v));
                                values.push_back(numbers.back().c_str());
                            } else if constexpr (std::is_same_v<V, double>) {
                                numbers.emplace_back(std::to_string(v));
                                values.push_back(numbers.back().c_str());
                            } else if constexpr (std::is_same_v<V, std::string>) {
                                // Points into the caller's binds array,
                                // which outlives this await.
                                values.push_back(v.c_str());
                            } else {
                                values.push_back(reinterpret_cast<const char*>(v.data()));
                            }
                        },
                        b.value);
                }

                if (PQsendQueryParams(conn_, command.c_str(), static_cast<int>(values.size()),
                                      nullptr, values.data(), nullptr, nullptr, 0) == 0) {
                    co_return std::unexpected(last_error());
                }

                for (;;) {
                    const int flushed = PQflush(conn_);
                    if (flushed == 0) break;
                    if (flushed < 0) co_return std::unexpected(last_error());
                    // 1 means the buffer is full: wait for writable.
                    const auto s = co_await reactor_->wait(PQsocket(conn_), coro::interest::write, std::chrono::milliseconds{-1});
                    if (s != coro::wait_status::ready) {
                        co_return std::unexpected(sql::error{.message = "sql: postgres flush interrupted"});
                    }
                }

                sql::detail::table t;
                std::expected<result, sql::error> out{};
                bool failed = false;
                for (;;) {
                    if (PQisBusy(conn_) != 0) {
                        if (PQconsumeInput(conn_) == 0) {
                            out = std::unexpected(last_error());
                            failed = true;
                            break;
                        }
                        if (PQisBusy(conn_) != 0) {
                            const auto s = co_await reactor_->wait(PQsocket(conn_), coro::interest::read, std::chrono::milliseconds{-1});
                            if (s != coro::wait_status::ready) {
                                out = std::unexpected(sql::error{.message = "sql: postgres read interrupted"});
                                failed = true;
                                break;
                            }
                            continue;
                        }
                    }
                    PGresult* const res = PQgetResult(conn_);
                    if (res == nullptr) break;
                    const auto status = PQresultStatus(res);
                    if (status == PGRES_TUPLES_OK) {
                        // Last result set wins (multi-statement strings are
                        // not really supported; the scanner still allows
                        // them through).
                        t = sql::detail::table{};
                        fill_table(res, t);
                    } else if (status == PGRES_COMMAND_OK) {
                        const char* const n = PQcmdTuples(res);
                        if (n != nullptr && n[0] != '\0') {
                            t.affected = static_cast<std::size_t>(std::strtoll(n, nullptr, 10));
                            t.has_affected = true;
                        }
                    } else if (status == PGRES_FATAL_ERROR || status == PGRES_NONFATAL_ERROR) {
                        out = std::unexpected(error_of(res));
                        failed = true;
                    }
                    PQclear(res);
                }
                if (failed) co_return std::move(out);
                co_return result{std::move(t), dialect::postgres};
            }

        private:
            [[nodiscard]] sql::error last_error() const {
                return sql::error{.message = PQerrorMessage(conn_), .sqlstate = {}, .code = 0};
            }

            [[nodiscard]] static sql::error error_of(PGresult* const res) {
                const char* const state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
                return sql::error{
                    .message = PQresultErrorMessage(res),
                    .sqlstate = state != nullptr ? state : "",
                    .code = 0};
            }

            static void fill_table(PGresult* const res, sql::detail::table& t) {
                const int cols = PQnfields(res);
                for (int c = 0; c < cols; ++c) {
                    t.add_column(PQfname(res, c));
                }
                for (int r = 0; r < PQntuples(res); ++r) {
                    for (int c = 0; c < cols; ++c) {
                        if (PQgetisnull(res, r, c) == 1) {
                            t.add_cell({}, true);
                            continue;
                        }
                        // BYTEAOID == 17: bytea columns arrive as text and
                        // must be decoded; everything else is verbatim.
                        if (PQftype(res, c) == 17) {
                            t.add_cell(detail::decode_bytea(std::string_view{PQgetvalue(res, r, c),
                                                                           static_cast<std::size_t>(PQgetlength(res, r, c))}),
                                       false);
                        } else {
                            t.add_cell(std::string_view{PQgetvalue(res, r, c), static_cast<std::size_t>(PQgetlength(res, r, c))},
                                       false);
                        }
                    }
                    t.end_row();
                }
            }

            void disconnect() {
                if (conn_ == nullptr) return;
                if (const int fd = PQsocket(conn_); fd != -1) reactor_->release(fd);
                PQfinish(conn_);
                conn_ = nullptr;
            }

            config cfg_;
            const sql::any_reactor* reactor_;
            PGconn* conn_ = nullptr;
        };
    };
}
