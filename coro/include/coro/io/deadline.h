#pragma once

// A wait budget as a point in time, for anything that hands one budget
// down through several waits.
//
// The reactor's convention is that a negative timeout means "no deadline",
// so a budget has to be turned into an absolute point once and back into a
// remaining duration at every wait. That conversion has three easy things
// to get subtly wrong: a lapsed budget must ask for a poll rather than
// wait forever, the rounding must be up so a sub-millisecond remainder
// never becomes an instant timeout, and "no deadline" must survive both
// directions. It is written here, beside the handle whose convention it
// is, rather than once per driver.

#include <chrono>
#include <optional>

namespace coro {
    class deadline final {
    public:
        // Unbounded: remaining() answers "no deadline" however long it is
        // held, which is what a driver with no configured timeout wants.
        deadline() = default;

        // A budget starting now. Negative is no deadline, the same
        // spelling wait() takes, so a config value passes straight in.
        explicit deadline(const std::chrono::milliseconds budget) {
            if (budget.count() >= 0) at_ = clock::now() + budget;
        }

        // What is left, in the form wait() wants: negative for no deadline,
        // never negative for a bounded one that has already lapsed.
        [[nodiscard]] std::chrono::milliseconds remaining() const noexcept {
            if (!at_) return std::chrono::milliseconds{-1};
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(*at_ - clock::now());
            return left.count() < 0 ? std::chrono::milliseconds{0} : left;
        }

        [[nodiscard]] bool bounded() const noexcept {
            return at_.has_value();
        }

    private:
        using clock = std::chrono::steady_clock;

        std::optional<clock::time_point> at_;
    };
}
