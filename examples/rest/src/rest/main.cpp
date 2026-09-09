// A small REST API on owl + sql, laid out the way an axum project is:
// state in app.h, the schema in db, auth in auth, one router per module
// under routes/, and this file composing them.
//
//   POST /auth/register   {"username", "password"}   -> 201 {"id", "username"}
//   POST /auth/login      {"username", "password"}   -> 200 {"token"}
//   GET  /posts                                      -> 200 [{"id", "author", "title", "body", "created_at"}, ...]
//   POST /posts           Authorization: Bearer <token>, {"title", "body"} -> 201 {"id"}
//   GET  /posts/{id}                                 -> 200 {...} or 404
//
// See README.md for curl lines and the Postman collection.

#include <cstdio>
#include <memory>
#include <string>

#include <owl/owl.h>

#include "rest/app.h"
#include "rest/db.h"
#include "rest/routes/auth.h"
#include "rest/routes/posts.h"

int main() {
    const std::string db_path = "rest.db";
    rest::migrate(db_path);

    auto router = owl::Router<rest::App>::make()
                  .nest<"/auth">(rest::auth::router())
                  .nest<"/posts">(rest::posts::router());

    const owl::Server<rest::App> server = owl::Server<rest::App>::builder()
                                          .router(std::move(router))
                                          .config({.port = 8080})
                                          .thread(4)
                                          .with_sqlite({.path = db_path})
                                          .build_with(std::make_shared<rest::App>());

    std::printf("listening on http://127.0.0.1:%u  (sqlite: %s)\n"
                "  POST /auth/register\n  POST /auth/login\n  GET  /posts\n  POST /posts  (Authorization: Bearer <token>)\n  GET  /posts/{id}\n",
                server.port(), db_path.c_str());
    std::fflush(stdout);
    server.start();
}
