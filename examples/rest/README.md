# rest

A small REST API on owl + sql + redis -- register, log in, read and write
posts -- laid out the way an axum project is. Users and posts in postgres,
sessions and rate limiting in redis.

| File | axum counterpart | Holds |
|---|---|---|
| `src/rest/main.cpp` | `main.rs` | `Config::make`, driver env, migrations, the composed router, the server |
| `src/rest/app.h` | `state.rs` | `App`, the state every handler can take as `owl::State<App>` |
| `src/rest/db.h/.cpp` | `db.rs` | the schema, applied once at startup through a standalone pool |
| `monkey.py` | -- | the monkey test: random traffic, then a restart of each dependency under load |
| `src/rest/auth.h/.cpp` | `auth.rs` + an extractor | PBKDF2 password hashing as tasks, sessions and the login throttle in redis, the `Bearer` extractor |
| `src/rest/models.h` | `models.rs` | request bodies (`Credentials`, `NewPost`) and the post JSON |
| `src/rest/error.h` | `error.rs` | `fail(status, message)` -> `{"error": "..."}` |
| `src/rest/routes/auth.h/.cpp` | `routes/auth.rs` | `POST /register`, `POST /login`, and their `router()` |
| `src/rest/routes/posts.h/.cpp` | `routes/posts.rs` | `GET /`, `POST /`, `GET /{id}`, and their `router()` |

Each module owns a router; `main.cpp` nests them under `/auth` and `/posts`.
Postgres comes from each worker's pool (`const Db&`) and redis from each
worker's client (`const Cache&`). Users and posts live in postgres, where a
duplicate username is caught by SQLSTATE `23505` rather than a driver code; a
session lives in redis under `session:<token>` for a day and expires on its own, and
`login:<username>` counts attempts for a minute so that the eleventh in that
minute answers 429. Password hashing is a task piped onto a small thread pool
and back:
`co_await (hash_password(pw) | coro::schedule_on(app->hashing) | coro::resume_on(loop))`.

## Build and run

This directory is its own CMake project. It pulls owl in with
`add_subdirectory(../..)`, the way an application vendors the library
(after `cmake --install`, `find_package(owl)` gives the same targets), and
needs what owl needs: libh2o-evloop, OpenSSL, zlib, libpq, and hiredis, plus
a running Postgres and Redis (`brew services start postgresql@17 redis`, then
`createdb rest`).

```sh
cmake -S examples/rest -B examples/rest/build
cmake --build examples/rest/build
./examples/rest/build/rest
```

Bind knobs and drivers are `owl::Config::make` (CLI > `OWL_*` env > defaults). The example overlays four postgres connections and a 5s query deadline:

| | Default | |
|---|---|---|
| `OWL_ADDRESS` / `--address` / `-a` | `127.0.0.1` | bind address; `0.0.0.0` in a container |
| `OWL_PORT` / `--port` / `-p` | `8080` | |
| `OWL_THREADS` / `--threads` / `-t` | `1` | compose uses `4` |
| `OWL_BACKLOG` / `--backlog` / `-b` | `1024` | |
| `OWL_PG` / `--pg` | `postgres://localhost/rest` | the postgres DSN; the schema is created if missing |
| `OWL_REDIS` / `--redis` | `127.0.0.1:6379` | `host` or `host:port` of the Redis server |

## Docker

`Dockerfile` builds the example on Ubuntu 24.04 (GCC 14, CMake 4.3 from
Kitware, libh2o-evloop from h2o's master, the rest from apt) into a slim
runtime image; `docker-compose.yml` runs it next to `postgres:17-alpine` and
`redis:7-alpine`, with the database on a named volume. The build context is
the repository root, because the example vendors owl with
`add_subdirectory(../..)`:

```sh
docker compose -f examples/rest/docker-compose.yml up --build
```

The first build compiles h2o and takes a few minutes; later ones reuse the
layer. The API is on `localhost:8080` as below.

## Monkey test

`monkey.py` (standard library only) throws random traffic at every route
from sixteen threads: methods the routes do not define, junk and
percent-encoded paths, bodies that are almost the JSON a handler wants,
absent and oversized headers, and raw bytes straight at the socket. The rule
it holds the server to is that it may refuse anything but may not fail, so
any 5xx or dropped connection is a defect.

```sh
python3 examples/rest/monkey.py http://localhost:8080
python3 examples/rest/monkey.py http://localhost:8080 --compose examples/rest/docker-compose.yml
```

With `--compose` it goes on to restart postgres and then redis while traffic
is still flowing, and holds the server to a weaker but still meaningful
rule: it must converge. Ten register / login / post flows have to succeed in
a row once the dependency is back, without the server being restarted
itself.

Some flows fail on the way there, and the run reports how many. That is the
price of a lazily reconnecting driver, and it is worth understanding rather
than hiding: a pooled connection opened before the restart is a corpse, and
the request that checks it out is the one that finds out. Both drivers then
discard it and open a fresh one, so the failures stop after roughly one per
connection that predated the restart -- here up to four workers times four
postgres connections. A service that must not show those to a client would
retry a statement that failed with `kind() == connection` on a connection
taken from the pool's idle list, which is safe because such a statement
provably never reached the server. The example does not, so that the cost
stays visible.

`--seed` makes a run reproducible.

## Try it

```sh
J='content-type: application/json'
curl -s -XPOST localhost:8080/auth/register -H "$J" -d '{"username":"ann","password":"hunter2hunter2"}'
TOKEN=$(curl -s -XPOST localhost:8080/auth/login -H "$J" -d '{"username":"ann","password":"hunter2hunter2"}' | sed 's/.*"token":"\([^"]*\)".*/\1/')
curl -s -XPOST localhost:8080/posts -H "authorization: Bearer $TOKEN" -H "$J" -d '{"title":"hello","body":"first post"}'
curl -s localhost:8080/posts
curl -s localhost:8080/posts/1
```

| Route | Answers |
|---|---|
| `POST /auth/register` | 201 `{"id", "username"}`; 409 if the name is taken; 422 for a bad field |
| `POST /auth/login` | 200 `{"token"}`; 401 for bad credentials; 429 from the eleventh attempt for a username in a minute |
| `GET /posts` | 200, newest first |
| `POST /posts` | needs `Authorization: Bearer <token>`; 201 `{"id"}`; 401 without it, or with an unknown or expired token |
| `GET /posts/{id}` | 200 or 404 |

A body that is not the JSON asked for gets the framework's own 415 / 400 / 422.

## Postman

Import `postman/owl-rest.postman_collection.json`. Run the requests top to
bottom: **Login** stores the token in the collection variable `token`,
**Create post** stores `postId`, and each request carries a test for its
expected status.
