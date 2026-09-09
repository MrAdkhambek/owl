// A small REST API on owl + sql + redis, laid out the way an axum project
// is: state in app.h, the schema in db, auth in auth, one router per
// module under routes/, and this file composing them. Sessions and the
// login throttle live in redis; users and posts in sqlite.
//
// Configuration is the environment, so the same binary runs from a
// checkout and from the compose file: REST_ADDRESS (127.0.0.1), REST_PORT
// (8080), REST_DB (rest.db), REST_REDIS (127.0.0.1:6379).
//
//   POST /auth/register   {"username", "password"}   -> 201 {"id", "username"}
//   POST /auth/login      {"username", "password"}   -> 200 {"token"}
//   GET  /posts                                      -> 200 [{"id", "author", "title", "body", "created_at"}, ...]
//   POST /posts           Authorization: Bearer <token>, {"title", "body"} -> 201 {"id"}
//   GET  /posts/{id}                                 -> 200 {...} or 404
//
// See README.md for curl lines and the Postman collection.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include <owl/owl.h>
#include <redis/redis.h>

#include "rest/app.h"
#include "rest/db.h"
#include "rest/routes/auth.h"
#include "rest/routes/posts.h"

namespace {
    std::string env_or(const char* const name, const std::string_view fallback) {
        const char* const value = std::getenv(name);
        return value != nullptr ? value : std::string{fallback};
    }

    // host:port, or just host for 6379.
    redis::config redis_at(const std::string_view spec) {
        redis::config cfg;
        const auto colon = spec.rfind(':');
        cfg.host = std::string{spec.substr(0, colon)};
        if (colon != std::string_view::npos) cfg.port = static_cast<std::uint16_t>(std::stoi(std::string{spec.substr(colon + 1)}));
        return cfg;
    }
}

int main() {
    const std::string db_path = env_or("REST_DB", "rest.db");
    const std::string address = env_or("REST_ADDRESS", "127.0.0.1");
    const auto port = static_cast<std::uint16_t>(std::stoi(env_or("REST_PORT", "8080")));
    const redis::config cache = redis_at(env_or("REST_REDIS", "127.0.0.1:6379"));
    rest::migrate(db_path);

    auto router = owl::Router<rest::App>::make()
                  .nest<"/auth">(rest::auth::router())
                  .nest<"/posts">(rest::posts::router());

    const owl::Server<rest::App> server = owl::Server<rest::App>::builder()
                                          .router(std::move(router))
                                          .config({.address = address, .port = port})
                                          .thread(4)
                                          .with_sqlite({.path = db_path})
                                          .with_redis(cache)
                                          .build_with(std::make_shared<rest::App>());

    std::printf("listening on http://%s:%u  (sqlite: %s, redis: %s:%u)\n"
                "  POST /auth/register\n  POST /auth/login\n  GET  /posts\n  POST /posts  (Authorization: Bearer <token>)\n  GET  /posts/{id}\n",
                address.c_str(), server.port(), db_path.c_str(), cache.host.c_str(), cache.port);
    std::fflush(stdout);
    server.start();
}
