#pragma once

// One identity per query literal. Each distinct fstr NTTP instantiates
// this template once, and the address of its tag is a constant expression
// unique to that instantiation -- a key for per-connection statement
// caches that costs nothing to compute and needs no hashing of the text.
// c_str exists because libpq wants a NUL-terminated command; fstr stores
// the NUL, so the literal's own buffer serves.

#include <string_view>

#include <fstr/fstr.h>

#include "sql/detail/placeholders.h"

namespace sql::detail {
    template <fstr::fstr Q>
    struct stmt_key final {
        static constexpr char tag = 0;
        static constexpr const void* id = &tag;
        static constexpr std::string_view text = Q.view();
        static constexpr const char* c_str = Q.data;
        static constexpr placeholder_scan scan = scan_placeholders(Q.view());
    };
}
