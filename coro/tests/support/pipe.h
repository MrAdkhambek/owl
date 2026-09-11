#pragma once

// Descriptors for the io suites, shared for the reason helpers.h gives:
// copies of a fixture drift apart.

#include <gtest/gtest.h>
#include <unistd.h>

namespace coro_test {
    // A pipe gives a readable fd on demand: nothing is readable until
    // something is written, which is exactly the edge the reactor parks on.
    class pipe_pair {
    public:
        pipe_pair() { EXPECT_EQ(::pipe(fds_), 0); }

        ~pipe_pair() {
            if (fds_[0] >= 0) ::close(fds_[0]);
            if (fds_[1] >= 0) ::close(fds_[1]);
        }

        pipe_pair(const pipe_pair&) = delete;
        pipe_pair& operator=(const pipe_pair&) = delete;

        [[nodiscard]] int read_end() const noexcept { return fds_[0]; }

        void write_byte() const { EXPECT_EQ(::write(fds_[1], "x", 1), 1); }

    private:
        int fds_[2]{-1, -1};
    };
}
