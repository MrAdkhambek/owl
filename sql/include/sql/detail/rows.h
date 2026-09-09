#pragma once

// The parts of a result that do not depend on where its cells live: the
// exact-width row-to-tuple conversion, the row iterator, and a result that
// owns copied rows. sqlite and the test fake are rows_result over their
// own cell types; psql views a PGresult instead and takes the first two.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "sql/error.h"

namespace sql::detail {
    template <typename... Ts, typename Row, std::size_t... Is>
    std::tuple<Ts...> row_tuple(const Row& row, std::index_sequence<Is...>) {
        return std::tuple<Ts...>{row[Is].template as<Ts>()...};
    }

    // Exactly the row's width: a tuple narrower than the row is a mistake
    // this cannot tell from the intended one, so it refuses. One type
    // answers the value itself, several answer a tuple.
    template <typename... Ts, typename Row>
    [[nodiscard]] auto row_as(const Row& row, const std::size_t width) {
        if (width != sizeof...(Ts)) {
            throw error{error_kind::conversion, "column count does not match the requested types"};
        }
        if constexpr (sizeof...(Ts) == 1) {
            return row[0].template as<Ts...>();
        } else {
            return row_tuple<Ts...>(row, std::index_sequence_for<Ts...>{});
        }
    }

    // Iteration by index through the result's own bounds-checked
    // operator[]; comparing the index alone is enough for begin/end.
    template <typename Result>
    struct row_iterator final {
        const Result* r;
        std::size_t i;

        [[nodiscard]] auto operator*() const {
            return (*r)[i];
        }

        row_iterator& operator++() {
            ++i;
            return *this;
        }

        bool operator==(const row_iterator& o) const {
            return i == o.i;
        }
    };

    // A result that owns its rows: the column names, every cell in
    // row-major order, and the affected count. Cell is the driver's view
    // type, built from a pointer to one Value. One flat vector rather than
    // one per row, so a result costs two allocations however many rows.
    template <typename Value, typename Cell>
    class rows_result final {
    public:
        class row final {
        public:
            row(const rows_result* const r, const std::size_t i) noexcept : r_(r), i_(i) {
            }

            [[nodiscard]] Cell operator[](const std::size_t c) const {
                if (c >= r_->columns()) throw error{error_kind::conversion, "column index out of range"};
                return Cell{&r_->cells_[i_ * r_->columns() + c]};
            }

            [[nodiscard]] Cell operator[](const std::string_view name) const {
                return (*this)[r_->column(name)];
            }

            template <typename... Ts>
            [[nodiscard]] auto as() const {
                return row_as<Ts...>(*this, r_->columns());
            }

        private:
            const rows_result* r_;
            std::size_t i_;
        };

        rows_result() = default;

        rows_result(std::vector<std::string> names, std::vector<Value> cells, const std::uint64_t affected) noexcept
            : names_(std::move(names)), cells_(std::move(cells)), affected_(affected) {
        }

        [[nodiscard]] std::size_t rows() const noexcept {
            return names_.empty() ? 0 : cells_.size() / names_.size();
        }

        [[nodiscard]] std::size_t columns() const noexcept {
            return names_.size();
        }

        [[nodiscard]] std::uint64_t affected() const noexcept {
            return affected_;
        }

        [[nodiscard]] row operator[](const std::size_t i) const {
            if (i >= rows()) throw error{error_kind::conversion, "row index out of range"};
            return row{this, i};
        }

        [[nodiscard]] row_iterator<rows_result> begin() const noexcept {
            return {this, 0};
        }

        [[nodiscard]] row_iterator<rows_result> end() const noexcept {
            return {this, rows()};
        }

    private:
        [[nodiscard]] std::size_t column(const std::string_view name) const {
            for (std::size_t i = 0; i < names_.size(); ++i) {
                if (names_[i] == name) return i;
            }
            throw error{error_kind::conversion, "no such column: " + std::string{name}};
        }

        std::vector<std::string> names_;
        std::vector<Value> cells_;
        std::uint64_t affected_ = 0;
    };
}
