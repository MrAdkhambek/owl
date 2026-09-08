#pragma once

// Failure as a value. Every driver checks its C API's status and builds
// one of these, so the try_ path never throws; the throwing wrappers throw
// the very same object, which is why it also derives std::runtime_error:
// one type for both spellings, and what() is the message either way.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sql {
    // What went wrong, coarse enough to branch on.
    //   connect     opening a connection failed; nothing is cached, the next
    //               checkout tries again
    //   connection  the link died mid-use; the pool discards it on release
    //   query       the server rejected the statement; the connection is fine
    //   timeout     a deadline passed; the connection was finished
    //   closed      checkout on a closed pool
    //   conversion  a cell could not be produced as asked -- thrown, never
    //               returned, because reading a cell is synchronous
    enum class error_kind : std::uint8_t { connect, connection, query, timeout, closed, conversion };

    [[nodiscard]] constexpr std::string_view to_string(const error_kind kind) noexcept {
        switch (kind) {
            case error_kind::connect: return "connect";
            case error_kind::connection: return "connection";
            case error_kind::query: return "query";
            case error_kind::timeout: return "timeout";
            case error_kind::closed: return "closed";
            case error_kind::conversion: return "conversion";
        }
        std::unreachable();
    }

    class error final : public std::runtime_error {
    public:
        // sqlstate is psql's five-character code ("UNKNOWN" when libpq has
        // none); code is psql's ExecStatusType or sqlite's extended result
        // code. Both stay empty / 0 for failures that are not a driver's.
        error(const error_kind kind, const std::string& message, std::string sqlstate = {}, const int code = 0)
            : std::runtime_error(message), kind_(kind), sqlstate_(std::move(sqlstate)), code_(code) {
        }

        [[nodiscard]] error_kind kind() const noexcept {
            return kind_;
        }

        [[nodiscard]] std::string_view sqlstate() const noexcept {
            return sqlstate_;
        }

        [[nodiscard]] int code() const noexcept {
            return code_;
        }

    private:
        error_kind kind_;
        std::string sqlstate_;
        int code_;
    };
}
