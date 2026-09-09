#pragma once

// Extraction as a protocol. A handler's declared parameters are pulled one
// by one through FromContext<T>, which answers expected<T, KickToken>:
// either the value, or the HTTP-shaped reason it could not be produced.
// This header is the bridge between the declarative extractors and the
// response layer -- the place where "missing" becomes a status code -- and
// it owns the kick mapping: headers and query parameters qualify a request
// for an existing resource, so their failures are client errors (400,
// 415/422 for a body), while a path parameter addresses the resource and
// fails with 404.

#include <concepts>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>

#include <fstr/fstr.h>

#include "extractors.h"
#include "owl/http/request.h"
#include "owl/http/response.h"
#include "parse.h"
#include "owl/coro/loop_scheduler.h"
#include "owl/core/state.h"
#include "owl/core/token.h"
#include "owl/util/util.h"

#if defined(OWL_ENABLE_POSTGRESQL) || defined(OWL_ENABLE_SQLITE)
#include <sql/pool.h>
#endif
#ifdef OWL_ENABLE_POSTGRESQL
#include <sql/psql.h>
#endif
#ifdef OWL_ENABLE_SQLITE
#include <sql/sqlite.h>
#endif

namespace owl {
    namespace detail {
        // Every KickToken factory shares one shape -- a message in, an
        // unexpected out -- which is what lets "which failure" travel
        // through lift as a plain function pointer.
        using KickFactory = std::unexpected<KickToken> (*)(std::string);

        // One voice for every kick message -- "missing header: x-api-key" --
        // rather than each specialization inventing its own format.
        [[nodiscard]] inline std::string
        describe(const std::string_view what, const std::string_view noun, const std::string_view name) {
            return std::format("{} {}: {}", what, noun, name);
        }

        // A view extractor already models "look it up, or it is not there"; all
        // FromContext adds is which kick a miss turns into.
        template <typename T>
        [[nodiscard]] std::expected<T, KickToken>
        lift(std::optional<T> found, const KickFactory missing, const std::string_view noun) {
            if (found) return std::move(*found);
            return missing(describe("missing", noun, T::name));
        }

        // Parsed extractors need the miss and the parse failure to stay
        // distinguishable, which a plain optional cannot carry.
        template <typename T, typename Raw>
        [[nodiscard]] std::expected<T, KickToken>
        lift_parsed(const std::optional<Raw> raw, const KickFactory kick, const std::string_view noun) {
            if (!raw) return kick(describe("missing", noun, T::name));
            if (auto parsed = fromString<decltype(T::value)>(*raw)) return T{*std::move(parsed)};
            return kick(describe("malformed", noun, T::name));
        }
    }

    // The extraction protocol: produce a T from the request, or a KickToken
    // saying why not. The primary template is left undefined on purpose --
    // a parameter type with no specialization is a compile error at the
    // use site, not a runtime 500. Specialize for each extractable type.
    template <class T>
    struct FromContext;

    // The spelling extract_all goes through. A variable rather than a type
    // keeps call sites free of braces.
    template <class T>
    inline constexpr FromContext<T> fromContext{};

    // Reference extraction, opted into per type: a handler parameter declared
    // const T& binds the object this answers with instead of materializing a
    // copy. The pointer must address something that outlives the request --
    // a Context member does, a temporary dangles -- which is why only
    // extractors able to make that guarantee offer it.
    template <class T>
    struct FromContextRef;

    template <class T>
    inline constexpr FromContextRef<T> fromContextRef{};

    // The body always exists, possibly empty, so there is no failure to
    // map; the specialization exists so BodyView is extractable at all.
    template <>
    struct FromContext<BodyView> {
        template <typename S>
        std::expected<BodyView, KickToken> operator()(const Context<S>&, const Request& req) const noexcept {
            return BodyView::extract(req);
        }
    };

    // A header qualifies the request rather than addressing the resource,
    // so a missing and a malformed header are the same kind of client
    // mistake -- 400 either way. Path is the exception; see its
    // specialization for why.
    template <fstr::fstr Pattern, typename T>
    struct FromContext<Header<Pattern, T>> {
        using TargetType = Header<Pattern, T>;

        template <typename S>
        std::expected<TargetType, KickToken> operator()(const Context<S>&, const Request& req) const {
            return detail::lift_parsed<TargetType>(req.header(TargetType::name), &KickToken::bad_request, "header");
        }
    };

    // lift() handles everything: a view lookup fails only by absence.
    template <fstr::fstr Pattern>
    struct FromContext<HeaderView<Pattern>> {
        using TargetType = HeaderView<Pattern>;

        template <typename S>
        std::expected<TargetType, KickToken> operator()(const Context<S>&, const Request& req) const {
            return detail::lift(TargetType::extract(req), &KickToken::bad_request, "header");
        }
    };

    // Three distinct failures, checked in order: the body is not declared
    // JSON (415 -- the client misdescribed it), it does not parse (400),
    // or, when T is a concrete type, it parses but does not fit T (422 --
    // the complaint is with the content, not the transport). Parse runs
    // with exceptions off and reports through the discarded sentinel, so
    // the no-JSON path never pays for unwinding.
    template <typename T>
    struct FromContext<Json<T>> {
        template <typename S>
        std::expected<Json<T>, KickToken> operator()(const Context<S>&, const Request& req) const {
            const auto content_type = req.header("content-type");
            if (!content_type || !util::is_json_content_type(*content_type)) {
                return KickToken::unsupported_media_type("expected a JSON body");
            }
            auto parsed = nlohmann::json::parse(req.body(), nullptr, false);
            if (parsed.is_discarded()) return KickToken::bad_request("body is not valid JSON");
            if constexpr (std::is_same_v<T, nlohmann::json>) {
                return Json<T>{std::move(parsed)};
            } else {
                try {
                    return Json<T>{parsed.template get<T>()};
                } catch (const nlohmann::json::exception& e) {
                    return KickToken::unprocessable(e.what());
                }
            }
        }
    };

    // The one family that kicks 404. A path parameter addresses the
    // resource: missing or malformed, the URL simply does not name a
    // resource here. Headers and query strings qualify a request for a
    // resource that is otherwise well addressed, which is why their
    // failures stay 400.
    template <fstr::fstr Pattern, typename T>
    struct FromContext<Path<Pattern, T>> {
        using TargetType = Path<Pattern, T>;

        template <typename S>
        std::expected<TargetType, KickToken> operator()(const Context<S>&, const Request& req) const {
            return detail::lift_parsed<TargetType>(req.param(TargetType::name), &KickToken::not_found, "path parameter");
        }
    };

    // Same 404 mapping as the parsed form; lift() carries it.
    template <fstr::fstr Pattern>
    struct FromContext<PathView<Pattern>> {
        using TargetType = PathView<Pattern>;

        template <typename S>
        std::expected<TargetType, KickToken> operator()(const Context<S>&, const Request& req) const {
            return detail::lift(TargetType::extract(req), &KickToken::not_found, "path parameter");
        }
    };

    // As with Header: 400 either way, a query parameter qualifies rather
    // than addresses.
    template <fstr::fstr Pattern, typename T>
    struct FromContext<Query<Pattern, T>> {
        using TargetType = Query<Pattern, T>;

        template <typename S>
        std::expected<TargetType, KickToken> operator()(const Context<S>&, const Request& req) const {
            return detail::lift_parsed<TargetType>(req.query(TargetType::name), &KickToken::bad_request, "query parameter");
        }
    };

    // As with HeaderView: lift() carries the whole story.
    template <fstr::fstr Pattern>
    struct FromContext<QueryView<Pattern>> {
        using TargetType = QueryView<Pattern>;

        template <typename S>
        std::expected<TargetType, KickToken> operator()(const Context<S>&, const Request& req) const {
            return detail::lift(TargetType::extract(req), &KickToken::bad_request, "query parameter");
        }
    };

    // Nothing to extract, so nothing to fail: the request is its own
    // extraction.
    template <>
    struct FromContext<RequestView> {
        template <typename S>
        std::expected<RequestView, KickToken> operator()(const Context<S>&, const Request& req) const noexcept {
            return RequestView::extract(req);
        }
    };

    // Router-attached state, shared across handlers. Missing state is a
    // wiring mistake by the application author, not a caller error, which
    // is why it kicks 500 rather than anything 4xx.
    template <typename T>
    struct FromContext<State<T>> {
        template <typename S>
        std::expected<State<T>, KickToken> operator()(const Context<S>& ctx, const Request&) const {
            if constexpr (std::is_same_v<S, T>) {
                if (ctx.state) return State<T>{ctx.state};
            }
            return KickToken::internal_error("no state attached to this router");
        }
    };

    // The worker's event loop, carried by the per-worker Context. Specialized
    // here rather than in loop_scheduler.h: Context owns the scheduler by
    // value, so loop_scheduler.h cannot include this header without closing
    // an include cycle through core/state.h.
    template <>
    struct FromContext<loop_scheduler> {
        template <typename S>
        std::expected<loop_scheduler, KickToken> operator()(const Context<S>& ctx, const Request&) const {
            if (ctx.loop) return ctx.loop;
            return KickToken::internal_error("not on an h2o worker");
        }
    };

    // The reference spelling of the loop extraction: the handler's const&
    // binds the Context's own scheduler -- zero copies, the live per-worker
    // member rather than a snapshot of it.
    template <>
    struct FromContextRef<loop_scheduler> {
        template <typename S>
        std::expected<const loop_scheduler*, KickToken> operator()(const Context<S>& ctx, const Request&) const {
            if (ctx.loop) return &ctx.loop;
            return KickToken::internal_error("not on an h2o worker");
        }
    };

#ifdef OWL_ENABLE_POSTGRESQL
    // The worker's postgres pool. A Context member outlives the request,
    // which is the FromContextRef guarantee; handlers take
    // const sql::pool<sql::psql>& -- a source is a capability, not state the
    // handler mutates. An empty optional (with_psql never called) is a
    // wiring mistake, so it kicks 500 rather than anything 4xx.
    template <>
    struct FromContextRef<sql::pool<sql::psql>> {
        template <typename S>
        std::expected<const sql::pool<sql::psql>*, KickToken> operator()(const Context<S>& ctx, const Request&) const {
            if (ctx.psql) return &*ctx.psql;
            return KickToken::internal_error("postgres pool is not wired");
        }
    };
#endif
#ifdef OWL_ENABLE_SQLITE
    template <>
    struct FromContextRef<sql::pool<sql::sqlite>> {
        template <typename S>
        std::expected<const sql::pool<sql::sqlite>*, KickToken> operator()(const Context<S>& ctx, const Request&) const {
            if (ctx.sqlite) return &*ctx.sqlite;
            return KickToken::internal_error("sqlite pool is not wired");
        }
    };
#endif

    // The handler-parameter contract, read in the spelling the handler
    // declared: a by-value T extracts a value through FromContext<T>; a
    // const T& binds the extractor's underlying object through FromContextRef<T>
    // -- zero copies, valid because the pointed-to value outlives the request.
    // A mutable T& is refused: extraction sees the Context as const, so there
    // is nothing mutable to bind.
    template <typename T, typename S>
    concept ExtractableValue = requires(const Context<S>& ctx, const Request& req) {
        { FromContext<T>{}(ctx, req) } -> std::same_as<std::expected<T, KickToken>>;
    };

    template <typename T, typename S>
    concept ExtractableRef = requires(const Context<S>& ctx, const Request& req) {
        { FromContextRef<T>{}(ctx, req) } -> std::same_as<std::expected<const T*, KickToken>>;
    };

    template <typename Arg, typename S>
    concept Extractable =
        (!std::is_reference_v<Arg> && ExtractableValue<std::remove_cvref_t<Arg>, S>) ||
        (std::is_reference_v<Arg> && std::is_const_v<std::remove_reference_t<Arg>>
            && ExtractableRef<std::remove_cvref_t<Arg>, S>);

namespace detail {
        // Slot for one handler parameter: a value parameter owns its
        // extraction; a reference parameter holds a pointer to the extracted
        // object's home rather than a copy of it.
        template <typename Arg>
        using ExtractSlot = std::conditional_t<
            std::is_reference_v<Arg>,
            const std::remove_cvref_t<Arg>*,
            std::optional<std::remove_cvref_t<Arg>>>;

        // Turns one slot into one tuple element: a reference parameter binds
        // the extracted object itself; a value parameter moves the extracted
        // value out.
        template <typename Arg, typename Slot>
        [[nodiscard]] decltype(auto) materialize_slot(Slot& slot) {
            if constexpr (std::is_reference_v<Arg>) {
                return *slot;
            } else {
                return std::move(*slot);
            }
        }
    }

    // Pulls every parameter a handler declares, or stops at the first
    // failure and returns it alone: a handler body never sees
    // half-populated parameters. Value slots are optionals so that a failure
    // can leave later ones empty; they are emplaced rather than assigned
    // because RequestView is not assignable (const member).
    template <typename... Args, typename S>
        requires ((Extractable<Args, S> && ...))
    [[nodiscard]] std::expected<std::tuple<Args...>, KickToken>
    extract_all(const Context<S>& ctx, const Request& req) {
        // The first failure wins; the fold stops there, so "first" and
        // "only" are the same thing.
        std::optional<KickToken> failure;
        std::tuple<detail::ExtractSlot<Args>...> slots;

        // The slot's own shape decides the route: a pointer slot is a
        // reference parameter and binds through FromContextRef; an optional
        // slot is a value parameter and owns its extraction.
        const auto run = [&](auto& slot) {
            using Slot = std::remove_cvref_t<decltype(slot)>;
            if constexpr (std::is_pointer_v<Slot>) {
                using T = std::remove_cv_t<std::remove_pointer_t<Slot>>;
                auto result = fromContextRef<T>(ctx, req);
                if (!result) {
                    failure = std::move(result).error();
                    return false;
                }
                slot = *result;
            } else {
                using T = typename Slot::value_type;
                auto result = fromContext<T>(ctx, req);
                if (!result) {
                    failure = std::move(result).error();
                    return false;
                }
                // emplace, not assign: RequestView holds a const member and so is
                // not assignable, but constructing it in place is fine.
                slot.emplace(*std::move(result));
            }
            return true;
        };

        // The fold short-circuits: nothing is extracted after the first failure.
        std::apply([&](auto&... slot) {
            (void)(run(slot) && ...);
        }, slots);

        if (failure) {
            return std::unexpected(*std::move(failure));
        }

        return std::apply([&](auto&... slot) {
            return std::tuple<Args...>{detail::materialize_slot<Args>(slot)...};
        }, slots);
    }

    // The bridge back to HTTP: a kick becomes a Response carrying the
    // status it named and the message it holds. It lives here rather than
    // in http/ because kicks are this layer's vocabulary.
    [[nodiscard]] inline Response to_response(KickToken kick) {
        const auto status = kick.status();
        return Response::ok(std::move(kick).message(), status);
    }
}
