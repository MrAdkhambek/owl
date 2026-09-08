#pragma once

// A driver whose behaviour is a script: what open() answers, what execute()
// answers, which rows come back, and whether a call parks until the test
// resumes it. Enough to drive pool, query and transaction without a
// database, on one thread, deterministically.

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <coro/task.h>
#include <fstr/fstr.h>

#include <sql/concepts.h>
#include <sql/convert.h>
#include <sql/detail/stmt_key.h>
#include <sql/error.h>

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

        class result;

        class row final {
        public:
            row(const result* const r, const std::size_t i) noexcept : r_(r), i_(i) {
            }

            [[nodiscard]] cell operator[](std::size_t c) const;
            [[nodiscard]] cell operator[](std::string_view name) const;

            template <typename... Ts>
            [[nodiscard]] auto as() const {
                if (width() != sizeof...(Ts)) {
                    throw sql::error{sql::error_kind::conversion, "column count does not match the requested types"};
                }
                if constexpr (sizeof...(Ts) == 1) {
                    return (*this)[0].template as<Ts...>();
                } else {
                    return as_tuple<Ts...>(std::index_sequence_for<Ts...>{});
                }
            }

        private:
            [[nodiscard]] std::size_t width() const noexcept;

            template <typename... Ts, std::size_t... Is>
            std::tuple<Ts...> as_tuple(std::index_sequence<Is...>) const {
                return std::tuple<Ts...>{(*this)[Is].template as<Ts>()...};
            }

            const result* r_;
            std::size_t i_;
        };

        class result final {
        public:
            result() = default;

            result(std::vector<cells> rows, const std::uint64_t affected) : rows_(std::move(rows)), affected_(affected) {
                const std::size_t width = rows_.empty() ? 0 : rows_.front().size();
                for (std::size_t i = 0; i < width; ++i) names_.push_back("c" + std::to_string(i));
            }

            [[nodiscard]] std::size_t rows() const noexcept {
                return rows_.size();
            }

            [[nodiscard]] std::size_t columns() const noexcept {
                return names_.size();
            }

            [[nodiscard]] std::uint64_t affected() const noexcept {
                return affected_;
            }

            [[nodiscard]] row operator[](const std::size_t i) const {
                if (i >= rows_.size()) throw sql::error{sql::error_kind::conversion, "row index out of range"};
                return row{this, i};
            }

            struct iterator final {
                const result* r;
                std::size_t i;

                row operator*() const {
                    return row{r, i};
                }

                iterator& operator++() {
                    ++i;
                    return *this;
                }

                bool operator==(const iterator& o) const {
                    return i == o.i;
                }
            };

            [[nodiscard]] iterator begin() const noexcept {
                return {this, 0};
            }

            [[nodiscard]] iterator end() const noexcept {
                return {this, rows_.size()};
            }

            [[nodiscard]] const cells& at(const std::size_t i) const {
                return rows_[i];
            }

            [[nodiscard]] const std::vector<std::string>& names() const noexcept {
                return names_;
            }

        private:
            std::vector<cells> rows_;
            std::vector<std::string> names_;
            std::uint64_t affected_ = 0;
        };

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
                co_return result{std::move(rows), affected};
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

    inline std::size_t fake::row::width() const noexcept {
        return r_->columns();
    }

    inline fake::cell fake::row::operator[](const std::size_t c) const {
        const auto& cs = r_->at(i_);
        if (c >= cs.size()) throw sql::error{sql::error_kind::conversion, "column index out of range"};
        return cell{&cs[c]};
    }

    inline fake::cell fake::row::operator[](const std::string_view name) const {
        const auto& n = r_->names();
        for (std::size_t i = 0; i < n.size(); ++i) {
            if (n[i] == name) return (*this)[i];
        }
        throw sql::error{sql::error_kind::conversion, "no such column: " + std::string{name}};
    }

    static_assert(sql::driver<fake>);
}
