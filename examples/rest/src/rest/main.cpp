// A small REST API on owl + sql + redis, laid out the way an axum project
// is: state in app.h, the schema in db, auth in auth, one router per
// module under routes/, and this file composing them. Sessions and the
// login throttle live in redis; users and posts in postgres.
//
// Bind knobs and drivers come from Config::make (CLI > OWL_* env >
// defaults). Rest overlays pool size and a query deadline so a handler
// can never park on the database for the life of the process.
//
//   POST /auth/register   {"username", "password"}   -> 201 {"id", "username"}
//   POST /auth/login      {"username", "password"}   -> 200 {"token"}
//   GET  /posts                                      -> 200 [{"id", "author", "title", "body", "created_at"}, ...]
//   POST /posts           Authorization: Bearer <token>, {"title", "body"} -> 201 {"id"}
//   GET  /posts/{id}                                 -> 200 {...} or 404
//
// See README.md for curl lines and the Postman collection.

#include <chrono>
#include <cstdio>
#include <memory>

#include <owl/owl.h>

#include "rest/app.h"
#include "rest/db.h"
#include "rest/routes/auth.h"
#include "rest/routes/posts.h"

int main(int argc, char** argv) {
    auto cfg = owl::Config::make(argc, argv);

    if (!cfg.psql) cfg.psql.emplace();
    if (cfg.psql->dsn.empty()) cfg.psql->dsn = "postgres://localhost/rest";
    cfg.psql->connections = 4;
    cfg.psql->query_timeout = std::chrono::seconds{5};
    if (!cfg.redis) cfg.redis.emplace();
    rest::migrate(*cfg.psql);

    auto router = owl::Router<rest::App>::make()
                  .nest<"/auth">(rest::auth::router())
                  .nest<"/posts">(rest::posts::router());

    const owl::Server<rest::App> server = owl::Server<rest::App>::builder()
                                          .router(std::move(router))
                                          .config(cfg)
                                          .build_with(std::make_shared<rest::App>());

    std::printf("listening on http://%s:%u  (postgres: %s, redis: %s:%u)\n"
                "  POST /auth/register\n  POST /auth/login\n  GET  /posts\n  POST /posts  (Authorization: Bearer <token>)\n  GET  /posts/{id}\n",
                cfg.address.c_str(), server.port(), cfg.psql->dsn.c_str(), cfg.redis->host.c_str(), cfg.redis->port);
    std::fflush(stdout);
    server.start();
}
