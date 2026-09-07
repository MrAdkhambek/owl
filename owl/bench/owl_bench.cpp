#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include <owl/routing/router.h>
#include <owl/server.h>

namespace {
    struct BenchState final {};

    owl::Response plaintext(owl::RequestView) {
        return owl::Response::ok("Hello, World!");
    }

    owl::Response json(owl::RequestView) {
        return owl::Response::json(R"({"message":"Hello, World!"})");
    }
}

int main(int argc, char** argv) {
    const auto port = static_cast<std::uint16_t>(argc > 1 ? std::atoi(argv[1]) : 8080);
    const auto threads = static_cast<unsigned>(argc > 2 ? std::atoi(argv[2]) : 1);
    try {
        auto router = owl::Router<BenchState>::make()
            .route<"/plaintext">(owl::get(plaintext))
            .route<"/json">(owl::get(json));
        owl::Server<BenchState> server = owl::Server<BenchState>::builder()
            .router(std::move(router))
            .config({.address = "127.0.0.1", .port = port, .threads = threads})
            .build_with(std::make_shared<BenchState>());
        std::printf("owl listening on http://127.0.0.1:%u (/plaintext, /json)\n", server.port());
        std::fflush(stdout);
        server.start();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "owl-bench: %s\n", e.what());
        return 1;
    }
}
