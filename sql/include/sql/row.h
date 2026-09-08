#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "sql/column.h"
#include "sql/result_traits.h"

namespace sql {
    class result;

    namespace detail {
        // Tuple-like = has tuple_size (covers std::tuple and std::pair).
        template <typename T, typename = void>
        struct is_tuple_like : std::false_type {};

        template <typename T>
        struct is_tuple_like<T, std::void_t<decltype(std::tuple_size<T>::value)>> : std::true_type {};

        template <typename T>
        inline constexpr bool is_tuple_like_v = is_tuple_like<std::remove_cvref_t<T>>::value;
    }

    // Non-owning view of one row: invalid after the result is destroyed.
    class row final {
    public:
        // Unchecked for index (taopq); name lookup and at throw.
        [[nodiscard]] column operator[](const std::size_t col) const;
        [[nodiscard]] column operator[](const std::string_view name) const;
        [[nodiscard]] column at(const std::size_t col) const;
        [[nodiscard]] std::size_t size() const;

        // Iterators are defined in result.h (they hand out column views).
        [[nodiscard]] auto begin() const;
        [[nodiscard]] auto end() const;

        // A tuple/pair consumes N columns left to right; any other T maps
        // from the first column.
        template <typename T>
        [[nodiscard]] T as() const {
            if constexpr (detail::is_tuple_like_v<T>) {
                return row_as_tuple<T>(std::make_index_sequence<std::tuple_size_v<T>>{});
            } else {
                return (*this)[0].as<T>();
            }
        }

    private:
        friend class result;
        row(const result* r, std::size_t row) : r_(r), row_(row) {}

        template <typename T, std::size_t... I>
        [[nodiscard]] T row_as_tuple(std::index_sequence<I...>) const {
            return T{(*this)[I].as<std::tuple_element_t<I, T>>()...};
        }

        const result* r_;
        std::size_t row_{};
    };
}
