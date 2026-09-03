#pragma once

// The extractor vocabulary: what a handler's parameters are made of. A
// handler declares what it wants -- Path<"id">, HeaderView<"x-api-key">,
// Json<> -- and each type here carries the two things extraction needs: a
// `name` constant that kick messages quote back, and an `extract` (where
// one exists) that lifts it out of a Request.
//
// The split that runs through the file is view vs parsed. View types
// borrow a slice of the request and fail only by absence; parsed types
// additionally convert, which adds a second failure mode -- the value is
// there but not parseable -- that a view's plain optional cannot carry.
// from_context.h is what turns either failure into a status code.

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include <fstr/fstr.h>

#include "owl/http/request.h"
#include "parse.h"

namespace owl {
    // The request body, viewed in place -- no copy, no parse. A body always
    // exists, possibly empty, so extraction cannot fail and extract()
    // returns the value rather than an optional.
    struct BodyView final {
        // Points into the request's entity storage; valid as long as the
        // request is.
        std::string_view value;

        // Explicit so a stray string_view never silently becomes a body.
        explicit BodyView(const std::string_view value) noexcept : value(value) {
        }

        [[nodiscard]] static BodyView extract(const Request& req) noexcept {
            return BodyView{req.body()};
        }
    };

    // A request header parsed into T. The fstr pattern names the header --
    // matched case-insensitively, as header names are -- and surfaces as
    // the `name` constant that kicks quote back:
    //
    //     Header<"x-api-key", std::uint64_t> key;
    //
    // Deliberately no extract(): a bare optional cannot tell a missing
    // header from an unparseable one, so FromContext drives the lookup and
    // keeps the two failures apart.
    template <fstr::fstr Pattern, Parseable T = std::string>
    struct Header {
        // The header name as written in the pattern, for error messages.
        static constexpr std::string_view name = Pattern.view();
        T value{};

        // Built by FromContext once the raw text has parsed.
        explicit Header(T value) : value(std::move(value)) {
        }
    };

    // A request header left as text. The view points into the request's
    // header table, which outlives the handler; reach for Header instead
    // when the value is meant to be typed.
    template <fstr::fstr Pattern>
    struct HeaderView {
        // The header name as written in the pattern, for error messages.
        static constexpr std::string_view name = Pattern.view();
        std::string_view value;

        // Explicit so a bare string_view never silently becomes a header.
        explicit HeaderView(const std::string_view value) noexcept : value(value) {
        }

        // Absence is the only failure: a view has nothing to parse, so the
        // optional means exactly "the header was not sent".
        [[nodiscard]] static std::optional<HeaderView>
        extract(const Request& req) noexcept {
            return req.header(name).transform([](const auto& val) noexcept {
                return HeaderView{val};
            });
        }
    };

    // The request body decoded as JSON. The default is the raw nlohmann
    // tree; a concrete T is converted out of it, and it is the conversion
    // -- not the parse -- that reports 422, because a body that parses but
    // does not fit T is a different complaint than one that is not JSON at
    // all. The failure mapping lives in FromContext<Json<T>>.
    template <typename T = nlohmann::json>
    struct Json {
        // The decoded tree, or the converted value for a concrete T.
        T value;
    };

    // A route parameter parsed into T. The pattern must name a parameter
    // the matched route template captures; a name the route never binds is
    // simply absent, and extraction reports it as a miss. As with Header,
    // there is no extract(): missing and malformed must stay distinguishable.
    template <fstr::fstr Pattern, Parseable T>
    struct Path {
        // The parameter name as written in the pattern, for error messages.
        static constexpr std::string_view name = Pattern.view();
        T value{};

        // Built by FromContext once the raw text has parsed.
        explicit Path(T value) : value(std::move(value)) {
        }
    };

    // A route parameter left as text. The view points into the request's
    // normalized path, which lives as long as the request does.
    template <fstr::fstr Pattern>
    struct PathView {
        // The parameter name as written in the pattern, for error messages.
        static constexpr std::string_view name = Pattern.view();
        std::string_view value;

        // Explicit so a bare string_view never silently becomes a parameter.
        explicit PathView(const std::string_view value) noexcept : value(value) {
        }

        // Absence is the only failure, as with HeaderView.
        [[nodiscard]] static std::optional<PathView>
        extract(const Request& req) noexcept {
            return req.param(name).transform([](const auto& val) noexcept {
                return PathView{val};
            });
        }
    };

    // A query-string parameter parsed into T. As with Header and Path,
    // there is no extract(): a bare optional cannot separate a missing
    // parameter from one that does not parse.
    template <fstr::fstr Pattern, Parseable T>
    struct Query {
        // The parameter name as written in the pattern, for error messages.
        static constexpr std::string_view name = Pattern.view();
        T value{};

        // Built by FromContext once the raw text has parsed.
        explicit Query(T value) : value(std::move(value)) {
        }
    };

    // A query-string parameter left as text, borrowed from the parsed
    // query string, which lives as long as the request does.
    template <fstr::fstr Pattern>
    struct QueryView {
        // The parameter name as written in the pattern, for error messages.
        static constexpr std::string_view name = Pattern.view();
        std::string_view value;

        // Explicit so a bare string_view never silently becomes a parameter.
        explicit QueryView(const std::string_view value) noexcept : value(value) {
        }

        // Absence is the only failure, as with HeaderView.
        [[nodiscard]] static std::optional<QueryView>
        extract(const Request& req) noexcept {
            return req.query(name).transform([](const auto& val) noexcept {
                return QueryView{val};
            });
        }
    };

    // Escape hatch: the whole Request, for handlers that need something no
    // other extractor covers -- raw h2o access, the method. Extraction
    // cannot fail.
    struct RequestView final {
        // A const pointer to a const Request: the view is fixed at
        // construction and cannot be reseated, which makes the type
        // non-assignable. That is why extract_all emplaces into its slots
        // instead of assigning them.
        const Request* const value;

        // Takes the address; only extract() has a request to hand over.
        explicit RequestView(const Request* const req) noexcept : value(req) {
        }

        [[nodiscard]] static RequestView
        extract(const Request& req) noexcept {
            return RequestView{&req};
        }

        // Pointer-like facade, so handler code reads as if it were holding
        // the request itself: `(*req).path()` or `req->raw()`.
        const Request& operator*() const noexcept {
            return *value;
        }

        const Request* operator->() const noexcept {
            return value;
        }
    };
}
