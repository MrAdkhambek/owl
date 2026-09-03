#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace owl {
    enum class Method : std::uint8_t {
        Get, Head, Post, Put, Delete, Patch, Options, Trace, Connect, Unknown
    };

    [[nodiscard]] constexpr std::string_view to_string(const Method method) noexcept {
        switch (method) {
        case Method::Get: return "GET";
        case Method::Head: return "HEAD";
        case Method::Post: return "POST";
        case Method::Put: return "PUT";
        case Method::Delete: return "DELETE";
        case Method::Patch: return "PATCH";
        case Method::Options: return "OPTIONS";
        case Method::Trace: return "TRACE";
        case Method::Connect: return "CONNECT";
        default: return "UNKNOWN";
        }
    }

    [[nodiscard]] constexpr Method method_from_string(const std::string_view method) noexcept {
        if (method == "GET") return Method::Get;
        if (method == "HEAD") return Method::Head;
        if (method == "POST") return Method::Post;
        if (method == "PUT") return Method::Put;
        if (method == "DELETE") return Method::Delete;
        if (method == "PATCH") return Method::Patch;
        if (method == "OPTIONS") return Method::Options;
        if (method == "TRACE") return Method::Trace;
        if (method == "CONNECT") return Method::Connect;
        return Method::Unknown;
    }

    [[nodiscard]] constexpr bool method_forbids_body(const Method method) noexcept {
        return method == Method::Head;
    }

    class MethodSet final {
    public:
        constexpr MethodSet() noexcept = default;

        constexpr void add(const Method method) noexcept { bits_ |= mask(method); }

        constexpr void merge(const MethodSet other) noexcept { bits_ |= other.bits_; }

        [[nodiscard]] constexpr bool has(const Method method) const noexcept {
            return (bits_ & mask(method)) != 0;
        }

        [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }

        [[nodiscard]] std::string to_allow_header() const {
            constexpr Method all[] = {
                Method::Get, Method::Head, Method::Post, Method::Put, Method::Delete,
                Method::Patch, Method::Options, Method::Trace, Method::Connect,
            };
            std::string out;
            for (const Method method : all) {
                if (!has(method)) continue;
                if (!out.empty()) out += ", ";
                out += to_string(method);
            }
            return out;
        }

    private:
        static constexpr std::uint16_t mask(const Method method) noexcept {
            return static_cast<std::uint16_t>(1u << std::to_underlying(method));
        }

        std::uint16_t bits_ = 0;
    };
}

template <>
struct std::formatter<owl::Method, char> : std::formatter<std::string_view, char> {
    auto format(const owl::Method method, auto& ctx) const {
        return std::formatter<std::string_view, char>::format(owl::to_string(method), ctx);
    }
};
