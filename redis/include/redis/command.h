#pragma once

// The two entry points. try_command is the primitive: assemble the argv,
// hand it to the client, return the client's expected untouched. command
// throws that error. Arguments are borrowed for the life of the returned
// task -- the frame holds references, not copies -- so await in the same
// full-expression that built the task, exactly as with sql::query; naming
// the task and awaiting it later with temporaries as arguments dangles.

#include <expected>
#include <utility>

#include <coro/task.h>

#include "redis/client.h"
#include "redis/detail/args.h"
#include "redis/error.h"
#include "redis/reply.h"

namespace redis {
    template <arg... Args>
        requires (sizeof...(Args) > 0)
    [[nodiscard]] coro::task<std::expected<reply, error>> try_command(const client& c, const Args&... args) {
        const detail::command_args argv{args...};
        co_return co_await c.try_command(argv.argv());
    }

    template <arg... Args>
        requires (sizeof...(Args) > 0)
    [[nodiscard]] coro::task<reply> command(const client& c, const Args&... args) {
        auto r = co_await try_command(c, args...);
        if (!r) throw std::move(r.error());
        co_return std::move(*r);
    }
}
