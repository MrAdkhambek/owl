#pragma once

// What arrives on a WebSocket.
//
// Owned, not a view into wslay's buffer: a handler may await between receiving
// a message and reading it, and by then the frame is gone.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace owl::ws {
    // Only the two data opcodes. Ping, pong and close are answered by wslay
    // itself and never surface here -- a handler that had to reply to a ping
    // to stay connected would be a trap.
    enum class Opcode : std::uint8_t { Text, Binary };

    class Message final {
    public:
        Message() = default;

        Message(std::string data, const Opcode opcode) noexcept
            : data_(std::move(data)), opcode_(opcode) {
        }

        [[nodiscard]] const std::string& data() const & noexcept { return data_; }
        [[nodiscard]] std::string data() && noexcept { return std::move(data_); }

        [[nodiscard]] Opcode opcode() const noexcept { return opcode_; }
        [[nodiscard]] bool is_text() const noexcept { return opcode_ == Opcode::Text; }
        [[nodiscard]] bool is_binary() const noexcept { return opcode_ == Opcode::Binary; }

        // Binary safe: a text frame may still contain NUL, and a binary one
        // usually does. size() is the only honest length.
        [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
        [[nodiscard]] bool empty() const noexcept { return data_.empty(); }

    private:
        std::string data_;
        Opcode opcode_ = Opcode::Text;
    };
}
