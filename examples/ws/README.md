# ws

The three `.ws` spellings: a coroutine echo, a room that owns its path
parameter, and a shared chat controller that also serves HTML on GET `/chat`.

```sh
cmake -S examples/ws -B examples/ws/build
cmake --build examples/ws/build
./examples/ws/build/ws
```

| | |
|---|---|
| `GET /chat` | the page; open it in two tabs |
| `WS /chat/{room}` | shared controller; `Path<"room", std::string>` extracted before the upgrade |
| `WS /rooms/{id}` | coroutine; owning `Path<"id", int>` |
| `WS /echo/{user}` | coroutine; owning `Path<"user", std::string>` |

```sh
websocat ws://127.0.0.1:8080/echo/alice
websocat ws://127.0.0.1:8080/rooms/1
```

Bind knobs are `owl::Config::make` (CLI > `OWL_*` env > defaults): `--address` /
`--port` / `--threads` / `--backlog`. See [`owl/README.md`](../../owl/README.md#server).
