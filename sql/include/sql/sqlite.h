#pragma once

#ifndef OWL_ENABLE_SQLITE
#error "sql/sqlite.h requires OWL_ENABLE_SQLITE (cmake -DOWN_ENABLE_SQLITE=ON, link owl::sql_sqlite)"
#endif

#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include <coro/task.h>

#include "sql/bind.h"
#include "sql/dialect.h"
#include "sql/error.h"
#include "sql/resume.h"
#include "sql/result.h"
#include "sql/detail/table.h"

namespace sql {
    namespace detail {
        // Runs one statement to completion on the reactor thread: prepare,
        // bind, step-copy, finalize. Everything is copied off before
        // finalize. sqlite3_column_type() is read BEFORE any accessor --
        // _text then _blob on one column is undefined (text converts in
        // place) -- so each column is touched through exactly one accessor
        // pair. Binds use SQLITE_STATIC: the job owns them until finalize.
        [[nodiscard]] inline std::expected<result, sql::error>
        execute_statement(sqlite3* const db, const std::string& sql_text, const std::vector<bind_value>& binds) {
            sqlite3_stmt* stmt = nullptr;
            const char* tail = nullptr;
            if (sqlite3_prepare_v2(db, sql_text.c_str(), -1, &stmt, &tail) != SQLITE_OK) {
                return std::unexpected(sql::error{.message = sqlite3_errmsg(db), .code = sqlite3_errcode(db)});
            }
            // Anything but whitespace after the first statement means a
            // second one was smuggled past the compile-time scan.
            if (tail != nullptr && std::string_view{tail}.find_first_not_of(" \t\r\n;") != std::string_view::npos) {
                sqlite3_finalize(stmt);
                return std::unexpected(sql::error{.message = "sql: one statement per call"});
            }

            detail::table t;
            const int cols = sqlite3_column_count(stmt);
            for (int c = 0; c < cols; ++c) {
                const char* const name = sqlite3_column_name(stmt, c);
                t.add_column(name != nullptr ? name : "");
            }

            for (std::size_t i = 0; i < binds.size(); ++i) {
                const int idx = static_cast<int>(i) + 1;
                const int rc = std::visit(
                    [&](const auto& v) {
                        using V = std::decay_t<decltype(v)>;
                        if constexpr (std::is_same_v<V, std::nullptr_t>) {
                            return sqlite3_bind_null(stmt, idx);
                        } else if constexpr (std::is_same_v<V, std::int64_t>) {
                            return sqlite3_bind_int64(stmt, idx, v);
                        } else if constexpr (std::is_same_v<V, double>) {
                            return sqlite3_bind_double(stmt, idx, v);
                        } else if constexpr (std::is_same_v<V, std::string>) {
                            return sqlite3_bind_text(stmt, idx, v.c_str(), static_cast<int>(v.size()), SQLITE_STATIC);
                        } else {
                            return sqlite3_bind_blob(stmt, idx, v.data(), static_cast<int>(v.size()), SQLITE_STATIC);
                        }
                    },
                    binds[i].value);
                if (rc != SQLITE_OK) {
                    const sql::error e{.message = sqlite3_errmsg(db), .code = sqlite3_errcode(db)};
                    sqlite3_finalize(stmt);
                    return std::unexpected(e);
                }
            }

            for (;;) {
                const int rc = sqlite3_step(stmt);
                if (rc == SQLITE_ROW) {
                    for (int c = 0; c < cols; ++c) {
                        if (sqlite3_column_type(stmt, c) == SQLITE_NULL) {
                            t.add_cell({}, true);
                            continue;
                        }
                        const void* const data = sqlite3_column_type(stmt, c) == SQLITE_BLOB
                                                     ? sqlite3_column_blob(stmt, c)
                                                     : sqlite3_column_text(stmt, c);
                        const int n = sqlite3_column_bytes(stmt, c);
                        t.add_cell(std::string_view{static_cast<const char*>(data), static_cast<std::size_t>(n)}, false);
                    }
                    t.end_row();
                } else if (rc == SQLITE_DONE) {
                    t.affected = static_cast<std::size_t>(sqlite3_changes(db));
                    t.has_affected = true;
                    break;
                } else {
                    const sql::error e{.message = sqlite3_errmsg(db), .code = sqlite3_errcode(db)};
                    sqlite3_finalize(stmt);
                    return std::unexpected(e);
                }
            }
            sqlite3_finalize(stmt);
            return result{std::move(t), dialect::sqlite};
        }
    }

    class sqlite final {
    public:
        static constexpr sql::dialect dialect = sql::dialect::sqlite;
        static constexpr bool owns_thread = true;

        struct config final {
            std::string path;
            std::chrono::milliseconds busy_timeout{5000};
        };

        [[nodiscard]] static std::size_t pool_size(const config&) {
            return 1;
        }

        // The single-thread reactor: one thread owns the sqlite3 handle and
        // drains the job queue. sqlite has no async API and file I/O is not
        // pollable, so blocking calls run here and completions hop back
        // through the origin's resumer.
        class reactor final {
        public:
            // std::function, not move_only_function: Apple Clang's libc++
            // does not ship the latter, and nothing here needs move-only --
            // the captures (shared state, the work std::function, the
            // copyable resumer) are all copyable.
            using job = std::function<void(sqlite3*&)>;

            reactor() : thread_([this] {
                run();
            }) {
            }

            ~reactor() {
                {
                    const std::lock_guard lock(mutex_);
                    stopping_ = true;
                }
                cv_.notify_all();
                thread_.join();
                if (db_ != nullptr) sqlite3_close(db_);
            }

            reactor(const reactor&) = delete;
            reactor& operator=(const reactor&) = delete;

            void post(job&& j) {
                {
                    const std::lock_guard lock(mutex_);
                    queue_.push_back(std::move(j));
                }
                cv_.notify_one();
            }

        private:
            // Drains everything queued before exiting: completions of
            // in-flight statements must fire even during teardown.
            void run() {
                for (;;) {
                    job j;
                    {
                        std::unique_lock lock(mutex_);
                        cv_.wait(lock, [this] {
                            return stopping_ || !queue_.empty();
                        });
                        if (queue_.empty()) return;
                        j = std::move(queue_.front());
                        queue_.pop_front();
                    }
                    j(db_);
                }
            }

            std::mutex mutex_;
            std::condition_variable cv_;
            std::deque<job> queue_;
            bool stopping_ = false;
            sqlite3* db_ = nullptr;
            std::thread thread_;
        };

        // The alias must come after the definition: an alias to an
        // elaborated-type-specifier before the class body would declare a
        // different, forever-incomplete sql::reactor.
        using reactor_type = reactor;

        class connection final {
        public:
            connection(const config cfg, reactor& r) : cfg_(cfg), reactor_(r) {
            }

            connection(const connection&) = delete;
            connection& operator=(const connection&) = delete;

            [[nodiscard]] bool healthy() const {
                return true;  // sqlite connections do not die underneath us
            }

            // Opens the database on the reactor thread (WAL + busy
            // timeout). The resumer is the caller's -- completions hop back
            // through it, exactly like statement completions.
            [[nodiscard]] coro::task<std::expected<void, sql::error>> connect(const sql::resumer* const res) {
                hop<void> h{
                    .r = reactor_,
                    .res = res != nullptr ? *res : sql::resumer{},
                    .work = [cfg = cfg_](sqlite3*& db, auto& st) {
                        if (db != nullptr) return;
                        if (sqlite3_open_v2(cfg.path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK) {
                            st.out = std::unexpected(sql::error{
                                .message = db != nullptr ? sqlite3_errmsg(db) : "sql: sqlite3_open failed",
                                .code = db != nullptr ? sqlite3_errcode(db) : SQLITE_CANTOPEN});
                            if (db != nullptr) {
                                sqlite3_close(db);
                                db = nullptr;
                            }
                            return;
                        }
                        sqlite3_busy_timeout(db, static_cast<int>(cfg.busy_timeout.count()));
                        // WAL: readers do not block the writer. On :memory:
                        // the pragma is accepted and reports "memory".
                        char* err = nullptr;
                        sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, &err);
                        sqlite3_free(err);
                    }};
                co_return co_await h;
            }

            [[nodiscard]] coro::task<std::expected<result, sql::error>>
            run(const std::string_view sql_text, const std::span<const bind_value> binds, const sql::resumer* const res) {
                hop<result> h{
                    .r = reactor_,
                    .res = res != nullptr ? *res : sql::resumer{},
                    .work = [sql_text = std::string{sql_text},
                             binds = std::vector<bind_value>{binds.begin(), binds.end()}](sqlite3*& db, auto& st) {
                        st.out = detail::execute_statement(db, sql_text, binds);
                    }};
                co_return co_await h;
            }

        private:
            config cfg_;
            reactor& reactor_;
        };

    private:
        // Parks the caller until the reactor thread ran one job. The job is
        // submitted from await_suspend -- after the continuation is
        // recorded -- so it cannot complete before the parked handle
        // exists. The resumer is copied out of the source at call time.
        template <typename T>
        struct hop final {
            struct state final {
                std::expected<T, sql::error> out{};
                std::coroutine_handle<> cont{};
            };

            reactor& r;
            sql::resumer res;
            std::function<void(sqlite3*&, state&)> work;

            bool await_ready() const noexcept {
                return false;
            }

            void await_suspend(const std::coroutine_handle<> h) {
                const auto s = slot;
                s->cont = h;
                r.post([s, work = std::move(work), res = res](sqlite3*& db) mutable {
                    work(db, *s);
                    res.post(s->cont);
                });
            }

            [[nodiscard]] std::expected<T, sql::error> await_resume() const {
                return std::move(slot->out);
            }

            std::shared_ptr<state> slot = std::make_shared<state>();
        };
    };
}
