#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace sql {
    // One bound parameter, owning its bytes. query/execute are not
    // coroutines -- they run bind_traits eagerly and hand the owning values
    // by value to the task they return, so
    //   auto t = sql::query<"...">(db, make_name()); co_await t;
    // binds a copy, not a reference to a destroyed temporary. Src& is the
    // one reference that survives; the source must outlive the await.
    struct bind_value final {
        std::variant<std::nullptr_t, std::int64_t, double, std::string, std::vector<std::byte>> value;
    };

    template <typename T, typename = void>
    struct bind_traits {
        static_assert(sizeof(T) == 0, "sql: no bind_traits for this type (builtins: arithmetic, bool, string, string_view, const char*, blob, optional<T>, nullptr)");
    };

    template <typename T>
        requires std::is_integral_v<T>
    struct bind_traits<T> {
        // Unsigned values above INT64_MAX truncate; sqlite's bind API is
        // int64-shaped and postgres text output is decimal anyway.
        [[nodiscard]] static bind_value make(const T v) { return bind_value{static_cast<std::int64_t>(v)}; }
    };

    template <typename T>
        requires std::is_floating_point_v<T>
    struct bind_traits<T> {
        [[nodiscard]] static bind_value make(const T v) { return bind_value{static_cast<double>(v)}; }
    };

    template <>
    struct bind_traits<bool> {
        [[nodiscard]] static bind_value make(const bool v) { return bind_value{static_cast<std::int64_t>(v)}; }
    };

    template <>
    struct bind_traits<std::string> {
        [[nodiscard]] static bind_value make(const std::string& v) { return bind_value{v}; }
    };

    template <>
    struct bind_traits<std::string_view> {
        [[nodiscard]] static bind_value make(const std::string_view v) { return bind_value{std::string{v}}; }
    };

    template <std::size_t N>
    struct bind_traits<char[N]> {
        [[nodiscard]] static bind_value make(const char (&v)[N]) { return bind_value{std::string{v}}; }
    };

    template <>
    struct bind_traits<const char*> {
        [[nodiscard]] static bind_value make(const char* v) { return bind_value{std::string{v}}; }
    };

    template <>
    struct bind_traits<std::vector<std::byte>> {
        [[nodiscard]] static bind_value make(const std::vector<std::byte>& v) { return bind_value{v}; }
    };

    template <typename T>
    struct bind_traits<std::optional<T>> {
        [[nodiscard]] static bind_value make(const std::optional<T>& v) {
            return v ? bind_traits<T>::make(*v) : bind_value{nullptr};
        }
    };

    template <>
    struct bind_traits<std::nullptr_t> {
        [[nodiscard]] static bind_value make(std::nullptr_t) { return bind_value{nullptr}; }
    };

    namespace detail {
        template <typename... Args>
        [[nodiscard]] auto make_binds(Args&&... args) {
            return std::array<bind_value, sizeof...(Args)>{
                bind_traits<std::remove_cvref_t<Args>>::make(std::forward<Args>(args))...};
        }
    }
}
