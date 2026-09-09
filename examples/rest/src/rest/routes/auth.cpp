#include "rest/routes/auth.h"

#include <cstdint>
#include <string>

#include <coro/run/resume_on.h>
#include <coro/run/schedule_on.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "rest/auth.h"
#include "rest/error.h"

namespace rest::auth {
    coro::task<owl::Response> register_user(
        const Db& db,
        const owl::State<App> app,
        const owl::loop_scheduler& loop,
        const owl::Json<Credentials> body
    ) {
        const auto& [username, password] = body.value;
        if (username.empty()) co_return fail(422, "username required");
        if (password.size() < 12) co_return fail(422, "password must be at least 12 characters");

        // The hash on the hashing pool, the rest of the handler back on
        // this worker: one piped expression instead of two hops.
        const auto hashed = co_await (hash_password(password) | coro::schedule_on(app->hashing) | coro::resume_on(loop));

        const auto inserted = co_await sql::try_query<"INSERT INTO users(username, password) VALUES ($1, $2) RETURNING id">(
            db, username, hashed);
        if (!inserted) {
            if (inserted.error().code() == SQLITE_CONSTRAINT_UNIQUE) co_return fail(409, "username taken");
            co_return fail(500, inserted.error().what());
        }
        co_return owl::Response::json(nlohmann::json{{"id", (*inserted)[0][0].as<std::int64_t>()}, {"username", username}}, 201);
    }

    coro::task<owl::Response> login(
        const Db& db,
        const owl::State<App> app,
        const owl::loop_scheduler& loop,
        const owl::Json<Credentials> body
    ) {
        const auto& [username, password] = body.value;
        const auto found = co_await sql::query<"SELECT id, password FROM users WHERE username = $1">(db, username);
        if (found.rows() == 0) co_return fail(401, "bad credentials");
        const auto [id, stored] = found[0].as<std::int64_t, std::string>();

        const bool ok = co_await (verify_password(password, stored) | coro::schedule_on(app->hashing) | coro::resume_on(loop));
        if (!ok) co_return fail(401, "bad credentials");

        const auto token = new_token();
        (void)co_await sql::execute<"INSERT INTO sessions(token, user_id) VALUES ($1, $2)">(db, token, id);
        co_return owl::Response::json(nlohmann::json{{"token", token}});
    }

    owl::Router<App> router() {
        return owl::Router<App>::make()
               .route<"/register">(owl::post(register_user))
               .route<"/login">(owl::post(login));
    }
}
