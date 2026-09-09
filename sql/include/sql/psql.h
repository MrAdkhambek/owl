#pragma once

// sql::psql -- the Postgres driver on async libpq. Every step is
// non-blocking: PQconnectStart / PQconnectPoll for the handshake,
// PQsendQueryParams + PQflush + PQconsumeInput / PQisBusy / PQgetResult for
// a statement, with every wait for socket readiness going through the
// reactor_ref the pool was built with. The result is not materialized: it
// owns the PGresult, cells are views into it, and from_text parses on
// demand. Text format for parameters and results (spec section 2).
//
// Errors are data (spec requirement 8): the raw PGresult* is wrapped in
// the RAII pointer before anything looks at it, and a status other than
// COMMAND_OK / TUPLES_OK becomes std::unexpected(error{query, message,
// SQLSTATE, status}). A wait that times out finishes the connection and
// reports timeout; a reactor failure or a bad PQstatus marks it broken and
// reports connection, and the pool replaces it.
//
// During the handshake the socket may change under us -- PQconnectPoll
// closes and reopens it on a multi-address host -- so each connect-phase
// wait releases its reactor wrapper right after: the reactor never holds
// a wrapper on an fd libpq may close. After PGRES_POLLING_OK the fd is
// stable for the connection's life, and execute keeps its wrapper until
// finish(). One command per literal: PQsendQueryParams refuses several.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <libpq-fe.h>

#include <coro/concepts/io_reactor.h>
#include <coro/task.h>
#include <fstr/fstr.h>

#include "sql/concepts.h"
#include "sql/convert.h"
#include "sql/detail/rows.h"
#include "sql/detail/stmt_key.h"
#include "sql/error.h"
#include "sql/io.h"

namespace sql {
    namespace detail {
        // Text-format parameters for PQsendQueryParams: NUL-terminated
        // pointers, null for SQL NULL. A std::string or C string is passed
        // as-is; everything else is rendered into scratch storage, which is
        // what a string_view needs anyway, since it need not be terminated.
        template <std::size_t N>
        struct text_params final {
            std::array<std::optional<std::string>, N> storage{};
            std::array<const char*, N> values{};

            // Not explicit: with zero arguments `params{}` must still reach
            // this constructor (a pack-only constructor is a default one).
            template <bindable... Args>
            text_params(const Args&... args) {
                [[maybe_unused]] std::size_t i = 0;
                (place(i++, args), ...);
            }

        private:
            template <bindable T>
            void place(const std::size_t i, const T& v) {
                using U = std::remove_cvref_t<T>;
                if constexpr (std::same_as<U, std::string>) {
                    values[i] = v.c_str();
                } else if constexpr (text_like<U> && !std::same_as<U, std::string_view>) {
                    values[i] = v;
                } else {
                    storage[i] = to_text(v);
                    values[i] = storage[i] ? storage[i]->c_str() : nullptr;
                }
            }
        };
    }

    struct psql final {
        struct config final {
            std::string dsn;
            unsigned connections = 1;
            std::chrono::milliseconds connect_timeout{5000};
            // < 0 means no deadline, the reactor's own convention.
            std::chrono::milliseconds query_timeout{-1};
        };

        using io = reactor_ref;

        struct result_deleter final {
            void operator()(PGresult* const r) const noexcept {
                PQclear(r);
            }
        };

        using pgresult_ptr = std::unique_ptr<PGresult, result_deleter>;

        class cell final {
        public:
            cell(const PGresult* const res, const int row, const int col) noexcept : res_(res), row_(row), col_(col) {
            }

            [[nodiscard]] bool is_null() const noexcept {
                return PQgetisnull(res_, row_, col_) != 0;
            }

            template <typename T>
            [[nodiscard]] T as() const {
                if constexpr (detail::is_optional_v<T>) {
                    if (is_null()) return std::nullopt;
                    return T{as<typename T::value_type>()};
                } else {
                    if (is_null()) throw error{error_kind::conversion, "NULL into a non-optional type"};
                    auto r = from_text<T>(view());
                    if (!r) throw std::move(r.error());
                    return std::move(*r);
                }
            }

        private:
            [[nodiscard]] std::string_view view() const noexcept {
                return {PQgetvalue(res_, row_, col_), static_cast<std::size_t>(PQgetlength(res_, row_, col_))};
            }

            const PGresult* res_;
            int row_;
            int col_;
        };

        class row final {
        public:
            row(const PGresult* const res, const int i) noexcept : res_(res), i_(i) {
            }

            [[nodiscard]] cell operator[](const std::size_t c) const {
                if (c >= width()) throw error{error_kind::conversion, "column index out of range"};
                return cell{res_, i_, static_cast<int>(c)};
            }

            // PQfnumber folds an unquoted name to lower case, as the server
            // does; a quoted "MixedCase" name is looked up verbatim.
            [[nodiscard]] cell operator[](const std::string_view name) const {
                const std::string key{name};
                const int c = PQfnumber(res_, key.c_str());
                if (c < 0) throw error{error_kind::conversion, "no such column: " + key};
                return cell{res_, i_, c};
            }

            template <typename... Ts>
            [[nodiscard]] auto as() const {
                return detail::row_as<Ts...>(*this, width());
            }

        private:
            [[nodiscard]] std::size_t width() const noexcept {
                return static_cast<std::size_t>(PQnfields(res_));
            }

            const PGresult* res_;
            int i_;
        };

        class result final {
        public:
            result() = default;

            explicit result(pgresult_ptr res) noexcept : res_(std::move(res)) {
            }

            [[nodiscard]] std::size_t rows() const noexcept {
                return res_ ? static_cast<std::size_t>(PQntuples(res_.get())) : 0;
            }

            [[nodiscard]] std::size_t columns() const noexcept {
                return res_ ? static_cast<std::size_t>(PQnfields(res_.get())) : 0;
            }

            // Rows a write touched: the count in an INSERT, UPDATE, DELETE or
            // MERGE command tag, RETURNING or not. Every other tag reads as
            // 0, as a read does on the sqlite driver, so execute<> means the
            // same thing on both: PQcmdTuples also carries a count for
            // SELECT, FETCH and MOVE, where it is rows returned or skipped.
            [[nodiscard]] std::uint64_t affected() const noexcept {
                if (!res_) return 0;
                const std::string_view tag = PQcmdStatus(res_.get());
                const bool write = tag.starts_with("INSERT") || tag.starts_with("UPDATE")
                    || tag.starts_with("DELETE") || tag.starts_with("MERGE");
                if (!write) return 0;
                const std::string_view t = PQcmdTuples(res_.get());
                if (t.empty()) return 0;
                return from_text<std::uint64_t>(t).value_or(0);
            }

            [[nodiscard]] row operator[](const std::size_t i) const {
                if (i >= rows()) throw error{error_kind::conversion, "row index out of range"};
                return row{res_.get(), static_cast<int>(i)};
            }

            using iterator = detail::row_iterator<result>;

            [[nodiscard]] iterator begin() const noexcept {
                return {this, 0};
            }

            [[nodiscard]] iterator end() const noexcept {
                return {this, rows()};
            }

            // The escape hatch the README asks for: "just call PQ* methods".
            [[nodiscard]] const PGresult* raw() const noexcept {
                return res_.get();
            }

        private:
            pgresult_ptr res_;
        };

        class connection final {
        public:
            static coro::task<std::expected<std::unique_ptr<connection>, error>>
            open(const config& cfg, const io& io) {
                PGconn* const raw = PQconnectStart(cfg.dsn.c_str());
                if (raw == nullptr) co_return std::unexpected(error{error_kind::connect, "PQconnectStart: out of memory"});
                auto c = std::unique_ptr<connection>(new connection{raw, io, cfg.query_timeout});
                if (PQstatus(raw) == CONNECTION_BAD) co_return std::unexpected(error{error_kind::connect, PQerrorMessage(raw)});

                const auto deadline = deadline_for(cfg.connect_timeout);
                for (;;) {
                    const PostgresPollingStatusType poll = PQconnectPoll(raw);
                    if (poll == PGRES_POLLING_OK) break;
                    if (poll == PGRES_POLLING_FAILED) co_return std::unexpected(error{error_kind::connect, PQerrorMessage(raw)});
                    const auto want = poll == PGRES_POLLING_WRITING ? coro::interest::write : coro::interest::read;
                    const int fd = PQsocket(raw);
                    const auto st = co_await io.wait(fd, want, remaining(deadline));
                    io.release(fd);
                    if (st == coro::wait_status::timeout) co_return std::unexpected(error{error_kind::timeout, "connect timed out"});
                    if (st != coro::wait_status::ready) co_return std::unexpected(error{error_kind::connect, "reactor failed while connecting"});
                }
                PQsetnonblocking(raw, 1);
                PQsetNoticeReceiver(raw, [](void*, const PGresult*) {}, nullptr);
                c->fd_ = PQsocket(raw);
                co_return std::move(c);
            }

            ~connection() {
                finish();
            }

            connection(const connection&) = delete;
            connection& operator=(const connection&) = delete;

            [[nodiscard]] bool ok() const noexcept {
                return ok_;
            }

            void abandon() noexcept {
                ok_ = false;
            }

            template <fstr::fstr Q, bindable... Args>
            [[nodiscard]] coro::task<std::expected<result, error>> execute(const Args&... args) {
                if (conn_ == nullptr || !ok_) co_return std::unexpected(error{error_kind::connection, "connection is broken"});

                const detail::text_params<sizeof...(Args)> params{args...};
                if (PQsendQueryParams(conn_, detail::stmt_key<Q>::c_str, static_cast<int>(sizeof...(Args)), nullptr,
                                      params.values.data(), nullptr, nullptr, 0) == 0) {
                    co_return std::unexpected(broken());
                }

                const auto deadline = deadline_for(query_timeout_);
                for (;;) {
                    const int flushed = PQflush(conn_);
                    if (flushed == 0) break;
                    if (flushed < 0) co_return std::unexpected(broken());
                    if (auto failed = after_wait(co_await io_.wait(fd_, coro::interest::write, remaining(deadline)))) {
                        co_return std::unexpected(std::move(*failed));
                    }
                }

                pgresult_ptr last;
                for (;;) {
                    while (PQisBusy(conn_) != 0) {
                        if (auto failed = after_wait(co_await io_.wait(fd_, coro::interest::read, remaining(deadline)))) {
                            co_return std::unexpected(std::move(*failed));
                        }
                        if (PQconsumeInput(conn_) == 0) co_return std::unexpected(broken());
                    }
                    PGresult* const raw = PQgetResult(conn_);
                    if (raw == nullptr) break;
                    last.reset(raw);
                }

                // A backend that died under us answers a FATAL result and then
                // a bad status; the status is the truth about the connection.
                if (PQstatus(conn_) != CONNECTION_OK) {
                    ok_ = false;
                    co_return std::unexpected(error{error_kind::connection,
                                                    last ? PQresultErrorMessage(last.get()) : PQerrorMessage(conn_)});
                }
                if (!last) co_return std::unexpected(broken());

                const ExecStatusType status = PQresultStatus(last.get());
                if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
                    const char* const sqlstate = PQresultErrorField(last.get(), PG_DIAG_SQLSTATE);
                    co_return std::unexpected(error{error_kind::query, PQresultErrorMessage(last.get()),
                                                    sqlstate != nullptr ? sqlstate : "UNKNOWN", static_cast<int>(status)});
                }
                co_return result{std::move(last)};
            }

        private:
            using clock = std::chrono::steady_clock;

            connection(PGconn* const conn, const io& io, const std::chrono::milliseconds query_timeout) noexcept
                : conn_(conn), io_(io), query_timeout_(query_timeout) {
            }

            [[nodiscard]] static std::optional<clock::time_point> deadline_for(const std::chrono::milliseconds budget) noexcept {
                if (budget.count() < 0) return std::nullopt;
                return clock::now() + budget;
            }

            [[nodiscard]] static std::chrono::milliseconds remaining(const std::optional<clock::time_point> deadline) noexcept {
                if (!deadline) return std::chrono::milliseconds{-1};
                const auto left = std::chrono::ceil<std::chrono::milliseconds>(*deadline - clock::now());
                return left.count() < 0 ? std::chrono::milliseconds{0} : left;
            }

            // What one readiness wait on the connection's fd came to. nullopt
            // means ready; otherwise the error to report, with the connection
            // already finished (timeout) or marked broken (anything else).
            [[nodiscard]] std::optional<error> after_wait(const coro::wait_status st) {
                if (st == coro::wait_status::ready) return std::nullopt;
                if (st == coro::wait_status::timeout) {
                    finish();
                    return error{error_kind::timeout, "query timed out"};
                }
                ok_ = false;
                return error{error_kind::connection, "reactor failed while waiting on the connection"};
            }

            [[nodiscard]] error broken() noexcept {
                ok_ = false;
                return error{error_kind::connection, conn_ != nullptr ? PQerrorMessage(conn_) : "connection is closed"};
            }

            // Release the reactor's wrapper, then let libpq close the fd, then
            // null everything so a second call (the destructor after a
            // timeout) is a no-op.
            void finish() noexcept {
                if (conn_ == nullptr) return;
                if (fd_ >= 0) io_.release(fd_);
                PQfinish(conn_);
                conn_ = nullptr;
                fd_ = -1;
                ok_ = false;
            }

            PGconn* conn_;
            io io_;
            std::chrono::milliseconds query_timeout_;
            int fd_ = -1;
            bool ok_ = true;
        };
    };

    static_assert(driver<psql>);
}
