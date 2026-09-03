#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <coro/task.h>

#include <fstr/fstr.h>

#include "owl/core/method.h"
#include "owl/coro/loop_scheduler.h"
#include "owl/extract/from_context.h"
#include "owl/http/request.h"
#include "owl/http/response.h"
#include "middleware.h"
#include "owl/core/state.h"

namespace owl {
    inline constexpr std::size_t max_path_segments = 32;

    // Chains attach at the root and at one node per descended segment, so a
    // matched path can never collect more of them than this. The cap being
    // provable is what keeps collection allocation-free with no silent drops.
    inline constexpr std::size_t max_middleware_chains = max_path_segments + 1;

    namespace detail {
        // ====================================================================
        // Route patterns: /users/{id}/posts/{pid}
        //
        // A segment is either a literal or exactly "{name}". Parsed at compile
        // time so a bad pattern is a static_assert, not a startup exception.
        // ====================================================================

        struct Segment {
            std::string_view text; // literal text, or the parameter name
            bool is_param = false;
        };

        struct PatternInfo {
            bool ok = false;
            const char* error = "uninitialised";
            std::size_t count = 0;
            std::size_t params = 0;
            std::array<Segment, max_path_segments> segments{};
        };

        [[nodiscard]] constexpr PatternInfo
        parse_pattern(const std::string_view pattern) noexcept {
            PatternInfo info{};
            if (pattern.empty()) {
                info.error = "pattern is empty";
                return info;
            }
            if (pattern.front() != '/') {
                info.error = "pattern must start with '/'";
                return info;
            }
            if (pattern.size() == 1) {
                // "/" is the root: zero segments, which is a valid route.
                info.ok = true;
                info.error = nullptr;
                return info;
            }

            const std::size_t n = pattern.size();
            for (std::size_t i = 0; i < n;) {
                ++i; // skip the '/'
                const std::size_t start = i;
                while (i < n && pattern[i] != '/') ++i;
                const std::string_view seg = pattern.substr(start, i - start);

                if (seg.empty()) {
                    info.error = "empty path segment";
                    return info;
                }
                if (info.count >= max_path_segments) {
                    info.error = "too many segments";
                    return info;
                }

                if (seg.front() == '{') {
                    if (seg.back() != '}') {
                        info.error = "unterminated '{' in segment";
                        return info;
                    }
                    const std::string_view name = seg.substr(1, seg.size() - 2);
                    if (name.empty()) {
                        info.error = "empty parameter name";
                        return info;
                    }
                    for (const char c : name) {
                        if (!util::is_word_char(c)) {
                            info.error = "invalid character in parameter name";
                            return info;
                        }
                    }
                    for (std::size_t k = 0; k < info.count; ++k) {
                        if (info.segments[k].is_param && info.segments[k].text == name) {
                            info.error = "duplicate parameter name";
                            return info;
                        }
                    }
                    if (info.params >= max_path_params) {
                        info.error = "too many parameters";
                        return info;
                    }
                    ++info.params;
                    info.segments[info.count++] = Segment{name, true};
                } else {
                    for (const char c : seg) {
                        if (c == '{' || c == '}') {
                            info.error = "brace inside a literal segment";
                            return info;
                        }
                    }
                    info.segments[info.count++] = Segment{seg, false};
                }
            }

            info.ok = true;
            info.error = nullptr;
            return info;
        }

        // Splits a request path into segments. Returns npos for anything that
        // cannot match: no leading '/', an empty segment, or too many.
        [[nodiscard]] inline std::size_t
        split_path(const std::string_view path, std::string_view* out, const std::size_t cap) noexcept {
            constexpr auto no_match = static_cast<std::size_t>(-1);

            if (path.empty() || path.front() != '/') [[unlikely]] return no_match;
            if (path.size() == 1) return 0; // "/" is the root

            // Hand-rolled rather than `trimmed | std::views::split('/')`: this
            // runs on every request, almost always over one or two segments,
            // and the range adaptor's machinery does not survive that at -O3.
            // Semantics are unchanged, including that "//", a trailing '/' and
            // an over-long path all report no_match.
            const std::size_t n = path.size();
            std::size_t count = 0;
            std::size_t i = 1; // skip the leading '/'

            for (;;) {
                const std::size_t start = i;
                while (i < n && path[i] != '/') ++i;

                if (i == start) [[unlikely]] return no_match; // "//"
                if (count >= cap) [[unlikely]] return no_match;
                out[count++] = path.substr(start, i - start);

                if (i == n) return count;
                ++i; // step over the '/'
                if (i == n) [[unlikely]] return no_match; // trailing '/'
            }
        }

        // Which state type an extractor asks for, if it asks for one. A
        // handler naming State<T> only makes sense on a Router<T>, and the
        // cast in FromContext<State<T>> is unchecked, so route() proves the
        // match at compile time rather than reinterpreting the pointer.
        template <class T>
        struct state_of {
            static constexpr bool is_state = false;
            using type = void;
        };

        template <class T>
        struct state_of<State<T>> {
            static constexpr bool is_state = true;
            using type = T;
        };

        // Which path parameter an extractor names, empty if it names none.
        template <class T>
        struct path_param_of {
            static constexpr std::string_view value{};
        };

        template <fstr::fstr Pattern>
        struct path_param_of<PathView<Pattern>> {
            static constexpr std::string_view value = Pattern.view();
        };

        template <fstr::fstr Pattern, Parseable T>
        struct path_param_of<Path<Pattern, T>> {
            static constexpr std::string_view value = Pattern.view();
        };

        template <fstr::fstr Pattern>
        [[nodiscard]] constexpr bool
        declares(const std::string_view name) noexcept {
            constexpr PatternInfo info = parse_pattern(Pattern.view());
            for (std::size_t i = 0; i < info.count; ++i) {
                if (info.segments[i].is_param && info.segments[i].text == name) return true;
            }
            return false;
        }
    }

    // ========================================================================
    // MethodRouter: the set of methods served at one path.
    //
    //     get(list_users).post(create_user)
    //
    // Handler signatures are carried in the type rather than erased on the
    // spot, because the path parameters a handler asks for can only be checked
    // once route<Pattern>() sees the pattern and the handlers together.
    // ========================================================================

    namespace detail {
        // R is the handler's return type -- Response or coro::task<Response>.
        // Carrying it in the type is what lets mount() wrap a synchronous
        // handler instead of forcing every handler to be a coroutine.
        template <Method M, typename R, typename... Args>
        struct Endpoint {
            static constexpr Method method = M;

            R (*handler)(Args...);
        };

        template <class R>
        inline constexpr bool is_handler_return_v =
            std::is_same_v<R, Response> || std::is_same_v<R, coro::task<Response>>;
    }

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

    // ========================================================================
    // The trie
    //
    // Each node owns its literal children (sorted, binary searched) plus at
    // most one parameter child. Matching walks segments and backtracks, trying
    // literals before the parameter -- so /users/new beats /users/{id} without
    // depending on registration order, which a linear scan cannot promise.
    //
    // Parameter names are owned by the nodes, so the views handed to Request
    // stay valid for the router's lifetime.
    //
    // ========================================================================

    namespace detail {
        struct RouteNode {
            std::string label; // this node's literal segment
            std::string param_name; // set on a parameter child
            std::vector<std::unique_ptr<RouteNode>> literals; // sorted by label
            std::unique_ptr<RouteNode> param;
            std::vector<std::pair<Method, Handler>> handlers;
            MiddlewareChain middleware;

            [[nodiscard]] const Handler* handler_for(const Method method) const noexcept {
                for (const auto& [key, handler] : handlers) {
                    if (key == method) return &handler;
                }
                return nullptr;
            }

            [[nodiscard]] RouteNode* find_literal(const std::string_view text) const noexcept {
                const auto pos = std::ranges::lower_bound(literals, text, {}, [](const auto& node) {
                    return std::string_view{node->label};
                });
                if (pos != literals.end() && (*pos)->label == text) return pos->get();
                return nullptr;
            }

            RouteNode& literal_child(const std::string_view text) {
                const auto pos = std::ranges::lower_bound(
                    literals, text, {}, [](const auto& node) {
                        return std::string_view{node->label};
                    });
                if (pos != literals.end() && (*pos)->label == text) return **pos;
                auto child = std::make_unique<RouteNode>();
                child->label = std::string(text);
                return **literals.insert(pos, std::move(child));
            }
        };

        // A view of the chains to run, outermost first. Points into the
        // MatchedChains the driver holds, which outlives the chain.
        struct ChainSpan {
            const MiddlewareChain* const * data;
            std::size_t count;
        };

        // The middleware a request collects while matching: pointers to the
        // chains of every group root on the matched path, outermost first.
        // The chains themselves live in the trie, so the pointers outlive any
        // request handed them; the array lives in whoever asked for the match.
        //
        // Slot 0 is left empty for the server-wide chain, which belongs to the
        // Server rather than the trie and is spliced in at dispatch. Reserving
        // it here is what lets the driver hand `Next` a view straight into
        // this array: it used to copy the whole thing into a second array in
        // its coroutine frame, costing 272 bytes of frame plus the memset that
        // zeroed them, on every request.
        struct MatchedChains {
            // Deliberately not zero-initialised: only [1, 1 + count) is ever
            // read, and count is set before any read. Value-initialising this
            // was ~272 bytes of memset per request for an array that is
            // usually empty.
            std::array<const MiddlewareChain*, max_middleware_chains + 1> chains;
            std::size_t count = 0; // entries live at [1, 1 + count)

            void push(const MiddlewareChain* chain) noexcept {
                chains[1 + count++] = chain;
            }

            [[nodiscard]] ChainSpan splice(const MiddlewareChain* front) noexcept {
                if (front == nullptr) return {.data = chains.data() + 1, .count = count};
                chains[0] = front;
                return {.data = chains.data(), .count = count + 1};
            }
        };

        [[nodiscard]] inline const Handler*
        descend(const RouteNode& node, const std::string_view* segments, const std::size_t count,
                const std::size_t index, const Method method,
                PathParam* captures, std::size_t& captured,
                MatchedChains* chains) {
            if (index == count) {
                if (const Handler* handler = node.handler_for(method)) {
                    return handler;
                }
                return nullptr;
            }

            if (const RouteNode* child = node.find_literal(segments[index])) {
                const std::size_t mark = captured;
                const std::size_t chain_mark = chains != nullptr ? chains->count : 0;
                if (chains != nullptr && !child->middleware.empty()) {
                    chains->push(&child->middleware);
                }
                if (const Handler* handler = descend(*child, segments, count, index + 1, method, captures, captured, chains)) {
                    return handler;
                }
                captured = mark;
                if (chains != nullptr) chains->count = chain_mark;
            }

            if (node.param) {
                const std::size_t mark = captured;
                const std::size_t chain_mark = chains != nullptr ? chains->count : 0;
                if (chains != nullptr && !node.param->middleware.empty()) {
                    chains->push(&node.param->middleware);
                }
                if (captured < max_path_params) {
                    captures[captured++] = {.name = std::string_view{node.param->param_name}, .value = segments[index]};
                }
                if (const Handler* handler = descend(*node.param, segments, count, index + 1, method,
                                                     captures, captured, chains)) {
                    return handler;
                }
                captured = mark;
                if (chains != nullptr) chains->count = chain_mark;
            }

            return nullptr;
        }

        // Collects every method reachable at this path. Unlike descend() it
        // does not stop at the first hit and does not backtrack away from a
        // subtree: a request can route through either the literal child or the
        // parameter child, so Allow is the union of both.
        inline void collect_methods(const RouteNode& node, const std::string_view* segments,
                                    const std::size_t count, const std::size_t index,
                                    MethodSet& out) noexcept {
            if (index == count) {
                for (const auto& method : node.handlers | std::views::keys) out.add(method);
                return;
            }
            if (const RouteNode* child = node.find_literal(segments[index])) {
                collect_methods(*child, segments, count, index + 1, out);
            }
            if (node.param) {
                collect_methods(*node.param, segments, count, index + 1, out);
            }
        }

        [[nodiscard]] inline std::size_t depth(const RouteNode& node) {
            std::size_t deepest = 0;
            for (const auto& child : node.literals) deepest = std::max(deepest, depth(*child));
            if (node.param) deepest = std::max(deepest, depth(*node.param));
            return deepest + 1;
        }

        // Splices one sub-trie into another. Subtrees move wholesale where the
        // target has nothing there yet, so this is mostly pointer moves rather
        // than a re-insertion of every nested route.
        inline void merge(RouteNode& into, RouteNode&& from) {
            for (auto& middleware : from.middleware) {
                into.middleware.push_back(std::move(middleware));
            }

            for (auto& [method, handler] : from.handlers) {
                if (into.handler_for(method)) {
                    throw std::invalid_argument("nested route collides with one already registered");
                }
                into.handlers.emplace_back(method, std::move(handler));
            }

            for (auto& child : from.literals) {
                merge(into.literal_child(child->label), std::move(*child));
            }

            if (from.param) {
                if (!into.param) {
                    into.param = std::move(from.param); // whole subtree, one pointer
                } else if (into.param->param_name != from.param->param_name) {
                    throw std::invalid_argument("nested route names a parameter differently at the same position");
                } else {
                    merge(*into.param, std::move(*from.param));
                }
            }
        }
    }

    template <typename S, typename F>
    Middleware wrap_layer(F fn) {
        if constexpr (requires(F f, const Request& r, State<S> s, Next n) {
            { f(r, s, n) } -> std::convertible_to<coro::task<Response>>;
        }) {
            static_assert(!std::is_void_v<S>, "layer with State<T> requires Router<T>");
            return [fn = std::move(fn)](const Request& req, Next next) -> coro::task<Response> {
                auto state = FromContext<State<S>>{}(req.dispatch_context(), req);
                if (!state) co_return to_response(std::move(state).error());
                co_return co_await fn(req, *std::move(state), next);
            };
        } else if constexpr (requires(F f, const Request& r, Next n) {
            { f(r, n) } -> std::convertible_to<coro::task<Response>>;
        }) {
            return [fn = std::move(fn)](const Request& req, Next next) -> coro::task<Response> {
                co_return co_await fn(req, next);
            };
        } else {
            static_assert(false,
                "layer expects task<Response>(const Request&, Next) "
                "or task<Response>(const Request&, State<S>, Next)");
        }
    }

    template <typename S = void>
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

        template <typename F>
        [[nodiscard]] Router layer(F middleware) && {
            root_.middleware.emplace_back(wrap_layer<S>(std::move(middleware)));
            return std::move(*this);
        }

        template <fstr::fstr Prefix, typename NestedS>
        [[nodiscard]] Router nest(Router<NestedS>&& nested) && requires std::same_as<NestedS, S> {
            static_assert(detail::parse_pattern(Prefix.view()).ok, "invalid nest prefix");
            static_assert(detail::parse_pattern(Prefix.view()).params == 0, "a nest prefix must be literal; parameters belong in the nested routes");
            graft(Prefix.view(), std::move(nested));
            return std::move(*this);
        }

        // Binds the state and hands back a router that no longer needs any:
        // the type is the proof, so a router reaching the Server without its
        // state is a compile error rather than a null read per request.
        [[nodiscard]] Router<void>
        with_state(std::shared_ptr<S> state) && requires (!std::is_void_v<S>) {
            if (!state) throw std::invalid_argument("Router: state is required");
            Router<void> bound;
            bound.root_ = std::move(root_);
            bound.size_ = size_;
            bound.state_ = std::move(state);
            return bound;
        }

        [[nodiscard]] const Handler*
        match(const Method method, const std::string_view path, Request& req, detail::MatchedChains* out = nullptr) const {
            if (state_) req.set_dispatch_state(state_);
            if (out != nullptr) {
                out->count = 0;
                if (!root_.middleware.empty()) {
                    out->push(&root_.middleware);
                }
            }

            std::array<std::string_view, max_path_segments> segments{};
            const std::size_t count = detail::split_path(path, segments.data(), segments.size());
            if (count == static_cast<std::size_t>(-1)) return nullptr;

            std::array<PathParam, max_path_params> captures{};
            std::size_t captured = 0;
            const Handler* handler = detail::descend(root_, segments.data(), count, 0, method,
                                                     captures.data(), captured, out);
            if (!handler) return nullptr;

            req.clear_params();
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
        // with_state() moves one instantiation's trie into another's.
        template <typename>
        friend class Router;

        Router() = default;

        bool insert(Method method, std::string_view pattern, Handler handler);

        template <fstr::fstr Pattern, Method M, typename R, typename... Args>
        void mount(const detail::Endpoint<M, R, Args...>& endpoint) {
            static_assert(detail::is_handler_return_v<R>, "a handler must return Response or coro::task<Response>");
            static_assert(
                ((detail::path_param_of<std::remove_cvref_t<Args>>::value.empty()
                    || detail::declares<Pattern>(detail::path_param_of<std::remove_cvref_t<Args>>::value)) && ...),
                "handler asks for a path parameter this route pattern does not declare");
            static_assert(
                ((!detail::state_of<std::remove_cvref_t<Args>>::is_state
                    || std::is_same_v<typename detail::state_of<std::remove_cvref_t<Args>>::type, S>) && ...),
                "handler asks for a State<T> that is not this router's state type");

            auto* const handler = endpoint.handler;
            add(M, Pattern.view(), [handler](const Request& req) -> coro::task<Response> {
                const auto ctx = req.dispatch_context();
                auto args = extract_all<std::remove_cvref_t<Args>...>(ctx, req);
                if (!args) [[unlikely]] co_return to_response(std::move(args).error());
                if constexpr (std::is_same_v<R, Response>) {
                    co_return std::apply(handler, *std::move(args));
                } else {
                    co_return co_await std::apply(handler, *std::move(args));
                }
            });
        }

        void graft(const std::string_view prefix, Router&& nested) {
            // Once with_state() has run both sides are Router<void>, so the
            // type system cannot tell a bound sub-app from a stateless one.
            // Merging would drop the inner state silently; refuse instead.
            if (nested.state_) {
                throw std::invalid_argument("nest a router before applying its state");
            }

            const detail::PatternInfo info = detail::parse_pattern(prefix);

            if (info.count + detail::depth(nested.root_) > max_path_segments) {
                throw std::invalid_argument(std::string("nesting under ").append(prefix).append(" exceeds the maximum path depth"));
            }

            detail::RouteNode* node = &root_;
            for (std::size_t i = 0; i < info.count; ++i) {
                node = &node->literal_child(info.segments[i].text);
            }

            detail::merge(*node, std::move(nested.root_));
            size_ += nested.size_;
        }

        void add(const Method method, const std::string_view pattern, Handler handler) {
            if (!insert(method, pattern, std::move(handler))) {
                throw std::invalid_argument(std::string("cannot register route: ").append(pattern));
            }
        }

        std::shared_ptr<void> state_;
        detail::RouteNode root_;
        std::size_t size_ = 0;
    };

    template <typename S>
    bool Router<S>::insert(const Method method, const std::string_view pattern, Handler handler) {
        const detail::PatternInfo info = detail::parse_pattern(pattern);
        if (!info.ok) return false;

        detail::RouteNode* node = &root_;
        for (std::size_t i = 0; i < info.count; ++i) {
            const detail::Segment& segment = info.segments[i];
            if (segment.is_param) {
                if (!node->param) {
                    node->param = std::make_unique<detail::RouteNode>();
                    node->param->param_name = std::string(segment.text);
                } else if (node->param->param_name != segment.text) {
                    // Two routes naming the same slot differently would make
                    // param() depend on which route won. Refuse instead.
                    return false;
                }
                node = node->param.get();
            } else {
                node = &node->literal_child(segment.text);
            }
        }

        if (node->handler_for(method)) return false; // duplicate route
        node->handlers.emplace_back(method, std::move(handler));
        ++size_;
        return true;
    }
}
