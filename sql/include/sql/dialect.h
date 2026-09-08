#pragma once

#include <cstdint>

namespace sql {
    // Which driver produced a value. Conversion is dialect-sensitive (bool
    // is 0/1 on sqlite, t/f on postgres), so it travels on cells alongside
    // the bytes. It is the bottom of the library: result_traits and
    // placeholders need it and neither may include a driver header.
    enum class dialect : std::uint8_t { sqlite, postgres };
}
