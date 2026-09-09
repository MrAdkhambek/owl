#pragma once

// The schema, applied once at startup.

#include <string>

namespace rest {
    void migrate(const std::string& path);
}
