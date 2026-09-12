#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <coro/task.h>

#include <fstr/fstr.h>

#include "owl/core/method.h"
#include "owl/core/state.h"
#include "owl/extract/from_context.h"
#include "owl/http/request.h"
#include "owl/http/response.h"
#include "owl/routing/detail/endpoint.h"
#include "owl/routing/detail/layer.h"
#include "owl/routing/detail/pattern.h"
#include "owl/routing/detail/traits.h"
#include "owl/routing/detail/trie.h"
#include "owl/ws/controller.h"
#include "owl/ws/detail/handshake.h"
#include "owl/ws/detail/routes.h"
#include "owl/ws/detail/views.h"
#include "middleware.h"

namespace owl {
    // ========================================================================
    // MethodRouter: the set of methods served at one path.
    //
    //     get(list_users).post(create_user)
    //
    // Handler signatures are carried in the type rather than erased on the
    // spot, because the path parameters a handler asks for can only be checked
    // once route<Pattern>() sees the pattern and the handlers together.
    // ========================================================================

    template <typename... Endpoints>
    class MethodRouter {
    public:
        constexpr explicit MethodRouter(Endpoints... endpoints) : endpoints_(endpoints...) {
        }

        template <typename R, typename... Args>
        [[nodiscard]] constexpr auto get(R (*handler)(Args...)) const {
            return with<Method::Get>(handler);
        }

        template <typename R, typename... Args>
        [[nodiscard]] constexpr auto post(R (*handler)(Args...)) const {
            return with<Method::Post>(handler);
        }

        template <typename R, typename... Args>
        [[nodiscard]] constexpr auto put(R (*handler)(Args...)) const {
            return with<Method::Put>(handler);
        }

        template <typename R, typename... Args>
        [[nodiscard]] constexpr auto del(R (*handler)(Args...)) const {
            return with<Method::Delete>(handler);
        }

        template <typename R, typename... Args>
        [[nodiscard]] constexpr auto patch(R (*handler)(Args...)) const {
            return with<Method::Patch>(handler);
        }

        template <typename R, typename... Args>
        [[nodiscard]] constexpr auto head(R (*handler)(Args...)) const {
            return with<Method::Head>(handler);
        }

        template <typename R, typename... Args>
        [[nodiscard]] constexpr auto options(R (*handler)(Args...)) const {
            return with<Method::Options>(handler);
        }

        [[nodiscard]] constexpr const std::tuple<Endpoints...>& endpoints() const noexcept {
            return endpoints_;
        }

    private:
        template <Method M, typename R, typename... Args>
        [[nodiscard]] constexpr auto with(R (*handler)(Args...)) const {
            using Added = detail::Endpoint<M, R, Args...>;
            return std::apply(
                [handler](const Endpoints&... existing) {
                    return MethodRouter<Endpoints..., Added>(existing..., Added{handler});
                },
                endpoints_);
        }

        std::tuple<Endpoints...> endpoints_;
    };

    template <typename R, typename... Args>
    [[nodiscard]] constexpr auto get(R (*handler)(Args...)) {
        using E = detail::Endpoint<Method::Get, R, Args...>;
        return MethodRouter<E>(E{handler});
    }

    template <typename R, typename... Args>
    [[nodiscard]] constexpr auto post(R (*handler)(Args...)) {
        using E = detail::Endpoint<Method::Post, R, Args...>;
        return MethodRouter<E>(E{handler});
    }

    template <typename R, typename... Args>
    [[nodiscard]] constexpr auto put(R (*handler)(Args...)) {
        using E = detail::Endpoint<Method::Put, R, Args...>;
        return MethodRouter<E>(E{handler});
    }

    template <typename R, typename... Args>
    [[nodiscard]] constexpr auto del(R (*handler)(Args...)) {
        using E = detail::Endpoint<Method::Delete, R, Args...>;
        return MethodRouter<E>(E{handler});
    }

    template <typename R, typename... Args>
    [[nodiscard]] constexpr auto patch(R (*handler)(Args...)) {
        using E = detail::Endpoint<Method::Patch, R, Args...>;
        return MethodRouter<E>(E{handler});
    }

    // Adapts whatever shape the layer was written in to the stored Middleware
    // signature. A layer that wants state takes const Context<S>&, which the
    // chain already carries; there is no second injection path.
    template <typename S, typename F>
    Middleware<S> wrap_layer(F fn) {
        if constexpr (detail::ContextLayer<F, S>) {
            return [fn = std::move(fn)](const Request& req, const Context<S>& ctx, Next<S> next) -> coro::task<Response> {
                co_return co_await fn(req, ctx, next);
            };
        } else if constexpr (detail::PlainLayer<F, S>) {
            return [fn = std::move(fn)](const Request& req, const Context<S>&, Next<S> next) -> coro::task<Response> {
                co_return co_await fn(req, next);
            };
        } else {
            static_assert(false,
                          "layer expects task<Response>(const Request&, const Context<S>&, Next) "
                          "or task<Response>(const Request&, Next)");
        }

        std::unreachable();
    }

    template <typename S>
    class Router {
    public:
        [[nodiscard]] static Router make() {
            return Router{};
        }

        Router(const Router&) = delete;
        Router& operator=(const Router&) = delete;
        Router(Router&&) noexcept = default;
        Router& operator=(Router&&) noexcept = default;
        ~Router() = default;

        template <fstr::fstr Pattern, typename... Endpoints>
        [[nodiscard]] Router route(const MethodRouter<Endpoints...>& methods) && {
            static_assert(detail::parse_pattern(Pattern.view()).ok, "invalid route pattern");
            std::apply([this](const Endpoints&... endpoint) {
                (mount<Pattern>(endpoint), ...);
            }, methods.endpoints());
            return std::move(*this);
        }

        // A coroutine on a parallel slot so GET and Upgrade can share a path.
        template <fstr::fstr Pattern, typename... Args>
        [[nodiscard]] Router ws(coro::task<void> (*handler)(ws::Socket, Args...)) && {
            static_assert(detail::parse_pattern(Pattern.view()).ok, "invalid route pattern");
            check_args<Pattern, Args...>();
            ws_checks<Args...>();
            if (!insert_ws(Pattern.view(), [handler](const Request& req, const Context<S>& ctx) -> coro::task<Response> {
                auto args = extract_all<Args...>(ctx, req);
                if (!args) [[unlikely]] co_return to_response(std::move(args).error());
                co_return Response::websocket(std::make_shared<detail::WsRoute<Args...>>(handler, *std::move(args)));
            })) {
                throw std::invalid_argument(std::string("cannot register websocket: ").append(Pattern.view()));
            }
            return std::move(*this);
        }

        // Controller form. Adopts an instance the caller built -- and, if
        // it keeps the shared_ptr, can still reach. That is the difference
        // from the overload below: there the controller is only reachable
        // from inside a connection.
        //
        // One instance serves every connection on the route. Extractors,
        // if the controller declares any, are the part that is *not*
        // shared: they are pulled per request below.
        template <fstr::fstr Pattern, typename C>
        [[nodiscard]] Router ws(std::shared_ptr<C> instance) && {
            static_assert(detail::parse_pattern(Pattern.view()).ok, "invalid route pattern");
            static_assert(ws::detail::is_extractor_tuple_v<ws::detail::extractors_of_t<C>>,
                          "a controller's Extractors must be a std::tuple<...> of extractors");

            if (instance == nullptr) {
                // Every connection on this route would dereference it.
                throw std::invalid_argument("owl::Router: ws controller instance is null");
            }

            register_controller<Pattern, C>(std::move(instance), static_cast<ws::detail::extractors_of_t<C>*>(nullptr));
            return std::move(*this);
        }

        // Constructor arguments, forwarded once. Excluded when the single
        // argument is already a shared_ptr<C>, or this and the overload
        // above would both match.
        template <fstr::fstr Pattern, typename C, typename... CtorArgs> requires (!(sizeof...(CtorArgs) == 1 && (std::is_same_v<
            std::remove_cvref_t<CtorArgs>, std::shared_ptr<C>> && ...)))
        [[nodiscard]] Router ws(CtorArgs... args) && {
            static_assert(detail::parse_pattern(Pattern.view()).ok, "invalid route pattern");
            static_assert(std::is_constructible_v<C, CtorArgs...>, "the controller cannot be constructed from these arguments");
            static_assert(ws::detail::is_extractor_tuple_v<ws::detail::extractors_of_t<C>>,
                          "a controller's Extractors must be a std::tuple<...> of extractors");

            // Constructed here, once -- not built per request. Every
            // connection on this route shares this one instance, which is
            // what makes members shared state rather than per-connection
            // state, and what requires the controller to be safe for
            // concurrent use when threads > 1.
            register_controller<Pattern, C>(std::make_shared<C>(std::move(args)...), static_cast<ws::detail::extractors_of_t<C>*>(nullptr));
            return std::move(*this);
        }

        template <typename F>
        [[nodiscard]] Router layer(F middleware) && {
            root_.middleware.emplace_back(wrap_layer<S>(std::move(middleware)));
            return std::move(*this);
        }

        template <fstr::fstr Prefix>
        [[nodiscard]] Router nest(Router&& nested) && {
            static_assert(detail::parse_pattern(Prefix.view()).ok, "invalid nest prefix");
            static_assert(detail::parse_pattern(Prefix.view()).params == 0, "a nest prefix must be literal; parameters belong in the nested routes");
            graft(Prefix.view(), std::move(nested));
            return std::move(*this);
        }

        [[nodiscard]] const Handler<S>*
        match(
            const Method method,
            const std::string_view path,
            Request& req,
            detail::MatchedChains<S>* out = nullptr
        ) const {
            if (out != nullptr) {
                out->count = 0;
                out->push(&root_.middleware);
            }

            std::array<std::string_view, max_path_segments> segments{};
            const std::size_t count = detail::split_path(path, segments.data(), segments.size());
            if (count == static_cast<std::size_t>(-1)) return nullptr;

            std::array<PathParam, max_path_params> captures{};
            std::size_t captured = 0;
            std::string_view route{};
            const bool websocket = method == Method::Get && ws::detail::wants_websocket(req);
            const Handler<S>* handler = detail::descend(
                root_,
                segments.data(),
                count,
                0,
                method,
                websocket,
                captures.data(),
                captured,
                out,
                &route
            );
            if (!handler) return nullptr;

            req.clear_params();
            req.set_route_pattern(route);
            for (std::size_t i = 0; i < captured; ++i) {
                req.add_param(captures[i].name, captures[i].value);
            }
            return handler;
        }

        [[nodiscard]] MethodSet
        allowed_methods(const std::string_view path) const noexcept {
            std::array<std::string_view, max_path_segments> segments{};
            const std::size_t count = detail::split_path(path, segments.data(), segments.size());

            MethodSet allowed;
            if (count == static_cast<std::size_t>(-1)) return allowed;
            detail::collect_methods(root_, segments.data(), count, 0, allowed);
            return allowed;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return size_;
        }

    private:
        Router() = default;

        bool insert(Method method, std::string_view pattern, Handler<S> handler);
        bool insert_ws(std::string_view pattern, Handler<S> handler);
        detail::RouteNode<S>* node_for(std::string_view pattern);

        template <fstr::fstr Pattern, typename... Args>
        static constexpr void check_args() {
            static_assert(
                ((detail::path_param_of<std::remove_cvref_t<Args>>::value.empty()
                    || detail::declares<Pattern>(detail::path_param_of<std::remove_cvref_t<Args>>::value)) && ...),
                "handler asks for a path parameter this route pattern does not declare");
            static_assert(
                ((!detail::state_of<std::remove_cvref_t<Args>>::is_state
                    || std::is_same_v<typename detail::state_of<std::remove_cvref_t<Args>>::type, S>) && ...),
                "handler asks for a State<T> that is not this router's state type");
        }

        template <typename... Args>
        static constexpr void ws_checks() {
            static_assert((!detail::is_view_extractor_v<std::remove_cvref_t<Args>> && ...),
                          "a WebSocket handler outlives its request: take Path/Query/Header with an owning T, "
                          "Json<T>, State<T>, or a const& to a driver -- not a view");
        }

        // What every controller must satisfy, whichever way it arrives.
        //
        // Args... is the pack from C::Extractors, empty when it declares
        // none. Every method is allowed either spelling -- with the pack
        // or without it -- so on_disconnect need not carry an auth header
        // it has no use for. With an empty pack the two are the same
        // check, which is why nothing changes for a controller that takes
        // no extractors.
        template <typename C, typename... Args>
        static constexpr void ws_controller_checks() {
            static_assert(ws::detail::HasOnMessage<C, Args...>
                          || ws::detail::HasOnMessage<C>,
                          "a WebSocket controller needs "
                          "on_message(ws::Socket, ws::Message) returning void or task<void>, "
                          "optionally followed by the controller's declared Extractors");
            // A method that exists but does not match is a silently dead
            // callback, which is worse than either a match or an absence.
            static_assert(!ws::detail::NamesOnConnect<C>
                          || ws::detail::HasOnConnect<C, Args...>
                          || ws::detail::HasOnConnect<C>,
                          "on_connect must be on_connect(ws::Socket), optionally followed by "
                          "the controller's declared Extractors, returning void or task<void>");
            static_assert(!ws::detail::NamesOnDisconnect<C>
                          || ws::detail::HasOnDisconnect<C, Args...>
                          || ws::detail::HasOnDisconnect<C>,
                          "on_disconnect must be on_disconnect(ws::Socket), optionally followed "
                          "by the controller's declared Extractors, returning void or "
                          "task<void> -- it takes the Socket because one controller serves "
                          "every connection and must be told which one ended");
        }

        // Registers a controller route with C::Extractors unpacked into a
        // parameter pack. The tuple pointer is never dereferenced: it is
        // there only so Args... can be *deduced* from a type alias, which
        // an explicit template argument list cannot do on its own.
        //
        // From here on this is the coroutine form's registration with a
        // controller in place of a function pointer -- the same
        // extraction, the same checks, the same pre-upgrade rejection.
        template <fstr::fstr Pattern, typename C, typename... Args>
        void register_controller(std::shared_ptr<C> instance, std::tuple<Args...>*) {
            // Order matters for the diagnostic, not the outcome: a pack
            // that names an undeclared path parameter also fails to match
            // on_message, and "your on_message is wrong" would be the wrong
            // thing to be told about a mistake in the pattern.
            check_args<Pattern, Args...>();
            ws_checks<Args...>();
            ws_controller_checks<C, Args...>();

            if (!insert_ws(Pattern.view(), [instance = std::move(instance)](const Request& req, const Context<S>& ctx) -> coro::task<Response> {
                auto args = extract_all<Args...>(ctx, req);
                if (!args) [[unlikely]] co_return to_response(std::move(args).error());
                co_return Response::websocket(
                    std::make_shared<detail::WsController<C, Args...>>(instance, *std::move(args)));
            })) {
                throw std::invalid_argument(std::string("cannot register websocket: ").append(Pattern.view()));
            }
        }

        template <fstr::fstr Pattern, Method M, typename R, typename... Args>
        void mount(const detail::Endpoint<M, R, Args...>& endpoint) {
            static_assert(detail::is_handler_return_v<R>, "a handler must return Response or coro::task<Response>");
            check_args<Pattern, Args...>();

            auto* const handler = endpoint.handler;
            add(M, Pattern.view(), [handler](const Request& req, const Context<S>& ctx) -> coro::task<Response> {
                auto args = extract_all<Args...>(ctx, req);
                if (!args) [[unlikely]] co_return to_response(std::move(args).error());
                if constexpr (std::is_same_v<R, Response>) {
                    co_return std::apply(handler, *std::move(args));
                } else {
                    co_return co_await std::apply(handler, *std::move(args));
                }
            });
        }

        void graft(const std::string_view prefix, Router&& nested) {
            const detail::PatternInfo info = detail::parse_pattern(prefix);

            if (info.count + detail::depth(nested.root_) > max_path_segments) {
                throw std::invalid_argument(std::string("nesting under ").append(prefix).append(" exceeds the maximum path depth"));
            }

            detail::RouteNode<S>* node = &root_;
            for (std::size_t i = 0; i < info.count; ++i) {
                node = &node->literal_child(info.segments[i].text);
            }

            detail::prefix_patterns(nested.root_, prefix);
            detail::merge(*node, std::move(nested.root_));
            size_ += nested.size_;
        }

        void add(const Method method, const std::string_view pattern, Handler<S> handler) {
            if (!insert(method, pattern, std::move(handler))) {
                throw std::invalid_argument(std::string("cannot register route: ").append(pattern));
            }
        }

        detail::RouteNode<S> root_;
        std::size_t size_ = 0;
    };

    template <typename S>
    detail::RouteNode<S>* Router<S>::node_for(const std::string_view pattern) {
        const detail::PatternInfo info = detail::parse_pattern(pattern);
        if (!info.ok) return nullptr;

        detail::RouteNode<S>* node = &root_;
        for (std::size_t i = 0; i < info.count; ++i) {
            const detail::Segment& segment = info.segments[i];
            if (segment.is_param) {
                if (!node->param) {
                    node->param = std::make_unique<detail::RouteNode<S>>();
                    node->param->param_name = std::string(segment.text);
                } else if (node->param->param_name != segment.text) {
                    // Two routes naming the same slot differently would make
                    // param() depend on which route won. Refuse instead.
                    return nullptr;
                }
                node = node->param.get();
            } else {
                node = &node->literal_child(segment.text);
            }
        }
        return node;
    }

    template <typename S>
    bool Router<S>::insert(const Method method, const std::string_view pattern, Handler<S> handler) {
        detail::RouteNode<S>* const node = node_for(pattern);
        if (node == nullptr || node->handler_for(method)) return false;
        node->handlers.emplace_back(method, std::move(handler));
        node->pattern = std::string(pattern);
        ++size_;
        return true;
    }

    template <typename S>
    bool Router<S>::insert_ws(const std::string_view pattern, Handler<S> handler) {
        detail::RouteNode<S>* const node = node_for(pattern);
        if (node == nullptr || static_cast<bool>(node->websocket)) return false;
        node->websocket = std::move(handler);
        node->pattern = std::string(pattern);
        ++size_;
        return true;
    }
}
