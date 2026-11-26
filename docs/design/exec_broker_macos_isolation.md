# Per-command isolation on macOS: one sub-broker per command

- **Status:** phase 1 implemented (#1218): `router_start` in `cap/exec_broker.c`,
  `hl_exec_plat_router` / `hl_exec_plat_router_lock` in `cap/exec_broker_macos.c`,
  `hl_exec_policy_slice` in `cap/exec.c`. Decisions in §10.
- **Parent design:** [exec_broker.md](exec_broker.md) (§6.4 macOS backend, §7 lifetime, §15.6
  the interim proposal this replaces).
- **Scope:** the macOS backend only. Linux is unchanged. No manifest change.

## 1. Problem

On Linux each declared command is its own principal. Each child has a Landlock domain that
executes only its own binary and loader and grants only its own `host` and `manifest.fs`
paths, plus a seccomp filter that admits `AF_UNIX` only under its own `host.unix_sockets`
(exec_broker.md §6.3).

On macOS a sandboxed process cannot narrow itself: `sandbox_init` in a sandboxed process fails
with `EPERM`. So the broker applies ONE Seatbelt profile, the union of every command's grants,
before `READY`, and every child inherits it (§6.4). That gives every command:

1. every other command's `host` and `manifest.fs` grants;
2. `AF_UNIX` when any command sets `host.unix_sockets`;
3. `process-exec` of every declared binary. A command that calls `execve` itself can run
   another command's binary with any arguments. The argv shape is matched by the broker, and
   that exec does not go through it.

The startup line `exec: limit: macOS runs every command in ONE shared sandbox ...` and `hull
inspect`'s "Effective authority" say so. The goal of #1218 is that each declared command is an
independently enforced principal on macOS too.

## 2. Constraints (measured on macOS 26, arm64, Hull v0.27.0)

- **No nested sandbox.** A profile can be applied only by a process that has none. Fork and
  exec inherit the profile. So every per-command profile must be applied by a process that
  was never sandboxed.
- **No process creation for commands.** `(allow process-fork (process-path (param "SELF")))`
  limits fork to this binary's image. A command cannot fork, vfork or posix_spawn. Keep this.
- **The manifest is unknown when the broker is spawned.** The server spawns the broker before
  phase 1, before the app loads (§5.1). The number of commands is known only at `POLICY`.
- **No `PR_SET_PDEATHSIG`.** A process learns of its parent's death by code it runs (a kqueue
  `EVFILT_PROC` / `NOTE_EXIT` watch, or EOF on a channel), never by a kernel signal.
- **Cost of one broker process:**
  - idle physical footprint (`vmmap --summary`): **4.0 MB**;
  - spawn, policy load and `sandbox_init`: about **7.5 ms** of app start (an `app.main` that
    returns at once: 45.8 ms without an `exec` section, 53.3 ms with one command);
  - spawn and exit of a bare broker: 10 ms (`/usr/bin/true`: 2.8 ms).
- **Limits:** at most `HL_EXEC_MAX_COMMANDS` (32) commands, `HL_EXEC_MAX_INFLIGHT` (8) calls in
  flight per process.

## 3. Options

| | Shape | Per-command profile | Broker sandboxed | Verdict |
|---|---|---|---|---|
| A | the server spawns one broker per command after the manifest is known | yes | yes | rejected (§3.1) |
| B | the broker stays unsandboxed and execs a helper per call that applies the command's profile, then execs the binary | yes | **no** | rejected (§3.2) |
| C | the broker becomes a ROUTER: at `POLICY` it spawns one sub-broker per command, each applies its own command's profile, then the router sandboxes itself | yes | yes | **recommended** (§4) |
| D | keep the union; refuse differing grants unless `--allow-degraded-sandbox` (§15.6 interim) | no | yes | rejected (§3.3) |

### 3.1 Why not A (server-spawned brokers)

- It changes the server, which today has one channel on every host. The server would
  multiplex N channels and route by name, and that code would differ between Linux and macOS.
- It moves the spawn to after the app's top level has run, breaking §5.1's rule that the
  broker is spawned before the app loads. On macOS that is not a new exposure (phase 1 is a
  no-op there, so the top level already runs unsandboxed). It is still a second rule to
  remember, and it would hold on macOS only.
- C gives the same result with the server unchanged.

### 3.2 Why not B (per-call helper)

- The helper must apply a profile, so whatever spawns it must be unsandboxed for good. That is
  a long-lived, unsandboxed process parsing frames from the app, which is the opposite of
  §6.2.
- Every call pays an extra exec of the 25 MB hull binary (about 10 ms, above), on top of the
  command's own exec.

### 3.3 Why not D (the interim refusal)

D is cheap (a grant comparison and one `--allow-degraded-sandbox` check), but it isolates
nothing. It only makes the union opt-in, and it breaks a v0.26.0 macOS app with differing
grants until the operator passes the flag. Shipping D and then C would change macOS behaviour
twice: once to demand the flag, once to make it unnecessary. C removes the union, and with it
the reason for D, so D is not built.

## 4. Design: router plus per-command sub-brokers

### 4.1 Process tree

```
 server (app VM; no exec / fork)
   │  one channel, frames unchanged ("HXEC")
   ▼
 exec broker = ROUTER   (macOS profile: no exec, no fork, signal children only)
   │  one channel per command (same frames)
   ├──► sub-broker "gpu-query"   (profile: gpu-query's grants only; fork SELF only)
   │       └──► job: /usr/sbin/system_profiler ...   (inherits; cannot fork)
   ├──► sub-broker "convert"     (profile: convert's grants only)
   │       └──► job: /opt/homebrew/bin/magick ...
   └──► ...
```

A sub-broker is TODAY's broker with a one-command policy: the same binary, `argv[1]`
`HL_EXEC_BROKER_ARGV1`, the same `hl_exec_broker_main`, `policy_load`, `cmd_load`, job table,
child setup and `hl_exec_plat_lock` with `n == 1`. Its profile (`hl_exec_macos_profile` with one
command) is the union of exactly one command, which is that command's own grants. The new code
is the router, and the router lives in the portable `exec_broker.c`.

**One command needs no router.** With `n == 1` the broker behaves exactly as today: no
sub-broker, no extra process, no extra hop. The router starts only for `n >= 2` on macOS, the
only case with a union to remove. That leaves two code paths on macOS, and the tests run both
(§8).

### 4.2 Start

At `POLICY`, the router (still unsandboxed, as today's broker is at this point):

1. Validates the whole policy (`hl_exec_policy_validate`) and runs `cmd_load` for every
   command, as today. An error refuses the start with the same report.
2. For each command `i`: makes a socketpair, and `hl_chan_spawn`s itself with
   `HL_EXEC_BROKER_ARGV1` and the same environment allowlist (`LANG`, `TZ`). All N are spawned
   before any is waited on, so their dyld and init overlap.
3. Sends each sub-broker a `POLICY` frame holding the same header (flags, app and state roots,
   protected database paths) and a policy of ONE command. The policy is the original 6-byte
   header with its count set to 1, followed by that command's entry bytes, sliced unchanged
   from the server's policy (entries are stored back to back; the router records each entry's
   start and end while it reads them). Nothing is re-encoded, so a sub-broker's command is
   byte-for-byte the one the server sent and `hl_exec_policy_validate` accepted. A sub-broker
   never sees another command's entry.
4. Waits for every sub-broker's `READY` or `REFUSED`, inside the server's existing 10 s wait
   for the broker's own `READY` (`hl_exec_broker_apply`). Any `REFUSED`, early exit or timeout
   refuses the whole start, naming the command and quoting the sub-broker's report.
5. Applies its OWN profile:
   - deny default;
   - no `process-exec`, no `process-fork` (a router that could exec itself could start a fresh,
     unsandboxed broker and hand it any policy);
   - `(allow signal (target children))`;
   - no file-system rule at all: everything the router uses after the lock (the server
     channel, the sub-broker channels, its kqueue watches) is a descriptor it already holds. A
     test checks that a router opening any path after `READY` is refused (§8).
6. Replies `READY` to the server with the merged report: one `exec: <name> -> <real path>` line
   per command, plus the macOS limit line (§4.5).

Every sub-broker must exist before step 5, so they are spawned eagerly, never on first use. A
lazy spawn needs the router to keep the right to exec itself, which undoes step 5.

### 4.3 One call

The server side is unchanged. The router does what the broker does today up to the fork:

1. It admits the `RUN`: live ids, `HL_EXEC_MAX_INFLIGHT`, per-command `concurrency`, the 32 MiB
   output reservation. The router is the one admission authority for the tree.
2. It matches the slots (`hl_exec_check_call`, INV-17).
3. It forwards the frame to command `i`'s sub-broker with the same id, instead of forking.

The sub-broker admits and matches the call again (defence in depth: the router is the only
process that can write to it, but a router bug must not widen a command). It then runs the
call exactly as today: re-open and re-check the binary, fork, child setup, exec.

Two consequences, accepted in phase 1 (review, #1218):

- A `KILL` frees the router's slot and reservation as soon as it is forwarded, while the
  command's broker still holds the killed child until it is reaped (milliseconds: it gets
  `SIGKILL`). For that moment the router's tree-wide caps under-count by one call; the command
  broker's own caps still hold.
- Forwarding is a bounded blocking write (a `RUN` 5 s, a `KILL` 1 s). A command broker always
  reads its channel, so this never deadlocks, but a large-stdin `RUN` to a slow broker stalls
  the router for that time, and a write that times out marks that command's broker lost. A
  per-command output queue would remove both; not needed until a deployment shows it.

A reply larger than the call's reservation (plus its 24-byte head) is taken as a broker gone
wrong: that command's broker is dropped. `PROBE` frames are relayed in test builds only.

`DONE` / `BUSY` / `REFUSED` frames go back the same way: the router reads each one from the
sub-broker's channel and queues it for the server, where its output already counts against the
reservation the router made in step 1. `KILL {id}` is forwarded to the sub-broker that holds the
id.

Output is buffered twice at most: up to the command's `stdout_max` / `stderr_max` in the
sub-broker until the job ends, then in the router's reply queue. The router's 32 MiB
reservation still bounds what the tree holds for delivery. Each sub-broker's own buffers are
bounded by its command's caps times its `concurrency`.

### 4.4 Lifetime (§7, macOS)

| Event | Today (one broker) | Router |
|---|---|---|
| server exits or is SIGKILLed | the broker sees NOTE_EXIT / EOF, kills every job's process group | the router sees it and exits; each sub-broker sees NOTE_EXIT on the router plus EOF, and kills its jobs |
| `hl_exec_broker_stop()` / EOF | the broker kills every job | the router closes every sub-broker channel; each kills its jobs |
| the broker (router) is SIGKILLed | **its running jobs live on** until they exit | each sub-broker sees EOF and NOTE_EXIT on the router and kills its jobs: **no job outlives it** |
| one sub-broker is SIGKILLed | n/a | **its own running jobs live on** (at most its `concurrency`); the router sees the channel close, fails that command's waiting calls with "the command broker is gone", and refuses later calls to it |

So the gap narrows from "the broker" to "one command's sub-broker", and to that command's jobs
only. A job still cannot fork, still runs under its `RLIMIT_CPU`, and is now inside its own
profile rather than the union.

Closing the last row is open question Q2 (§7).

### 4.5 What is reported

- The union line `exec: limit: macOS runs every command in ONE shared sandbox ...` is removed.
- The parent-death line becomes: `exec: limit: no parent-death signal on macOS: a command's
  sub-broker killed with SIGKILL leaves that command's running calls until they exit (they
  cannot fork; their CPU limit holds)`.
- `hull inspect` "Effective authority" prints each command's own grants on macOS, as on Linux,
  and drops the union.

## 5. Equivalence with Linux

| Guarantee (Linux, §6.3) | macOS today | macOS with the router |
|---|---|---|
| a command reads and writes only its own `host` / `manifest.fs` grants | union | **own grants only** |
| a command executes only its own binary and loader | any declared binary | **own binary only** (`process-exec` of one path) |
| `AF_UNIX` only under its own `host.unix_sockets` | any command's flag opens it for all | **own flag only** |
| device ioctls only on its own `host.write` devices | union | **own only** |
| no process creation | yes (`process-fork` SELF only) | yes, unchanged |
| no job outlives the broker | no: a SIGKILLed broker leaves its jobs | no, narrower: a SIGKILLed sub-broker leaves that command's jobs |
| the broker cannot widen its own policy after `READY` | yes | yes: the router can neither exec nor fork; a sub-broker's profile is fixed |
| a broker bug cannot exec command B for command A | no (one profile executes every binary) | **yes**: each sub-broker's profile executes one binary. The router executes nothing |

The last row is stronger than Linux. Linux's broker domain holds `EXECUTE` on every declared
binary (§1, layer 3), so a broker bug there could still exec B on A's behalf. The per-child
domain is what stops the job itself.

## 6. Cost

- **Processes:** 1 + N for N >= 2 commands (today: 1). An app with one command is unchanged.
- **Memory:** about 4 MB per sub-broker, idle. 3 commands: about 12 MB; the 32-command cap:
  about 128 MB.
- **Start time:** the N spawns overlap; the serial part is the per-sub-broker policy load and
  `sandbox_init`. Estimated a few ms per command, to be measured (Q1).
- **Per call:** one extra frame hop through the router, a `memcpy` of the reply. Small next to
  `fork` + `exec` (§5.2 charges 2 ms).

Forking the sub-brokers from the unsandboxed router instead of exec'ing them would share pages
copy-on-write and skip dyld. It was rejected (§10): a forked sub-broker inherits every
descriptor the router holds, including the server channel, other sub-brokers' channels and
other commands' opened grants, and must close all of them before `sandbox_init`. An open
descriptor bypasses Seatbelt, so one missed close is a grant leak. Exec gives a clean image by
construction (every descriptor is `CLOEXEC`).

## 7. Open questions

- **Q1. Start time and memory, measured.** N = 2, 8, 32 on an M-series Mac and on the macOS CI
  runner: wall time to `READY`, and each sub-broker's footprint. Measured in phase 1 and
  recorded here. The cap stays 32 (§10); revisit only if the measurement is far from the
  estimate.
- **Q2. Reaping a dead sub-broker's jobs.** The router could kill the process groups of a dead
  sub-broker's jobs. Each sub-broker would report `STARTED {id, pgid}`, and the router would
  `killpg` them on NOTE_EXIT of that sub-broker. Two problems:
  - the jobs are the router's grandchildren, so `(target children)` probably does not cover
    them (to measure), and a wider signal rule lets a compromised router signal any process of
    the uid;
  - once the sub-broker is dead, nobody reaps the job. Its pid can be reused after it exits,
    so a late `killpg` can hit an unrelated group.

  Decided: not in phase 1. Keep the narrowed limit and its report line.
- **Q3. Linux.** Should Linux adopt the router for uniformity? Decided: no. Linux's per-child
  domain is already per command, and PDEATHSIG gives the stronger lifetime guarantee. The router
  would add processes for no gain.

## 8. Tests

`tests/e2e_exec_broker_macos.sh` (CI job `exec-broker-macos`, `make HL_EXEC_TEST_HOOKS=1`):

- replace the union assertions (`...and to the app's other commands (the union)`) with their
  inverse:
  - command A cannot read or write B's `manifest.fs` or `host` grant;
  - A without `unix_sockets` cannot `socket(AF_UNIX)` while B has it;
  - A's `execve` of B's binary fails with `EPERM`;
- the start report no longer has the union line, and has the reworded parent-death line;
- **router SIGKILL:** a running job of each command is killed. Today the same test asserts the
  job survives;
- **sub-broker SIGKILL:** that command's running job survives (the documented limit), its
  waiting call fails with "the command broker is gone", a later call to it is refused, and the
  other commands keep working;
- a sub-broker `REFUSED` (an unsafe path for one command) refuses the whole start, naming that
  command;
- one command: exactly one broker process, as today; the whole suite runs once with one
  command and once with several, so both macOS code paths are covered;
- the test hooks' `PROBE` frames reach the server through the router (the existing probe
  cases pass unchanged with several commands);
- the router cannot exec, fork or open any path after `READY` (a test hook that tries each,
  `EPERM`);
- `hull inspect` on macOS prints per-command authority.

Unit (`test_exec_broker_macos`): each one-command slice passes `hl_exec_policy_validate`, and
its entry bytes are identical to that entry in the original policy; the router's profile text
has no `process-exec`, no `process-fork` and no file rule.

## 9. Phasing and compatibility

1. **Phase 1:** the router and sub-brokers, the reports and `hull inspect` change, and the
   tests above. One PR, macOS only. CHANGELOG under **Security**: on macOS each command now runs
   in its own sandbox; a command that relied on another command's grants (never documented as
   a guarantee) loses them.
2. **Phase 2 (only if Q2 is reopened):** reaping a dead sub-broker's jobs.

No manifest, frame or server change. Linux behaviour and encodings are byte-for-byte unchanged.
`exec_broker.md` §6.4 and §7 get updated in phase 1, and §15.6's interim proposal is marked as
superseded.

## 10. Decisions (review, 2026-11-26)

| # | Question | Decision |
|---|---|---|
| 1 | Approach | C, router plus one sub-broker per command (§4); A, B and D rejected (§3) |
| 2 | How a sub-broker starts | exec of this binary (clean image), not fork (§6) |
| 3 | An app with one command | keeps today's single broker; the router starts at two commands (§4.1) |
| 4 | A SIGKILLed sub-broker's running jobs | not reaped in phase 1; documented, narrowed limit (§4.4, Q2) |
| 5 | macOS command cap | 32, as on Linux; Q1 measures it in phase 1 |
| 6 | Linux | unchanged (Q3) |
| 7 | Policy given to a sub-broker | the original entry bytes behind a one-command header, never re-encoded (§4.2) |
| 8 | Router after `READY` | no exec, no fork, no file rule; tested (§4.2, §8) |
| 9 | Test-hook `PROBE` frames | forwarded by the router (§8) |
