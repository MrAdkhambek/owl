#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "sql/column.h"
#include "sql/detail/table.h"
#include "sql/dialect.h"
#include "sql/row.h"

namespace sql {
    // The owned query answer: a driver-neutral table. Public headers never
    // leak sqlite3*/PGresult* -- the drivers copy the data off before
    // returning. Rows and columns are views into this object; none of them
    // outlives it.
    class result final {
    public:
        result() = default;

        // Drivers and tests only: built from the detail table.
        explicit result(detail::table t, const sql::dialect from) noexcept
            : t_(std::make_unique<detail::table>(std::move(t))), from_(from) {
        }

        [[nodiscard]] std::size_t size() const { return t_ ? t_->rows : 0; }
        [[nodiscard]] bool empty() const { return size() == 0; }
        [[nodiscard]] std::size_t columns() const { return t_ ? t_->cols : 0; }

        [[nodiscard]] std::string_view name(const std::size_t col) const {
            if (col >= columns()) throw std::out_of_range("sql: column index out of range");
            return t_->names[col];
        }

        [[nodiscard]] std::size_t index(const std::string_view name) const {
            for (std::size_t i = 0; i < t_->names.size(); ++i)
                if (name == t_->names[i]) return i;
            throw std::out_of_range("sql: no such column");
        }

        // Unchecked for row index (taopq); at throws.
        [[nodiscard]] row operator[](const std::size_t r) const { return row{this, r}; }

        [[nodiscard]] row at(const std::size_t r) const {
            if (r >= size()) throw std::out_of_range("sql: row index out of range");
            return (*this)[r];
        }

        [[nodiscard]] bool has_rows_affected() const { return t_ && t_->has_affected; }
        [[nodiscard]] std::size_t rows_affected() const { return t_ ? t_->affected : 0; }

        class iterator final {
        public:
            using value_type = row;
            using difference_type = std::ptrdiff_t;

            explicit iterator(const result* r, const std::size_t i) : r_(r), i_(i) {}
            [[nodiscard]] row operator*() const { return (*r_)[i_]; }
            iterator& operator++() { ++i_; return *this; }
            [[nodiscard]] bool operator!=(const iterator& o) const { return i_ != o.i_; }

        private:
            const result* r_;
            std::size_t i_;
        };

        [[nodiscard]] iterator begin() const { return iterator{this, 0}; }
        [[nodiscard]] iterator end() const { return iterator{this, size()}; }

        // Row 0, column 0. Throws on the wrong shape -- a mis-shaped
        // .as<T>() is a programmer error, not SQL failure.
        template <typename T>
        [[nodiscard]] T as() const {
            shape_check(1, 1);
            return (*this)[0][0].as<T>();
        }

        template <typename T>
        [[nodiscard]] std::optional<T> optional() const {
            if (empty()) return std::nullopt;
            shape_check(1, 1);
            return (*this)[0][0].optional<T>();
        }

        // First column of every row.
        template <typename T>
        [[nodiscard]] std::vector<T> vector() const {
            std::vector<T> out;
            out.reserve(size());
            for (const auto& r : *this) out.push_back(r[0].as<T>());
            return out;
        }

        // First row as a tuple/pair over all its columns.
        template <typename T>
        [[nodiscard]] T tuple() const {
            shape_check(1, std::tuple_size_v<T>);
            return (*this)[0].as<T>();
        }

    private:
        friend class column;
        friend class row;

        void shape_check(const std::size_t rows, const std::size_t cols) const {
            if (size() != rows || columns() != cols)
                throw std::runtime_error("sql: wrong row/column count for as<T>");
        }

        [[nodiscard]] cell cell_at(const std::size_t r, const std::size_t c) const {
            return cell{.bytes = t_->cell_bytes(r, c), .from = from_, .is_null = t_->nulls[r * t_->cols + c]};
        }

        [[nodiscard]] std::string_view column_name(const std::size_t c) const { return t_->names[c]; }

        std::unique_ptr<detail::table> t_;
        sql::dialect from_{};
    };

    class row_iterator final {
    public:
        using value_type = column;
        using difference_type = std::ptrdiff_t;

        row_iterator(const result* r, const std::size_t row, const std::size_t col) : r_(r), row_(row), col_(col) {}
        [[nodiscard]] column operator*() const { return (*r_)[row_][col_]; }
        row_iterator& operator++() { ++col_; return *this; }
        [[nodiscard]] bool operator!=(const row_iterator& o) const { return col_ != o.col_; }

    private:
        const result* r_;
        std::size_t row_;
        std::size_t col_;
    };

    // column / row inline members, deferred until result was complete.
    inline std::string_view column::name() const { return r_->column_name(col_); }
    inline bool column::is_null() const { return r_->cell_at(row_, col_).is_null; }
    inline const char* column::get() const { return r_->cell_at(row_, col_).bytes.data(); }
    inline std::string_view column::bytes() const { return r_->cell_at(row_, col_).bytes; }
    inline cell column::as_cell() const { return r_->cell_at(row_, col_); }
    inline bool column::operator==(null_t) const { return is_null(); }

    inline column row::operator[](const std::size_t col) const { return column{r_, row_, col}; }
    inline column row::operator[](const std::string_view name) const {
        try {
            return column{r_, row_, r_->index(name)};
        } catch (const std::out_of_range&) {
            throw std::out_of_range(std::string{"sql: no such column: "}.append(name));
        }
    }
    inline column row::at(const std::size_t col) const {
        if (col >= r_->columns()) throw std::out_of_range("sql: column index out of range");
        return (*this)[col];
    }
    inline std::size_t row::size() const { return r_->columns(); }
    inline auto row::begin() const { return row_iterator{r_, row_, 0}; }
    inline auto row::end() const { return row_iterator{r_, row_, r_->columns()}; }
}
