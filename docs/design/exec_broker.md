# Design: declared commands through a spawn broker (`hull/exec`)

- **Status:** PR 1 (#1210: manifest section, matcher, stubs), PR 2 (#1211: the native Linux
  broker), PR 3 (the host backends split behind `hull/cap/exec_platform.h`, and the macOS
  Seatbelt backend) and PR 4 (#1212: the `sha256` pin and the `hull build` / `hull check`
  lint) implemented. OpenBSD is not supported (§6.4, §9). Review rounds 1-3
  folded in: seccomp inheritance, child process creation, broker-side admission, required
  Landlock rights, file identity vs content, `unix_sockets` scope, `sha256` until enforced.
  Released in v0.26.0. A command's own manifest (§3.1, #1217) implemented after v0.26.0.
  §15 is the design review of per-executable authority that led to §3.1; its remaining
  proposals (Hull callees, nested calls) are not implemented.
- **Motivation:** hsctl needs to read node state that only a system tool reports: `nvidia-smi`
  for GPUs, `systemctl show` for its own `hs-*` units. Today an app cannot run any program: the
  app sandbox has no `exec` / `proc` / `fork`, and `hl_tool_spawn` exists only in tool mode.
- **Builds on:** the GPU compile helper (`docs/design/gpu_compile_helper.md`,
  `src/hull/cap/gpu_helper.c`): a Hull-binary helper in a hidden `argv[1]` mode, spawned before
  the phase-1 sandbox and stopped once the manifest shows it is not needed. Also builds on
  `include/hull/shared/child_channel.h`, the framed-socket channel the helper and the build
  plugin runner already share.
- **Code:** `src/hull/cap/exec.c` (the policy, the shared argv matcher, the app side),
  `src/hull/cap/exec_broker.c` (the broker's portable core: frames, admission, jobs, the
  owner / mode / identity / pin checks, the fork, the child's common setup, the server's
  side), the host backends behind `include/hull/cap/exec_platform.h`
  (`src/hull/cap/exec_broker_linux.c`, `src/hull/cap/exec_broker_macos.c`, each with its own
  unit test, `tests/hull/cap/test_exec_broker_{linux,macos}.c`), `include/hull/cap/exec.h`,
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
| Manifest | An `exec` section: name -> `{ path, sha256, argv, host, manifest, timeout_ms, stdout_max, stderr_max, env, concurrency, stdin_max }`, plus `"hull/exec@1"` in `modules`. Both are needed, as for `tui`. `manifest` is the command's own manifest, a strict subset of the app manifest (§3.1); `host` holds the grants an app manifest has no field for. |
| Slot shapes | **Structured, not regex** (§4): `prefix` / `suffix` / `chars` / `min` / `max` / `enum` / `repeat`. One C matcher is used by both the cap layer and the broker. |
| Process | Hull's own binary in a hidden mode (`argv[1] == "__hull_exec_broker"`), started with `hl_chan_spawn` before phase 1 and stopped after the manifest if there is no `exec` section. This is the GPU helper's lifecycle. |
| Policy delivery | Exactly ONE `POLICY` frame, built in C from the sealed `HlManifest` after `hl_sig_check_runtime_policy`. The broker then locks itself down; a second `POLICY` frame makes it exit. |
| Child | Forked by the broker. It gets `setsid`, closed fds, stdin from `/dev/null` (or a pipe the broker fills with the call's `stdin`, when the command declares `stdin_max`), capped stdout / stderr pipes, a fixed environment, rlimits, `PR_SET_PDEATHSIG`, its own narrower Landlock domain, and a seccomp filter that **refuses process creation** (threads allowed). It runs the binary through `execveat(fd, "", ..., AT_EMPTY_PATH)` on the descriptor that was checked. |
| Seccomp | Filters are inherited and can only narrow, so the broker's filter admits the union of what the broker AND the supported commands need (`socket` for `AF_UNIX` only, for `systemctl`). Each child then adds a filter removing what its command does not need (§6.2). |
| Admission | **The broker is authoritative** for the in-flight cap, per-command concurrency, live request ids and aggregate buffered output. The binding checks the same caps only to answer `busy` fast (§5.2). |
| Kernel | Linux: Landlock `EXECUTE` on the declared binaries and their `PT_INTERP` only, with a defined minimum ABI per guarantee (§6.5). macOS: one Seatbelt profile, the union of the commands' grants (§6.4). OpenBSD and Windows: **not supported** (§9). |
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
      host = { read = { "/dev/nvidiactl", "/dev/nvidia0" }, write = { "/dev/nvidiactl", "/dev/nvidia0" } },
    },
    ["unit-state"] = {
      path = "/usr/bin/systemctl",
      argv = { "show", "--no-pager", "-p", "ActiveState,NRestarts,ActiveEnterTimestamp", "--",
               { prefix = "hs-", suffix = ".service", chars = "a-z0-9-", max = 64,
                 ["repeat"] = { min = 1, max = 32 } } },
      host = { unix_sockets = true },   -- systemctl connects to systemd over a pathname socket
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
- **`host`:** the grants an app manifest has no field for. `read` / `write`: extra Landlock
  grants for this command (§6.3), each absolute, `write` only under `/dev`, `/run` or `/tmp`
  (device ioctls included). `unix_sockets`: below. Through §5-§9, a command's `fs.read`,
  `fs.write` and `unix_sockets` mean these (`host.read`, `host.write`, `host.unix_sockets`; the
  v0.26.0 names), with the `manifest.fs` grants of §3.1 added to the same rules.
- **`manifest`:** the command's own manifest (§3.1).
- **`sha256`:** 64 hex characters (either case), optional. The broker hashes the main
  executable from the descriptor it will exec, at policy load (a mismatch refuses the start)
  and on every call (a mismatch refuses the call, naming the digest it found). A pinned binary
  is hashed whole, so one over 512 MiB is refused. Until PR 4 a declared pin refused app load,
  so no signed manifest ever advertised a pin nothing checked.
- **`host.unix_sockets`:** boolean, default false. Lets the child create `AF_UNIX` sockets (§5.3);
  `systemctl` needs it to reach systemd. No other socket family is ever allowed.
- **`stdin_max`:** 1..1 MiB (`HL_EXEC_STDIN_CEIL`), optional. A call may then pass `stdin`
  (`exec.run(name, args, { stdin = bytes })`), up to that many bytes, written to the child's
  stdin. Without it the child reads `/dev/null`, and a call that passes stdin is refused. It is
  the channel for secrets and records: an argument shows in the process listing and in the
  audit record (§12), stdin in neither. Encoded as a command flag (`HL_EXEC_CMD_STDIN`) and a
  u32 after the pin.

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

`hull build` / `hull check` add a lint, a WARN rather than a boundary
(`stdlib/cli/lua/hull/exec_lint.lua`):

- a `path` whose basename is a general-purpose program (the list in §1, plus interpreters,
  editors, pagers, fetchers and container CLIs; a versioned name such as `python3.12` counts as
  its family);
- a slot with no `chars` or `enum`;
- a slot with `allow_dash`;
- a `path` outside `/usr`, `/bin`, `/sbin` and `/opt`;
- a non-`enum` slot after fixed arguments with no `--` before it (once per command).

### 3.1 The command's own manifest (`exec.<name>.manifest`)

The contract: each command entry has a nested `manifest`, and its value is a **strict subset
of the app manifest schema**: the same field names, nesting, types and meaning, including path
resolution. The executable `path`, the `sha256` pin, the `argv` policy and `host` are siblings
of `manifest` on the entry, never inside it. So is the call contract (`timeout_ms`,
`stdout_max`, `stderr_max`, `concurrency`, `stdin_max`) and `env`: an app's `env` is an
allowlist of variable names it may read, a command's is a map of literal values it is given,
so the same name with another type and meaning could not go in the subset.

**What the subset takes: `fs` only.** Derived from the app schema by asking, for each field,
whether the broker can enforce it on an exec'd child:

| App field | In a command's manifest | Why |
|---|---|---|
| `fs.read` / `fs.write` | yes, same syntax and meaning (app root or `@state/`, `hl_fs_grant_parse`) | Landlock / Seatbelt grant files and directories |
| `hosts`, `http`, `ssh`, `databases`, `kv` | refused | the broker's seccomp ceiling refuses every socket family but `AF_UNIX` for every child (§6.2), and no kernel rule names a host |
| `env`, `secrets` | refused | a child gets only literal values (§3); no secret reaches one |
| `state`, `modules`, `exec`, `tui`, `gpu`, `compute`, `wasm`, `limits`, `csp`, `cors`, `allow_dynamic_*` | refused | properties of a Hull runtime, which a native child is not |

A refusal names the command and field: an app field
(`exec.report.manifest.hosts: an app capability a command cannot be given`), an entry field
(`exec.report.manifest.sha256: not a manifest field; it belongs on the command entry
(exec.report.sha256)`), or `unknown key`. The rules live in `cap/exec_child.c`, which both
extractors call, so Lua and JS refuse with the same words.

**Values: valid for an app, refused for a command.** An app's own kernel grant is WIDER than
its `fs` grant, on purpose: `hl_sandbox_resolve_grant` grants a file write's parent directory,
a pattern's literal directory, and `.` / `@state` as the whole root (`.hull` included), and the
capability layer narrows each call back to the grant in process. A command has no capability
layer: its kernel grant is the whole boundary. Reusing the app's kernel resolution would break
an existing guarantee, for example:

- `fs.read = { "." }` gives an app every file but `.hull` (audit 37: the internal database,
  the dev-session sidecars). A command given the same grant through Landlock would read
  `.hull/internal.db` (sessions, TOTP secrets): Landlock can only add access, so the
  subdirectory cannot be taken back out;
- `fs.write = { "out/report.json" }` lets an app write that file; the app's kernel grant is
  all of `out/`, so a command given it would write every file there.

So `hl_exec_child_grant_check` refuses `.` and `@state` (the whole root) and any pattern, and
the broker resolves each entry to EXACTLY the file or directory it names
(`hl_exec_child_grant_resolve`, at policy load: each component `lstat`ed, no symbolic link, a
regular file or a directory). Such a path must exist when the app starts, except one ending in
`/`, a directory the app's sandbox would create, which is created. A read grant gives read; a
write grant gives write, truncate and create (files and directories) but not read, as an app's
`fs.write` does not imply `fs.read` (`hl_fs_policy_select`). No device ioctl: that is a `host`
grant. The rules below were added after an independent review of the first version.

**Independent authority, not delegation.** A command's `manifest.fs` grant is not required to
lie inside the app's own `fs` grants: a command may be granted `out/` though the app holds no
`fs.write` there. "Subset" describes the schema, not the authority. This is the model the
host grants already had (a command reaches `/dev/nvidiactl`, which the app process never
can), and it is the least-privilege one: requiring containment would make the app hold every
grant its commands need. Both are declared by the same author in the same signed manifest
and fixed before the app serves any input (§5.1). (A future Hull-built callee that declares
its own authority is the case for an intersection, the caller capping what the callee may
take; not this section.)

**What the broker refuses at policy load, beyond the parser** (`hl_exec_child_grant_open`):

- an app root that is `/` or the home (a built tool run from there, whose app directory is
  its working directory): the grant would name the user's own files. A tool run from anywhere
  uses `@state/`; `hl_sandbox_manifest_rootless_ok` counts a non-`@state/` `manifest.fs` entry
  as disqualifying. Judged by the SERVER (`hl_exec_broker_apply`, with its own `$HOME`; the
  broker's environment holds only `LANG` and `TZ`) before the broker creates anything;
- any component that IS either root's `.hull`, compared by inode, and, directly under a root,
  with that root's `.hull` as it is right after any `mkdirat` (on HFS+ a name with an ignorable
  code point is `.hull` itself, and may not exist yet at load; a directory made that turns out
  to be it is removed). Also a directory that is either root or an ancestor of one (a state
  directory inside the app directory, or the reverse). That check walks up from the root by
  `..` comparing inodes; a walk it cannot finish (a directory it cannot search, no descriptor
  left, past 256 levels), or a root that does not exist, refuses the grant rather than reading
  as "not contained" (audit 53);
- a grant holding a control character (it would be quoted in the broker's report and the
  start error);
- for a write grant, any database file the app's registry would open by name - the default
  (`-d` or the manifest's), `databases.named`, the internal database - or its sidecars
  (SQLite's `-wal` / `-shm` / `-journal`; DuckDB's `.wal`, `.wal.checkpoint`,
  `.wal.recovery` and `.tmp` directory), or a directory holding one: truncating a live `-shm`
  kills the app with SIGBUS, which the app's own fs layer refuses (`database_file_in_use`) and
  a command has no layer to refuse. The server resolves each to its real path
  (`hl_db_registry_file_paths`: a symbolic link followed, a relative `-d` against its own
  working directory, and a file whose directory is not made yet - a first start - as its
  nearest existing ancestor's real path plus the rest), and the broker compares by inode and,
  for a directory grant, also by path (the database's directory may not exist yet); a database
  directory that exists but cannot be walked refuses the grant. A path that cannot be resolved
  refuses the start, so does a file database whose DSN the registry refuses (a planted
  symbolic link where its file or a sidecar should be: it could not be protected, and a later
  `db.connect` would open it once the link is gone; an unset `"$VAR"` names no file), and so
  does an app with more than
  `HL_EXEC_MAX_PROTECT` (16) database files when a command has a `manifest.fs` write grant.
  Not covered: a `db.open` file (its DSN is chosen at run time), a hard link
  to a database file planted under another name inside a granted directory after start (a
  command cannot make one; another local principal with write access to the tree could), and a
  `host.write` path under `/tmp` naming a database there (the operator's absolute path, as in
  v0.26.0).

**No window between resolving and granting.** The walk is by descriptor from the root: each
component `fstatat(AT_SYMLINK_NOFOLLOW)`, opened `O_NOFOLLOW`, and the opened object compared
with the one checked; a directory made for a trailing-`/` grant (`mkdirat`) is checked again
the same way, `EEXIST` included. On Linux the Landlock rule is made from the final descriptor
(`O_PATH`), so nothing is looked up by path again; on macOS the profile takes the path from
`F_GETPATH` on that descriptor, never a second `realpath`. A component swapped for a symbolic
link between the check and the rule cannot redirect the grant.

**What an exact grant binds.** Linux binds the inode of the descriptor opened at load: a
rename moves the grant with the object, and whatever is put at the path afterwards is not
covered. **On macOS a command's grant is a path, not an object.** Seatbelt matches the
resolved path at each open; the broker opens each grant without following links and records
its real path (`F_GETPATH`), so a replacement during the walk is refused, but from then on, for
the broker's whole life, whatever directory or file is renamed into that path is covered, and
the original is not once it moves away. A symbolic link put at the path gains nothing
(Seatbelt matches the resolved target). Doing that needs write access to the directory above
the grant, i.e. to the app or state tree, and the same limit already applies to the `host`
grants and to the app's own Seatbelt profile. On Linux a grant covers its inode, so a hard
link to a granted file at another pathname is reachable too; on macOS a grant on one pathname
does not by itself grant access through a hard link at another pathname. On both, a hard link
placed inside a granted directory is reachable through it (a child cannot make one: Linux grants no REFER, macOS refuses the link). A write
grant gives write, truncate and create, never read, delete or rename, on both (macOS
`file-write-data file-write-create`).

**Defaults.** An absent `manifest` (or `fs`) grants nothing, as an app with no `fs` reaches
no file. Every command also reads the system trees (`/usr /lib /lib64 /etc /proc /sys /dev` on
Linux, §6.2), which is the broker's, not the manifest's.

**What a grant means on each platform.** Linux: each command gets exactly its own `host` and
`manifest.fs` grants (§6.3). macOS: all commands of an app share ONE Seatbelt profile (§6.4),
so a command's grant is reachable by every command of the app; `hull inspect` and the start
report (`exec: limit:`) say so. The same text therefore grants one command a file on Linux
and every command that file on macOS.

**Before and after** (`gpu-query` as v0.26.0 published it, then in the new spelling with a
child manifest):

```lua
-- v0.26.0
["gpu-query"] = {
  path = "/usr/bin/nvidia-smi",
  argv = { "--format=csv,noheader" },
  fs = { read = { "/dev/nvidiactl" }, write = { "/dev/nvidiactl" } },
  unix_sockets = false,
},
-- now
["gpu-query"] = {
  path = "/usr/bin/nvidia-smi",
  sha256 = "<64 hex>",
  argv = { "--format=csv,noheader" },
  host = { read = { "/dev/nvidiactl" }, write = { "/dev/nvidiactl" } },
  manifest = { fs = { read = { "data/gpu-ids" } } },   -- as the app writes its own grant
},
```

**Compatibility.** A command with no `manifest` encodes to exactly the v0.26.0 bytes (a new
command flag, `HL_EXEC_CMD_APP_FS`, appends the child lists only when there are any; a golden
test holds the v0.26.0 encoding), so a v0.26.0 policy still means what it meant. The v0.26.0
spellings `fs` and `unix_sockets` on the entry were taken as `host = { read, write }` and
`host.unix_sockets` through 0.27.x, with one WARN at load. Since 0.28.0 they are refused at
load (JS `fs`, `unixSockets` and `unix_sockets` too), the error naming the command and the
replacement (`exec.<name>.fs: removed in 0.28.0, use exec.<name>.host`); write them as `host`.
A manifest using `manifest` or `host` is refused by v0.26.0 (an unknown key), which fails
closed.

**`hull inspect`** shows a command's whole authority: `host.read`, `host.write`,
`manifest.fs.read`, `manifest.fs.write`, the scope of `host.unix_sockets`, and the system trees
each platform adds.
`hull agent overview` lists the same four lists per command (`host_read`, `host_write`,
`manifest_fs_read`, `manifest_fs_write`).

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
     │  fork + execveat(checked fd)          (macOS: execve(checked path))
     ▼
 child  (setsid; own Landlock domain; NNP; rlimits; PDEATHSIG; no process creation)
        (macOS: its broker's Seatbelt profile, inherited; no process creation)
```

On macOS an app with two or more commands has one more level: the broker is a ROUTER that
starts one broker per command and forwards each call to it (§6.4,
[exec_broker_macos_isolation.md](exec_broker_macos_isolation.md)).

Everything that differs between kernels is behind one internal interface,
`hull/cap/exec_platform.h`, one translation unit per host: the broker's death-with-the-server
setup, the extra leaf check, the binary format check (ELF / Mach-O), the lock-down, the exit
watch (pidfd / kqueue), and the child's four steps (die with the broker, confine, the syscall
filter, the exec). `cap/exec_broker.c` holds everything else and is the same on every host. A
new backend is one more `exec_broker_<host>.c` and its unit test.

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
6. Send `RUN {id, name, args[], stdin}` and park (`stdin` already checked against the
   command's `stdin_max`). The loop watches the channel fd through the async
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
2. Match the call again (§4), and check its `stdin` against the command's `stdin_max`. A
   mismatch is a `REFUSED {id}` reply. The stdin counts toward the `HL_EXEC_MAX_BUFFERED`
   reservation with the output caps.
3. Open the binary and check it again (§6.1). With a pin, hash the descriptor.
4. Fork the child.
5. Write the call's stdin, if any, to the child's stdin pipe as it takes it (non-blocking,
   from the same loop), then close it. When the child exits or closes its stdin first, the
   rest is dropped (`EPIPE`; the broker ignores `SIGPIPE`).
6. Read stdout and stderr into buffers up to their caps.
   - It keeps DRAINING past a cap and discards the extra, so the child cannot block on a full
     pipe.
   - It records `*_truncated`.
7. At `timeout_ms` the broker sends `kill(-pgid, SIGKILL)`, reaps the child and sets
   `timed_out`. The child cannot have created processes (§5.3), so its process group is the
   whole job.
8. Reply `DONE {id, status, signal, ...}`, with a payload capped at
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
   - `dup2` of `/dev/null` (opened by the broker before lock-down) onto fd 0, or of the
     read end of the call's stdin pipe when it sent stdin;
   - the pipes onto fds 1 and 2;
   - the binary's descriptor moved to fd 3;
   - `close_range(4, ~0)`.
4. `chdir("/")` and `umask(077)`.
5. Set rlimits:
   - `CORE` 0;
   - `CPU` set to the timeout plus 1 s;
   - `NOFILE` 256;
   - `FSIZE` 0 unless the command has `host.write` or `manifest.fs.write`.

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
   - NOT the Landlock calls, `kill` or `wait4`. An earlier draft listed them; the shipped
     filter (`seccomp_child`, `cap/exec_broker_linux.c`) leaves them, and that is the better
     rule: a Landlock domain can only narrow, the ABI 6 signal scope already keeps `kill`
     inside the child's own domain, and a child that is itself a Hull binary needs the
     Landlock calls to apply its own sandbox (§15.4).

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
- Its first bytes are the host's native executable magic (ELF on Linux, a 64-bit Mach-O on macOS), so a
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

### 6.4 macOS (Seatbelt), and why not OpenBSD

**macOS** (`cap/exec_broker_macos.c`). Measured on macOS 26 (arm64):

- **One broker per command.** macOS refuses a second sandbox in a sandboxed process:
  `sandbox_init` fails with `EPERM`. So a child cannot narrow itself after fork as on Linux
  (§6.3), and whatever profile a broker applies, its children inherit. With two or more
  commands the broker becomes a ROUTER
  ([exec_broker_macos_isolation.md](exec_broker_macos_isolation.md)): at `POLICY` it checks
  every command as below, starts one broker per command (this binary again, a clean image),
  sends each the `POLICY` head and that command's entry alone (its original bytes behind a
  one-command header, `hl_exec_policy_slice`), waits for every `READY`, and only then locks
  itself with `(deny default)` and `(allow signal (target children))`: no exec, no fork, no
  file rule, only the descriptors it holds. It admits and matches each call, forwards it to
  the command's broker and relays the reply. An app with one command keeps one broker. Each
  broker applies ONE Seatbelt profile to itself before `READY`, the grants of its one command,
  and every child inherits it:
  - deny by default; `file-read-metadata`;
  - reads of `/` itself (dyld reads it to find the shared cache, and a child aborts at start
    without it), `/usr`, `/bin`, `/sbin`, `/System`, `/private/etc`, `/private/var/db/dyld`,
    `/Library/Preferences/Logging`, `/dev/urandom`, `/dev/random`, `/dev/zero`; executable
    maps of `/System` and `/usr/lib`; `/dev/null` read and write; `sysctl-read`; the
    `opendirectoryd.libinfo` lookup (getpwuid, so `id` and `ls -l` work);
  - per command: `process-exec`, `file-read*` and `file-map-executable` of its binary by its
    real path (a parameter, never text in the profile), `file-read*` of each `fs.read`,
    `file-read* file-write* file-ioctl` of each `host.write` (resolved: Seatbelt matches the
    real path; a path that does not exist is skipped, as on Linux), and `file-read*` /
    `file-write*` of each `manifest.fs` entry, resolved to the exact path as on Linux (§3.1);
  - `AF_UNIX` (`system-socket` of that domain, `network-outbound` to a unix socket) only when
    some command sets `host.unix_sockets`; no other network.

  So each command gets its own grants, its own `unix_sockets` and `process-exec` of its own
  binary only, as on Linux. Until the router (v0.27.0 and before) every command got the union
  of all the commands' grants and could exec every declared binary. Each call runs under a
  keeper, the job's parent, which ends it when the broker goes (§7); what is left is reported
  at start as `exec: limit: no parent-death signal on macOS: each call's keeper ends it when
  the broker goes; a keeper killed with SIGKILL leaves its call running ...`. A command broker
  lost while serving fails its waiting calls ("the command broker is gone") and refuses later
  calls to its command; the other commands go on.
  The router holds the admission caps for the tree; each command broker checks them again.
- **No process creation, kept.** `(allow process-fork (process-path (param "SELF")))`: the
  fork right is filtered to the broker's own image. The broker forks; anything it execs gets
  `EPERM` from `fork`, `vfork` and `posix_spawn` (measured, and asserted by
  `test_exec_broker_macos` and `e2e_exec_broker_macos.sh` through `time(1)`). A command whose
  real path IS this hull binary would run with that right, so it is refused at start.
- **No `execveat` / `fexecve`.** The child execs the checked path. The descriptor check (owner,
  mode, set-ID, dev / ino, the pin) runs just before, and the window is the `safe_path` rule's
  job (§6.1): only root can swap a file under a root-owned path.
- **No file capabilities** to refuse. The binary check takes a 64-bit Mach-O, thin or
  universal (a Java class file starts with the universal magic; its "count" is a version
  number, so a count over 16 is refused), and refuses a 32-bit or foreign-order one, a script
  and anything else. Every dynamic Mach-O's loader is `/usr/lib/dyld`, checked like a
  `PT_INTERP`.
- **No PR_SET_PDEATHSIG.** See §7: the broker watches the server's pid with kqueue
  (`EVFILT_PROC`, `NOTE_EXIT`) on top of the channel's EOF; each call runs under a keeper
  that watches the broker the same way and ends the job; the broker watches each keeper's
  exit with a kqueue in place of a pidfd.

**OpenBSD: not supported.** Linux's pledge / unveil polyfill does not stand in for it: the
Linux broker uses Landlock and seccomp directly, since the polyfill cannot express per-binary
execute grants on the inode checked, a per-child filter on top of the broker's, or per-child
`fs.write` devices. An OpenBSD backend would be a third `exec_broker_openbsd.c` (unveil `x`
per binary and loader, `pledge("stdio proc exec", "stdio rpath wpath tty")`, the union of
grants as on macOS). No CI runner or deployment uses OpenBSD, and an untested sandbox backend
is worse than a refusal, so it is refused like FreeBSD (§9) until one does.

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

**macOS** has no `PR_SET_PDEATHSIG`, so the first fact does not carry over, and the guarantee
there is narrower:

- the second fact holds: no child can create a process (§6.4), so a job is one process, and
  killing its process group kills all of it;
- every end of the broker that runs code kills each job's process group first: EOF on the
  channel, `hl_exec_broker_stop()`, and the server's exit, which the broker sees through a
  kqueue `EVFILT_PROC` / `NOTE_EXIT` watch on the server's pid as well as through the EOF
  (`hl_exec_plat_broker_init`). A server killed with `SIGKILL` therefore takes its commands;
- a **broker** killed with `SIGKILL` runs no code, so each call runs under a KEEPER
  (`hl_exec_plat_keeper`, [exec_broker_macos_isolation.md](exec_broker_macos_isolation.md)
  §4.6): the broker forks the keeper (its own image, never exec'd), the keeper keeps only the
  job's descriptors, forks the job and waits. The keeper is the job's parent, so it reaps it:
  a late kill cannot hit a reused pid, and signalling a child is all the profile lets it do.
  It watches its call's kill pipe (the broker holds the only write end: closing it ends the
  call, and the broker's death closes it), the broker's pid (kqueue `NOTE_EXIT`) and the job
  (`NOTE_EXIT`). When the broker goes, however it goes, the keeper kills the job's process
  group and reaps it; when the job exits, the keeper exits the same way (the code, or the
  signal), so the broker's `waitpid` reads the job's status. It ignores every signal but
  `SIGKILL`, `SIGSTOP`, `SIGCHLD` and the faults. With two or more commands the ROUTER is the
  broker the server sees; a router killed with `SIGKILL` takes every running command (each
  command broker sees it go and ends its calls), and a command broker killed with `SIGKILL`
  takes its own (its keepers see it go). What is left: a **keeper** killed with `SIGKILL`
  leaves its one job running until it exits, still under its `RLIMIT_CPU`, unable to fork,
  inside its command's profile; its call is answered once the job exits. Reported at start
  (`exec: limit: no parent-death signal on macOS: each call's keeper ends it ...`), and
  `e2e_exec_broker_macos.sh` asserts both sides. A keeper costs about 0.85 MB per running
  call.

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
- **macOS e2e** (`tests/e2e_exec_broker_macos.sh`, `make e2e-exec-broker-macos`, CI job
  `exec-broker-macos`): the same cases through `tests/fixtures/exec_probe_macos.c`, plus the
  isolation between commands (another command's grant, `unix_sockets` and binary refused),
  one broker for one command and a router with one broker each for two, the `exec: limit:`
  lines, this binary refused as a command, the broker's `SIGKILL` leaving the command, the
  router's taking it and a command broker's leaving only its call (§7). The raw-frames
  admission test runs on both hosts.
- **Unit:** `test_exec` covers the slot matcher (every rule, boundary lengths, `repeat`) and
  the policy codec over untrusted bytes. Each host backend has its own suite:
  `test_exec_broker_linux` (the ELF / `PT_INTERP` reader, file capabilities, the child filter,
  PDEATHSIG, the broker's lock, the pidfd) and `test_exec_broker_macos` (the Mach-O check, the
  profile text and parameters, the profile applied in a child: undeclared exec, fork, the home
  directory and the network refused; the kqueue watches). Each skips on the other host.
- **Fuzz:** `fuzz_exec_policy` (frames to the broker parser) and `fuzz_exec_slot`.

## 9. Platforms

"Demonstrated" means the e2e and the acceptance runs of §8 pass on that host. Until then a
row's guarantees are a plan, not a claim.

| Host | Status | Kernel layer | Guarantees demonstrated |
|---|---|---|---|
| Linux, native, Landlock ABI 6+ | PR 2 | Landlock + seccomp (raw syscalls) | every row of §6.5: `tests/e2e_exec_broker.sh` on x86_64 (ABI 10) and aarch64 (ABI 8); §8 acceptance passed on spark-7468 (GB10, `hs-metrics`, 2026-11-25) |
| Linux, cosmo APE | planned | as native, once the APE build carries the broker | none; refused at start (no broker) unless `--allow-degraded-sandbox` |
| Linux, Landlock below ABI 6 | PR 2 | as above, minus the missing rights | refused unless `--allow-degraded-sandbox`, which names what is missing |
| macOS | PR 3; per-command brokers #1218 | a Seatbelt profile per command (§6.4) | `tests/e2e_exec_broker_macos.sh` and `test_exec_broker_macos` on macOS 26 (arm64) and the `macos-15` runner: no process creation, exec only of the command's own binary, its own grants, no home directory or network; a command broker killed with `SIGKILL` leaving its running calls (§7) is a limit, reported at start |
| OpenBSD | not supported (§6.4) | none (an OpenBSD backend is not written) | refused unless `--allow-degraded-sandbox` |
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
    `shared/generation.c`, `commands/dev.c` and `cap/gpu_helper.c`, plus `cap/exec_broker.c`
    and its host backends `cap/exec_broker_linux.c` and `cap/exec_broker_macos.c` (which hold
    the exec itself).
  - Any other spawn site needs an allowlist entry with a reason.
- **INV-17:** the slot matcher is called from one place on each side: `hl_exec_slot_match` in
  the binding path and in the broker. Neither side has a second matcher.
- **CLAUDE.md:** gets one paragraph under "Security": the broker, the one-POLICY rule, the
  no-kernel-mechanism refusal, and that a new spawn site goes through INV-16.

## 12. Audit and observability

`--audit` gets two records per call, written by the server's side of the channel
(`exec_audit_start` / `exec_audit_end` in `cap/exec_broker.c`, the same `id` in both):
- `start`, once the broker has the RUN: the command name, each argument escaped
  (`hl_log_escape_line`, at most 1 KiB of each), the stdin length;
- `end`, when the call's slot goes: the outcome (`done`, `busy`, `refused`, `lost`,
  `cancelled`). `done` carries status / signal, `timed_out`, duration and output byte counts;
  the others carry a duration and a reason. A stream's `end` also carries the stdin bytes
  written.

stdout / stderr and stdin are never logged (stdin by its length only). Startup logs one line per command with its resolved path,
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
3. **PR 3 (done):** the host backends split behind `hull/cap/exec_platform.h` (Linux moved
   unchanged into `cap/exec_broker_linux.c`), and the macOS Seatbelt backend with its own unit
   test and e2e. OpenBSD dropped: not supported (§6.4).
4. **PR 4 (done):** the optional `sha256` pin (the broker verifies it, and only then does the
   manifest accept the field), and the `hull build` / `hull check` lint.

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

## 15. Addendum: the authority of every executable the broker runs (design review)

- **Status:** reviewed and decided (2026-11-26). §1-§14 shipped in v0.26.0 (#1210-#1214). The
  native-command part of this review shipped in v0.27.0 as §3.1 (#1217, audit 53 fixes #1219):
  each command has a nested `manifest`, a strict subset of the app manifest (`fs` today,
  resolved like the app's own), beside `path`, `sha256`, `argv` and `host` (absolute host
  grants and `unix_sockets`). The rest is decided as follows (§15.9 has the reasons):
  - **Hull callees** (§15.3, §15.4) and an intersection with a callee's own authority:
    **deferred, not scheduled.** The design below stands; it is built when a real callee fits
    the rootless, no-network, no-command class, and not before.
  - **Nested calls** (§15.5): **deferred** behind Hull callees. The running example's goal is
    met today by a separate service under its own uid (§15.7).
  - **macOS shared trust domain** (§15.6, the old slice 0): **moved to issue #1218**, which has
    its own design and chooses between the interim refusal proposed here and per-command
    isolation.

  In this section **shipped** means code on main, **proposed** means designed but not
  scheduled, and syntax marked `-- proposed` does not parse today: `cap/exec.c` refuses an
  unknown command key at load.
- **Question reviewed:** should each executable the broker launches carry a manifest in Hull's
  own vocabulary, so the broker confines it to that manifest, a Hull callee can run its own
  declared commands, and an operator can approve the whole command tree in one place?
- **Running example:** `hs-gateway -> hs-wifi -> nmcli`. The gateway may ask for a Wi-Fi scan
  through hs-wifi; hs-wifi may run the approved `nmcli` operation; the gateway has no direct
  route to `nmcli` and not the OS permission `nmcli` uses.

### 15.1 Verdict

One capability vocabulary: yes. One manifest per executable: only for Hull executables, and
not yet in the shape proposed. Five corrections:

1. **The OS-permission goal cannot be met inside one broker.** Every job runs as the app's
   uid, with NO_NEW_PRIVS and every credential change refused (§5.3, §14). NetworkManager
   authorises a scan by polkit, per uid, so `nmcli` under the gateway's broker uses the
   *gateway's* rights. Worse, a gateway that declares outbound `hosts` holds pledge `unix` on
   Linux (`sandbox.c`, phase 2: "nsswitch IPC"), and Landlock does not mediate pathname
   sockets, so a natively compromised gateway process can speak D-Bus to NetworkManager
   without `nmcli` at all. Only a separate uid gives "the gateway lacks the permission". That
   is a separate hs-wifi service, which v0.26.0 can already deploy (§15.7). The broker tree
   can give "the gateway process has no exec route to `nmcli`"; it cannot give more.
2. **A Hull binary does not run as a declared command today** (§15.4): the child starts in
   `/`, which a built Hull app refuses as its app directory unless it is rootless; it cannot
   spawn its own broker; on macOS it cannot apply its own sandbox at all. Making a Hull callee
   work is the prerequisite for everything else here, and it is Linux-only.
3. **No sidecar for native binaries.** The inline declaration already is a digest-bound,
   signed, reviewed policy (§15.2). A sidecar adds a second place to review and is trusted
   only as far as whoever can write it, the same party who can replace the binary.
   **Implemented** (§3.1): the native child manifest is inline, `exec.<name>.manifest`, with
   independent authority: a command's grant need not lie inside the app's own.
4. **Nested calls are a protocol change, not a policy merge** (§15.5). Folding descendants
   into the broker's one command table would let the gateway request `nmcli` directly. The
   broker must bind each request to the job that sent it, by channel.
5. **No field promises what a backend cannot enforce** (§15.6). There is no network for any
   child (the broker's ceiling refuses every socket family but `AF_UNIX`), no hostname rule
   anywhere in the kernel layer, `unix_sockets` cannot be narrowed to one socket, and macOS
   gives every command the union of grants. A callee manifest asking for any of these is
   refused at start, never accepted and silently weakened.

### 15.2 Manifest reuse

**A Hull executable** already has a signed manifest: `package.sig`, which `hull build --sign`
writes BESIDE the binary (`stdlib/cli/lua/hull/build.lua`, "Write package.sig"). Its Ed25519
signature, under the developer key named in the same file, covers `binary_hash` (SHA-256 of
the built binary), `manifest`, `modules_resolved`, `files`, `platform`, `build` and
`trampoline_hash` (`stdlib/cli/lua/hull/app_sig.lua`). So the broker can consume it as is,
with two limits:

- it is not inside the executable (it cannot be: it hashes the executable), so it needs a
  location: **proposed** `<real path>.sig`, held to the same root-owned `safe_path` rule as the
  binary (§6.1). `hull build -o` and `hull deploy` would write it there;
- its key is whatever `public_key` the file names. Self-consistency proves nothing about who
  signed it; the caller must pin the key (§15.3). `hull inspect` today checks the signature
  only against the key in the same file, and never hashes the binary
  (`stdlib/cli/lua/hull/inspect.lua`); `hull verify --binary` does hash it.

No schema change is needed for Hull callees: their `fs`, `state`, `databases`, `hosts`,
`exec`, ... are already the vocabulary. What is new is a translation from that vocabulary to a
broker domain (§15.4), and the refusal of the parts the broker cannot give.

**A native executable** keeps an inline declaration: since #1217, `manifest.fs` (relative
to the app root or `@state/`) and `host` (absolute paths, `unix_sockets`), §3.1. Compared with
a digest-bound sidecar:

| | inline (shipped) | sidecar (proposed by the question) |
|---|---|---|
| who writes the grants | the caller's developer | whoever installs the sidecar |
| who signs them | the caller's `package.sig` | nobody, unless a new signing step |
| what makes the digest trustworthy | it is inside the signed caller manifest | the sidecar's file ownership (root) |
| where the operator reviews it | `hull inspect <caller>`, one place | the caller, plus every sidecar |
| what it buys | | sharing one tool's grants across several callers |

The trust row decides it: a sidecar binds the binary's digest to the grants, but the expected
digest is only as trusted as the sidecar, and the party who can rewrite the sidecar (root)
can replace the binary too. It authenticates nothing the `safe_path` rule does not. Sharing
grants across callers is real but small (a handful of `fs` paths) and can be had later with a
build-time include, without a runtime file. **Drop the native sidecar.**

### 15.3 Identity and updates

| Pin | Establishes | On a rebuild | Shipped? |
|---|---|---|---|
| `sha256` | these exact main-executable bytes | every rebuild fails the start (and a replace after start fails the call) | yes |
| signer (**proposed** `signer`) | some binary signed by this key | any rebuild by the key passes, whatever it now requests | no |
| authority (**proposed** `manifest_sha256`) | the callee's signed `manifest` hashes to this value | a rebuild with the same manifest passes; any change to its authority fails the start | no |
| app identity | nothing yet: no signed field names the app (`state.name` is optional and means a directory) | | no |

A signer pin alone is not enough. Two tools signed by one key are interchangeable at the
callee's path, and a signed rebuild that asks for more would be accepted silently. The
smallest predictable policy for a Hull callee, all **proposed**:

- `signer` is required: no callee manifest is read without a pinned key;
- plus `sha256` (exact bytes; the shipped behaviour, unchanged), or `manifest_sha256` (any code
  that the key signed with exactly this authority). Neither is refused at load;
- `manifest_sha256` is SHA-256 of the canonical JSON of `package.sig.manifest`, encoded as
  `hull.app_sig` encodes the payload, and printed by `hull inspect` so it is copied, never
  computed by hand.

Outcomes:

- **Two tools under one key:** a different tool at the path is refused unless its manifest is
  byte-identical, in which case it has exactly the approved authority. Pin `sha256` when that
  residue matters.
- **A signed rebuild asking for more** (or less) fails app START, at broker policy load,
  naming the command, the expected and found digests, and the line to re-run (`hull inspect`).
  Never a call-time surprise.
- **A change between inspection and launch:** inspection is advisory, launch is authoritative.
  At policy load the broker opens the binary, verifies the sidecar under the pinned key,
  hashes the descriptor it will exec against `binary_hash`, and checks `manifest_sha256`. On
  every call it re-checks `(st_dev, st_ino)` and re-hashes, as the `sha256` pin does today
  (§6.1; a Hull binary is a few MB, an estimated 10 ms of SHA-256 per call, to be measured).
- **Existing deployments:** a command without `signer` behaves exactly as in v0.26.0. The
  broker reads no sidecar for it, and `sha256` keeps its meaning.

The approval lives in the CALLER's signed manifest: `signer` plus `manifest_sha256` name the
callee's identity and authority, and the callee's grants live once, in its own `package.sig`.
Re-approving a broader callee means rebuilding the caller with the new digest. Hull has no
operator-side policy file, and adding one is not justified by one tree.

### 15.4 A Hull executable as a command (direct calls)

What stops it today, from the code:

| Obstacle | Where | Fix (**proposed**) |
|---|---|---|
| the child runs in `/`; a built Hull binary takes its working directory as its app directory and refuses `/` unless rootless | `exec_broker.c` `chdir("/")`; `sandbox.c` `hl_sandbox_manifest_rootless_ok` | require the callee to be ROOTLESS (built, `app.main` only, `state` declared, every `fs` and file database under `@state/`), the existing rule, no new one |
| it writes nowhere but `/dev`, `/run`, `/tmp`; `RLIMIT_FSIZE` is 0 without `fs.write` | §3, §5.3 | derive a write grant for the callee's state directory (`$XDG_STATE_HOME` is unset in the child, so `<passwd home>/.local/state/<name>`, resolved by the same `hl_state_dir_resolve`) |
| it cannot open `AF_INET` / `AF_INET6` | broker ceiling, `seccomp_broker` | refuse at start a callee with `hosts`, a network database, or any module that listens or dials (`http-server`, `http-client`, `smtp`, ...) |
| it cannot spawn its own broker or GPU helper (`clone` without `CLONE_THREAD` is `EPERM`) | `seccomp_child`; `serve_cli.c` ignores the failed broker spawn | refuse a callee with `exec` (until §15.5), `gpu`, `tui` |
| `--verify-sig` looks for `package.sig` next to the entry, i.e. `/package.sig` | `signature.c` | none needed: the broker verifies the sidecar itself, before the exec |
| macOS refuses a second sandbox, and Hull's own Seatbelt `sandbox_init` failure is fatal | §6.4; `sandbox.c` | refuse a `signer` command on macOS at start, even with `--allow-degraded-sandbox` (as Windows: no setting produces the guarantee) |

On Linux the callee then runs inside TWO confinements, and both are real: the broker's
per-child Landlock domain and seccomp filter (outer, from the derived grants), and the
callee's own phase-2 sandbox from its own manifest (inner; Landlock and seccomp stack, and the
shipped child filter leaves the Landlock calls, §5.3). Whether that stack starts cleanly is
**untested**: the acceptance below must show it.

The derived grants are the callee's manifest translated, never widened, into the §3.1
child-manifest shape (`manifest.fs` under `@state/`, no `host`): the default read set,
its state directory read-write, no sockets, its `stdin_max` and limits from the CALLER's
declaration (the caller owns its call's timeout and output caps).

### 15.5 Delegation and nested calls

Three separate things, for each process:

- **direct authority:** what its own sandbox lets it do (its manifest, enforced by its own
  kernel sandbox; for a job, also the broker's per-child domain);
- **invocable operations:** the command shapes its OWN manifest declares;
- **delegation bound:** nothing a callee does widens its caller's direct authority, and a
  callee's operations are the callee's, never the caller's.

The proposal to reuse ONE broker for the tree, keeping process creation unavailable inside
every job, holds up. It is the only shape that keeps §7's lifetime guarantee, because the
broker stays the only process that forks, so PDEATHSIG and "no grandchild" still cover every
job. But a global command table loses the third rule. Merged, the broker would accept
`RUN nm-scan` from the gateway's channel. The design must instead be:

- **Policy is a tree.** The single `POLICY` frame carries the root's commands, and for each
  Hull command the callee's commands (from its verified `package.sig`) as a child node.
  Depth bounded (4 proposed); the whole tree's binaries are checked at load.
- **Identity is the channel, not the pid.** When the broker forks a Hull job whose node has
  commands, it creates a socketpair for it and passes one end at a fixed fd with a broker-set
  environment value (`HULL_EXEC_CHANNEL=4`, **proposed**). The callee's runtime adopts that
  channel instead of spawning a broker and sends no `POLICY`. A `RUN` on that channel is
  matched against that node only. A pid (`SO_PEERCRED`) would be reusable and racy; a
  channel is a capability the job either holds or does not.
- **Lifetime and admission are per tree.** Killing a job kills its sub-jobs first; a
  sub-call's timeout is clamped to what is left of its parent's; the in-flight and
  buffered-output caps are the tree's, not each node's.
- **Layer 3 of §1 weakens per caller.** The broker's Landlock `EXECUTE` set becomes the
  tree's union, so a broker bug could exec `nmcli` for the gateway. The per-channel match is
  the boundary there, and §1's claim must say so.

This is a new frame layout, a runtime path that adopts an inherited channel, and job-tree
bookkeeping. It stays out of the first slice. It is also refused on macOS, where every
command of the tree would share one profile.

**Can the argument policy express "scan"?** Yes, with fixed arguments and no slot:

```lua
["nm-scan"] = {
  path = "/usr/bin/nmcli",
  argv = { "--terse", "--fields", "SSID,SIGNAL,SECURITY", "device", "wifi", "list",
           "--rescan", "yes" },
  host = { unix_sockets = true },   -- nmcli reaches NetworkManager over the system D-Bus socket
  timeout_ms = 20000, stdout_max = 65536,
},
```

The argument shape fixes what `nmcli` is asked, but not what the job could reach: under
`host.unix_sockets = true` the job can connect to any pathname socket the uid can (§3). A
per-interface variant cannot be one command with an optional `ifname <name>` pair, because
only the last entry may `repeat`, so a `min = 0` slot after a fixed `ifname` would leave
`ifname` dangling. Declare a second command that always takes it.

**Walk-through, nested model (Linux, proposed):**

| Step | Allowed scan | Direct gateway -> `nmcli` |
|---|---|---|
| app code | gateway `exec.run("wifi-scan")`: the binding matches it against the gateway's node | `exec.run("nm-scan")`: not in the gateway's manifest, an error before any frame |
| a compromised gateway sends a raw frame | `RUN wifi-scan` on the gateway's channel: admitted | `RUN nm-scan` on the gateway's channel: `REFUSED` (not in that node) |
| broker | checks hs-wifi (owner, `(st_dev, st_ino)`, hash vs signed `binary_hash`), forks it with its derived domain and a job channel | nothing forked |
| hs-wifi | its own sandbox applies; its `exec.run("nm-scan")` goes out on ITS channel | n/a |
| broker | matches `nm-scan` against hs-wifi's node, forks `nmcli` with `unix_sockets` | n/a |
| kernel | `nmcli`: execute only `nmcli` and its loader, `AF_UNIX` only, no process creation | the gateway process has no exec at all; the BROKER's domain does execute `nmcli` (layer 3 is per tree, not per caller) |
| OS | polkit: the gateway's uid must be allowed `wifi.scan` | a gateway with pledge `unix` can call NetworkManager over D-Bus with that same uid's rights |

The last row is correction 1 again.

### 15.6 What each field can promise

| Field | Linux (per child) | macOS | Native command | Hull callee |
|---|---|---|---|---|
| `fs.read` / `fs.write` | Landlock, per child | union of all commands | `manifest.fs` (app-relative, `@state/`) and `host.read` / `host.write` (absolute), shipped §3.1 | derived from `@state/` only |
| device ioctls | ABI 5, per child | `file-ioctl`, union | inline `fs.write` under `/dev` | refused (no `gpu`) |
| `unix_sockets` | seccomp, per child; any pathname socket the uid reaches | union | `host.unix_sockets`, boolean (shipped) | refused |
| network, `hosts` | none: the broker's ceiling refuses every family but `AF_UNIX`; Landlock filters TCP ports, never names | none | refused (shipped: no field) | refused at start |
| own sandbox | stacks under the broker's | impossible (nested `sandbox_init` is `EPERM`) | n/a | Linux only |
| nested `exec` | §15.5 | refused (union) | n/a | refused until §15.5 |

The explicit behaviour when isolation is unavailable: **refuse at start**. A weaker but
well-defined guarantee may run under `--allow-degraded-sandbox`, one `exec: degraded:` line
per guarantee lost (§6.5's rule). Never accept a field and enforce less.

**Superseded:** per-command brokers removed the macOS union (#1218,
[exec_broker_macos_isolation.md](exec_broker_macos_isolation.md)); the interim rule below was
not built. **The macOS union deserves the same rule.** It is documented as a shared trust domain (§3.1,
§6.4: one profile, so every command reaches every command's grants and may exec every
declared binary), shown by `hull inspect` and logged at start (`exec: limit:`), and it runs.
Per-command isolation on macOS is tracked in issue #1218, which has its own design; the
proposal below is one option there, not a decision of this section.
That is a guarantee §6.3 makes on Linux and macOS silently lacks, which is §6.5's definition of
degraded. Proposed: when the commands' grants DIFFER (`fs`, `unix_sockets`), a macOS start is
refused unless `--allow-degraded-sandbox`; identical grants need nothing, since the union then
loses nothing. That reuses the existing operator flag rather than adding one. The cost is that
an app with differing grants, released on macOS in v0.26.0, needs the flag after the change.
That is a breaking change for that case only: note it in the CHANGELOG, and the macOS e2e's
union case passes the flag.

### 15.7 The running example without new runtime work

The property the example asks for (the gateway lacks the OS permission) needs a second uid,
and v0.26.0 already has the parts:

- **hs-wifi** is its own service (a systemd unit, `User=hs-wifi`), a Hull app with
  `http-server` bound to `127.0.0.1`, and a direct `exec` of `nm-scan` (§15.5's declaration).
  A polkit rule grants `org.freedesktop.NetworkManager.wifi.scan` to `hs-wifi` alone.
- **hs-gateway** declares `hosts = { "127.0.0.1" }` and a shared token in `secrets`, calls
  `http.fetch("http://127.0.0.1:<port>/scan", ...)`, and has no `exec` of `nmcli` and no
  NetworkManager rights.
- The gap is the loopback port, which any local process can reach, hence the token. A Unix
  socket with peer credentials would close it, but Hull has neither a Unix-socket listener nor
  a client today. That is the one feature worth adding for this example.

### 15.8 Worked example (direct call, slice 1)

`hs-wifi` here is a leaf (it reads hardware state from `/sys`, runs nothing). `nmcli`
belongs to §15.5/§15.7.

```lua
-- hs-gateway/app.lua (signed by the gateway's key)
app.manifest({
  modules = { "hull/exec@1", "hull/http-server@1" },
  exec = {
    ["wifi-status"] = {
      path = "/opt/hs/bin/hs-wifi",
      argv = { "status", "--", { chars = "a-z0-9", max = 15 } },   -- interface name
      timeout_ms = 10000, stdout_max = 65536,
      signer = "3f9c...e1",              -- proposed: the hs release key
      manifest_sha256 = "a41b...07",     -- proposed: printed by hull inspect
    },
  },
})
```

```lua
-- hs-wifi/app.lua (built with hull build --sign; package.sig installed as hs-wifi.sig)
app.manifest({
  modules = { "hull/json@1", "hull/fs@1" },
  state = { name = "hs-wifi" },
  fs = { write = { "@state/last.json" } },
})
app.main(function(ctx) ... end)
```

Effective authority (Linux):

| Process | Direct | May invoke | Bounded by |
|---|---|---|---|
| hs-gateway (app process) | its own manifest; no exec / fork / clone | `wifi-status` with one interface-shaped argument | its signed manifest; the broker's match |
| hs-wifi (job) | outer: execute its binary and loader, the default read set, `~hs/.local/state/hs-wifi` read-write, no sockets, no process creation; inner: its own sandbox, `@state/last.json` | nothing | `signer` + `manifest_sha256` in the gateway's manifest; refused at start if its manifest asks for more than §15.6 allows |

What `hull inspect` should show for a Hull callee (**proposed**; for a native command it
already prints path, argv, slot rules, `host` and `manifest` grants and the effective
authority per platform, §3.1):

```
Commands (exec):
  wifi-status  /opt/hs/bin/hs-wifi  status -- <a-z0-9, 1..15>
    callee:    Hull app, signer 3f9c...e1 (pinned)      signature VALID
    binary:    sha256 9d02...c4 matches the signed binary_hash
    authority: manifest sha256 a41b...07 matches the pin
    grants:    state dir (read-write), no network, no sockets, no commands
```

A missing sidecar, a different key, a binary that does not hash to `binary_hash`, or a
manifest that does not match prints `UNVERIFIED: <reason>` on that line. The same check runs
at app start, where it is fatal.

### 15.9 Slices, compatibility, acceptance

**Order.** The evidence changes the order the question proposed. The running example's
security goal is met by §15.7 today, and nested calls need a Hull callee that runs at all. So:

0. **Done (v0.27.0, #1217 + #1219):** native child manifests (§3.1): `manifest.fs` with
   app-relative and `@state/` grants opened by descriptor, `host` for absolute grants,
   independent authority, the legacy `fs` / `unix_sockets` keys read as `host` through 0.27.x
   (refused since 0.28.0), and `hull inspect` showing both sets and the effective authority per
   platform.
1. **macOS shared trust domain: moved to issue #1218.** This review proposed an interim rule
   (§15.6: differing grants refused on macOS unless `--allow-degraded-sandbox`, plus a §1 note
   that layer 3 is per broker, not per caller). #1218's design weighs it against per-command
   isolation and decides; whichever lands, Linux is unchanged.
2. **Slice 1: a Hull leaf callee, Linux only (§15.4, §15.3). Deferred, not scheduled.**
   `signer` and `manifest_sha256` (new command keys; a new command flag in the policy
   encoding, as `stdin_max` was, so existing encodings and the fuzz corpus stay valid); the
   sidecar check at policy load and the per-call re-hash; derived grants; the refusals of
   §15.6; `hull inspect` verification. The trigger to build it is a real callee in the
   rootless, no-network, no-command class. None exists: the running example's callee needs
   `nmcli` (slice 2) and, for its security goal, a second uid (§15.7), and hsctl / hscli
   declare no commands.
3. **Slice 2: nested calls (§15.5), Linux only. Deferred** behind slice 1: it needs a Hull
   callee that runs at all, and the property it adds (no exec route from the caller to the
   grandchild) is weaker than the separate-uid service of §15.7, which ships today.

**Also not scheduled:** a Unix-socket listener and client (§15.7), the one feature that would
close the separate-service design's loopback gap. Raise it when a deployment needs it.

**Compatibility.** A manifest without the new keys encodes and behaves byte-for-byte as in
v0.26.0 (§3.1 keeps that: a command with no `manifest` encodes to the v0.26.0 bytes, and the
legacy `fs` / `unix_sockets` keys were read as `host` until 0.28.0, which refuses them). A manifest WITH them is refused by v0.26.0 (unknown key), which fails closed. Slice 0
breaks one case: macOS, several commands, differing grants, now needs
`--allow-degraded-sandbox`.

**Slice 1 acceptance** (CI, `make HL_EXEC_TEST_HOOKS=1`; root-owned fixtures as §8):

- a built rootless Hull tool runs as a declared command and returns its output; its own
  `[sandbox] applied` line is present (the two layers stack);
- start is refused, naming the reason, for each of: no sidecar, a bad signature, another
  key, a binary that does not hash to `binary_hash`, a `manifest_sha256` mismatch, a callee
  declaring `hosts`, `exec`, `gpu`, `tui` or a non-`@state/` grant, and any `signer` command
  on macOS;
- a signed rebuild with the same manifest starts with no change to the caller, and one with
  a widened `fs.write` does not;
- the callee's job writes its state directory and nothing else (probe), and opens no socket;
- replaced binary: `ESTALE`; rewritten in place: the per-call hash refuses the call;
- `hull inspect` prints VERIFIED only after hashing the binary, and `UNVERIFIED: <reason>`
  for each start-refusal case above;
- a v0.26.0 manifest's policy encoding is unchanged (a golden test).
