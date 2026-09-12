#pragma once

// Per-connection state, shared by Socket, the engine and the driver.
//
// Heap, not the request pool: the upgrade destroys the request. Complete
// here, with no wslay type, so the accessors below can be inline and every
// translation unit that holds a WebSocket handler links -- not only the
// ones that include the engine.

#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include <h2o.h>

#include <coro/task.h>

#include "owl/ws/message.h"

namespace owl::ws::detail {
    struct Session;
    struct Handle;

    // What one connection may hold. Copied into the Session at upgrade, so
    // changing default_limits never races a live connection.
    struct Limits final {
        // One inbound message. wslay fails the connection with 1009 past it.
        std::size_t max_message_bytes = 16 * 1024 * 1024;
        // Delivered but not yet taken by the handler. Reading pauses here
        // and resumes when the handler next parks in recv(), so a peer that
        // outruns its handler is held back by TCP, not by the heap.
        std::size_t max_pending_bytes = 16 * 1024 * 1024;
        // Queued for the peer. Past it the connection is dropped: send()
        // does not suspend yet, so a peer that stops reading would grow this
        // without bound.
        std::size_t max_unsent_bytes = 64 * 1024 * 1024;
    };

    // Process-wide defaults. Not yet a Server option; the tests lower them
    // before their workers start.
    inline Limits default_limits{};

    // h2o_timer_t carries no user data, so a callback walks back from the
    // timer with H2O_STRUCT_FROM_MEMBER -- which needs a standard-layout
    // struct, and Session (a deque, a task) is not one.
    struct SessionTimer final {
        h2o_timer_t timer{};
        Session* session = nullptr;
    };

    // What only the engine can do. Pointers rather than declarations, so a
    // translation unit that never includes the engine still links.
    struct SessionOps final {
        void (*enqueue)(Handle*, std::string, Opcode) noexcept;
        void (*close)(Handle*) noexcept;
        void (*resume_reading)(Session*) noexcept;
    };

    // What every Socket copy points at, and what outlives the Session. A
    // Socket kept in a shared map after its connection ended stays safe to
    // call -- send and close find no session and do nothing -- and its id()
    // cannot be handed to a new connection while the old copy is alive,
    // which a session address could.
    //
    // session is touched only on the owner thread. owner, hop and ops are
    // set at upgrade, before the handler starts, so before any Socket can
    // reach another thread. open is the one field any thread may read.
    struct Handle final {
        std::atomic<std::size_t> refs{1};           // the Session's, plus one per Socket
        std::atomic<bool> open{false};              // what Socket::open() reports, on any thread
        Session* session = nullptr;                 // null once the session is destroyed
        std::thread::id owner{};                    // the worker thread running the session
        h2o_multithread_receiver_t* hop = nullptr;  // that worker's WebSocket receiver
        const SessionOps* ops = nullptr;            // null until upgrade()
    };

    inline void retain(Handle* handle) noexcept {
        handle->refs.fetch_add(1, std::memory_order_relaxed);
    }

    inline void release(Handle* handle) noexcept {
        if (handle->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete handle;
    }

    struct Session final {
        // The Session's own reference is dropped first; the handler's frame,
        // destroyed after this body as the first-declared member, drops the
        // Socket copies it holds.
        Session() {
            handle->session = this;
        }

        ~Session() {
            handle->session = nullptr;
            handle->open.store(false, std::memory_order_relaxed);
            release(handle);
        }

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // The connection is this coroutine; see engine.h for how it ends.
        coro::task<void> handler;
        // Messages the handler has not taken yet. A queue rather than a slot:
        // a handler that awaits inside on_message sees later frames wait here
        // instead of interleaving.
        std::deque<Message> pending;
        std::coroutine_handle<> waiter;       // set while parked in recv()
        Handle* const handle = new Handle;

        h2o_loop_t* loop = nullptr;           // saved before the request dies
        h2o_socket_t* sock = nullptr;         // set in on_complete
        void* wslay = nullptr;                // wslay_event_context_ptr, opaque here
        std::array<h2o_iovec_t, 4> batch{};   // writes in flight, owned until they complete
        std::size_t batched = 0;
        Limits limits;                        // copied from default_limits at upgrade
        std::size_t pending_bytes = 0;        // the payload bytes sitting in pending
        std::size_t unsent_bytes = 0;         // queued for the peer, not yet handed to h2o
        SessionTimer reaper;                  // destroys the session on the loop's next pass
        SessionTimer kicker;                  // runs proceed() on the loop's next pass

        bool closing = false;                 // nothing more will be delivered
        bool dead = false;                    // the socket failed; nothing more can be written
        bool finished = false;                // the handler has returned
        bool reading_paused = false;          // stopped at max_pending_bytes; park() restarts it
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
        session->pending_bytes -= message.size();
        return message;
    }

    // Parking means pending is empty, so a paused read may resume. It
    // resumes from the loop, not from here: inline, the engine would run
    // inside the handler's own await_suspend.
    inline void park(Session* session, const std::coroutine_handle<> waiter) noexcept {
        session->waiter = waiter;
        if (session->reading_paused && session->handle->ops != nullptr) {
            session->handle->ops->resume_reading(session);
        }
    }

    inline void enqueue(Handle* handle, std::string data, const Opcode opcode) noexcept {
        if (handle->ops != nullptr) handle->ops->enqueue(handle, std::move(data), opcode);
    }

    inline void begin_close(Handle* handle) noexcept {
        if (handle->ops != nullptr) handle->ops->close(handle);
    }
}
