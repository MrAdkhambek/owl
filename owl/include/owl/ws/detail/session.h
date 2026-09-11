#pragma once

// Per-connection state, shared by Socket, the engine and the driver.
//
// Heap, not the request pool: the upgrade destroys the request. Complete
// here, with no wslay type, so the accessors below can be inline and every
// translation unit that holds a WebSocket handler links -- not only the
// ones that include the engine.

#include <array>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <utility>

#include <h2o.h>

#include <coro/task.h>

#include "owl/ws/message.h"

namespace owl::ws::detail {
    struct Session;

    // h2o_timer_t carries no user data, so the callback walks back from the
    // timer with H2O_STRUCT_FROM_MEMBER -- which needs a standard-layout
    // struct, and Session (a deque, a task) is not one.
    struct Reaper final {
        h2o_timer_t timer{};
        Session* session = nullptr;
    };

    // What only the engine can do. Pointers rather than declarations, so a
    // translation unit that never includes the engine still links.
    struct SessionOps final {
        void (*enqueue)(Session*, std::string, Opcode) noexcept;
        void (*close)(Session*) noexcept;
    };

    struct Session final {
        // The connection is this coroutine; see engine.h for how it ends.
        coro::task<void> handler;
        // Messages the handler has not taken yet. A queue rather than a slot:
        // a handler that awaits inside on_message sees later frames wait here
        // instead of interleaving.
        std::deque<Message> pending;
        std::coroutine_handle<> waiter;       // set while parked in recv()

        const SessionOps* ops = nullptr;      // null until upgrade()
        h2o_loop_t* loop = nullptr;           // saved before the request dies
        h2o_socket_t* sock = nullptr;         // set in on_complete
        void* wslay = nullptr;                // wslay_event_context_ptr, opaque here
        std::array<h2o_iovec_t, 4> batch{};   // writes in flight, owned until they complete
        std::size_t batched = 0;
        Reaper reaper;

        bool closing = false;                 // nothing more will be delivered
        bool dead = false;                    // the socket failed; nothing more can be written
        bool proceeding = false;              // inside the engine's pump
        bool finished = false;                // the handler has returned
    };

    [[nodiscard]] inline bool has_message(const Session* session) noexcept {
        return !session->pending.empty();
    }

    [[nodiscard]] inline bool is_open(const Session* session) noexcept {
        return session->sock != nullptr && !session->closing;
    }

    [[nodiscard]] inline std::optional<Message> take_message(Session* session) noexcept {
        if (session->pending.empty()) return std::nullopt;
        Message message = std::move(session->pending.front());
        session->pending.pop_front();
        return message;
    }

    inline void park(Session* session, const std::coroutine_handle<> waiter) noexcept {
        session->waiter = waiter;
    }

    inline void enqueue(Session* session, std::string data, const Opcode opcode) noexcept {
        if (session->ops != nullptr) session->ops->enqueue(session, std::move(data), opcode);
    }

    inline void begin_close(Session* session) noexcept {
        if (session->ops != nullptr) session->ops->close(session);
    }
}
