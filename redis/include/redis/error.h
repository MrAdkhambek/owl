#pragma once

// Failure as a value, sql's rule: the connection checks hiredis's status and
// builds one of these, so the try_ path never throws; the throwing wrappers
// throw the very same object, which is why it also derives
// std::runtime_error -- one type for both spellings, and what() is the
// message either way.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace redis {
    // What went wrong, coarse enough to branch on.
    //   connect     opening failed (TCP, HELLO, AUTH or SELECT refused);
    //               nothing is kept, the next command tries again
    //   connection  the link died mid-use, or was dropped because another
    //               command timed out; the next command reconnects
    //   command     the server answered an error reply; the connection is fine
    //   timeout     a deadline passed; the connection was finished
    //   closed      a command on a closed client
    //   conversion  a reply could not be produced as asked -- thrown, never
    //               returned, because reading a reply is synchronous
    enum class error_kind : std::uint8_t { connect, connection, command, timeout, closed, conversion };

    [[nodiscard]] constexpr std::string_view to_string(const error_kind kind) noexcept {
        switch (kind) {
            case error_kind::connect: return "connect";
            case error_kind::connection: return "connection";
            case error_kind::command: return "command";
            case error_kind::timeout: return "timeout";
            case error_kind::closed: return "closed";
            case error_kind::conversion: return "conversion";
        }
        std::unreachable();
    }

    class error final : public std::runtime_error {
    public:
        error(const error_kind kind, const std::string& message) : std::runtime_error(message), kind_(kind) {
        }

        [[nodiscard]] error_kind kind() const noexcept {
            return kind_;
        }

        // The first word of a server error reply -- ERR, WRONGTYPE, NOSCRIPT,
        // NOAUTH -- which is how Redis types its errors, and enough to branch
        // NOSCRIPT for an EVALSHA-then-EVAL fallback. Empty for every other
        // kind: those messages are ours, not the server's.
        [[nodiscard]] std::string_view prefix() const noexcept {
            if (kind_ != error_kind::command) return {};
            const std::string_view msg = what();
            const auto end = msg.find_first_of(" \t\r\n");
            return end == std::string_view::npos ? msg : msg.substr(0, end);
        }

    private:
        error_kind kind_;
    };
}
