#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

#include "sql/cell.h"
#include "sql/null.h"
#include "sql/result_traits.h"

namespace sql {
    class result;
    class row;

    // Non-owning view of one cell: invalid after the result is destroyed.
    class column final {
    public:
        [[nodiscard]] std::string_view name() const;
        [[nodiscard]] std::size_t index() const { return col_; }
        [[nodiscard]] bool is_null() const;
        // NUL-terminated C string view of the cell: valid because every
        // stored cell carries the sentinel one past its end. On a NULL cell
        // the view is empty -- check is_null() first, as taopq also demands.
        [[nodiscard]] const char* get() const;
        // The length-carrying form -- what blobs need.
        [[nodiscard]] std::string_view bytes() const;
        [[nodiscard]] cell as_cell() const;

        template <typename T>
        [[nodiscard]] T as() const {
            return result_traits<T>::from(as_cell());
        }

        template <typename T>
        [[nodiscard]] std::optional<T> optional() const {
            return result_traits<std::optional<T>>::from(as_cell());
        }

        [[nodiscard]] bool operator==(null_t) const;

    private:
        friend class row;
        friend class result;
        column(const result* r, std::size_t row, std::size_t col) : r_(r), row_(row), col_(col) {}

        const result* r_;
        std::size_t row_{};
        std::size_t col_{};
    };
}
