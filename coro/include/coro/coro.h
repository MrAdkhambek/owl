#pragma once

// The library's advertised entry point: every public header, nothing else.
// detail/ headers are deliberately absent -- they arrive transitively
// through the public headers that need them.
//
// This list is checked against the directory tree and the CMake FILE_SET at
// configure time. Do not hand-prune it; if a header should not be here, it
// should not be public.

#include "coro/async_generator.h"
#include "coro/task.h"

#include "coro/concepts/awaitable.h"
#include "coro/concepts/delayed_scheduler.h"
#include "coro/concepts/io_reactor.h"
#include "coro/concepts/scheduler.h"

#include "coro/algo/combine.h"
#include "coro/algo/fmap.h"
#include "coro/algo/wait_all.h"
#include "coro/algo/when_all.h"
#include "coro/algo/when_any.h"

#include "coro/result/as_result.h"
#include "coro/result/as_tuple.h"
#include "coro/result/result.h"

#include "coro/run/resume_on.h"
#include "coro/run/schedule_on.h"
#include "coro/run/sync_wait.h"

#include "coro/executors/static_thread_pool.h"
#include "coro/executors/timer_scheduler.h"

#include "coro/io/reactor.h"

#include "coro/sync/async_mutex.h"
