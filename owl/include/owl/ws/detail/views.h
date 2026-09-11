#pragma once

// Explicit list, not structural detection: a silent miss would dangle.
// A WebSocket handler outlives its request, so views into that request
// must not compile.

#include <string_view>

#include <fstr/fstr.h>

#include "owl/extract/extractors.h"

namespace owl::detail {
    template <typename T>
    inline constexpr bool is_view_extractor_v = false;

    template <fstr::fstr P>
    inline constexpr bool is_view_extractor_v<PathView<P>> = true;

    template <fstr::fstr P>
    inline constexpr bool is_view_extractor_v<QueryView<P>> = true;

    template <fstr::fstr P>
    inline constexpr bool is_view_extractor_v<HeaderView<P>> = true;

    template <>
    inline constexpr bool is_view_extractor_v<BodyView> = true;

    template <>
    inline constexpr bool is_view_extractor_v<RequestView> = true;

    template <fstr::fstr P>
    inline constexpr bool is_view_extractor_v<Path<P, std::string_view>> = true;

    template <fstr::fstr P>
    inline constexpr bool is_view_extractor_v<Query<P, std::string_view>> = true;

    template <fstr::fstr P>
    inline constexpr bool is_view_extractor_v<Header<P, std::string_view>> = true;
}
