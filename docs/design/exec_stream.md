# Streamed, long-running declared commands (`exec.stream`)

- **Status:** design, for review. Nothing implemented.
- **Parent design:** [exec_broker.md](exec_broker.md) (§2 API, §3 manifest, §5 broker, §7 lifetime,
  §12 audit) and [exec_broker_macos_isolation.md](exec_broker_macos_isolation.md) (the router).
- **Scope:** a second way to call a declared command, opted into per command. The sandbox, the
  binary checks, the slot matcher and the process tree do not change.

## 1. Problem

`exec.run` waits for the command to end and hands back its whole output:

- `timeout_ms` is at most 60 s (`HL_EXEC_TIMEOUT_MAX_MS`);
- `stdout_max` is at most 16 MiB, `stderr_max` at most 1 MiB, buffered in the broker until the
  command exits and then sent in one `DONE` frame;
- the app sees nothing until the end.

That rules out every command that is slow or chatty, or both:

- a firmware flash or model download that reports progress;
- `journalctl -f -u hs-api.service`, `nvidia-smi dmon`, `tail -f` (no end at all);
- a backup or `rsync` that runs for minutes;
- a long command with tens of MiB of output that the app would process line by line.

hsctl wants the progress and follow cases, and privd lists "timeout above 60 s" as deferred
(exec_broker.md §15). Raising the two ceilings on `exec.run` alone does not fix it: the output
would still arrive at the end, and a 10-minute call holding up to 17 MiB in the broker is a poor
trade.

## 2. Goals and non-goals

Goals:

1. The app reads a command's stdout and stderr as they are written, in chunks.
2. A streaming command may run for up to 10 minutes; its total output is bounded by a cap the
   command declares, not by what the broker can hold.
3. Memory stays bounded and small whatever the command does: the broker, the router and the
   server each hold at most a fixed WINDOW per stream, not the whole output.
4. An app that stops reading stops the command (back pressure), never makes Hull buffer more.
5. Every existing guarantee still holds: the call is matched by `hl_exec_check_call`, the child
   can create no process, it dies with its broker, a cancel kills it.

Non-goals (follow-ups, §11):

- streamed STDIN (an interactive pipe); stdin stays the one-shot `stdin_max` buffer;
- a pty; commands that need a terminal are out of scope, as today;
- raising the ceilings of `exec.run`.

## 3. Manifest

A command opts in with `stream = true`. Only a streaming command may declare a timeout over 60 s or
an output total over the `exec.run` ceilings:

```lua
exec = {
  ["unit-log"] = {
    path = "/usr/bin/journalctl",
    argv = { "--no-pager", "-o", "cat", "-f", "-u",
             { prefix = "hs-", suffix = ".service", chars = "a-z0-9-", max = 64 } },
    stream = true,
    timeout_ms = 600000,           -- 10 min, the stream ceiling
    stdout_max = 268435456,        -- total over the call's life (256 MiB)
    host = { unix_sockets = true },
  },
}
```

| Field | `exec.run` command | `stream = true` command |
|---|---|---|
| `timeout_ms` | 100..60000 (default 5000) | 100..600000 (`HL_EXEC_STREAM_TIMEOUT_MAX_MS`; default 5000) |
| `stdout_max` | ..16 MiB: what is buffered | ..1 GiB (`HL_EXEC_STREAM_TOTAL_CEIL`): the total the call may write, never buffered whole |
| `stderr_max` | ..1 MiB | ..1 GiB, same meaning |
| `concurrency` | 1..8 | 1..8; the process-wide `HL_EXEC_MAX_INFLIGHT` (8) still counts both kinds |

Validation lives in `cap/exec.c` next to the existing checks; both extractors read the key (JS
`stream: true`). The flag is a command flag bit in the encoded policy (`HL_EXEC_CMD_STREAM`), so
it is signed with the manifest and seen by the broker and by every command broker.

A streaming command still answers `exec.run`, with the `exec.run` rules: its output is buffered
up to the `exec.run` ceilings (16 MiB / 1 MiB, or its own caps if lower) and its timeout clamped
to 60 s. That keeps a "just run it" call to a follow-style command from holding a slot for ten
minutes.

## 4. App API

Lua:

```lua
local exec = require("hull.exec")
local s = exec.stream("unit-log", { "hs-api.service" }, { stdin = nil })
while true do
  local chunk = s:read()             -- parks; nil once both pipes are closed
  if not chunk then break end
  -- chunk.fd is "stdout" or "stderr", chunk.data a string (at most 64 KiB)
  if chunk.data:find("Started") then s:close() break end
end
local r = s:result()                 -- parks until the command has ended
-- r = { status, signal, timed_out, stdout_truncated, stderr_truncated,
--       stdout_bytes, stderr_bytes, duration_ms }
```

JS:

```js
import { exec } from "hull:exec";
const s = exec.stream("unit-log", ["hs-api.service"]);
for await (const chunk of s) {          // { fd: "stdout" | "stderr", data: Uint8Array }
  if (new TextDecoder().decode(chunk.data).includes("Started")) { s.close(); break; }
}
const r = await s.result;               // same fields, camelCase
```

- `exec.stream` itself does not wait: it submits the call and returns the stream object. It goes
  through the same wait gate as `exec.run` (no open transaction, not in middleware, not at the
  module top level) and carries the same refusal codes (`busy` for admission).
- `read()` parks until a chunk is available, the stream ends (nil / iterator done), or the run
  is cancelled. Each wake is a new run: the budget and the run deadline are re-armed, as for
  `tui.poll` and `hull.sleep`. A read is refused while another read of the same stream is waiting
  (coded `busy`).
- `close()` kills the command (a `KILL` frame), drops what is buffered, and makes later reads
  return nil. It is idempotent. `result()` after `close()` returns the result with `signal =
  "SIGKILL"`.
- Breaking out of `for await` calls the iterator's `return`, which closes the stream.
- The stream object is owned by the run that made it, like a `db.open` handle: it may be passed
  to a task (`hull.async` / `hull:_task`) of the same app. A finalizer closes a stream that is
  collected unclosed (Lua `__gc` / `__close`, JS a class finalizer), so an abandoned stream never
  holds its slot until the timeout.
- `chunk.data` is a Lua string / JS `Uint8Array`, at most `HL_EXEC_STREAM_CHUNK_MAX` (64 KiB).
  Small writes are coalesced by the broker up to that size, or 20 ms after the first byte,
  whichever comes first, so a command that writes byte by byte does not cost a run per byte.
- Each chunk is charged before it is handed to the app: `HL_WORK_EXEC_CHUNK_UNITS` (320, a WS
  frame's measured floor) plus 1 unit per 8 bytes.

A request handler may stream (an SSE endpoint relaying `journalctl -f` is the obvious use).
When the client disconnects, the request is cancelled, which closes every stream the request's
run still has open. A detached task's streams close at teardown, through the runtime's teardown
list (`HlAsyncTeardown`), before the broker is stopped.

## 5. Flow control: credit, not buffering

The core of the design. Every stream has a WINDOW of `HL_EXEC_STREAM_WINDOW` (256 KiB) per pipe:

1. The broker reads a child pipe only while that pipe has credit. With no credit it stops
   polling the pipe, so the child's next write blocks once the kernel pipe buffer (16-64 KiB) is
   full. Hull holds at most the window; the command waits.
2. Each `CHUNK` the broker sends uses up credit for its bytes.
3. The server returns credit (`CREDIT {id, fd, bytes}`) once the APP has consumed a chunk (the
   `read()` that returned it), not when the server received it. So an app that stops reading
   stops the command after at most one window per pipe in flight.
4. The total caps (`stdout_max` / `stderr_max` of a stream command) are counted by the broker as
   it reads. Past one, it sends what is within the cap, marks the pipe truncated and kills the
   command (a stream that never ends must not be left to run once its consumer can no longer
   be told the truth). `stdout_truncated` / `stderr_truncated` say so.

Memory per stream, worst case: one window per pipe in the broker (2 x 256 KiB), the same in
flight in the router queue (macOS) and in the server's per-stream queue. With 8 streams in flight
that is at most 8 x 2 x 3 x 256 KiB = 12 MiB process-wide, fixed, against today's up to 17 MiB
PER `exec.run` call held for delivery. The router's 32 MiB reservation counts a stream as 2
windows, not as its total caps.

Off-heap accounting in the server: a chunk waiting for the app is held on the VM's off-heap total
(`HlOffheapHold`, as `http.fetch` bodies are), so the window is also bounded by the heap limit.

## 6. Protocol

The server, the broker and the command brokers are always the same binary, so the channel
protocol changes without a version field.

| Frame | Direction | Payload |
|---|---|---|
| `RUN` | server -> broker | unchanged, plus one flag byte at the end: `0x1` = stream |
| `CHUNK` (new, 9) | broker -> server | `u8 fd` (1 stdout, 2 stderr), bytes (1..`HL_EXEC_STREAM_CHUNK_MAX`) |
| `CREDIT` (new, 10) | server -> broker | `u8 fd`, `u32 bytes` |
| `DONE` | broker -> server | for a stream: the same head, with `stdout_bytes` / `stderr_bytes` (u64 each) in place of the output |
| `KILL` | server -> broker | unchanged |
| `BUSY` / `REFUSED` | broker -> server | unchanged |

Rules the broker enforces (the server's are only a fast path, §5.2 of the parent):

- a stream RUN for a command without `HL_EXEC_CMD_STREAM` is `REFUSED`;
- `CREDIT` for an unknown id is ignored; credit that would take a pipe past one window is a
  protocol error, and the broker drops that call (kills it, replies `REFUSED`) - a compromised
  server could otherwise lift back pressure and make the broker buffer without bound;
- `DONE` is sent only after the last `CHUNK` of both pipes, and only when both are closed and the
  child is reaped, so the server needs no reordering.

The macOS router relays `CHUNK` frames from a command broker to the server's queue and
`CREDIT` frames to the command broker's queue (the per-broker queue of #1234). It forwards credit
only for ids it routed to that broker, and checks the same window bound. The server channel's
backpressure stays as today: the router stops reading the server when its small-frame queue is
nearly full.

## 7. Time

- The broker times a stream command by its own `timeout_ms` (up to 10 min), as today. Time spent
  blocked on back pressure COUNTS: a consumer that never reads makes the command time out, never
  keeps it alive.
- `RLIMIT_CPU` is the timeout plus 1 s, as today, so a 10-minute stream may burn 10 minutes of
  CPU. That is the declared authority; `hull build` / `hull check` warn about a stream command
  over 60 s (`hull/exec_lint.lua`), naming its timeout.
- The server's per-op deadline (`exec_op_deadline`, the wedged-broker backstop) becomes a
  per-read deadline for a stream: the command's timeout plus a grace, measured from the call's
  start.
- The run deadline does not apply to the time a read is parked (no run is going), exactly as for
  `http.fetch`.

## 8. Audit, inspect, tests

- `--audit`: today's one record per call, written when the call starts, plus one `exec.stream.end`
  record with status, byte counts, truncation and duration. Chunks are not logged.
- `hull inspect` and the startup report show `stream` and the timeout for each stream command.
- `hull test`: `test.exec_stub(name, result_or_fn)` gains a stream form: a stub result may carry
  `chunks = { { fd = "stdout", data = "..." }, ... }`, delivered in order, one per read, then the
  result. A stub function may return the same. `exec.stream` with no stub refuses, as `exec.run`
  does.
- Types: `types/hull` gains `exec.stream`, `ExecStream`, `ExecChunk`; `make check-types`' drift
  check covers it.

New tests (each fails on the code before its PR):

- `e2e_exec_frames.c`: a stream RUN to a non-stream command is refused; credit past one window
  drops the call; a stream with no credit stops reading (the child's write blocks, its pid still
  alive, no `CHUNK` past the window); `DONE` follows the last `CHUNK`.
- `tests/e2e_exec_stream.sh` (both runtimes, Linux and macOS, `HL_EXEC_TEST_HOOKS=1`):
  - chunks arrive before the command ends (a command that prints, sleeps 2 s, prints);
  - a 200 MiB stream through a 256 KiB window with the server's RSS bounded;
  - an app that stops reading: the command times out, `timed_out = true`, no growth;
  - `close()` and a client disconnect kill the command (its pid gone within 1 s);
  - the total cap kills the command and sets `stdout_truncated`;
  - a 70 s stream completes where `exec.run` of the same command is clamped to 60 s;
  - an abandoned stream (no `close()`) is finalized and its slot freed;
  - a macOS router with two stream commands, one consumer stopped, the other unaffected.

## 9. Security review of the change

| Concern | Answer |
|---|---|
| Longer-lived children | Only for commands that declare `stream = true` and a timeout, both signed with the manifest; linted over 60 s. The child is the same: no process creation, same domain, dies with its broker. |
| Memory | Fixed per stream (§5), independent of the command's output; the server's queue is on the off-heap total under the heap limit. |
| A compromised app process | It can send `CREDIT` frames itself. The broker bounds credit to one window per pipe, so the worst it can do is what a reading app does. It cannot start a stream on a non-stream command (broker check) nor raise a timeout (broker-side, from the signed policy). |
| CPU on the event loop | Each chunk is a run charged before the app sees it; coalescing bounds the chunk rate a byte-at-a-time writer can force (at most one chunk per 20 ms per pipe unless the window fills). |
| Holding slots | A stream holds one of the 8 in-flight slots for up to its timeout. `concurrency` is per command, so a stream command with `concurrency = 1` cannot take all 8. The startup report names stream commands. |
| Cancellation | Every path that frees the op (request cancel, close, finalizer, teardown) sends `KILL`; the broker kills through the keeper (macOS) or the process group (Linux). |

## 10. Plan

1. **PR 1, broker and protocol (Linux + macOS):** `stream` flag in the policy encoder and
   validator, the stream ceilings in `include/hull/limits/exec.h`, `CHUNK` / `CREDIT` /
   stream `DONE` in the broker and the router, credit enforcement. Tests: `e2e_exec_frames.c`
   cases, `fuzz_exec_policy` corpus entries.
2. **PR 2, bindings:** `exec.stream` in both runtimes, the server's per-stream queue and credit
   return, wait gate, finalizers, teardown, charges, audit records, manifest extractors, types.
   Tests: `tests/e2e_exec_stream.sh`.
3. **PR 3, test stubs and docs:** the stream form of `test.exec_stub`, `hull inspect`, the lint,
   docs/app_api_reference.md, CLAUDE.md, CHANGELOG.

## 11. Open questions for review

1. **The stream timeout ceiling.** 10 min proposed (`app.main`'s default run limit). Is there a
   hsctl / privd command that needs longer (a backup)? A higher ceiling is one constant; the
   RLIMIT_CPU consequence is the thing to weigh.
2. **`exec.run` on a stream command.** Proposed: allowed, under the `exec.run` rules. The
   alternative is to refuse it, which keeps one way to call each command.
3. **Window and chunk sizes.** 256 KiB window, 64 KiB chunk, 20 ms coalescing. Measure the
   throughput of a 200 MiB stream before settling them.
4. **Streamed stdin** as a follow-up (an `s:write(bytes)` / `s.write(bytes)` with the same credit
   scheme in the other direction), or not at all?
