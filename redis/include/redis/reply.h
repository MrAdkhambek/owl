#pragma once

// The RESP3 value a command comes back with. reply owns one redisReply tree;
// reply_view is a borrowed node in it, so walking an aggregate allocates
// nothing, and a view is valid exactly while its owning reply lives.
//
// as<T>() is the one way to read a value, and it parses on demand: a bulk
// string holding "42" reads as an int, because GET on a counter hands text
// back, and an integer reads as a std::string, because a caller that wants
// text should not care which RESP type the server chose. A pairing that
// cannot be honest -- nil into a non-optional, an error element into
// anything, an array into an int -- throws error{conversion}: reading is
// synchronous, so the failure cannot be returned.
//
// A map flattens into a std::vector<T> as alternating keys and values, the
// RESP2 shape, so HGETALL reads the same under both protocols; anything
// richer walks size() / operator[] or raw().
//
// The error reply type never reaches a caller at the top level: the
// connection turns it into error{command} first. Nested error elements stay
// where they are and convert to nothing.

#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <hiredis/hiredis.h>

#include "redis/error.h"

namespace redis {
    enum class reply_type : std::uint8_t {
        nil, status, string, verbatim, integer, real, boolean, error, array, map, set, push, bignum, attribute
    };

    [[nodiscard]] constexpr std::string_view to_string(const reply_type t) noexcept {
        switch (t) {
            case reply_type::nil: return "nil";
            case reply_type::status: return "status";
            case reply_type::string: return "string";
            case reply_type::verbatim: return "verbatim";
            case reply_type::integer: return "integer";
            case reply_type::real: return "real";
            case reply_type::boolean: return "boolean";
            case reply_type::error: return "error";
            case reply_type::array: return "array";
            case reply_type::map: return "map";
            case reply_type::set: return "set";
            case reply_type::push: return "push";
            case reply_type::bignum: return "bignum";
            case reply_type::attribute: return "attribute";
        }
        std::unreachable();
    }

    namespace detail {
        template <typename T>
        inline constexpr bool is_optional_v = false;
        template <typename T>
        inline constexpr bool is_optional_v<std::optional<T>> = true;

        template <typename T>
        inline constexpr bool is_vector_v = false;
        template <typename T, typename A>
        inline constexpr bool is_vector_v<std::vector<T, A>> = true;

        // bool and char are integral to the language and not to a reply.
        template <typename T>
        concept reply_integral = std::integral<T> && !std::same_as<T, bool> && !std::same_as<T, char>;

        [[nodiscard]] constexpr reply_type type_of(const int hiredis_type) noexcept {
            switch (hiredis_type) {
                case REDIS_REPLY_STRING: return reply_type::string;
                case REDIS_REPLY_ARRAY: return reply_type::array;
                case REDIS_REPLY_INTEGER: return reply_type::integer;
                case REDIS_REPLY_NIL: return reply_type::nil;
                case REDIS_REPLY_STATUS: return reply_type::status;
                case REDIS_REPLY_ERROR: return reply_type::error;
                case REDIS_REPLY_DOUBLE: return reply_type::real;
                case REDIS_REPLY_BOOL: return reply_type::boolean;
                case REDIS_REPLY_MAP: return reply_type::map;
                case REDIS_REPLY_SET: return reply_type::set;
                case REDIS_REPLY_ATTR: return reply_type::attribute;
                case REDIS_REPLY_PUSH: return reply_type::push;
                case REDIS_REPLY_BIGNUM: return reply_type::bignum;
                case REDIS_REPLY_VERB: return reply_type::verbatim;
                default: return reply_type::nil;
            }
        }

        [[nodiscard]] inline error conversion_error(const reply_type seen, const std::string_view asked) {
            return error{
                error_kind::conversion,
                "cannot read a " + std::string{to_string(seen)} + " reply as " + std::string{asked}
            };
        }
    }

    class reply_view final {
    public:
        explicit reply_view(const redisReply* const node) noexcept : node_(node) {
        }

        [[nodiscard]] reply_type type() const noexcept {
            return node_ == nullptr ? reply_type::nil : detail::type_of(node_->type);
        }

        [[nodiscard]] bool is_null() const noexcept {
            return type() == reply_type::nil;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return is_aggregate() ? node_->elements : 0;
        }

        [[nodiscard]] reply_view operator[](const std::size_t i) const {
            if (i >= size()) throw error{error_kind::conversion, "reply element index out of range"};
            return reply_view{node_->element[i]};
        }

        template <typename T>
        [[nodiscard]] T as() const {
            if constexpr (detail::is_optional_v<T>) {
                if (is_null()) return std::nullopt;
                return T{as<typename T::value_type>()};
            } else if constexpr (detail::is_vector_v<T>) {
                if (!is_aggregate()) throw detail::conversion_error(type(), "a sequence");
                T out;
                out.reserve(size());
                for (std::size_t i = 0; i < size(); ++i) out.push_back((*this)[i].template as<typename T::value_type>());
                return out;
            } else if constexpr (std::same_as<T, std::string>) {
                switch (type()) {
                    case reply_type::status:
                    case reply_type::string:
                    case reply_type::verbatim:
                    case reply_type::bignum:
                    case reply_type::real:
                        return std::string{text()};
                    case reply_type::integer:
                        return std::to_string(node_->integer);
                    default:
                        throw detail::conversion_error(type(), "std::string");
                }
            } else if constexpr (std::same_as<T, std::string_view>) {
                switch (type()) {
                    case reply_type::status:
                    case reply_type::string:
                    case reply_type::verbatim:
                    case reply_type::bignum:
                    case reply_type::real:
                        return text();
                    default:
                        throw detail::conversion_error(type(), "std::string_view");
                }
            } else if constexpr (std::same_as<T, bool>) {
                if (type() == reply_type::boolean) return node_->integer != 0;
                if (type() == reply_type::integer) {
                    if (node_->integer == 0) return false;
                    if (node_->integer == 1) return true;
                    throw error{error_kind::conversion, "integer reply is neither 0 nor 1"};
                }
                throw detail::conversion_error(type(), "bool");
            } else if constexpr (detail::reply_integral<T>) {
                if (type() == reply_type::integer) return checked<T>(node_->integer);
                if (has_text()) return parse<T>(text());
                throw detail::conversion_error(type(), "an integer");
            } else if constexpr (std::floating_point<T>) {
                if (type() == reply_type::real) return static_cast<T>(node_->dval);
                if (type() == reply_type::integer) return static_cast<T>(node_->integer);
                if (has_text()) return parse<T>(text());
                throw detail::conversion_error(type(), "a floating point number");
            } else {
                static_assert(false, "redis: as<T> supports std::string, std::string_view, integers, floating point, bool, std::optional<U> and std::vector<U>");
            }
        }

        [[nodiscard]] const redisReply* raw() const noexcept {
            return node_;
        }

    private:
        [[nodiscard]] bool is_aggregate() const noexcept {
            switch (type()) {
                case reply_type::array:
                case reply_type::map:
                case reply_type::set:
                case reply_type::push:
                case reply_type::attribute:
                    return node_->element != nullptr || node_->elements == 0;
                default:
                    return false;
            }
        }

        // The kinds hiredis keeps text for; a real carries its text beside
        // dval. An error element has text too, but as<T> refuses it above.
        [[nodiscard]] bool has_text() const noexcept {
            switch (type()) {
                case reply_type::status:
                case reply_type::string:
                case reply_type::verbatim:
                case reply_type::bignum:
                case reply_type::real:
                    return node_->str != nullptr;
                default:
                    return false;
            }
        }

        [[nodiscard]] std::string_view text() const noexcept {
            return {node_->str, node_->len};
        }

        template <detail::reply_integral T>
        [[nodiscard]] static T checked(const long long v) {
            if (!std::in_range<T>(v)) throw error{error_kind::conversion, "integer reply out of range for the requested type"};
            return static_cast<T>(v);
        }

        template <typename T>
        [[nodiscard]] static T parse(const std::string_view s) {
            T v{};
            const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
            if (ec != std::errc{} || end != s.data() + s.size()) {
                throw error{error_kind::conversion, "reply text is not a number: " + std::string{s}};
            }
            return v;
        }

        const redisReply* node_;
    };

    struct reply_deleter final {
        void operator()(redisReply* const r) const noexcept {
            freeReplyObject(r);
        }
    };

    class reply final {
    public:
        reply() = default;

        // Adopts a redisReply* the way hiredis hands it out.
        explicit reply(redisReply* const raw) noexcept : owned_(raw) {
        }

        [[nodiscard]] reply_view view() const noexcept {
            return reply_view{owned_.get()};
        }

        [[nodiscard]] reply_type type() const noexcept {
            return view().type();
        }

        [[nodiscard]] bool is_null() const noexcept {
            return view().is_null();
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return view().size();
        }

        [[nodiscard]] reply_view operator[](const std::size_t i) const {
            return view()[i];
        }

        template <typename T>
        [[nodiscard]] T as() const {
            return view().template as<T>();
        }

        // The escape hatch: anything hiredis's struct can answer.
        [[nodiscard]] const redisReply* raw() const noexcept {
            return owned_.get();
        }

    private:
        std::unique_ptr<redisReply, reply_deleter> owned_;
    };
}
