#pragma once

// What a reactor must do, with none of how. Kept apart from io/reactor.h
// so that constraining a template on io_reactor -- or writing a reactor of
// your own -- costs neither the kqueue/epoll body nor <sys/event.h>.
//
// Unlike the other concepts here, this one depends on task.h: the
// operations are defined in terms of task<wait_status>.

#include <chrono>
#include <concepts>
#include <cstdint>

#include "coro/task.h"

namespace coro {
    // Which readiness to wait for on a descriptor.
    enum class interest : std::uint8_t { read, write, read_write };

    // How a wait ended: the descriptor became ready, the deadline
    // passed first, the reactor tore down underneath the waiter, or
    // something failed. Callers must check -- only ready licenses an
    // I/O syscall's success.
    enum class wait_status : std::uint8_t { ready, timeout, cancelled, error };

    // The reactor contract in full: wait for readiness on an fd, or
    // just wait out a duration. Both answer as task<wait_status>, so a
    // reactor never throws through a co_await -- even failure and
    // cancellation arrive as data.
    template <typename R>
    concept io_reactor = requires(R& r, int fd, interest i, std::chrono::milliseconds t) {
        { r.wait(fd, i, t) } -> std::same_as<task<wait_status>>;
        { r.sleep(t) } -> std::same_as<task<wait_status>>;
    };
}
