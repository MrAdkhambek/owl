#pragma once

// The route trie
//
// Each node owns its literal children (sorted, binary searched) plus at
// most one parameter child. Matching walks segments and backtracks, trying
// literals before the parameter -- so /users/new beats /users/{id} without
// depending on registration order, which a linear scan cannot promise.
//
// Parameter names are owned by the nodes, so the views handed to Request
// stay valid for the router's lifetime.

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "owl/core/method.h"
#include "owl/http/request.h"
#include "owl/routing/detail/pattern.h"
#include "owl/routing/middleware.h"

namespace owl {
    // Chains attach at the root and at one node per descended segment, so a
    // matched path can never collect more of them than this. The cap being
    // provable is what keeps collection allocation-free with no silent drops.
    inline constexpr std::size_t max_middleware_chains = max_path_segments + 1;
}

namespace owl::detail {
    template <typename S>
    struct RouteNode {
        std::string label; // this node's literal segment
        std::string param_name; // set on a parameter child
        std::vector<std::unique_ptr<RouteNode>> literals; // sorted by label
        std::unique_ptr<RouteNode> param;
        std::vector<std::pair<Method, Handler<S>>> handlers;
        // Registered template for this node. graft() prefixes nested
        // tries so /api/v1 + /ping stays "/api/v1/ping", not "/ping".
        std::string pattern;
        MiddlewareChain<S> middleware;

        [[nodiscard]] const Handler<S>* handler_for(const Method method) const noexcept {
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
    template <typename S>
    struct ChainSpan {
        const MiddlewareChain<S>* const * data;
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
    template <typename S>
    struct MatchedChains {
        // Deliberately not zero-initialised: only [1, 1 + count) is ever
        // read, and count is set before any read. Value-initialising this
        // was ~272 bytes of memset per request for an array that is
        // usually empty.
        std::array<const MiddlewareChain<S>*, max_middleware_chains + 1> chains;
        std::size_t count = 0; // entries live at [1, 1 + count)

        void push(const MiddlewareChain<S>* chain) noexcept {
            chains[1 + count++] = chain;
        }

        [[nodiscard]] ChainSpan<S> splice(const MiddlewareChain<S>* front) noexcept {
            if (front == nullptr) return {.data = chains.data() + 1, .count = count};
            chains[0] = front;
            return {.data = chains.data(), .count = count + 1};
        }
    };

    template <typename S>
    [[nodiscard]] const Handler<S>*
    descend(
        const RouteNode<S>& node,
        const std::string_view* segments,
        const std::size_t count,
        const std::size_t index,
        const Method method,
        PathParam* captures,
        std::size_t& captured,
        MatchedChains<S>* chains,
        std::string_view* route
    ) {
        if (index == count) {
            if (const Handler<S>* handler = node.handler_for(method)) {
                if (route != nullptr) *route = node.pattern;
                return handler;
            }
            return nullptr;
        }

        if (const RouteNode<S>* child = node.find_literal(segments[index])) {
            const std::size_t mark = captured;
            const std::size_t chain_mark = chains != nullptr ? chains->count : 0;
            if (chains != nullptr && !child->middleware.empty()) {
                chains->push(&child->middleware);
            }
            if (const Handler<S>* handler = descend(*child, segments, count, index + 1, method, captures, captured, chains, route)) {
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
            if (const Handler<S>* handler = descend(*node.param, segments, count, index + 1, method,
                                                    captures, captured, chains, route)) {
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
    template <typename S>
    inline void collect_methods(const RouteNode<S>& node, const std::string_view* segments,
                                const std::size_t count, const std::size_t index,
                                MethodSet& out) noexcept {
        if (index == count) {
            for (const auto& method : node.handlers | std::views::keys) out.add(method);
            return;
        }
        if (const RouteNode<S>* child = node.find_literal(segments[index])) {
            collect_methods(*child, segments, count, index + 1, out);
        }
        if (node.param) {
            collect_methods(*node.param, segments, count, index + 1, out);
        }
    }

    template <typename S>
    [[nodiscard]] inline std::size_t depth(const RouteNode<S>& node) {
        std::size_t deepest = 0;
        for (const auto& child : node.literals) deepest = std::max(deepest, depth(*child));
        if (node.param) deepest = std::max(deepest, depth(*node.param));
        return deepest + 1;
    }

    [[nodiscard]] inline std::string join_route(const std::string_view prefix, const std::string_view inner) {
        if (inner.empty() || inner == "/") return std::string(prefix);
        if (prefix.empty() || prefix == "/") return std::string(inner);
        return std::string(prefix) + std::string(inner);
    }

    template <typename S>
    inline void prefix_patterns(RouteNode<S>& node, const std::string_view prefix) {
        if (!node.pattern.empty()) node.pattern = join_route(prefix, node.pattern);
        for (auto& child : node.literals) prefix_patterns(*child, prefix);
        if (node.param) prefix_patterns(*node.param, prefix);
    }

    // Splices one sub-trie into another. Subtrees move wholesale where the
    // target has nothing there yet, so this is mostly pointer moves rather
    // than a re-insertion of every nested route.
    template <typename S>
    inline void merge(RouteNode<S>& into, RouteNode<S>&& from) {
        for (auto& middleware : from.middleware) {
            into.middleware.push_back(std::move(middleware));
        }

        if (!from.pattern.empty()) into.pattern = std::move(from.pattern);

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
