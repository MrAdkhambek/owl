#pragma once

// The controller front-end, and the rule both front-ends obey.
//
// A controller is sugar over the coroutine form, not a second mechanism:
//
//     class Chat {
//     public:
//         explicit Chat(std::shared_ptr<AppState> app);   // optional
//         void on_connect(owl::ws::Socket sock);          // optional
//         void on_message(owl::ws::Socket sock, owl::ws::Message msg);
//         void on_disconnect(owl::ws::Socket sock);       // optional
//     };
//
//     .ws<"/chat", Chat>(app)
//
// Every method may return `void` **or** `coro::task<void>`, deduced the way
// a handler's return type is. That is the whole reason the controller is
// built on a coroutine: a callback controller cannot await, so on_message
// could never reach Redis, which is the first thing a chat handler wants
// to do.
//
// A controller may also take extractors, by declaring which ones it wants:
//
//     using Extractors = std::tuple<owl::Path<"room", int>,
//                                   owl::Header<"authorization">>;
//
//     void on_connect(owl::ws::Socket sock, owl::Path<"room", int> room);
//     coro::task<void> on_message(owl::ws::Socket, owl::ws::Message,
//                                 owl::Path<"room", int>,
//                                 owl::Header<"authorization">);
//     void on_disconnect(owl::ws::Socket sock);   // may skip the pack
//
// They are extracted per request, before the upgrade, and live in the
// per-connection loop below -- *not* in the controller, which is shared by
// every connection and could not hold one client's values without racing the
// next. Whether a method takes the pack is optional per method, decided the
// same way sync-versus-async is.

#include <concepts>
#include <exception>
#include <memory>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include <coro/task.h>

#include "owl/ws/detail/views.h"
#include "owl/ws/socket.h"

namespace owl::ws::detail {
    // A method may be sync or async; awaiting a void is a no-op, so both
    // collapse to the same call site below.
    template <typename T>
    concept VoidOrTask = std::is_void_v<T> || std::is_same_v<T, coro::task<void>>;

    // Each of these is asked twice: once with the route's extractor pack and
    // once without it. An empty pack makes the two spellings the same
    // concept, which is why a controller that declares no Extractors sees no
    // change at all.
    template <typename C, typename... Args>
    concept HasOnMessage = requires(C c, Socket s, Message m, Args... a) {
        { c.on_message(s, std::move(m), a...) };
        requires VoidOrTask<decltype(c.on_message(s, std::move(m), a...))>;
    };

    template <typename C, typename... Args>
    concept HasOnConnect = requires(C c, Socket s, Args... a) {
        { c.on_connect(s, a...) };
        requires VoidOrTask<decltype(c.on_connect(s, a...))>;
    };

    // Takes the Socket like the others: one controller serves every
    // connection, so a bare on_disconnect() could not say which one ended.
    template <typename C, typename... Args>
    concept HasOnDisconnect = requires(C c, Socket s, Args... a) {
        { c.on_disconnect(s, a...) };
        requires VoidOrTask<decltype(c.on_disconnect(s, a...))>;
    };

    // The extractors a controller asks for, declared rather than deduced from
    // on_message: deduction breaks on overloads, templates and defaulted
    // parameters, and fails unreadably when it breaks. Absent means an empty
    // pack, which is exactly the behaviour before this existed.
    template <typename C>
    concept HasExtractors = requires { typename C::Extractors; };

    template <typename C>
    struct extractors_of {
        using type = std::tuple<>;
    };

    template <HasExtractors C>
    struct extractors_of<C> {
        using type = typename C::Extractors;
    };

    template <typename C>
    using extractors_of_t = typename extractors_of<C>::type;

    // `using Extractors = int;` would otherwise fail deep inside the
    // registration, where the message names none of the user's types.
    template <typename T>
    inline constexpr bool is_extractor_tuple_v = false;

    template <typename... Ts>
    inline constexpr bool is_extractor_tuple_v<std::tuple<Ts...>> = true;

    // "Has a member with this name", regardless of signature. Paired with
    // the concepts above, this turns a wrong signature into a compile error
    // instead of a callback that is silently never invoked -- which is what
    // happens when an optional method is detected by shape alone, and is
    // exactly how on_disconnect() stopped being called when it gained a
    // Socket parameter.
    template <typename C>
    concept NamesOnConnect = requires { &C::on_connect; };

    template <typename C>
    concept NamesOnDisconnect = requires { &C::on_disconnect; };

    // Calls a method whether it is sync or async, so the loop below does
    // not need two spellings of every call.
    template <typename Invocable>
    coro::task<void> invoke(Invocable&& call) {
        if constexpr (std::is_void_v<decltype(call())>) {
            call();
            co_return;
        } else {
            co_await call();
        }
    }

    // One coroutine per connection, driving **one controller shared by all
    // of them**. The frame holds a shared_ptr to it, so a connection keeps
    // its controller alive however long it outlives the route.
    //
    // Two consequences worth stating, because they are the whole difference
    // from a per-connection instance:
    //
    //   * Members are *not* per-connection state. A counter here counts
    //     every connection together. Key on Socket::id() for per-connection
    //     data -- Socket is equality-comparable and hashable for exactly
    //     that.
    //   * With threads > 1 the instance is reached from every worker's loop
    //     at once, so it must be safe for concurrent use -- the same
    //     contract State<T> carries.
    // The extractors arrive **by value** -- except a `const T&`, which binds
    // a Context member that outlives the connection -- so they live in this
    // frame, one per connection, for as long as the connection does. That is the
    // whole trick: the request they came from is destroyed by the upgrade, and
    // the controller is shared, so this is the only storage with both the
    // right lifetime and the right multiplicity. They are passed as lvalues,
    // so a method may take them by value or by const reference.
    //
    // on_disconnect runs for every connection whose on_connect was entered,
    // including one where on_connect or on_message threw. A controller that
    // keys state on the Socket in on_connect would otherwise keep it after
    // the connection is gone. co_await is not allowed in a handler, so the
    // exception is parked, on_disconnect runs, and then it is rethrown for
    // the engine to close the connection with 1011.
    template <typename C, typename... Args>
    coro::task<void> controller_loop(Socket sock, std::shared_ptr<C> instance, Args... extracted) {
        C& controller = *instance;
        std::exception_ptr failure;
        try {
            if constexpr (HasOnConnect<C, Args...>) {
                co_await invoke([&] { return controller.on_connect(sock, extracted...); });
            } else if constexpr (HasOnConnect<C>) {
                co_await invoke([&] { return controller.on_connect(sock); });
            }

            while (std::optional<Message> message = co_await sock.recv()) {
                if constexpr (HasOnMessage<C, Args...>) {
                    co_await invoke([&] {
                        return controller.on_message(sock, std::move(*message), extracted...);
                    });
                } else {
                    co_await invoke([&] { return controller.on_message(sock, std::move(*message)); });
                }
            }
        } catch (...) {
            failure = std::current_exception();
        }

        if constexpr (HasOnDisconnect<C, Args...>) {
            co_await invoke([&] { return controller.on_disconnect(sock, extracted...); });
        } else if constexpr (HasOnDisconnect<C>) {
            co_await invoke([&] { return controller.on_disconnect(sock); });
        }

        if (failure) std::rethrow_exception(failure);
    }
}
