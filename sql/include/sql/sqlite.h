#pragma once

// sql::sqlite -- the SQLite driver. sqlite3_* calls are synchronous file
// I/O, so each pooled connection is pinned to its own thread: execute hops
// there, runs prepare / bind / step to completion, copies the rows out,
// and hops back through the scheduler_ref the pool was built with. The
// loop never blocks; the price is that rows are materialized on that
// thread -- in sqlite's own storage classes (INTEGER, FLOAT, TEXT, BLOB,
// NULL) rather than flattened to text, because sqlite already hands over
// a typed value and a text round trip would be work for nothing.
//
// The on-thread section is one noexcept member writing an expected. That
// is what guarantees the coroutine frame never completes off-thread:
// nothing on that path throws except allocation, and allocation failure
// terminates, as it would on the loop.
//
// Placeholders are $1..$N, shared with psql. sqlite numbers them by order
// of first appearance -- `select $2, $1` gives $1 index 2 -- so argument k
// is bound through the index sqlite3_bind_parameter_index("$k") answers,
// resolved once when the statement is prepared. Statements are cached per
// connection, keyed by the literal's identity. One statement per literal:
// sqlite3_prepare_v2 stops at the first semicolon and the tail is ignored.

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <sqlite3.h>

#include <coro/executors/static_thread_pool.h>
#include <coro/task.h>
#include <fstr/fstr.h>

#include "sql/concepts.h"
#include "sql/convert.h"
#include "sql/detail/rows.h"
#include "sql/detail/stmt_key.h"
#include "sql/error.h"
#include "sql/io.h"

namespace sql {
    struct sqlite final {
        struct config final {
            std::string path;
            unsigned connections = 1;
            std::chrono::milliseconds busy_timeout{5000};
        };

        using io = scheduler_ref;

        struct null_t final {
            bool operator==(const null_t&) const noexcept = default;
        };

        using value = std::variant<null_t, std::int64_t, double, std::string, blob>;

        class cell final {
        public:
            explicit cell(const value* const v) noexcept : v_(v) {
            }

            [[nodiscard]] bool is_null() const noexcept {
                return std::holds_alternative<null_t>(*v_);
            }

            // Direct when the storage classes match; from_text for TEXT into
            // a number or bool, because a column without affinity can hold
            // '42'; a conversion error for every other pairing, rather than
            // a guess.
            template <typename T>
            [[nodiscard]] T as() const {
                if constexpr (detail::is_optional_v<T>) {
                    if (is_null()) return std::nullopt;
                    return T{as<typename T::value_type>()};
                } else {
                    return std::visit([](const auto& held) -> T {
                        return convert<T>(held);
                    }, *v_);
                }
            }

        private:
            template <typename T>
            static T convert(const null_t&) {
                throw error{error_kind::conversion, "NULL into a non-optional type"};
            }

            template <typename T>
            static T convert(const std::int64_t held) {
                if constexpr (std::same_as<T, bool>) {
                    return held != 0;
                } else if constexpr (detail::sql_integral<T>) {
                    if (!std::in_range<T>(held)) throw error{error_kind::conversion, "INTEGER out of range for the requested type"};
                    return static_cast<T>(held);
                } else if constexpr (std::floating_point<T>) {
                    return static_cast<T>(held);
                } else {
                    throw error{error_kind::conversion, "INTEGER cannot convert to the requested type"};
                }
            }

            template <typename T>
            static T convert(const double held) {
                if constexpr (std::floating_point<T>) {
                    return static_cast<T>(held);
                } else {
                    throw error{error_kind::conversion, "FLOAT cannot convert to the requested type"};
                }
            }

            template <typename T>
            static T convert(const std::string& held) {
                if constexpr (std::same_as<T, std::string>) {
                    return held;
                } else if constexpr (std::same_as<T, std::string_view>) {
                    return std::string_view{held};
                } else if constexpr (std::same_as<T, bool> || detail::sql_integral<T> || std::floating_point<T>) {
                    auto r = from_text<T>(held);
                    if (!r) throw std::move(r.error());
                    return std::move(*r);
                } else {
                    throw error{error_kind::conversion, "TEXT cannot convert to the requested type"};
                }
            }

            template <typename T>
            static T convert(const blob& held) {
                if constexpr (std::same_as<T, blob>) {
                    return held;
                } else {
                    throw error{error_kind::conversion, "BLOB cannot convert to the requested type"};
                }
            }

            const value* v_;
        };

        using result = detail::rows_result<value, cell>;
        using row = result::row;

        class connection final {
        public:
            static coro::task<std::expected<std::unique_ptr<connection>, error>>
            open(const config& cfg, const io& io) {
                auto c = std::unique_ptr<connection>(new connection{io});
                co_await c->thread_.schedule();
                auto opened = c->open_on_thread(cfg);
                co_await io.schedule();
                if (!opened) co_return std::unexpected(std::move(opened.error()));
                co_return std::move(c);
            }

            // Runs on the loop after every query has hopped back, so the
            // thread is idle: sequential use from another thread is what
            // SQLITE_OPEN_NOMUTEX permits.
            ~connection() {
                for (const auto& [id, prepared] : stmts_) sqlite3_finalize(prepared.stmt);
                if (db_ != nullptr) sqlite3_close(db_);
            }

            connection(const connection&) = delete;
            connection& operator=(const connection&) = delete;

            [[nodiscard]] bool ok() const noexcept {
                return ok_;
            }

            void abandon() noexcept {
                ok_ = false;
            }

            [[nodiscard]] std::size_t cached_statements() const noexcept {
                return stmts_.size();
            }

            template <fstr::fstr Q, bindable... Args>
            [[nodiscard]] coro::task<std::expected<result, error>> execute(const Args&... args) {
                co_await thread_.schedule();
                auto out = run_on_thread<Q>(args...);
                co_await io_.schedule();
                co_return out;
            }

        private:
            // A cached statement with its $k -> sqlite index map, so a bind
            // never looks a name up again.
            struct statement final {
                sqlite3_stmt* stmt;
                std::vector<int> params;
            };

            // Reset and unbind on every way out of run_on_thread, so a failed
            // bind or step never leaves the cached statement mid-flight.
            struct reset_guard final {
                sqlite3_stmt* stmt;

                ~reset_guard() {
                    sqlite3_reset(stmt);
                    sqlite3_clear_bindings(stmt);
                }
            };

            explicit connection(const io& io) noexcept : io_(io), thread_(1) {
            }

            [[nodiscard]] std::expected<void, error> open_on_thread(const config& cfg) noexcept {
                const int rc = sqlite3_open_v2(cfg.path.c_str(), &db_,
                                               SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
                if (rc != SQLITE_OK) {
                    error e{error_kind::connect, db_ != nullptr ? sqlite3_errmsg(db_) : sqlite3_errstr(rc), {}, rc};
                    if (db_ != nullptr) {
                        sqlite3_close(db_);
                        db_ = nullptr;
                    }
                    return std::unexpected(std::move(e));
                }
                sqlite3_busy_timeout(db_, static_cast<int>(cfg.busy_timeout.count()));
                sqlite3_extended_result_codes(db_, 1);
                return {};
            }

            template <fstr::fstr Q, bindable... Args>
            [[nodiscard]] std::expected<result, error> run_on_thread(const Args&... args) noexcept {
                const statement* const prepared = this->prepared<Q>();
                if (prepared == nullptr) return std::unexpected(failure(sqlite3_extended_errcode(db_)));
                sqlite3_stmt* const stmt = prepared->stmt;
                const reset_guard guard{stmt};

                // An index of 0 is sqlite's own "no such parameter", which
                // bind answers with SQLITE_RANGE.
                [[maybe_unused]] std::size_t k = 0;
                int rc = SQLITE_OK;
                ((rc = rc == SQLITE_OK ? bind_at(stmt, k < prepared->params.size() ? prepared->params[k] : 0, args) : rc, ++k), ...);
                if (rc != SQLITE_OK) return std::unexpected(failure(rc));

                const int ncol = sqlite3_column_count(stmt);
                std::vector<std::string> names;
                names.reserve(static_cast<std::size_t>(ncol));
                for (int i = 0; i < ncol; ++i) names.emplace_back(sqlite3_column_name(stmt, i));
                std::vector<value> cells;
                for (;;) {
                    rc = sqlite3_step(stmt);
                    if (rc == SQLITE_ROW) {
                        read_row(stmt, ncol, cells);
                        continue;
                    }
                    if (rc == SQLITE_DONE) break;
                    return std::unexpected(failure(rc));
                }
                const std::uint64_t affected = sqlite3_stmt_readonly(stmt) ? 0 : static_cast<std::uint64_t>(sqlite3_changes64(db_));
                return result{std::move(names), std::move(cells), affected};
            }

            template <fstr::fstr Q>
            [[nodiscard]] const statement* prepared() noexcept {
                const auto it = stmts_.find(detail::stmt_key<Q>::id);
                if (it != stmts_.end()) return &it->second;
                sqlite3_stmt* stmt = nullptr;
                const int rc = sqlite3_prepare_v2(db_, detail::stmt_key<Q>::c_str,
                                                  static_cast<int>(detail::stmt_key<Q>::text.size()), &stmt, nullptr);
                if (rc != SQLITE_OK || stmt == nullptr) return nullptr;
                std::vector<int> params;
                params.reserve(detail::stmt_key<Q>::scan.count);
                for (std::size_t k = 1; k <= detail::stmt_key<Q>::scan.count; ++k) {
                    char name[16];
                    std::snprintf(name, sizeof name, "$%u", static_cast<unsigned>(k));
                    params.push_back(sqlite3_bind_parameter_index(stmt, name));
                }
                return &stmts_.emplace(detail::stmt_key<Q>::id, statement{stmt, std::move(params)}).first->second;
            }

            // SQLITE_STATIC everywhere: the argument outlives the step loop
            // because the caller's frame is suspended across the hop.
            template <bindable T>
            [[nodiscard]] static int bind_at(sqlite3_stmt* const stmt, const int idx, const T& v) noexcept {
                using U = std::remove_cvref_t<T>;
                if constexpr (std::same_as<U, std::nullopt_t>) {
                    return sqlite3_bind_null(stmt, idx);
                } else if constexpr (detail::is_optional_v<U>) {
                    if (!v) return sqlite3_bind_null(stmt, idx);
                    return bind_at(stmt, idx, *v);
                } else if constexpr (std::same_as<U, bool>) {
                    return sqlite3_bind_int(stmt, idx, v ? 1 : 0);
                } else if constexpr (detail::sql_integral<U>) {
                    return sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(v));
                } else if constexpr (std::floating_point<U>) {
                    return sqlite3_bind_double(stmt, idx, static_cast<double>(v));
                } else if constexpr (detail::text_like<U>) {
                    const std::string_view s{v};
                    return sqlite3_bind_text64(stmt, idx, s.data(), s.size(), SQLITE_STATIC, SQLITE_UTF8);
                } else {
                    const blob_view b{v};
                    // A null pointer binds NULL, so an empty blob must be spelled
                    // as a zero-length zeroblob.
                    if (b.empty()) return sqlite3_bind_zeroblob(stmt, idx, 0);
                    return sqlite3_bind_blob64(stmt, idx, b.data(), b.size(), SQLITE_STATIC);
                }
            }

            static void read_row(sqlite3_stmt* const stmt, const int ncol, std::vector<value>& cells) {
                for (int i = 0; i < ncol; ++i) {
                    switch (sqlite3_column_type(stmt, i)) {
                        case SQLITE_INTEGER:
                            cells.emplace_back(std::in_place_type<std::int64_t>, static_cast<std::int64_t>(sqlite3_column_int64(stmt, i)));
                            break;
                        case SQLITE_FLOAT:
                            cells.emplace_back(std::in_place_type<double>, sqlite3_column_double(stmt, i));
                            break;
                        case SQLITE_TEXT: {
                            const auto* const t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                            const auto n = static_cast<std::size_t>(sqlite3_column_bytes(stmt, i));
                            cells.emplace_back(std::in_place_type<std::string>, t, n);
                            break;
                        }
                        case SQLITE_BLOB: {
                            const auto* const b = static_cast<const std::byte*>(sqlite3_column_blob(stmt, i));
                            const auto n = static_cast<std::size_t>(sqlite3_column_bytes(stmt, i));
                            cells.emplace_back(std::in_place_type<blob>, b, b + n);
                            break;
                        }
                        default:
                            cells.emplace_back(std::in_place_type<null_t>);
                            break;
                    }
                }
            }

            // The families that mean the handle itself is no longer trustworthy
            // mark the connection broken so the pool replaces it; everything
            // else is the statement's problem and the connection stays.
            [[nodiscard]] error failure(const int rc) noexcept {
                const int primary = rc & 0xff;
                const bool broken = primary == SQLITE_IOERR || primary == SQLITE_MISUSE
                    || primary == SQLITE_NOMEM || primary == SQLITE_CORRUPT;
                if (broken) ok_ = false;
                return error{broken ? error_kind::connection : error_kind::query, sqlite3_errmsg(db_), {}, rc};
            }

            io io_;
            coro::static_thread_pool thread_;
            sqlite3* db_ = nullptr;
            std::unordered_map<const void*, statement> stmts_;
            bool ok_ = true;
        };
    };

    static_assert(driver<sqlite>);
}
