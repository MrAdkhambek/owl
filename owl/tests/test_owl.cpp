#include <owl/server.h>

namespace {
    struct App final {
        int n = 0;
    };
}

static_assert(sizeof(owl::Server<App>) >= 1);
