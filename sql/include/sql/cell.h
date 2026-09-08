#pragma once

#include <string_view>

#include "sql/dialect.h"

namespace sql {
    // One decoded value: bytes with a length, not a C string -- a blob
    // containing 0x00 must not truncate at the first NUL. The byte one past
    // the end of bytes is always '\0' (every writer appends the sentinel),
    // so get()-shaped consumers may still treat bytes.data() as a C string;
    // the length is what blobs actually need. Conversion needs the dialect:
    // the same C++ type decodes differently per driver (bool is 0/1 vs t/f).
    // is_null rides along because result_traits<std::optional<T>> must be
    // answerable from a cell alone.
    struct cell final {
        std::string_view bytes;
        sql::dialect from;
        bool is_null;
    };
}
