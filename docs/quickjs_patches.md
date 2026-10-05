# QuickJS patches

Hull vendors QuickJS directly in `vendor/quickjs/` (edited in tree, unlike
WAMR, which is a submodule patched into `build/wamr-patched/`; see
[wamr_patches.md](wamr_patches.md)). Every local change is marked in the source
with a `HULL PATCH` comment and listed here.

**Vendored version:** Bellard QuickJS **2026-06-04**
(`https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz`, SHA-256
`b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a`; upstream
publishes no checksum, so this is the hash of the tarball as fetched). The
version string lives in one place, `QJS_VERSION` in `mk/vendor/quickjs.mk`
(it feeds `CONFIG_VERSION` and the bytecode / template cache keys).

**On a QuickJS upgrade:** grep the new tree for `HULL PATCH`, confirm each entry
below is still needed against upstream, and re-apply the ones that are. Then
check the interrupt cadence (`JS_INTERRUPT_COUNTER_INIT` in `quickjs.c`,
`INTERRUPT_COUNTER_INIT` in `libregexp.c`) against `HL_JS_INTERRUPT_WEIGHT`
(`runtime/js/internal.h`), and any C code that switches on `JS_VALUE_GET_TAG`.

## Upgrade 2024-01-13 -> 2026-06-04 (audit 6 H5)

The reason for the upgrade: in 2024-01-13 a regular expression's
backtracking ran inside one native `RegExp.prototype.exec` call that never
polled the interrupt handler, so `/^(a+)+$/.test("a".repeat(40) + "!")`
held the event loop for exponential time, under any instruction limit.
QuickJS 2025-04-26 added the poll, and 2026-06-04 keeps it:
`libregexp.c` `lre_poll_timeout` decrements a counter on every backtracking
step and every `INTERRUPT_COUNTER_INIT` (10000) steps calls
`lre_check_timeout(opaque)`, which `quickjs.c` implements by calling the
runtime's interrupt handler; a non-zero answer returns `LRE_RET_TIMEOUT`,
and `js_regexp_exec` / `js_regexp_Symbol_replace` raise it through
`JS_ThrowInterrupted` (uncatchable). Hull's handler charges each of those
polls `HL_JS_INTERRUPT_WEIGHT` (10000), the same as a bytecode poll, so the
weight is unchanged: both pollers still run 10000 steps per poll.
Guard: `test_js.c`, `js_audit6.catastrophic_regexp_trips_the_instruction_limit`.

What else changed for Hull:

- `libbf.c` is gone (the bignum extensions were removed upstream) and
  `dtoa.c` is new; `CONFIG_BIGNUM` is no longer defined.
- QuickJS is compiled `-std=gnu11` (it uses the `asm` keyword) and `-fwrapv`,
  as upstream's own Makefile does; Hull's own code stays `-std=c11`.
- Uncatchability is now a property of the pending exception
  (`JS_SetUncatchableException(ctx, flag)`), not of the error object:
  `hl_js_budget_throw` uses it, and the 0003 export of
  `JS_SetUncatchableError` is no longer needed.
- Concatenated strings may be ropes (`JS_TAG_STRING_ROPE`); `JS_IsString`
  covers them, and the two tag switches in `mod_db.c` / `mod_db_udf.c` now
  take both tags (a concatenated SQL parameter used to bind as NULL).
- Patch 0001 is dropped: upstream now initialises `label_lvalue` in the array
  branch of `js_parse_destructuring_element` (the `enum_depth = 0` branch sets
  `label_lvalue = -1` alongside `opcode` / `scope`).

## Patch 0001 - dropped at 2026-06-04 (fixed upstream)

Initialised `label_lvalue` in the array-destructuring branch of
`js_parse_destructuring_element` (found by MSan, #547). Upstream 2026-06-04
sets it there itself, so the patch is gone; the MSan CI job remains its guard.

## Patch 0002 - a stack frame for bound-function calls

**File:** `vendor/quickjs/quickjs.c`, `js_call_bound_function`
**Found by:** the second capability audit (stdlib-identity spoofing)
**Upstream:** Hull-specific; not a QuickJS bug.

Hull decides whether a database call may touch the reserved `_hull_*` tables
by asking who called it: `JS_GetScriptOrModuleName(ctx, 1)` inside the native
binding names the immediate JS caller, and a `hull:` module is the stdlib.
`js_call_c_function` pushes a frame for the native function itself, so level 1
is its caller.

A bound function had no frame. Calling `f.bind(conn, sql)` ran the native
target with the bound function's CALLER as its level-1 frame. So a stdlib
helper that invokes an app-supplied callback - `retry.run(fn, { retryOn })`
calls `opts.retryOn(value)` - lent its stdlib identity to
`conn.exec.bind(conn)`, and app SQL ran against `_hull_*` tables.

The patch pushes a frame for the bound function in the non-constructor branch,
set up the way `js_call_c_function` sets up its own (`prev_frame`, `js_mode`,
`cur_func`, `arg_buf`, `arg_count`), and pops it on return. The frame's
function is the bound function object, which is not bytecode, so
`JS_GetScriptOrModuleName` returns no name for it and Hull treats the call as
app code. Frame walkers already handle non-bytecode frames (every native call
has one): backtraces print the bound function as a native frame, and the
strict/math-mode checks see the same zero `js_mode` a native frame carries.

The constructor branch (`new boundFn()`) is unchanged: a constructor call
cannot target a database binding.

**Guard:** `tests/hull/runtime/js/test_js.c`,
`js_cap.stdlib_helpers_do_not_lend_their_identity`. Without the patch the
bound-`retryOn` case runs the `_hull_*` statement and the test fails.

## Patch 0003 - an interrupt is polled again at the very next step

**File:** `vendor/quickjs/quickjs.c`, `JS_ThrowInterrupted` (since 2026-06-04;
it was in `__js_poll_interrupts` before)
**Found by:** the fifth runtime audit (instruction-limit bypass, H2)
**Upstream:** Hull-specific; upstream relies on the error being uncatchable.

The instruction limit is an interrupt: the handler returns 1 and QuickJS throws
an "uncatchable" error. That error skips `catch` blocks, but an async function
body and a promise reaction job turn ANY exception into a rejection
(`js_async_function_resume`, `promise_reaction_job`), and the code that called
them carries on. QuickJS reset its poll countdown to 10000 steps at every poll,
so the next poll landed inside the next async call too:

```js
const burn = async () => { for (;;) {} };
for (;;) burn();          // never interrupted: each poll lands in burn()
```

Hull's handler is sticky (once over the limit it returns 1 until the runtime
re-arms the budget at its next entry point). The patch sets the countdown to 1
when the handler interrupts, so the next call or backward jump - in the caller,
outside the async body - polls, finds the handler still tripped, and throws
there as well. Nothing can run more than a straight line of code after a trip,
and a resolve / reject call is itself a call, so a tripped run settles nothing
further: its promise jobs fail at their first step, which is how
`hl_js_run_jobs` discards them.

Since 2026-06-04 the patch sits in `JS_ThrowInterrupted`, which both
interrupt sources reach - the bytecode poll and the regexp backtracking poll
(`LRE_RET_TIMEOUT`) - so a trip inside a regexp is sticky the same way. The
earlier version also exported `JS_SetUncatchableError` for bindings that
re-raise a caught interrupt (a SQL UDF, a `compute.stream` callback); upstream
now provides `JS_SetUncatchableException`, which `hl_js_budget_throw` uses.

**Guard:** `tests/hull/runtime/js/test_js.c`,
`js_audit5.async_bodies_do_not_escape_the_instruction_limit`. Without the patch
the async-burn loop is never interrupted and the test hangs.

## Patch 0004 - build_backtrace holds its own reference to the error

**File:** `vendor/quickjs/quickjs.c`, `build_backtrace` (wrapping the original
body, renamed `build_backtrace1`)
**Found by:** the nightly deep JS-source fuzzer (`fuzz/fuzz_js_source.c`), on
QuickJS 2026-06-04, as an ASan SEGV in `find_own_property` under
`build_backtrace` -> `JS_DefinePropertyValue(..., JS_ATOM_stack, ...)`.
**Upstream:** not reported upstream yet.

Three callers pass `rt->current_exception` without a reference of their own:
`JS_CallInternal`'s exception path, the parser's error path and the module
loader's. `build_backtrace` allocates while it works (each frame's function
name, the stack string), and an allocation that runs out of memory throws "out
of memory", which replaces `rt->current_exception` and frees the error object
still being decorated. The final `JS_DefinePropertyValue` of `stack` then wrote
through freed memory. Hull's JS source analyzer runs in a heap-limited QuickJS
session, so malformed app source that exhausts it reached this path.

The patch makes `build_backtrace` a wrapper that takes a reference to the
error object for the duration of the call (`JS_DupValue` / `JS_FreeValue`).

**Guard:** `fuzz/corpus_js_source/regress_backtrace_oom_uaf`, the fuzzer's
crashing input, replayed by the per-PR JS-source fuzz job.
