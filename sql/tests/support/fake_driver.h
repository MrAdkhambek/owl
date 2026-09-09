#pragma once

// A driver whose behaviour is a script: what open() answers, what execute()
// answers, which rows come back, and whether a call parks until the test
// resumes it. Enough to drive pool, query and transaction without a
// database, on one thread, deterministically. The fixture and driver the
// fake-driver tests share live here too.

#include <gtest/gtest.h>

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <coro/task.h>
#include <fstr/fstr.h>

#include <sql/concepts.h>
#include <sql/convert.h>
#include <sql/detail/rows.h>
#include <sql/detail/stmt_key.h>
#include <sql/error.h>
#include <sql/pool.h>

namespace sql_test {
    struct fake final {
        using cells = std::vector<std::optional<std::string>>;

        struct script final {
            std::deque<std::expected<void, sql::error>> opens;
            std::deque<std::expected<std::uint64_t, sql::error>> runs;
            std::deque<std::vector<cells>> rows;
            std::vector<std::string> log;
            int opened = 0;
            int destroyed = 0;
            bool suspend = false;
            bool suspend_open = false;
            std::vector<std::coroutine_handle<>> parked;

            void resume_all() {
                auto handles = std::move(parked);
                parked.clear();
                for (const auto h : handles) h.resume();
            }
        };

        struct config final {
            unsigned connections = 1;
            script* script_ = nullptr;
        };

        struct io final {
        };

        class cell final {
        public:
            explicit cell(const std::optional<std::string>* const v) noexcept : v_(v) {
            }

            [[nodiscard]] bool is_null() const noexcept {
                return !v_->has_value();
            }

            template <typename T>
            [[nodiscard]] T as() const {
                if constexpr (sql::detail::is_optional_v<T>) {
                    if (is_null()) return std::nullopt;
                    return T{as<typename T::value_type>()};
                } else {
                    if (is_null()) throw sql::error{sql::error_kind::conversion, "NULL into a non-optional"};
                    auto r = sql::from_text<T>(**v_);
                    if (!r) throw std::move(r.error());
                    return std::move(*r);
                }
            }

        private:
            const std::optional<std::string>* v_;
        };

        using result = sql::detail::rows_result<std::optional<std::string>, cell>;

        // Columns are named c0, c1, ... after the first row's width.
        static result make_result(std::vector<cells> rows, const std::uint64_t affected) {
            const std::size_t width = rows.empty() ? 0 : rows.front().size();
            std::vector<std::string> names;
            for (std::size_t i = 0; i < width; ++i) names.push_back("c" + std::to_string(i));
            std::vector<std::optional<std::string>> flat;
            flat.reserve(rows.size() * width);
            for (auto& r : rows) {
                for (auto& c : r) flat.push_back(std::move(c));
            }
            return result{std::move(names), std::move(flat), affected};
        }

        class connection final {
        public:
            explicit connection(script& s) noexcept : s_(&s) {
            }

            ~connection() {
                ++s_->destroyed;
            }

            static coro::task<std::expected<std::unique_ptr<connection>, sql::error>>
            open(const config& cfg, const io&) {
                script& s = *cfg.script_;
                ++s.opened;
                co_await park{&s, s.suspend_open};
                if (!s.opens.empty()) {
                    auto verdict = std::move(s.opens.front());
                    s.opens.pop_front();
                    if (!verdict) co_return std::unexpected(std::move(verdict.error()));
                }
                co_return std::make_unique<connection>(s);
            }

            [[nodiscard]] bool ok() const noexcept {
                return ok_;
            }

            void abandon() noexcept {
                ok_ = false;
            }

            template <fstr::fstr Q, sql::bindable... Args>
            [[nodiscard]] coro::task<std::expected<result, sql::error>> execute(const Args&...) {
                s_->log.emplace_back(sql::detail::stmt_key<Q>::text);
                co_await park{s_, s_->suspend};
                std::uint64_t affected = 1;
                if (!s_->runs.empty()) {
                    auto verdict = std::move(s_->runs.front());
                    s_->runs.pop_front();
                    if (!verdict) {
                        if (verdict.error().kind() == sql::error_kind::connection) ok_ = false;
                        co_return std::unexpected(std::move(verdict.error()));
                    }
                    affected = *verdict;
                }
                std::vector<cells> rows;
                if (!s_->rows.empty()) {
                    rows = std::move(s_->rows.front());
                    s_->rows.pop_front();
                }
                co_return make_result(std::move(rows), affected);
            }

        private:
            struct park final {
                script* s;
                bool should_park;

                bool await_ready() const noexcept {
                    return !should_park;
                }

                void await_suspend(const std::coroutine_handle<> h) const {
                    s->parked.push_back(h);
                }

                void await_resume() const noexcept {
                }
            };

            script* s_;
            bool ok_ = true;
        };
    };

    static_assert(sql::driver<fake>);

    // A scripted pool on one thread, the way every fake-driver test starts.
    struct pool_fixture final {
        fake::script s;
        sql::pool<fake> pool;

        explicit pool_fixture(const unsigned connections = 1) : pool({.connections = connections, .script_ = &s}, {}) {
        }
    };

    // Runs a task that must finish without parking: the point of the
    // single-threaded tests is that nothing waits on anything external.
    inline void drive(coro::task<>&& t) {
        t.start();
        EXPECT_TRUE(t.done());
    }
}
