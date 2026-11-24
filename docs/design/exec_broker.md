# Design: declared commands through a spawn broker (`hull/exec`)

- **Status:** PR 1 (#1210: manifest section, matcher, stubs) and PR 2 (the native Linux broker)
  implemented; macOS / OpenBSD (PR 3) and the `sha256` pin (PR 4) planned. Review rounds 1-3
  folded in: seccomp inheritance, child process creation, broker-side admission, required
  Landlock rights, file identity vs content, `unix_sockets` scope, `sha256` until enforced.
- **Motivation:** hsctl needs to read node state that only a system tool reports: `nvidia-smi`
  for GPUs, `systemctl show` for its own `hs-*` units. Today an app cannot run any program: the
  app sandbox has no `exec` / `proc` / `fork`, and `hl_tool_spawn` exists only in tool mode.
- **Builds on:** the GPU compile helper (`docs/design/gpu_compile_helper.md`,
  `src/hull/cap/gpu_helper.c`): a Hull-binary helper in a hidden `argv[1]` mode, spawned before
  the phase-1 sandbox and stopped once the manifest shows it is not needed. Also builds on
  `include/hull/shared/child_channel.h`, the framed-socket channel the helper and the build
  plugin runner already share.
- **New code (planned):** `src/hull/cap/exec.c` (the policy, the shared argv matcher, the app
  side), `src/hull/cap/exec_broker.c` (the broker), `include/hull/cap/exec.h`,
  `include/hull/limits/exec.h`, `runtime/{lua,js}/mod_exec.c`, and module `hull/exec@1`.

## 1. Problem and principle

Spawning a process is not the risk in itself. The risk is spawning something general-purpose:
an allowlist that holds `sh`, `bash`, `python3`, `env`, `find` (`-exec`), `xargs`, `ssh`,
`sudo`, `systemd-run` or `busybox` gives back everything the sandbox took away. So **the unit
of permission is a declared command shape, not a binary**. That is the binary, its fixed
arguments, and the few slots the app may fill, each with a shape it must match.

Three layers hold the boundary, each one independent of the others:

1. **Shape.** The app names a declared command, never a path. Fixed arguments cannot change,
   and a slot takes only a value its shape admits.
2. **Process.** The app process still cannot exec, fork or clone. Only a small broker process
   can, and the broker runs only what the policy declares.
3. **Kernel.** The broker's own kernel sandbox grants execute on exactly the declared binaries
   (and their ELF interpreter). A bug in the broker still cannot exec an undeclared path.

This is the privilege-separation split of OpenSSH's monitor. Hull already has the narrow form
of it in `tool.spawn` with a compiler allowlist.

## 2. Decision summary

| Question | Decision |
|---|---|
| Manifest | An `exec` section: name -> `{ path, argv, timeout_ms, stdout_max, stderr_max, env, concurrency, fs, unix_sockets, sha256 }`, plus `"hull/exec@1"` in `modules`. Both are needed, as for `tui`. |
| Slot shapes | **Structured, not regex** (§4): `prefix` / `suffix` / `chars` / `min` / `max` / `enum` / `repeat`. One C matcher is used by both the cap layer and the broker. |
| Process | Hull's own binary in a hidden mode (`argv[1] == "__hull_exec_broker"`), started with `hl_chan_spawn` before phase 1 and stopped after the manifest if there is no `exec` section. This is the GPU helper's lifecycle. |
| Policy delivery | Exactly ONE `POLICY` frame, built in C from the sealed `HlManifest` after `hl_sig_check_runtime_policy`. The broker then locks itself down; a second `POLICY` frame makes it exit. |
| Child | Forked by the broker. It gets `setsid`, closed fds, stdin from `/dev/null`, capped stdout / stderr pipes, a fixed environment, rlimits, `PR_SET_PDEATHSIG`, its own narrower Landlock domain, and a seccomp filter that **refuses process creation** (threads allowed). It runs the binary through `execveat(fd, "", ..., AT_EMPTY_PATH)` on the descriptor that was checked. |
| Seccomp | Filters are inherited and can only narrow, so the broker's filter admits the union of what the broker AND the supported commands need (`socket` for `AF_UNIX` only, for `systemctl`). Each child then adds a filter removing what its command does not need (§6.2). |
| Admission | **The broker is authoritative** for the in-flight cap, per-command concurrency, live request ids and aggregate buffered output. The binding checks the same caps only to answer `busy` fast (§5.2). |
| Kernel | Linux: Landlock `EXECUTE` on the declared binaries and their `PT_INTERP` only, with a defined minimum ABI per guarantee (§6.5). OpenBSD and macOS: **planned** (§6.4). Windows: **not supported** (§9). |
| No kernel mechanism, or a required right missing | `exec` is refused at app load, unless `--allow-degraded-sandbox` is given, which names each guarantee that is unavailable. This is the existing rule for an incomplete backend. |
| App API | Async only: `exec.run(name, args?)` returns a result table (Lua parks the coroutine, JS returns a Promise). It goes through the wait gate and carries refusal codes. |
| Result | `{ status, signal, stdout, stderr, timed_out, stdout_truncated, stderr_truncated, duration_ms }` |
| `hull test` / `hull agent` | No broker. `exec.run` answers only from stubs a test registers (`test.exec_stub`); with no stub it refuses (§8). |
| Binary trust | The ownership and mode check (root-owned path, not group / world-writable, no setuid / setgid, no file capabilities) is **mandatory**, with or without a pin. Replacement is detected by `(st_dev, st_ino)`. The optional `sha256` checks the main executable's content only (§6.1). |
| Taxonomy | A **base cap module** behind a manifest gate, not a `--with` feature, because the broker has to be in the runner's hidden mode before load (§10). |

## 3. Manifest

```lua
app.manifest({
  modules = { "hull/exec@1" },
  exec = {
    ["gpu-query"] = {
      path = "/usr/bin/nvidia-smi",
      argv = { "--query-gpu=index,name,memory.used,memory.total,temperature.gpu,power.draw,utilization.gpu",
               "--format=csv,noheader,nounits" },
      timeout_ms = 5000, stdout_max = 65536,
      fs = { read = { "/dev/nvidiactl", "/dev/nvidia0" }, write = { "/dev/nvidiactl", "/dev/nvidia0" } },
    },
    ["unit-state"] = {
      path = "/usr/bin/systemctl",
      argv = { "show", "--no-pager", "-p", "ActiveState,NRestarts,ActiveEnterTimestamp", "--",
               { prefix = "hs-", suffix = ".service", chars = "a-z0-9-", max = 64,
                 ["repeat"] = { min = 1, max = 32 } } },
      unix_sockets = true,   -- systemctl connects to systemd over a pathname socket
      timeout_ms = 5000, stdout_max = 262144,
    },
  },
})
```

`repeat` is a reserved word in Lua, so the key is written `["repeat"]`. `min = 1` matters
here: `systemctl show` with no unit queries the manager itself, which is not what this command
declares. A `repeat` without `min` defaults to 1 for the same reason; `min = 0` must be written.

JS takes the same shape (`timeoutMs`, `stdoutMax`, ...), read by both manifest extractors.

Load-time validation is in `manifest.c` and `cap/exec.c`, and failure refuses app load:

- **Names:** `[a-z0-9][a-z0-9-]{0,63}`; at most `HL_EXEC_MAX_COMMANDS` (32).
- **`path`:** absolute, with no `.` / `..` component.
  - Resolved with `realpath` once, by the broker. A path that resolves elsewhere is recorded
    under its real path and logged.
- **`argv`:**
  - At most 64 entries and 16 KiB of fixed text.
  - Fixed entries are strings with no NUL.
  - A slot is a table. Only the LAST entry may carry `repeat`.
- **Limits:** `timeout_ms` is 100..60000 (default 5000). `stdout_max` is up to 16 MiB (default
  64 KiB). `stderr_max` is up to 1 MiB (default 16 KiB). `concurrency` is 1..8 (default 2).
- **`env`:** a name -> LITERAL value map. No `"$VAR"` reference and no secret ever reaches a
  child. The names `LD_*`, `DYLD_*`, `PATH` and `IFS` are refused.
- **`fs`:** extra Landlock grants for this command (§6.3). Each must be absolute, and `write`
  is only under `/dev`, `/run` or `/tmp`.
- **`sha256`:** 64 hex characters, optional. **Refused at app load until the broker enforces
  it (PR 4):** a signed manifest must never advertise a pin the broker ignores.
- **`unix_sockets`:** boolean, default false. Lets the child create `AF_UNIX` sockets (§5.3);
  `systemctl` needs it to reach systemd. No other socket family is ever allowed.

  The grant is broader than "talk to systemd". Landlock does not mediate pathname sockets, so
  the child can `connect` to ANY pathname socket the service account can reach under the
  filesystem's own permissions: the system D-Bus, `/run/systemd/private`, a database's
  socket, a container runtime's socket if the account is in its group. The ABI 6
  abstract-socket scope (§6.2) is a separate restriction: it keeps the child off abstract
  sockets outside its domain and does not narrow pathname ones. v1 keeps the boolean and
  makes its scope visible instead of narrowing it.

The section is part of the manifest, so `hull build` signs it and `--verify-sig` compares it
like every other field. `hull inspect` and `hull agent overview` list the declared commands,
which is the disclosure an operator reads before running an app. For each command they show
the resolved path, the fixed argv, each slot's rules, `fs` grants, and, for
`unix_sockets = true`, the scope in words:

```
unit-state  /usr/bin/systemctl
  local IPC: may connect to any pathname unix socket the service account can reach
             (not limited to systemd)
```

`hull build` / `hull check` add a lint, a WARN rather than a boundary:

- a `path` whose basename is a general-purpose program (the list in §1, plus interpreters and
  `env`);
- a slot with no `chars` or `enum`;
- a slot with `allow_dash`;
- a `path` outside `/usr`, `/bin`, `/sbin` and `/opt`.

## 4. Slot shapes (why not regex)

The proposal's `{"match": "^hs-[a-z0-9-]+\\.service$"}` is clear, but a regex in the broker
costs three things:

1. **Attack surface.** The broker is the process we want to keep around 400 lines with no
   engine in it.
2. **Dialects.** Lua patterns, JS RegExp and POSIX ERE differ. The cap layer and the broker
   must agree on every byte.
3. **Cost.** Backtracking makes the cost quadratic or worse, and Hull has spent many audit
   rounds charging exactly that.

A slot is instead a conjunction of linear checks, all of which must hold:

| Key | Meaning |
|---|---|
| `enum` | the value is one of these strings (exclusive with the rest) |
| `prefix` / `suffix` | literal; the value must start / end with it |
| `chars` | the allowed bytes of the part between prefix and suffix: ASCII ranges and literals (`a-z0-9._-`); no class means `[A-Za-z0-9._-]` |
| `min` / `max` | byte length of the whole value (defaults 1 / 256, ceiling 4096) |
| `repeat` | `{ min = M, max = N }`: the slot takes M..N values, each checked alone (N <= 64; M defaults to 1) |
| `allow_dash` | the value may start with `-` (default: never) |

These rules apply to every slot whatever it declares:

- no NUL;
- not empty;
- no leading `-` without `allow_dash`;
- all values of one call together under `HL_EXEC_MAX_ARG_BYTES` (64 KiB).

The leading-dash rule closes **option injection**: a value such as `--host=evil` or
`--output=/x` is the classic way to steer a fixed binary with an argument the app chose. That
is also why the example puts `--` before the slot. The lint suggests `--` when a slot follows
fixed arguments.

`hl_exec_slot_match` (`cap/exec.c`) is one function, linked into both sides: the binding calls
it for a precise error, and the broker calls it again as the authority.

## 5. Process model

```
 app process (VM, sandboxed: no exec/fork/clone)
     │  fd: socketpair (child_channel frames, magic "HXEC")
     ▼
 exec broker  (__hull_exec_broker; no VM; single thread)
     │  fork + execveat(checked fd)
     ▼
 child  (setsid; own Landlock domain; NNP; rlimits; PDEATHSIG; no process creation)
```

### 5.1 Start, policy, lock-down

1. **Spawn.** `hull_serve` / the `app.main` runner call `hl_exec_broker_start()` next to
   `hl_gpu_helper_start`. That is before `hl_sandbox_apply_pledge`, before any thread, and
   before the app loads.
   - The manifest is not known yet, so the broker is spawned unconditionally on a host where
     it could ever run (§9).
   - It costs one `posix_spawn` at start (about 1 ms) and an idle process until step 2.
   - `HULL_NO_EXEC_BROKER=1` skips it. `exec` is then refused.
   - The spawn uses `hl_chan_spawn` with an environment allowlist of nothing but `LANG` and
     `TZ`, so the broker never holds the app's environment or secrets.
2. **Stop if unused.** After manifest extraction, seal and `hl_sig_check_runtime_policy`: if
   there is no admitted `exec` section, the broker is closed (EOF) and reaped. This is the GPU
   helper's stop point.
3. **Send the policy.** Otherwise the server sends one `POLICY` frame, serialised in C from the
   sealed manifest. App code never sees the socket: no binding exposes it, the fd is
   `CLOEXEC`, and it is above the fds the app can name.
4. **Check the policy.** The broker validates the policy again. It refuses on any error, and a
   refusal fails app start. For each command, the broker:
   - resolves `path` with `realpath`;
   - checks the binary (§6.1) and reads its `PT_INTERP`.
5. **Lock down.** The broker applies its own sandbox (§6.2) and replies `READY` with the
   resolved paths, which the server logs once (`[hull:c] exec: gpu-query -> /usr/bin/nvidia-smi`).
   - From here a `POLICY` frame is a protocol error, and the broker exits.
   - A run-time compromise of the app therefore cannot widen the policy.
6. **Continue startup.** The server applies phase 2. Its seccomp and Landlock stay exactly as
   today: no exec, fork or clone for the app.

**Whose code produced the policy.** The app's top level produced it, but the top level also
produced every other part of the manifest, and the manifest is the app's own declaration.
What matters is that it is fixed BEFORE the app serves any input:

- a request-time compromise of the VM gains nothing;
- a built binary under `--verify-sig` runs only the signed policy.

### 5.2 One call

The app side (binding, then the cap layer):

1. Check the wait gate. Refused with `wait_refused` inside a transaction, in middleware or at
   top level.
2. Look up the command. An undeclared name is an error; this is a programming mistake, not a
   refusal.
3. Match the slots. A mismatch is an error naming the argument index and the rule it broke.
4. Check admission, as a fast path only.
   - At most `HL_EXEC_MAX_INFLIGHT` (8) calls per process, and `concurrency` per command.
   - Past either cap the call is refused with `busy` without a round trip.
   - This check is a convenience. A compromised app process can skip it, so the broker
     enforces the same caps itself (below).
5. Charge the run `HL_WORK_EXEC_UNITS` (128k, about 2 ms of fork + exec at 64 units/us) plus
   `hl_work_bulk_units` of the argument bytes.
6. Send `RUN {id, name, args[]}` and park. The loop watches the channel fd through the async
   backend.
7. The timeout is the command's `timeout_ms`. It is not clamped to the run's deadline: the run
   is parked, so the wait holds no loop.
8. Cancellation sends `KILL {id}`. That covers a cancelled continuation (client gone), the
   `HlAsyncTeardown` entry at shutdown, and a request whose run tag fails.

The broker treats the app as untrusted after `READY`. It is the authority on admission:

1. **Admit.** A `RUN` is refused with a `BUSY {id}` reply, and nothing forked, when:
   - its `id` is already live (an id stays live until its reply is delivered or discarded);
   - `HL_EXEC_MAX_INFLIGHT` calls, or the command's `concurrency`, are already accepted;
   - the replies it has queued but not yet written would pass `HL_EXEC_MAX_BUFFERED`
     (32 MiB), counting this call's worst case of `stdout_max + stderr_max`.

   An accepted call counts against all three caps from admission until its `DONE` frame is
   fully written to the socket, or until the broker discards it (a `KILL`, or EOF). A
   completed call whose reply the app does not read therefore still holds its slot, so a
   stalled reader cannot pile up finished replies outside the cap. Writes are non-blocking;
   a reply waits in the broker's queue, never blocks the broker's loop.
2. Match the call again (§4). A mismatch is a `REFUSED {id}` reply.
3. Open the binary and check it again (§6.1). With a pin, hash the descriptor.
4. Fork the child.
5. Read stdout and stderr into buffers up to their caps.
   - It keeps DRAINING past a cap and discards the extra, so the child cannot block on a full
     pipe.
   - It records `*_truncated`.
6. At `timeout_ms` the broker sends `kill(-pgid, SIGKILL)`, reaps the child and sets
   `timed_out`. The child cannot have created processes (§5.3), so its process group is the
   whole job.
7. Reply `DONE {id, status, signal, ...}`, with a payload capped at
   `stdout_max + stderr_max + 64`.

The resume charges the output 1 unit per 8 bytes and builds the result table. stdout and stderr
are byte strings (a Lua string; in JS a string of byte code units, as `hull/ssh` does, plus
`stdoutBytes` as an ArrayBuffer).

### 5.3 The child, between fork and exec

The child runs these steps in order. Each is async-signal-safe and allocates nothing.

1. `prctl(PR_SET_PDEATHSIG, SIGKILL)`, then `if (getppid() != broker_pid) _exit(127)`. That
   closes the race where the broker died before the prctl. The broker is single-threaded, so
   the "death of the thread that forked" quirk of PDEATHSIG does not apply.

   The setting must still be in force while the PROGRAM runs, after `execve`, since that is
   where the guarantee is relied on. The kernel clears it in four cases, and each is closed:

   | Clears PDEATHSIG | Closed by |
   |---|---|
   | a `fork` (the new process does not inherit it) | step 6 refuses process creation |
   | exec of a set-user-ID or set-group-ID file | §6.1 refuses such a file |
   | exec of a file with file capabilities (`security.capability`) | §6.1 refuses such a file |
   | a credential change (effective / filesystem UID or GID) | step 6 refuses the `set*id`, `setgroups` and `capset` calls |

   A program could also clear or replace the setting itself with `prctl(PR_SET_PDEATHSIG, ...)`;
   step 6 refuses that `prctl` option (a filter can read it).
2. `setsid()` gives the child its own process group, so the timeout kill reaches the whole
   job with one `kill(-pgid, ...)`.
3. Set up stdio:
   - `dup2` of `/dev/null` (opened by the broker before lock-down) onto fd 0;
   - the pipes onto fds 1 and 2;
   - the binary's descriptor moved to fd 3;
   - `close_range(4, ~0)`.
4. `chdir("/")` and `umask(077)`.
5. Set rlimits:
   - `CORE` 0;
   - `CPU` set to the timeout plus 1 s;
   - `NOFILE` 256;
   - `FSIZE` 0 unless the command has `fs.write`.

   `NPROC` is NOT lowered: it counts every process of the user, and `nvidia-smi` and
   `systemctl` both use threads.
6. Apply the command's own Landlock domain (§6.3), then the child's seccomp filter, which
   stacks on the broker's (§6.2) and only removes:
   - **process creation:** `fork`, `vfork`, `clone` without `CLONE_THREAD`, and `clone3`
     (answered `ENOSYS`: its flags are in memory a filter cannot read, and glibc then falls
     back to `clone`, whose flags it can). Threads still work, which both motivating tools
     use;
   - `socket` for every domain the command does not need. `AF_UNIX` stays only for a command
     declared `unix_sockets = true` (`unit-state`); none gets `AF_INET` / `AF_INET6` /
     `AF_NETLINK`;
   - anything that would clear or replace PDEATHSIG (step 1): `setuid`, `setgid`,
     `setreuid`, `setregid`, `setresuid`, `setresgid`, `setfsuid`, `setfsgid`, `setgroups`,
     `capset`, and `prctl(PR_SET_PDEATHSIG, ...)`. An unprivileged status tool has no use for
     them; they fail with `EPERM` as they would for any unprivileged caller;
   - the broker-only calls it no longer needs (`kill` of other pids, `wait4`, the Landlock
     calls).

   `PR_SET_NO_NEW_PRIVS` is already set from the broker, so a setuid binary gains nothing; a
   setuid binary is refused anyway (§6.1).

   **Open question for PR 2:** whether `nvidia-smi` and `systemctl show` really create no
   processes in their query paths. `--no-pager` keeps `systemctl` from spawning a pager, and
   `show` asks no polkit agent. If either turns out to need a subprocess, process creation is
   NOT simply allowed: a grandchild escapes PDEATHSIG and an abruptly killed broker cannot
   sweep it, so the design must first name who owns and terminates the whole job after broker
   death (a cgroup the service manager kills, for instance). Until then, a command that needs
   to fork is out of scope.
7. `execveat(3, "", argv, envp, AT_EMPTY_PATH)`. The filter must allow `execveat` for this
   call, so the program could exec again afterwards; its Landlock domain admits only its own
   binary and loader, so that reaches nothing new. The environment is only
   `PATH=/usr/bin:/bin`, `LANG=C`, `LC_ALL=C` and the command's literal `env`.

   `argv[0]` is the resolved path. No shell is involved anywhere: the arguments reach the
   program exactly as matched.

## 6. Kernel enforcement

### 6.1 The binary check (broker, at policy load and on every call)

These checks are mandatory. A `sha256` pin is added on top of them, never instead of them.

- The leaf is a regular file, executable, not setuid or setgid, and carries no file
  capabilities (no `security.capability` extended attribute, read with `fgetxattr` on the
  descriptor). Both kinds would raise privilege without NNP, and an exec of either clears
  PDEATHSIG (§5.3).
- Its first bytes are the host's native executable magic (ELF on Linux and OpenBSD), so a
  `#!` script is refused. A script would bring its interpreter, and the Landlock grant would
  have to include `bash`.
- Every component of the real path is owned by root and is not group- or world-writable (the
  sshd `safe_path` rule), so only root can replace it or write to it.
- At policy load the broker records the binary's `(st_dev, st_ino)`.
- On every call the broker opens it with `O_RDONLY | O_NOFOLLOW | O_CLOEXEC`, `fstat`s the
  descriptor, re-checks, compares `(st_dev, st_ino)` with the recorded pair, optionally
  hashes, and execs THAT descriptor. Nothing is done by path between check and exec.

What each part proves:

| Check | Proves | Does not prove |
|---|---|---|
| `execveat` on the checked fd | the file that ran is the file that was checked | that its content did not change; that the loader and libraries are what they were |
| `(st_dev, st_ino)` | the path still names the inode found at load; a replaced binary is detected | anything about content |
| ownership and modes | only root can replace or rewrite the binary or any directory above it | that root did not |
| `sha256` pin | the main executable's bytes, read from the exec'd descriptor, match the pin | the loader, shared libraries, or a write by root between the hash and the exec |

The pin is therefore a check on the **main executable** across restarts and package
upgrades. It does not authenticate the program as a whole.

### 6.2 The broker's domain (Linux)

The broker restricts itself, after the policy is checked, with:

- **`EXECUTE`** on exactly each declared binary and each distinct `PT_INTERP`. The ELF loader
  is opened by `open_exec` with `FMODE_EXEC`, so Landlock checks execute on it too. Shared
  libraries are `mmap`ed by the loader, which Landlock does not mediate, so they need only
  read.
- **`READ_FILE` / `READ_DIR`** on the union of what the children need: the default set
  (`/usr`, `/lib`, `/lib64`, `/etc`, `/proc`, `/sys`, `/dev`) plus every command's `fs.read`.
- **`WRITE_FILE`** on `/dev/null` plus every command's `fs.write`. **`IOCTL_DEV`** (ABI 5) is
  handled and granted only on the `fs.write` device paths. `nvidia-smi` needs `ioctl` on
  `/dev/nvidia*`.
- **TCP** bind and connect (ABI 4) handled with no grant, as defence in depth behind the
  seccomp socket rule below. Pathname unix sockets are not mediated by Landlock, which is
  what `systemctl` needs for `/run/systemd/private` or D-Bus.
- **Scopes** (ABI 6): `LANDLOCK_SCOPE_SIGNAL` and `LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET`, so a
  child can signal and reach abstract sockets only inside its own domain.

**Broker seccomp: the inherited filter.** A seccomp filter is inherited across fork and
`execve`, and a filter added later can only restrict further; it can never re-allow what an
earlier filter refused. So the filter the broker installs on itself is the CEILING for every
child, and it must admit the syscalls the broker needs **and** every syscall a supported
command needs. It refuses:

- `socket` for any family but `AF_UNIX` (a filter can read the `domain` argument), so no
  child ever opens a network socket;
- `ptrace`, `process_vm_*`, `io_uring_*` (`IORING_OP_SOCKET` bypasses the `socket` check),
  `bpf`, `userfaultfd`, `perf_event_open`, `kexec_*`, `mount` / `umount2` / `pivot_root`,
  `unshare` and `clone` / `setns` with namespace flags, `keyctl` / `add_key` /
  `request_key`, and foreign ABIs.

Everything else a status tool uses (threads, `mmap`, `ioctl`, `AF_UNIX` `connect`, ...) stays
allowed in this filter, and Landlock and the child's own filter (§5.3) narrow per command. The
broker itself is a small fixed program, so the fact that its own filter is wider than it needs
is bounded by its code, not by the filter. An earlier draft had the broker refuse `socket`
outright and assumed the child's filter could widen it; that would have broken `systemctl`,
which must connect to systemd.

PR 2's acceptance test therefore includes a real `systemctl show` against a running system
manager, under the shipping filter (§8). `/bin/true` and `/bin/echo` never open a socket and
would not catch a filter that is too narrow.

**Driver and package updates.** A Landlock rule is bound to the inode that was open when the
rule was added. A package upgrade that renames a new `nvidia-smi` over the old one gives it a
new inode, which has no `EXECUTE` right. A domain can only narrow, and the app cannot respawn
the broker (it cannot spawn). The broker detects the replacement by `(st_dev, st_ino)` (§6.1)
before trying to exec. Under the broker's own domain the replacement cannot even be opened
(its read right went with the old inode too), so an `EACCES` on the open is checked with `stat`,
which Landlock does not mediate. Either way the call fails coded as:

```
exec: /usr/bin/nvidia-smi was replaced since the app started; restart the app (ESTALE)
```

This is deliberate: the replacement is a new binary nobody has checked under this process,
and a restart re-runs the policy load. It says nothing about whether the new binary then works
(a userspace tool upgraded ahead of its kernel module may still fail until the module is
reloaded, which a Hull restart does not do).

**Missing Landlock, or a missing required right.** See §6.5.

### 6.3 Each child's domain

Before `execveat`, the child adds a second Landlock domain that narrows the broker's to this
command alone:

- execute on its binary and loader;
- read on the default set plus its own `fs.read`;
- write on `/dev/null` plus its own `fs.write`;
- **`IOCTL_DEV` handled in this layer too** (ABI 5), granted only on its own `fs.write`
  device paths. Landlock restricts only the rights a ruleset handles, so a child ruleset that
  left `IOCTL_DEV` out would inherit the broker's union of device ioctls unchanged;
- the same TCP and scope restrictions as the broker's domain.

Without this, every child could reach what the union grants. In particular `unit-state` could
issue ioctls on `/dev/nvidia*`, and `gpu-query` could write where `unit-state` may.

The bound that matters most is reads. A child runs as the app's user and reads whatever its
domain allows, so the default read set leaves out `/home`, `/root`, `/srv`, `/var` and the app
directory. A command that legitimately needs one of them names it in `fs.read`.

### 6.4 OpenBSD and macOS (planned)

Neither backend is designed in detail or demonstrated yet; PR 3 does that. Known so far:

- **OpenBSD:**
  - The broker calls `unveil(bin, "x")` and `unveil(interp, "x")` for each binary, plus `r` for
    the read set, then `unveil(NULL, NULL)`.
  - It pledges `stdio proc exec` with `execpromises` set to `stdio rpath wpath tty`, plus
    `inet` only if a later version ever needs it.
  - `execpromises` constrains the child itself, which Linux cannot express.
  - The child-side narrowing of §6.3 is not available. The union applies.
- **macOS:**
  - The broker applies a Seatbelt profile before `READY`. It is deny-default, with
    `process-fork` allowed, `(allow process-exec (literal "<bin>"))` for each binary,
    `file-read*` on the read set, and `file-write*` only on `/dev/null` and `fs.write`.
  - Children inherit it. There is no per-child narrowing (`sandbox_init` once per process), so
    the union applies.
  - There is no `execveat` either, so the check-then-exec window is the `safe_path` ownership
    rule's job: only root can swap a file under a root-owned path.
  - There is no `PR_SET_PDEATHSIG`, and nothing that refuses a child's `fork`, so the
    abrupt-death guarantee of §7 does not carry over as it stands. PR 3 must close that or
    say so in the platform table.
  - The binary check takes Mach-O (thin or fat), not ELF, and the loader is `dyld`.

### 6.5 Required rights (Linux)

"Landlock is present" is not enough: Landlock restricts only the rights a ruleset handles, and
each right arrived in a later ABI. Every guarantee this design advertises maps to what it
needs, and the broker probes `landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)`
and checks them all BEFORE it sends `READY`:

| Guarantee | Needs | ABI |
|---|---|---|
| exec only of the declared binaries and loaders | `EXECUTE` | 1 |
| child reads and writes only its declared paths | `READ_FILE` / `READ_DIR` / `WRITE_FILE` / `MAKE_*` / `REMOVE_*` | 1 |
| no rename or link out of a granted tree | `REFER` | 2 |
| no truncation outside `fs.write` | `TRUNCATE` | 3 |
| child issues device ioctls only on its own devices | `IOCTL_DEV` | 5 |
| child signals and reaches abstract sockets only within its domain | scopes `SIGNAL` / `ABSTRACT_UNIX_SOCKET` | 6 |
| no network | seccomp `socket` family rule (§6.2); TCP rules as defence in depth | 4 (TCP) |

The supported minimum is therefore **ABI 6**. Any required right missing, or Landlock absent,
is handled exactly like an incomplete backend: `exec` fails app start with a hint, unless
`--allow-degraded-sandbox` is given. Under that flag the startup log names each guarantee that
is not held, one line per row (`exec: degraded: no IOCTL_DEV (ABI 5): device ioctls not
narrowed per command`), and `hull agent overview` reports the same list. A missing TCP right
alone is not degraded, since seccomp holds that guarantee.

## 7. Lifetime and teardown

The guarantee: **no process a declared command started outlives the broker.** It rests on
two facts together:

- every child has `PR_SET_PDEATHSIG(SIGKILL)` against the broker (§5.3), which the kernel
  delivers however the broker ends, `SIGKILL` included, and which stays in force through and
  after `execve` because every way of clearing it is closed (the table in §5.3 step 1);
- no child can create a process (§5.3), so there is no grandchild that PDEATHSIG would miss.

Nothing depends on the broker running cleanup code, because a broker killed with `SIGKILL`
runs none. (An earlier draft relied on the broker sweeping a subreaper's orphans on exit; a
broker killed abruptly cannot do that, and a child's fork clears PDEATHSIG in the grandchild,
so that version did not hold.)

The broker in turn dies with the app in three ways:

- `PR_SET_PDEATHSIG(SIGKILL)` on Linux, set in the broker right after spawn, with the same
  `getppid` check. The broker is spawned from the main thread before any other thread
  exists, and the main thread lives as long as the process, so the per-thread trigger of
  PDEATHSIG fires only when the app exits;
- EOF on the channel, everywhere;
- `hl_exec_broker_stop()` in teardown.

On EOF or a stop the broker also SIGKILLs every live child's process group before it exits;
on its own abrupt death the children's PDEATHSIG does the same. Teardown order: the broker
stops in step (1), next to the thread pool. Each in-flight op's cancel_fn releases its
continuation, as `hl_async_ctx_drop_unrun` does today.

`--vm-thread`: the channel is watched by the loop thread, and the result is delivered as an
async resume (an offloaded entry), so nothing new crosses threads.

## 8. Tests and development mode

- **`hull app.lua` / `hull dev`:** the broker runs, exactly as in a built binary. Development
  mode has the policy from the app source.
- **`hull test` / `hull agent`:** no broker. Those entry points have no kernel sandbox to keep
  exec out of, and a test suite that runs `systemctl` on a CI runner is a flaky test.
  - Instead `test.exec_stub(name, result_or_fn)` (JS `test.execStub`) registers a reply.
  - `exec.run` still runs the full shape match (§4) before consulting the stub, so a test
    catches a bad argument.
  - With no stub registered, the call raises `exec: no stub for 'gpu-query' in hull test`.
- **e2e** (`tests/e2e_exec.sh`, both runtimes, Linux sandboxed):
  - a declared `/usr/bin/true`, a fixed-argv `/bin/echo`, and a slot command;
  - a purpose-built fixture (`tests/fixtures/exec_probe.c`, built root-owned into a
    root-owned directory by the test) that tries `fork`, `vfork`, `clone3`, an `AF_INET`
    socket and a second thread, and reports which succeeded: process creation and the
    socket must fail, the thread must work;
  - the timeout kills the fixture in its sleep mode;
  - output truncation;
  - option injection refused;
  - an undeclared name;
  - a second POLICY frame from a test-only hook ends the broker;
  - the app process itself still gets `EPERM` / SIGSYS on `execve`, the existing sandbox test
    extended;
  - a `#!` script refused;
  - a replaced binary answering `ESTALE`;
  - the fixture also tries each call that would clear PDEATHSIG (`prctl(PR_SET_PDEATHSIG, 0)`,
    `setresuid` to its own uid, `setgroups`, `capset`) and must see each refused;
  - a binary with file capabilities (`setcap` on a fixture copy) is refused like a setuid one;
  - **orderly EOF and abrupt death are tested separately**, both at the point hsctl relies
    on, after exec. The fixture, once running as the exec'd program, reads its own setting
    with `prctl(PR_GET_PDEATHSIG)`, requires `SIGKILL`, and only then writes a readiness line.
    The test waits for that line, then either closes the channel (teardown with a call in
    flight) or sends the broker `kill -9`, and checks that the fixture's pid is gone within a
    bound. `exec.run` buffers stdout until `DONE`, so the harness cannot see the line through
    it: in a build with `-DHL_EXEC_TEST_HOOKS` only (the GPU helper's pattern), the broker
    sends a `PROBE_READY {id, pid}` frame as soon as it reads the line from the child's
    stdout, and a test hook surfaces it while the call is still running. A release build
    neither sends nor accepts the frame;
  - **broker-side admission:** a test-only hook that writes raw `RUN` frames past the
    binding's caps (duplicate ids, more than `HL_EXEC_MAX_INFLIGHT`, past a command's
    `concurrency`, and replies left unread until `HL_EXEC_MAX_BUFFERED`) gets `BUSY`, and
    nothing is forked for them.
- **PR 2 acceptance on a real host** (not CI-only, recorded in the PR), using the §3 manifest
  exactly as written, `unix_sockets = true` included:
  - `unit-state` against a known running `hs-*.service`, through a real `systemctl show`
    against the running system manager, under the shipping filter and domain;
  - `gpu-query` on an NVIDIA node, run under the service account hsctl will use.

  hsctl adopts `exec` only after both pass, alongside the abrupt-death and admission tests.
- **Unit:** `test_exec` covers the slot matcher (every rule, boundary lengths, `repeat`), the
  policy codec over untrusted bytes, and the ELF / `PT_INTERP` reader.
- **Fuzz:** `fuzz_exec_policy` (frames to the broker parser) and `fuzz_exec_slot`.

## 9. Platforms

"Demonstrated" means the e2e and the acceptance runs of §8 pass on that host. Until then a
row's guarantees are a plan, not a claim.

| Host | Status | Kernel layer | Guarantees demonstrated |
|---|---|---|---|
| Linux, native, Landlock ABI 6+ | PR 2 | Landlock + seccomp (raw syscalls) | every row of §6.5: `tests/e2e_exec_broker.sh` on x86_64 (ABI 10) and aarch64 (ABI 8); the §8 acceptance runs pending |
| Linux, cosmo APE | planned | as native, once the APE build carries the broker | none; refused at start (no broker) unless `--allow-degraded-sandbox` |
| Linux, Landlock below ABI 6 | PR 2 | as above, minus the missing rights | refused unless `--allow-degraded-sandbox`, which names what is missing |
| OpenBSD | **planned** (PR 3) | unveil `x` + pledge `execpromises` | none; union of grants, no per-child narrowing |
| macOS | **planned** (PR 3) | Seatbelt `process-exec literal` | none; union of grants, no `execveat`, no PDEATHSIG |
| Windows (cosmo APE) | not supported | none | **refused at app load**, even with `--allow-degraded-sandbox` |
| FreeBSD / NetBSD | not supported | none (no kernel sandbox Hull uses) | refused unless `--allow-degraded-sandbox` |

Windows is refused outright, not degraded, because the shape guarantee itself does not survive
there. `CreateProcess` passes ONE command-line string, and each program splits it with its own
rules (`CommandLineToArgvW`, the MSVC CRT, or neither). A slot value that matched as a single
argument can therefore become several. No sandbox setting fixes that.

## 10. Where it sits in the taxonomy

By CLAUDE.md's decision procedure, `exec` adds a new authority. Rule 4 says that is normally a
`--with` feature. It is a base cap module here, behind a manifest gate:

- The broker is a hidden mode of the runner (`argv[1]` taken first thing in `hull_serve`), and
  it must be spawned before the app loads. A feature archive composed at `hull build` cannot
  add that to a binary that was never built with it, and a dev-mode `hull app.lua` composes
  nothing.
- It is small: no vendored engine, about 400 lines for the broker and 600 for the app side.
- The authority is off unless the manifest declares commands. An app with no `exec` section
  stops the broker before it serves, and a module set without `hull/exec` cannot reach the
  binding. That mirrors how `tui = true` + `hull/tui` gate the terminal.

## 11. Invariants (linted)

The new "must" statements this design adds, each with a rule in `scripts/check_invariants.py`
and fixtures in the same PR:

- **INV-16:**
  - `fork`, `vfork`, `execve*`, `posix_spawn*` and `fexecve` appear only in the files that
    already spawn: `cap/tool.c`, `compiler.c`, `sandbox_tool.c`, `shared/child_channel.c`,
    `shared/generation.c`, `commands/dev.c` and `cap/gpu_helper.c`, plus `cap/exec_broker.c`.
  - Any other spawn site needs an allowlist entry with a reason.
- **INV-17:** the slot matcher is called from one place on each side: `hl_exec_slot_match` in
  the binding path and in the broker. Neither side has a second matcher.
- **CLAUDE.md:** gets one paragraph under "Security": the broker, the one-POLICY rule, the
  no-kernel-mechanism refusal, and that a new spawn site goes through INV-16.

## 12. Audit and observability

`--audit` gets one record per call: command name, argument count, each argument escaped
(`hl_log_escape_line`), status / signal, `timed_out`, duration and output byte counts.
stdout / stderr are never logged. Startup logs one line per command with its resolved path,
and `exec: degraded` when it applies.

## 13. Phasing

1. **PR 1:**
   - the slot matcher, the manifest section (both extractors), the policy codec, `test_exec`
     and the fuzzers;
   - no broker yet: `exec.run` refuses on every host except under `hull test` stubs;
   - a declared `sha256` refuses app load (§3), so no signed manifest carries a pin nothing
     checks.

   This lands the API and its types (`types/hull` drift check).
2. **PR 2:** the Linux broker (Landlock with the §6.5 rights check, the inherited and
   per-child seccomp filters, PDEATHSIG, broker-side admission, `execveat`), `e2e_exec.sh`,
   INV-16 / 17 and the CLAUDE.md paragraph. Done when the §8 acceptance runs pass; hsctl
   adopts it then, not before.
3. **PR 3:** macOS Seatbelt, then OpenBSD.
4. **PR 4:** the optional `sha256` pin (the broker verifies it, and only then does the
   manifest accept the field), and the `hull build` lint.

## 14. What this does not cover

- **What a declared binary does.** A shape bounds the arguments, not the program's behaviour.
  A command whose slot is a path, an expression or a host (`git -C`, `tar --to-command`,
  `ssh <host>`, `curl <url>`) remains as dangerous as that program. The lint warns about the
  known ones, and the operator reads `hull inspect`.
- **Output trust.** stdout is the tool's text, often shaped by things the app does not
  control, such as unit names other users created. It is data for the app to parse, and is
  charged and capped, nothing more.
- **Privilege.** Nothing here can raise privilege: NNP plus the setuid refusal. A command that
  needs root (`systemctl restart`) is not in reach by design. Changing system state belongs to
  a separate root-owned agent the app talks to over a socket, not to this.
