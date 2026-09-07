#pragma once

// Questions about handler and layer parameter types, answered from the
// extractor vocabulary. mount() asserts on these where the pattern is known,
// so a handler asking for the wrong State<T> or an undeclared path parameter
// is a compile error at the route, not a 500 at extract time.

#include <string_view>

#include <fstr/fstr.h>

#include "owl/core/state.h"
#include "owl/extract/extractors.h"

namespace owl::detail {
    // A handler naming State<T> only makes sense on a Router<T>.
    // route() proves the match at compile time.
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
}
