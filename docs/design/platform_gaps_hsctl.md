# Design: Closing the hsctl platform gaps

- **Status:** Accepted
- **Owner:** TBD
- **Related:** `~/herdingsparks/src/hsctl/PLATFORM_GAPS.md` (the eight gaps, as the app
  reported them); BOOTSTRAP.md "PLATFORM_GAPS.md protocol"; docs/ssh.md; docs/ssh_module_design.md
  §9-§10; docs/cli_mode.md; CLAUDE.md "App Lifecycle", "Manifest & Sandbox", "Path traversal
  blocked", "HULL_CACHE_DIR"

## 1. Context

hsctl is the first `app.main` tool built on Hull by an outside team (a JavaScript CLI that
drives a fleet over `hull:ssh`). Following the BOOTSTRAP protocol it recorded, instead of
coding around, eight things it needed from Hull. Five are CLI-shaped: a CLI writes bytes,
keeps per-user state, stores secrets, is interrupted, and runs from wherever the operator is.
Two are about the SSH trust model. One was fixed in #1025 and is unreleased.

Decisions taken with the owner (2026-11-07):

- v0.18.3 is cut AFTER the small gaps land, not before.
- The per-user state directory is named by a reserved path prefix (`@state/`), not a
  separate fs object. `~/.ssh` stays unreadable: keys are imported into the state directory.
- A built CLI that declares a state directory and nothing app-relative stops depending on
  its working directory.
- `fs.write` takes both an octal `mode` and a `private` shorthand.
- An interrupted `app.main` with no handler registered exits after teardown.
- The live SSH connection exposes its session id beside its host key.

## 2. Goals

- **G1.** Every gap in the hsctl report is either closed or answered with a documented
  "by design" and an alternative the app can use.
- **G2.** No new authority: nothing here widens what an app can reach beyond what its
  manifest declares, and every new path, mode or login is reviewable in the manifest or the
  call site.
- **G3.** Both runtimes get the same surface, named in each runtime's convention.
- **G4.** Each change ships with a test that exercises the reported failure.

## 3. Non-goals

- Absolute or home-relative `fs` grants. The policy refuses them on purpose
  (`cap/fs_policy.c`, `hl_fs_grant_parse`), and a tool that reads the operator's real
  `~/.ssh` is a different trust statement from one that holds its own copy.
- A handshake-time host-key callback. Both ssh implementations reject it (ssh.lua:49-52,
  hostkey.lua:399-404 and the JS twins): an `on_unknown` hook gets wired to `return true`.
- A per-path "secret" attribute on `fs.write` grants. Grants carry no attributes; an
  explicit mode at the call is reviewable.
- Graceful drain for the HTTP serve loop. That exists (`--drain-timeout`); this doc covers
  only the window in which `app.main` runs.

## 4. Order of work

| # | Gap | Size | Ships as |
|---|-----|------|----------|
| 8 | Linux kills a built app whose stdout is /dev/null | done (#1025) | v0.18.3, after rows 6-2 |
| 6 | JS `ctx.stdout.write` takes text only | small | one PR |
| 1 | `connect_failed` carries no errno | small | one PR |
| 5 | `ssh.connect.users` is compiled in | small | one PR |
| 2 | no file mode on `fs.write` | small | one PR |
| 7 | `app.main` cannot catch an interrupt | medium | one PR |
| 4 | one transport, one host key | medium | one PR |
| 3 | a CLI's state lives in its working directory | large | §11 here, then 2-3 PRs |

Rows 6, 1, 5, 2 are independent and may land in any order. Row 3 depends on nothing above
but is last because it is the only one that changes the fs policy's root model.

## 5. Gap 6: `ctx.stdout.write` takes bytes (JS)

**Today.** `js_cli_write` (`runtime/js/runtime.c`) runs `JS_ToCStringLen` on every argument:
an ArrayBuffer writes `[object ArrayBuffer]`, a Uint8Array writes `1,2,3`. Lua's
`cli_writer_write` is already binary-safe (`luaL_checklstring` + `fwrite` of the length).

**Change.** `js_cli_write` takes each argument through `js_get_buffer` (`mod_buffer.h`, the
helper `fs.write` and `res.bytes` use): a string is written as UTF-8 as before, an
ArrayBuffer or typed array as its bytes, a DataView refused (the convention: a DataView is
not a byte view any binding takes), anything else a TypeError. The per-byte charge is
unchanged. `ctx.stderr.write` gets the same.

**Follow-up, not in this PR.** `ctx.stdin.read(n, { bytes: true })` returning an ArrayBuffer,
since `js_cli_read` builds strings and binary input does not survive either.

**Test.** `test_js.c`: a main writing a Uint8Array holding 0x00, 0x80 and 0xff to a pipe; the
reader sees exactly those three bytes.

## 6. Gap 1: `connect_failed` names the errno

**Today.** `cap/net_stream.c` reads the socket error at both connect-failure points (the
non-blocking callback, ~:344, and the synchronous path, ~:373) and then passes only
`KL_ERR_CONNECT`, which becomes `HL_NET_E_CONNECT`; `struct HlNetStream` has no field to
keep it. The ssh bridges (`mod_ssh.c` in each runtime) map RESOLVE, CONNECT and TLS all to
`connect_failed`, and `detail` is the fixed string from `hl_net_stream_strerror`
("connection refused or unreachable"). So an EHOSTUNREACH is gone before the binding.

**Change.**

1. `HlNetStream.last_errno`, set at both failure points (socket creation and the
   non-blocking setup included), with an accessor `hl_net_stream_connect_errno()` beside
   `hl_net_stream_connect_result()` in `include/hull/cap/net_stream.h`. DNS and TLS failures
   leave it 0: a TLS handshake failure is deliberately undetailed (net_stream.c ~:687) and
   stays so.
2. The bridges push a fourth value, the errno's symbolic name (`"EHOSTUNREACH"`,
   `"ECONNREFUSED"`, ...; a small table in `shared/`, unknown values as `"E<number>"`), and
   `detail` becomes `<strerror string>: EHOSTUNREACH (No route to host)` from the C
   `strerror`. No host, port or password is added to `detail`: the app knows what it dialled.
3. The stdlib (`ssh.lua` / `ssh.js` connect wrappers, and the tunnel path) sets `err.errno`
   to the name, so an app branches on `e.errno === "EHOSTUNREACH"` and never on text. The
   error shape stays `{code, detail}` plus fields, as every ssh error.
4. The macOS hint lives in the stdlib, not C: when the name is EHOSTUNREACH and the host is
   an IP literal in a private or link-local range (`cap/host_match` already classifies
   CIDRs), `detail` gains one sentence: "on macOS 15+ a binary not yet allowed under
   Local Network privacy gets this; allow it in System Settings > Privacy & Security >
   Local Network, and again after every hull build". docs/ssh.md §10 gets the same row.

**Test.** `test_net_stream.c`: connect to a closed loopback port; the result is CONNECT and
the errno accessor returns ECONNREFUSED. A Lua stdlib suite case asserts `err.errno` and the
hint's presence for a 169.254/16 literal with an injected errno (the suites inject through
`opts.crypto`-style seams already; add the net result as one). EHOSTUNREACH itself is not
reproducible portably and is not asserted live.

## 7. Gap 5: `ssh.connect.users` takes `$VAR` entries

**Today.** `hl_ssh_check_connect` (`cap/ssh_policy.c:34-58`) compares each login with
`strcmp`. `hl_manifest_check_env_refs` (manifest.c) checks references in `ssh.connect.hosts`
and `ssh.tunnel.hosts` but not `users`, so a literal `"$X"` is just the string `$X`. The
stated reason for no pattern vocabulary is that the only pattern anyone reaches for is `*`.

**Change.** An entry that is exactly `"$VAR"` or `"${VAR}"` is an env reference, resolved at
check time by the same reader `hl_host_match_any_env` uses (`utils/host_match.c`), so one
reference is one login. The resolved value is accepted only if it is a plausible login:
non-empty, no whitespace, comma, glob character, `/` or control byte, at most 255 bytes;
anything else, and an unset variable, matches nothing. The comparison stays exact and
case-sensitive. The load-time reference check extends to `users`, so the variable must be
declared in `env` or `secrets`. The sealed manifest seals the literal `"$VAR"`, as for hosts.
The `*` rationale survives: an env value cannot widen a grant to more than one login.

**Docs.** docs/ssh.md §2 (`:86-88`, "Only `hosts` entries may be `$VAR`") becomes "hosts and
users"; ports stay literal, since a port grant that changes shape after review is the thing
that rule refuses.

**Test.** `test_ssh_policy.c`: a `$VAR` user allowed when set, denied when unset, denied when
the value is `*` or `a,b`; `test_manifest*`: an undeclared variable in `users` refuses load.
Add the missing coverage for `$VAR` in `ssh.connect.hosts` in the same PR.

## 8. Gap 2: `fs.write` takes a mode

**Today.** `hl_cap_fs_open_write_fd` opens with mode 0644; `fs_write_atomic` creates its temp
file 0644 (`cap/fs.c` ~:483), copies an existing target's bits onto it, writes, fsyncs,
renames. Implicitly created parents are `mkdirat(..., 0755)` (`cap/fs_resolve.c` ~:500).
Neither binding takes options; the only cap flag is `HL_FS_WRITE_NO_SYNC`.

**Change.**

- Binding: `fs.write(path, data, opts)` with `opts.mode` (Lua) / `opts.mode` (JS), an octal
  STRING (`"600"`, `"640"`, `"644"`), and `opts.private = true` as shorthand for `"600"`.
  Both given and disagreeing is an error. A number is refused (a decimal 600 is a classic
  mistake). Any bit outside 0666 is refused: no execute, setuid, setgid or sticky bit is
  reachable from app code.
- Cap: `hl_cap_fs_write_ex` gains a `mode_t mode` (0 = unchanged behavior). The temp file
  is ALWAYS created 0600 and `fchmod`ed to the final bits right before the rename, so a
  secret is never world-readable for the write's duration (a strict hardening even with no
  mode given; the copy of an existing target's bits is applied the same way). The in-place
  fallback `fchmod`s after its truncate. With a mode given, the target's existing bits are
  replaced, not merged.
- Directories: when the mode has no group or other bits, the parents the resolver creates
  for THIS write are made 0700 instead of 0755 (`HL_FS_OPEN_*` gains a private-dirs bit the
  resolver reads at its mkdir). Existing directories are never re-moded. **This does not
  make hsctl's `state/` check (mode & 077 == 0) satisfiable from the app** (gap 9, §14, found
  once hsctl pinned v0.18.3): the sandbox pre-creates every `dir/` write grant at startup,
  with 0755, so the granted root always exists by the time the app writes and the private
  bit only ever reaches the file and any parent the write itself creates below it. The check
  is satisfied by the `@state/` root (§11, created 0700), not by this change.
- Charging: unchanged (the floor and per-byte charge cover the extra `fchmod`).

**Test.** `test_fs.c`: created-with-600 stat reads 0600 and its new parent 0700; `"755"`
and 0o600-as-number refused; a 0644 target rewritten with `"600"` reads 0600; a temp file
observed mid-write (via the live-file lock seam) is 0600. Both runtime suites: the options
parse, `private` and `mode` together refused.

## 9. Gap 7: `app.main` observes an interrupt

**Today.** No runner installs SIGINT/SIGTERM handlers while main runs: `serve_cli.c` has
none at all, and `serve.c` runs main (`hl_serve_run_main`) BEFORE `kl_http_server_run`,
which is where Keel installs its handler. Ctrl-C kills the process with no teardown and no
stdout flush. Only `cap/tui.c` installs a handler, to restore the terminal.

**Change.** One shared module, `src/hull/shared/main_signal.c`, used by both runners around
the main window:

1. **Handler.** Installed just before main is invoked, removed when main settles. It sets an
   atomic flag and wakes the loop: the Keel backend's `keel_stop` is already documented as
   signal-safe, and the poll backend treats `poll()` EINTR as a tick. It chains to a
   previously installed handler (the TUI's) rather than replacing it.
2. **Delivery.** On the loop's next tick the runtime delivers the interrupt to the app as a
   NEW RUN (budget and deadline re-armed through the same entry path a `hull._task` /
   `hull:_task` task uses, so the stale-transaction guard and the end-of-entry check run).
   - JS: `ctx.signal`, a minimal AbortSignal-shaped frozen object made in C: `aborted`,
     `reason` (`"SIGINT"` / `"SIGTERM"`), `onabort` (settable once, before any signal), and
     `ctx.signal.promise`. QuickJS has no AbortSignal, and Hull adds no event machinery.
   - Lua: `ctx.interrupted()` returns `nil` or the signal name; `ctx.on_interrupt(fn)`
     registers one callback.
3. **Grace.** A deadline starts at the first signal: default 5000 ms (`HL_DEFAULT_DRAIN_TIMEOUT_MS`,
   the same constant the serve loop uses), set by `--interrupt-grace-ms N`, a downgrade flag
   in `hl_runtime_flags()` so a built binary takes it only as `--hull-interrupt-grace-ms`.
   If main settles within it, main's return value is the exit code. If not, teardown runs
   (the normal order: pool, `stop_async`, app context, loop) and the exit code is 128 + signal.
4. **No handler registered.** Main is not continued: teardown, then exit 128 + signal. This
   differs from today only by the teardown and the stdout flush (decision, §1).
5. **Second signal** during the grace: teardown, exit 128 + signal at once.
6. **Scope.** The main window only. Once a full build enters the serve loop, Keel's drain
   applies as today.

**Test.** `tests/e2e_cli_poll.sh` gains legs, both runtimes: a main parked on `hull.sleep`
gets SIGINT, its handler writes a line and main returns 3, exit 3; no handler, SIGTERM, exit
143 with buffered stdout flushed; a handler that never returns, exit 130 after the grace;
two SIGINTs, exit 130 at once. Full build (`serve.c`) covered by one leg since the module is
shared.

## 10. Gap 4: trust for this connection only, host key and session id exposed

**Today.** Trust is only `opts.trust`, a store with `get` / `put` / `forget` / `entries`
(`memory_store`, `file_store`, `kv_store`). An unknown key fails the connect with
`host_unknown` carrying `fingerprint` and `key_blob` / `keyBlob`, so meeting a key costs one
connection and authenticating costs a second (hsctl's adoption). The transport holds the
host-key blob and the session id but the handle exposes only `fingerprint()` and
`negotiated()`. docs/ssh_module_design.md §10 describes a `host_key` field that was never
built.

**Change.**

1. **`ssh.pending_trust()`** (JS `ssh.pendingTrust()`): a single-use trust object. It accepts
   the first key presented to it, refuses a second `connect` with `already_used`, and is
   never a persistent store (no `put` reaches disk). The connection it produces has
   `conn:trust_pending()` → true (JS `conn.trustPending`). A rekey on that connection is
   still held to the same key (`host_changed_midsession` as today).
2. **`conn:host_key()`** (JS `conn.hostKey()`) on EVERY connection: `{ blob, algorithm,
   fingerprint }`, blob as bytes (Lua string / ArrayBuffer), algorithm the negotiated
   host-key algorithm name. **`conn:session_id()`** (JS `conn.sessionId()`): the RFC 4253 §7.2
   session identifier, bytes. The proof "one transport, one host key, one transcript" binds
   to exactly these two.
3. **`conn:accept_host(store)`** (JS `conn.acceptHost(store)`): persists the connection's key
   into a real store through the existing `accept_host`, which never overwrites an existing
   entry and may fail `already_trusted` / `host_revoked`.

The enrollment flow becomes one transport: connect with pending trust; authenticate (the
publickey userauth signature is bound to this session's exchange hash, so a man in the
middle cannot forward it); receive the node's proof; check it against `host_key()`,
`session_id()` and the operator's out-of-band fingerprint; `accept_host` or `close`. The
app's check is the trust decision, and it runs AFTER the key is known rather than inside the
handshake, which is why this is not the rejected callback.

**Docs.** docs/ssh.md §3 gains "Pending trust": a pending connection is untrusted until the
app's check passes, and nothing the server says before that check is evidence. Fix the
drift: rewrite ssh_module_design.md §10 to what is built; add `key_blob` to the Lua connect
comment (ssh.lua:562-564).

**Test.** Lua + JS stdlib suites (`test_ssh_hostkey*`, `test_ssh_transport*`): pending trust
connects to an unknown key, `host_key()` equals the presented blob, second use refused,
`accept_host` lands the key in a memory store and refuses a second time; a rekey with a
different key fails `host_changed_midsession`. `tests/e2e_ssh_tunnel.sh` live-sshd phase:
one pending connection, a command run, the key then accepted into a `file_store`.

## 11. Gap 3: a per-user state directory for `app.main` tools

**Today.** A built binary's app directory is its working directory (`serve_cli.c` ~:443-465,
`serve.c` ~:972-1050, `realpath(".")`), every `fs` grant is relative to it, and
`hl_sandbox_apply` refuses an app directory that is `/` or `$HOME` or above. So a CLI must
be run from a dedicated directory, and `cd ~ && hsctl status` is refused outright. The one
precedent for a non-app path is the runtime cache: `hl_hull_cache_dir()` (`shared/cache_dir.c`)
is trusted (owned by the user, not group/world writable, parents too) and auto-granted rwc
by Seatbelt and unveil.

**Manifest.**

```lua
app.manifest({
    state = { name = "hsctl" },          -- JS: state: { name: "hsctl" }
    fs = { read = { "@state/keys/" }, write = { "@state/" } },
    databases = { named = { default = "@state/data.db" } },
})
```

`name` is `[a-z0-9][a-z0-9._-]{0,63}`, no `..`. The directory resolves to
`$XDG_STATE_HOME/<name>`, else `<home>/.local/state/<name>`, Windows `%LOCALAPPDATA%\<name>`;
home through `hl_host_home` (audit 20: never a raw `$HOME`). Created 0700 with mkdir-p,
then held to the cache dir's trust check (`cache_dir_trusted`, lifted into `shared/`);
`HULL_STATE_DIR=/abs/path` overrides it under the same `hl_host_path_too_broad` /
`hl_host_path_covers_home` / inside-`~/.hull` refusals as `HULL_CACHE_DIR`. Resolution
happens before the sandbox applies, in every entry point (serve, serve_cli, app_context).

**Policy: a second root.** `hl_fs_policy_compile` opens a second anchored fd, `state_fd`.
`hl_fs_grant_parse` and `split_caller` (`cap/fs_policy.c`) recognise a first component of
exactly `@state` and route the rest to that root; `@` is otherwise refused as a first
component (the reserved-name reader that refuses `.hull` gains `@*`), so no app-relative
path can collide. Everything below the prefix is the existing grammar and the existing
resolver (`fs_resolve.c`), with the state root registered through `hl_fs_reserve_root` so
the `.hull` and symlink rules hold there too. Every capability that selects through the fs
policy gets `@state/` for free: `fs.*`, `compute.stream` output, blob roots, `db.open` file
DSNs, `tar`.

**Database.** `confine_manifest_dsn` (`cap/db_registry.c`, audit 34) and `db.open`'s file
check accept `@state/<path>` and confine it to the state root (same symlink / sidecar rules).
`-d` is unchanged. With no `-d` and a `@state/` default, `hull migrate` and the agent open
the same file; `hl_default_db_path_check` runs on it.

**Kernel sandbox.** Seatbelt: one `subpath` param, read+write, like `HULL_CACHE`; `.hull`
under it denied last as for the app root. unveil: `rwc` on the directory. No finer grant:
the state root is the app's own, as the app directory is.

**Working directory.** A BUILT `app.main` binary whose manifest declares `state` and has NO
app-relative fs grant and NO app-relative file DSN (default included) is "rootless": the
runners skip the app-directory read grant and the `/` / `$HOME` refusal, since the embedded
VFS (`hl_vfs_embedded`) needs nothing from disk. `app_dir` is still set (to the working
directory) for messages and `realpath` of the entry, but nothing is granted on it. Any
app-relative grant makes the app rooted again, with today's rules. Dev mode (`hull app.js`)
is always rooted: it reads source from disk.

**`~/.ssh`.** By design, no. The state directory is where an operator-owned key goes, and
hsctl cannot read a `<path>` argument outside its roots either, so the import reads the key
from STDIN (`cat ~/.ssh/id_ed25519 | hsctl key import`) and writes it with
`fs.write(..., { private = true })` under `@state/keys/`. This is written up in the hsctl
answer, not in Hull.

**PRs.** (a) resolver + policy second root + sandbox grant + `fs` bindings, with
`HULL_STATE_DIR`; (b) DSN confinement + `-d`-less default + migrate/agent; (c) rootless
built binaries. Each with its e2e: `tests/e2e_state_dir.sh` (both runtimes: write/read under
`@state/`, an `@other/` grant refused at load, `..` after the prefix refused, DB under the
state dir migrated, the binary run from `$HOME` and from `/`, `HULL_STATE_DIR` too broad
refused with the WARN).

## 12. Gap 8: release

#1025 (`b1761b20`) is on main and in no tag; v0.18.2 is the commit before it. v0.18.3 is cut
once §5-§8 have landed (decision, §1), with the CHANGELOG `[Unreleased]` section carrying all
of them. hsctl then bumps `HULL_VERSION` in its CI and drops its gap 8 workaround.

## 13. What hsctl changes, per gap, once released

| Gap | hsctl change |
|-----|--------------|
| 1 | `remote.describe` prints `e.detail`; the `--via cloudflare` note in its README stays |
| 2 | `trust.key(true)` writes with `{ private: true }`; the `state/` mode check becomes a one-time repair hint |
| 3 | manifest `state = { name = "hsctl" }`, grants under `@state/`, `data.db` under it, `hsctl key import` from stdin; README "fleet directory" section goes |
| 4 | enrollment uses `pendingTrust()` + `hostKey()` + `sessionId()`; adoption may stay two-connection or move to the same flow |
| 5 | `users: ["$HSCTL_LOGIN"]` with `HSCTL_LOGIN` in `env`, default documented |
| 6 | `hsctl run` writes the remote's bytes straight through; `--json` keeps base64 |
| 7 | `pair start` traps `ctx.signal` and stops both ranks within the grace |
| 8 | CI pin to v0.18.3 |
| 9 | none until §11 lands: `state/` is pre-created 0755 by the sandbox, so the mode check stays a repair hint (`chmod 700 state`) rather than something the app can satisfy; `@state/` (created 0700) retires it |

**Status after v0.18.3 (2026-11-08).** hsctl pinned CI to v0.18.3 and closed gaps 1, 2, 5,
6 and 8 as above. Gaps 3, 4 and 7 stay open. hsctl reported one new gap, 9, below.

## 14. Gap 9: the sandbox pre-creates write-grant directories with 0755

**Today.** `hl_sandbox_resolve_grant` (`src/hull/sandbox.c`) walks every `fs.write` grant
before unveil / Seatbelt so the kernel grant can be canonicalised, and a `dir/` grant whose
directory does not exist yet is made on the spot with `mkdir(buf, 0755)` - before the app's
first line runs. So an app that writes its first secret with `fs.write(path, bytes,
{ private = true })` under a `state/` grant gets a 0600 file inside a 0755 `state/`: the
private-dirs bit of §8 applies only to parents the resolver creates during that write, and
the granted root is never one of them. Nothing secret is exposed (the file is 0600, and a
sibling account can at most list the directory), but a tool that checks its state
directory's mode, as hsctl does, cannot satisfy that check from app code, and §8's claim
that it could was wrong.

**Resolution.** By the `@state/` root (§11): it is created 0700 with mkdir-p, by Hull,
before the sandbox, and a tool's state no longer lives in a write grant under the working
directory at all. hsctl keeps its mode check as a one-time repair hint until then. A
narrower fix - pre-creating a write grant 0700 - is NOT taken: a grant does not say whether
its contents are private (non-goal: "a per-path secret attribute on `fs.write` grants"), and
a 0700 `uploads/` or `static/` root would break a reverse proxy or a second user reading the
app's own output.
