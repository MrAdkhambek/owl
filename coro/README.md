<div align="center">

# coro

**Header-only C++23 coroutine toolkit**

Tasks, generators, schedulers, an I/O reactor, and async primitives.
Nothing runs until you await it.

[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/target-owl::coro-064F8C?logo=cmake&logoColor=white)](#)
[![header-only](https://img.shields.io/badge/header--only-yes-success)](#)

[Tasks](#tasks) · [Generators](#async-generators) · [Combinators](#driving-and-combining) · [Schedulers](#schedulers) · [Reactor](#io-reactor)

</div>

> [!WARNING]
> Part of [owl](../README.md). Experimental. Not production-ready.

```cpp
#include <coro/coro.h>

coro::task<int> answer() { co_return 42; }

int main() {
    int n = coro::sync_wait(answer());   // 42
}
```

Link `owl::coro` (pulls `Threads`). `coro.h` is the umbrella and includes every public header.

## Layout

| Directory | Holds |
|---|---|
| *(root)* | `task.h`, `async_generator.h` — the types you make |
| `concepts/` | what a type must do: `awaitable`, `scheduler`, `delayed_scheduler`, `io_reactor` |
| `algo/` | combining awaitables: `when_all`, `when_any`, `wait_all`, `fmap`, `combine` |
| `result/` | failure as data: `result`, `as_result`, `as_tuple` |
| `run/` | into and out of coroutine-land: `sync_wait`, `schedule_on`, `resume_on` |
| `executors/` | things that actually run work: `static_thread_pool`, `timer_scheduler` |
| `io/` | `native_reactor`, `reactor_ref` |
| `sync/` | `async_mutex` |

## Tasks

`coro::task<T>` / `coro::task<>` (void). Lazy: the body does not start until the task is awaited or driven.

```cpp
coro::task<int> add(int a, int b) {
    co_return a + b;
}

coro::task<int> twice(int x) {
    int n = co_await add(x, x);
    co_return n;
}

int main() {
    coro::task<int> t = twice(21);
    int n = coro::sync_wait(std::move(t));   // 42
}
```

Drive without `sync_wait` when nothing suspends:

```cpp
auto t = add(2, 3);
int n = std::move(t).get();   // resumes on this thread
```

Awaiting consumes the task -- `co_await std::move(t)` -- so a task is awaited exactly once:

```cpp
coro::task<> once() {
    coro::task<int> t = add(2, 3);
    int a = co_await std::move(t);   // consumes t
    // co_await std::move(t);        // throws: already awaited
}
```

- `coro::is_task<T>` / `coro::is_task_v<T>`
- `coro::task_value_t<Task>` — `int` for `task<int>`

## Async generators

`coro::async_generator<T>`. Pull with `next()` → `task<optional<T>>`. End of stream is `nullopt`. Pulls are lockstep -- the producer never runs ahead of the consumer -- and a body exception is rethrown at the pull site. Destroying a half-consumed generator destroys the frame, so the producer's remaining body simply never runs:

```cpp
coro::async_generator<int> counted(int n) {
    for (int i = 0; i < n; ++i) co_yield i;
}

coro::task<> print_three() {
    auto gen = counted(3);
    while (auto v = co_await gen.next()) {
        use(*v);   // 0, 1, 2
    }
}

coro::sync_wait(print_three());
```

Stop early and the frame is torn down at the scope exit:

```cpp
coro::task<> take_two(coro::async_generator<int> g) {
    co_await g.next();
    co_await g.next();
}   // g dies here; the producer never yields a third time
```

## Driving and combining

### `sync_wait`

Runs a task to completion on this thread: the body starts eagerly on the calling thread, which then blocks until the task finishes. `nodiscard`.

```cpp
int n = coro::sync_wait(add(20, 22));
```

### `wait_all` — tasks, fail-late

Starts every child, waits for the last, then rethrows the first failure in **argument order**. Void children occupy a `void_value` slot.

```cpp
#include <coro/algo/wait_all.h>

coro::task<> page(int id) {
    auto [user, posts] = co_await coro::wait_all(fetch_user(id), fetch_posts(id));
    use(user, posts);
}
```

Two 40 ms and 60 ms sleeps cost ~60 ms together, not ~100 ms.

### `when_all` / `when_any` — any awaitable

Borrow their children; await them where they were created (they delete move). `when_any` tears down the losers. An empty `when_any` range throws rather than waiting forever.

```cpp
#include <coro/algo/when_all.h>
#include <coro/algo/when_any.h>

auto [user, posts] = co_await coro::when_all(fetch_user(id), fetch_posts(id));

coro::timer_scheduler timer;
auto winner = co_await coro::when_any(fetch(url), timer.schedule_after(500ms));
if (winner.index() == 1) { /* timed out */ }
```

### `fmap`

```cpp
#include <coro/algo/fmap.h>

auto doubled = co_await (add(21, 21) | coro::fmap([](int n) { return n * 2; }));
// 84

auto named = co_await (fetch_user(id) | coro::fmap([](int id) -> coro::task<std::string> {
    co_return co_await load_name(id);   // flatten
}));
```

### `combine`

Lockstep zip of two generators. Ends when either side dries up.

```cpp
#include <coro/algo/combine.h>

auto zipped = std::move(ids) | coro::combine(std::move(names));
while (auto pair = co_await zipped.next()) {
    auto [id, name] = *pair;   // (0, "a"), (1, "b"), (2, "c")
}
```

## Errors as data

```cpp
#include <coro/result/as_result.h>
#include <coro/result/as_tuple.h>

using coro::result;   // std::expected<T, std::exception_ptr>

auto r = co_await (fetch_user(id) | coro::as_result());
if (!r) std::rethrow_exception(r.error());
int id = *r;

auto [err, body] = co_await coro::as_tuple(fetch(url));
if (err) { /* failed */ }

auto [ok, bad] = co_await coro::when_all(
    coro::as_result(fetch_user(1)),
    coro::as_result(fetch_user(-1)));
```

`as_tuple` / the awaitable `as_result` allocate no extra frame. `as_tuple` must be able to default-construct `T` for the failure case's value slot -- a type that cannot is `as_result`'s job. `T` on `result<T>` must not be an lvalue reference.

## Schedulers

| Concept | Requirement |
|---|---|
| `coro::scheduler` | `schedule()` is awaitable |
| `coro::delayed_scheduler` | `schedule_after(duration)` |
| `coro::postable` | `post(coroutine_handle<>)` |

```cpp
#include <coro/executors/static_thread_pool.h>
#include <coro/executors/timer_scheduler.h>

coro::static_thread_pool pool{4};
coro::timer_scheduler timer;

coro::task<int> work() {
    co_return co_await coro::schedule_on(pool, []() -> coro::task<int> {
        co_return expensive();          // runs on the pool
    }());
}

coro::task<> nap() {
    co_await timer.schedule_after(50ms);
}

int n = coro::sync_wait(coro::resume_on(pool, add(2, 3)));
```

`schedule_on` starts the task on the scheduler -- it moves the work. `resume_on` moves the continuation instead: the task runs where it would have anyway, and the code after the `co_await` resumes on the scheduler. Both have pipe forms:

```cpp
int a = co_await (fetch(id) | coro::schedule_on(pool));   // body on the pool
int b = co_await (fetch(id) | coro::resume_on(pool));     // result ready on the pool
```

The timer thread does the waking: `schedule_after` resumes there, `schedule()` is a plain hop to that thread, and a non-positive `schedule_after` never suspends at all.

```cpp
coro::task<> hops(coro::timer_scheduler& timer) {
    co_await timer.schedule();            // now running on the timer thread
    co_await timer.schedule_after(0ms);   // no-op: runs on where it is
    co_await timer.schedule_after(50ms);  // resumes on the timer thread
}
```

| Executor | Models |
|---|---|
| `static_thread_pool` | `scheduler`, `postable` |
| `timer_scheduler` | `delayed_scheduler` (owns a private timer queue) |

The pool posts round-robin, one queue per worker, with no work stealing. Destroy a pool only when nothing outstanding refers to it.

## I/O reactor

`coro::native_reactor` — kqueue (macOS/BSD) or epoll (Linux). Background thread.

```cpp
coro::native_reactor reactor;

coro::task<> read_ready(int fd) {
    auto st = co_await reactor.wait(fd, coro::interest::read, 5s);
    if (st == coro::wait_status::timeout) co_return;
    // fd is readable
}

co_await reactor.sleep(20ms);   // timeout < 0 means forever
```

- `interest`: `read`, `write`, `read_write`
- `wait_status`: `ready`, `timeout`, `cancelled`, `error`
- Concept `coro::io_reactor` — `wait` and `sleep` return `task<wait_status>`
- Completions resume on the reactor thread
- Tear down only when nothing is parked: shutdown force-resumes stragglers with `wait_status::cancelled`, and a coroutine resumed that way must not touch the reactor again

### `reactor_ref`

`coro::reactor_ref` is a non-owning, type-erased handle to any `io_reactor`: a driver that must not be a template on the reactor type (sql's postgres connection, redis's connection) takes one and calls `wait(fd, interest, timeout)` and `release(fd)` through it. A default-constructed handle is null: `wait` answers `wait_status::error` without parking and `release` is a no-op. The reactor must outlive every handle.

```cpp
coro::native_reactor reactor;
coro::reactor_ref io{reactor};
const auto st = co_await io.wait(fd, coro::interest::read, 500ms);
```

## Async mutex

Acquiring suspends the coroutine. Not reentrant. No timed acquire.

```cpp
coro::async_mutex m;

coro::task<> critical() {
    auto guard = co_await m.scoped_lock();
    // ...
}   // releases here

coro::task<> manual() {
    co_await m.lock();
    // ...
    m.unlock();
}

if (m.try_lock()) {
    // ...
    m.unlock();
}
```

Waiters acquire FIFO. Uncontended acquire never suspends. `unlock()` resumes the next waiter **inline**. Neither copyable nor movable.
