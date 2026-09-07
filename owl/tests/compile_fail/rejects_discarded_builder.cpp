#pragma clang diagnostic error "-Wunused-result"

#include <owl/server.h>

namespace {
    struct App final {
        int n = 0;
    };
}

int main() {
    owl::Server<App>::builder().config({});
}
