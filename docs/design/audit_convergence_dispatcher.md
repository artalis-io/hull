# Audit convergence phase 2: the binding dispatcher and the request deadline

Amendment to *Design: Audit convergence - invariant ratchets and structural
bounds* (`docs/design/audit_convergence.md`, branch
`design/audit-convergence-phase1`), recording how §6.1 and §6.2 were built and
answering open questions 1 and 2 (§12).

## 1. §6.1 Measured-time charging in the binding dispatcher

### 1.1 What is charged

Around every call of an app-facing binding the dispatcher reads a monotonic
clock and charges the run

```
max(0, (wall_time - excluded_time) x HL_WORK_UNITS_PER_US - charged_during_the_call)
```

with `HL_WORK_UNITS_PER_US = 64` (`include/hull/shared/work_charge.h`, the rate
the explicit floors were sized from). `charged_during_the_call` is read from a
monotonic per-VM meter that every charge feeds: the binding's own explicit
charges, SQL progress, Lua patch 0004 / QuickJS patch 0005 work charges, the
instructions of script the binding called back into, and nested dispatched
calls. Reading it before and after the call (plus the work the engine has done
but not yet reported: `lua_hlpending`, `JS_GetPendingCharge`) means nothing is
counted twice and a nested call is charged once, to itself.

The design said `max(declared_floor, elapsed)`. Floors are charged BEFORE the
work by each binding (so a call over budget is refused before it runs), and the
dispatcher charges only the EXCESS of measured time after it. The net cost of a
call is therefore `max(floor, measured)`, as specified, without the dispatcher
knowing each binding's floor.

As built, an explicit charge counts DOUBLE against measured time
(`HL_METER_PAID_WEIGHT`): `owed = max(0, measured - 2 x charged)`, so a call
costs `max(charged, measured / 2)` at least. The floors were sized from the
cost measured on an idle machine, and the same work takes up to about twice
as long on a slower or loaded one (core siblings busy, a low-power core): at
1x, 500 ordinary fs writes on a loaded Mac crossed the headroom test's 15M
budget from CPU time alone. The dispatcher is there to catch floors that are
missing or far too small - the audits found 0 units charged for work worth
thousands - which a factor of 2 does not hide.

#### Wall time, CPU time and the off-CPU allowance

Wall time also counts time the thread did not run: preempted by another
process on a loaded host (a CI runner: a 4 ms time slice is 256k units, a
quarter of a 1M test budget), or blocked in a wait nobody bracketed. Charging
that in full made budget tests flaky on slow machines, and a CPU clock on
every call is too dear (`clock_gettime(CLOCK_THREAD_CPUTIME_ID)` is a syscall,
~170 ns on Apple M-series, against the dispatcher's ~25 ns). So
(`hl_meter_settle`, `include/hull/shared/dispatch_meter.h`):

- A VM measures in **wall mode** by default: two monotonic reads per call,
  charged as above.
- A call that owes more than `HL_METER_SLOW_UNITS` (256 us, 16384 units)
  past what it charged itself is *slow*. It has no CPU reading of its own, so
  it is charged at most the thread's CPU time since the run was armed plus
  the off-CPU allowance below: every arm (`hl_lua_budget_arm`,
  `hl_js_budget_arm_kind`, a worker.dispatch job) reads the thread's CPU
  clock once (`hl_meter_arm`, only when no binding call is in progress, so
  the reading always precedes the call), and that bounds the call's own CPU
  work from above. A slow call also switches the VM to **CPU mode**.
- In CPU mode every call also reads the thread's CPU clock and is measured as
  `min(wall - excluded, cpu + HL_METER_OFFCPU_MAX_UNITS)`: all the CPU work it
  did, plus at most `HL_METER_OFFCPU_MAX_UNITS` (32 us, 2048 units) of time
  off the CPU (preemption, or an unbracketed blocking wait). The allowance is
  small because on a loaded host many calls of one run are preempted: at
  256 us each, 500 preempted fs writes cost 8M units more. Every slow call
  keeps the VM in CPU mode; 100 ms of wall time (`HL_METER_CPU_WINDOW_UNITS`)
  after the last one it goes back to wall mode, so only a VM that is being
  preempted pays for the CPU clock. A host with no per-thread CPU clock
  measures wall time.

The guarantee is therefore: a call is never charged less than half the CPU
work it did (all of it when it charged nothing itself), nor more than its
wall time. Time off the CPU is charged only as far as the run's CPU time so
far covers it (a slow call in wall mode) or 32 us a call (CPU mode), so an
unbracketed blocking wait is bounded by the run's deadline (§6.2) rather
than by the budget - as a bracketed one always was.

Under load this holds the headroom tests: the budget tests ran 20-25 times
each with 5, 10 and 16 busy processes on a 10-core Mac with no failure,
and with a simulated 3 ms fsync the fs headroom tests stay inside 15M.

#### Device flushes

`fsync` (the fs.write data and directory syncs, blob writes) is an excluded
wait (`hl_meter_fsync`): waiting for the disk is not the run's work, and on a
CI disk it is milliseconds that vary run to run. The fs / blob floors
(`HL_WORK_FS_WRITE_UNITS` and the blob ones) were sized from the measured cost
of the whole write, flush included, so a loop of writes still pays per write.
Without this, 500 ordinary page writes in `ordinary_fs_work_keeps_its_headroom`
cost ~2 ms of fsync each on a Linux runner, 64M units, over a 15M budget.

#### A trip while a binding raises

A call that raises its own error (a bad argument) is still charged, by the
throw hook, as it unwinds. If that charge trips the run, `pcall` / `xpcall`
re-raise the TRIP (`hl_lua_budget_raise`), not the binding's error that was
unwinding: before, `for ... do pcall(h.digest) end` ended with "bad argument"
instead of the limit. A binding's floor is charged before its argument checks
where it can be (`sha256` hasher `update` / `digest`), so a refused call pays
it.

Before the call the dispatcher refuses it, with the uncatchable trip, when the
run's budget is already spent or its deadline has passed. After the call, if
the excess trips the budget, it raises the trip: the call's effects stand (a
row inserted stays inserted), the run ends.

### 1.2 Lua

Modules register through `hl_lua_setfuncs` / `hl_lua_newlib` /
`hl_lua_pushcclosure` / `hl_lua_pushcfunction` (`runtime/lua/budget.c`). Each
function becomes a C closure over its real function and the VM's
`HlLuaBudget`, after the binding's own upvalues (so `lua_upvalueindex(i)` still
names them; up to 2 are supported, which covers every Hull module). The
trampoline calls the real function directly, in the same `CallInfo`:

- `lua_getstack` levels, the caller's name (`namewhat`) and the stdlib frame
  checks see exactly what they saw before;
- a `lua_yieldk` inside the function suspends this frame; on resume Lua calls
  the continuation, not the trampoline. Yieldability is preserved.

A call that does not return - an error or a yield - leaves through a longjmp
the trampoline never sees, so **Lua HULL PATCH 0007** adds a throw hook called
at the top of `luaD_throw`. Each call in progress keeps a frame (thread,
innermost error handler, clock, excluded time, meter). On a yield the hook
settles every frame of the yielding thread (they are its whole C stack, and
the parked time that follows is not the run's); on an error, the frames of the
throwing thread that started inside the handler the error is thrown to. The C
stack is LIFO across coroutines, so a normal return pops its own frame and any
stale ones above it.

`__gc` and `__close` entries are never wrapped (finalizers run in whatever run
a collection happens in), nor is anything in the tool VM (`meter_off`: its one
long run is `hull build`).

### 1.3 JavaScript

The prompt suggested a `JS_NewCFunctionData` trampoline. It was not used: a
data function pushes no stack frame, so `JS_GetScriptOrModuleName(ctx, 1)` -
which the `_hull_*` namespace check and `log.*` use to identify the caller -
would have read one level too far. Instead **QuickJS HULL PATCH 0011** adds
embedder call hooks to `js_call_c_function` (after the frame is pushed) and
`js_c_function_data_call`, for functions marked as the embedder's: the public
`JS_NewCFunction2` / `JS_NewCFunctionData` set the mark, QuickJS's own call
sites use new unmarked internal constructors. So every function Hull makes is
dispatched with no change at its ~250 call sites, and builtins are not. A Hull
function list goes through `hl_js_set_function_list` (QuickJS instantiates
`JS_SetPropertyFunctionList` entries itself, unmarked). A JS binding never
blocks across a wait: an op that waits returns a promise at once.

### 1.4 Exclusions

Time that is not the run's work is bracketed with `hl_meter_exclude_begin` /
`_end` (a thread-local total the dispatcher subtracts; only the outermost
bracket counts, and nothing inside a bracket may raise):

| Wait | Where | Why excluded |
|------|-------|--------------|
| Database socket polls (connect pump, sliced send / recv, wait readable) | `cap/db_transport.c` | the server's time; bounded by `read_timeout`, asks the run each slice |
| Postgres LISTEN wait | `cap/pg_conn.c` | the server's time |
| Valkey command wait | `cap/valkey_conn.c` | the server's time |
| TLS handshake poll | `shared/tls_client.c` | the peer's time |
| Synchronous HTTP request | `cap/http.c` | DNS, connect, transfer: the server's time; floor + body charged explicitly, bounded by the timeout |
| SMTP pump | `cap/smtp_transport.c` | the server's time |
| Terminal input poll, OSC 11 query | `cap/tui.c` | the user's / terminal's time (`tui.pollSync`) |
| app.main `ctx.stdin` reads | `runtime/{lua,js}/runtime.c` | the user's time |
| WASM guest execution | `cap/wasm.c` | metered by `gas` and bounded by `timeout_ms` |
| GPU device waits | `cap/gpu_wgpu.c` | the device's time; bounded by the clamped timeout |
| `fsync` of written files and their directory | `cap/fs.c`, `shared/blob_store.c` (`hl_meter_fsync`) | the device's time; paid by the write floors |

Waits that PARK (`hull.sleep`, `http.fetch`, `db.async`, Lua `tui.poll`,
multipart reads) are not inside the call at all and need no bracket.

### 1.5 Sanitizers and measured overhead

Instrumented builds divide measured time by `HL_METER_SLOWDOWN` so tests that
fit the budget in a release build fit it under a sanitizer or coverage: the
instructions a script runs are counted, not timed, so only the measured half of
a run slows down. The factor is per instrumentation, with at least 3x of margin
over the worst slowdown measured on the PBKDF2-heavy TOTP tests: MSan + UBSan
64 (19x on an x86_64 GitHub runner, where mbedTLS is built without asm and
every SHA-256 round is shadowed; 2.4x on aarch64), TSan 16, ASan + UBSan 8
(2.3x), coverage 8 (the Makefile passes `-DHL_METER_SLOWDOWN=8u`; the compiler
names no feature for `--coverage`). A release build is 1, so production
charging is unchanged. With a single factor of 8, MSan tripped the default
budget on `hash_password` loops that use a fraction of it in a release build
(PR #981 CI), and coverage tripped `lua_audit18.encoding_codecs_are_charged`.

Measured on Apple M-series (release build, `lua_dispatcher.overhead_per_call_is_small`
/ `js_dispatcher.overhead_per_call_is_small`, 2M calls of a no-op binding):

- Lua: ~22 ns per call (21.7 ns plain, 43.3 ns dispatched);
- QuickJS: within noise of the ~63 ns a C call costs from a loop.

On Linux each clock read is a vDSO `clock_gettime` (~15-20 ns), so expect up to
~40 ns. The tests assert a generous 300 ns.

## 2. §6.2 Per-request wall budget

The run watchdog (`cap/run_watchdog.c`) now has four kinds:

| Kind | Default | Manifest |
|------|---------|----------|
| `HL_RUN_HTTP` - request handler, middleware, SSE endpoint handler, and each resume of one | 10 s (`HL_RUN_HTTP_DEFAULT_MS`) | `limits.request_ms` / `requestMs` |
| `HL_RUN_CALLBACK` - timer, WebSocket server / client callback, SSE callback, and each resume | 5 s (`HL_RUN_CALLBACK_DEFAULT_MS`) | `limits.callback_ms` / `callbackMs` |
| `HL_RUN_MAIN` - app.main and its resumes | 10 min | - |
| `HL_RUN_ENTRY` - tasks, test cases, `worker.dispatch` jobs, module load | 60 s | - |

A continuation records the kind of the run that parked (`HlLuaAsyncCont` /
`HlJsAsyncCont.run_kind`), so a resume gets the same deadline. `--max-run-ms` /
`HULL_MAX_RUN_MS` override every kind. A request whose run is stopped by its
deadline is answered `503 Service Unavailable: the request exceeded its time
limit` (an instruction-limit trip stays a 500).

The deadline bounds each uninterrupted run - the time a run holds the event
loop - not the request's total time, which the op timeouts bound. Per-route
deadlines need router options Hull does not have; follow-up.

## 3. Open questions answered

**Q1 - charge measured time in worker.dispatch and app.main too?** Yes,
everywhere: the main app VMs, `worker.dispatch` VMs (Lua and JS) and app.main.
A worker job holds a pool thread that `db.async` / `compute.async` / SMTP share,
and app.main already has a 10 min deadline, so charging its binding time only
makes the instruction budget mean the same thing in every run. The tool VM is
the one exception.

**Q2 - 5 s or 10 s for the HTTP default?** 10 s. 5 s would stop legitimate
handlers on a loaded host (a few synchronous Postgres queries plus a password
hash, a slow disk). One run holding the loop for 10 s is already a severe
outage for that process, and the instruction budget (now including binding
time) stops a CPU-bound run in ~1.5 s anyway; the deadline is the backstop for
whatever the budget still cannot see.

## 4. INV-1 follow-up (done in this PR)

PR #977's invariant lint INV-1 required an explicit charge per binding plus an
allowlist. With the dispatcher it now requires that every app-facing binding
is registered through it: no `luaL_setfuncs` / `luaL_newlib` /
`luaL_register` in `src/hull/runtime/lua/` (outside `budget.c` and
`mod_tool.c`), every Lua binding a row of a table passed to
`hl_lua_setfuncs` / `hl_lua_newlib` (or pushed row by row with
`hl_lua_pushcclosure`) or pushed with `hl_lua_pushcfunction`, and no
`JS_SetPropertyFunctionList` / `JS_SetModuleExportList` in
`src/hull/runtime/js/` outside `hl_js_set_function_list`. An explicit charge
alone no longer passes. The allowlist shrank from 208 entries to 32: the
functions still pushed raw (finalizers and `__close`, protected-call bodies
and continuations, the coroutine library, cheap metamethods, module openers).
