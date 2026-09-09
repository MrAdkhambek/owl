#pragma once

// Budgets the reactor's way: a negative duration is no deadline. Shared by
// the connection (one wait at a time) and the client (a whole command, from
// queueing to reply), which is why it is not a connection member.

#include <chrono>
#include <optional>

namespace redis::detail {
    using clock = std::chrono::steady_clock;

    [[nodiscard]] inline std::optional<clock::time_point> deadline_for(const std::chrono::milliseconds budget) noexcept {
        if (budget.count() < 0) return std::nullopt;
        return clock::now() + budget;
    }

    [[nodiscard]] inline std::chrono::milliseconds remaining(const std::optional<clock::time_point> deadline) noexcept {
        if (!deadline) return std::chrono::milliseconds{-1};
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(*deadline - clock::now());
        return left.count() < 0 ? std::chrono::milliseconds{0} : left;
    }
}
