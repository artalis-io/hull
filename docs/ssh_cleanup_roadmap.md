# `hull/ssh` cleanup roadmap

What is still missing for `hull/ssh` (the Lua SSH-2 client, its C stream
binding, and the async/net layers under it) to be safe, secure and usable,
plus the architecture and clean-code debt found in the same review.

Source: a read-only review of `main` after #581 (the SSH audit fixes), across
three passes: the Lua protocol stack, the C layer, and the API / architecture.
Every item in groups 1 and 2 was confirmed against the code; items marked
*(reported)* were found by the review and not yet re-traced line by line.

User guide: [`ssh.md`](ssh.md). Design record: [`ssh_module_design.md`](ssh_module_design.md).

## Status

| Group | Theme | State |
|---|---|---|
| 1 | Memory safety and data integrity | done (`fix/ssh-lifetimes-and-sftp`); item 2 has no direct test, see below |
| 2 | Protocol correctness | done (`fix/ssh-protocol-correctness`); see below for two behaviour changes |
| 3 | Timeouts and the error model | done (`fix/ssh-timeouts-and-errors`); see below |
| 4 | Usability: SFTP, trust store, algorithms | SFTP + trust store done (`feat/ssh-sftp-files-and-known-hosts`); ECDSA/RSA host keys and RSA user keys done |
| 5 | Architecture, DRY, clean code | open |
| 6 | Tests and docs | open |

## What is already solid

Recorded so nobody re-audits it: the host key is verified before auth, with
the Ed25519 signature checked over the exchange hash; unknown and changed keys
are hard errors and the key is pinned across rekeys; `accept_new` never
overwrites. The KDF and exchange hash are correct; an all-zero X25519 secret
is rejected in C. GCM nonce handling, wrap and the 1 GiB rekey trigger are
right; padding is checked after authentication. The wire readers bounds-check;
private keys are fully validated, bcrypt rounds are capped, `passphrase_env`
keeps the passphrase out of Lua. `partial_success` is not success. The module
layering has no cycles, and the manifest gate (host, port, user) is enforced in
C.

## Group 1: memory safety and data integrity

1. **Close leaves the watcher registered.** `hl_net_stream_close`
   (`cap/net_stream.c`) closes the descriptor without `watcher_del` and leaves
   `io_mask` set; `cancel` / `maybe_release` then call `watcher_del` with the
   invalidated fd, which removes nothing. A close with a read parked (the
   normal SSH close, and `__gc`) leaves `{old fd, io_ready, freed stream}` in
   the table. Poll backend: fd reuse (any `fs` open) dispatches into freed
   memory. Keel: the stale node makes the next stream on that fd hang. The
   POLLNVAL comment in `async/poll.c` claiming no consumer does this is wrong.
2. **Cancel of an attached park frees what the stream still points at.**
   `ssh_park` (`runtime/lua/mod_ssh.c`) creates the ctx with
   `hl_async_on_cancel`, which frees it; nothing clears `o->ctx` or retracts
   the stream op. A client disconnecting while a handler waits on SSH leaves
   the next SSH bytes resuming freed memory.
3. **Two parks on one stream.** `ssh_park` never checks `o->ctx`. A second
   coroutine overwrites it (the first is orphaned), and if the backend then
   refuses the second suspend, the ctx is freed while Keel still holds its
   `KlAsyncOp`.
4. **SFTP replies are not matched to requests.** Error paths in `Sftp:read` /
   `Sftp:list` / `Sftp:write` send CLOSE without reading its status, and no
   reply id is compared, so every later reply shifts by one. A `write(C)` can
   receive another file's handle and write into that file.
5. **Peer-declared packet length is used before it is bounded.**
   `Transport:read_packet` calls `fill(n + 4)` / `fill(frame_size(n))` before
   any check, and the binding's `read` allocates whatever size is asked. A
   length of `0xFFFFFFFF` (pre-auth, or by a MITM since the GCM length is
   cleartext) forces a multi-GiB allocation attempt.

**How group 1 was fixed.** Each item has a regression test verified to fail
without its fix, except item 2:

- 1: `io_unwatch` removes the watcher before any close; `HlNetStreamConfig`
  gained an optional `backend` so `test_net_stream` covers the poll backend
  inside a Keel-linked binary.
- 2: the HTTP-attached ctx carries its own `on_cancel` that detaches and closes
  the stream before the ctx is freed. **Not directly tested**: it needs a live
  Keel connection the unit harness does not have; an HTTP-server e2e that
  aborts a request parked on SSH is the missing test (group 6).
- 3: `read` / `write` refuse a second waiter ("busy"); the stream op is armed
  before the HTTP suspend so a failure never unwinds Keel's op.
- 4: `Sftp:request` allocates the id and refuses any other reply;
  `close_handle` reads CLOSE's status on every path. First tests for the SFTP
  client itself.
- 5: `read_packet` lets the parser vet the length before waiting; reads are
  capped at 32 KiB in Lua and at the stream's read cap in C.

Found on the way: two `lua_ssh_bridge` denial tests passed without reaching
the rules they named (the policy was never wired, and the fallback message
contains "hosts" and "users"). They now apply the manifest as `serve_cli.c`
does and assert the exact reason.

## Group 2: protocol correctness

6. **Client-started rekey reads back what it just deferred.** `run_kex` defers
   channel data until the peer's KEXINIT, then `expect()` and the wrong-guess
   discard go through `next_message`, which returns deferred messages first:
   "expected KEX_ECDH_REPLY but the peer sent message 94". The comment above
   `next_message` says the opposite. Use `read_message` inside the exchange.
7. **`want_reply` channel requests are never answered.** OpenSSH's
   `ClientAliveInterval` (`keepalive@openssh.com`, want_reply=1) then
   disconnects long `exec` / SFTP sessions. Reply `CHANNEL_FAILURE`. A
   server-initiated `CHANNEL_OPEN` raises instead of getting
   `CHANNEL_OPEN_FAILURE` (RFC 4254 5.1).
8. **`Sftp:close()` does not drain its channel.** The peer's CLOSE / EOF /
   exit-status is left for the next `exec` to trip over. Reuse `drain_channel`.
9. **Through a relay, the grant does not constrain the destination.** The
   manifest checks `opts.host`; the relay routes by
   `Cf-Access-Jump-Destination`, which the app writes freely in
   `tunnel.headers`. Generate that header in `ssh.lua` from `host:port` and
   refuse a caller-supplied one.
10. **The raw stream is reachable from app code.** `conn.t.stream` is a field;
    `read` / `write` / `close` skip the `caller_is_stdlib` check that `connect`
    has. `conn.t` also exposes `authenticate` / `send_packet`. Keep the
    transport in a private weak-keyed table and gate all stream methods.
11. **Lower severity:**
    - trust store keyed by host name only (ignores port; not case-normalized);
    - SFTP status text returned unsanitized (use `wire.safe_name`);
    - SFTP write chunks not sized to the peer's window / max packet; outbound
      data not capped at our own 32 KiB;
    - strict KEX: no sequence numbers tracked, pre-KEXINIT IGNORE/DEBUG skipped
      in strict mode, `strict_kex` recomputed per rekey. Harmless with GCM,
      required before ChaCha20-Poly1305 or an EtM mode;
    - `safe_text` passes 8-bit CSI (0x9B) and bidi overrides; DISCONNECT text
      keeps `\n`;
    - `opts.offer` not validated against what is implemented;
    - ws-stream: `Upgrade` / `Connection` response headers not checked, NUL
      not rejected in `no_crlf`;
    - *(reported)* TCP + TLS can take twice `connect_ms`; a failed TLS
      handshake may spin; `read` drops buffered bytes on a reset; `close`
      drops the queued send buffer though `net_stream.h` promises a drain;
      port / timeout narrowed to `int` without a range check; `--no-ca-bundle`
      silently covers the relay too (warn); bcrypt cap of 2^20 rounds still
      allows an hour-long stall.

**How group 2 was fixed.** Every item below has a regression test verified
to fail without its fix unless noted.

- 6: the key exchange reads the wire (`read_message`), not the deferred queue.
- 7: `Transport:channel_message` answers `want_reply` requests with
  `CHANNEL_FAILURE` and replaces three parse-then-handle copies; a
  server-initiated `CHANNEL_OPEN` gets `OPEN_FAILURE`.
- 8: `Transport:close_channel` ends every channel (exec's three paths and
  `Sftp:close`), sending CLOSE once. **Severity corrected:** the review said the
  next `exec` would fail; `open_session` skips up to 16 stray messages, so it
  survived a normal sftp close. The exposure was a longer tail or another reader.
- 9: the relay's destination header is written from the granted `host:port`;
  a caller-supplied one must match. `tunnel.destination_header` names another
  header, `false` sends none (documented as unconstrained).
- 10: connections and sftp sessions are field-less handles over a private
  weak table; the stream binding checks its caller on read / write / close.
- 11, done:
  - sanitisers decode UTF-8 and drop C1, raw 0x9B and bidi overrides; SFTP
    status text and DISCONNECT text use `safe_name`;
  - host keys are stored as `host` / `[host]:port`, lower-cased;
  - SFTP messages are split to the window / packet size, and `sendable` caps
    at 32 KiB;
  - strict KEX: first-packet rule, latched from the first exchange,
    sequence numbers kept and reset at NEWKEYS;
  - `opts.offer` validated against the implemented set;
  - ws-stream checks `Upgrade` / `Connection` and refuses NUL;
  - net_stream: a failed handshake is not re-driven, buffered bytes are read
    before a stored error, the TLS handshake shares the connect budget, close
    flushes what the socket takes (header corrected: it does not linger);
  - port / via.port / timeout_ms range-checked instead of wrapped;
  - a WARN when a relay's certificate goes unverified (`--no-ca-bundle`).
- Not directly tested: the shared TLS budget and the close-time flush (not
  observable deterministically over loopback) and the WARN (log output is
  not captured).

**Behaviour changes for callers.**
- `ssh.accept_host` / `ssh.forget_host` take an optional trailing `port`
  (default 22). An entry persisted for a non-22 port, or under a mixed-case
  name, reports `host_unknown` once and must be accepted again.
- `timeout_ms = 0` used to mean "the default"; it is now refused. Omit it.

**Declined.** Lowering `HL_BCRYPT_MAX_ROUNDS` below 2^20: #581 chose that cap
so no key `ssh-keygen` writes is refused. A lower cap trades that for a shorter
worst-case stall and is a policy call, not a defect.

Not in any group yet: the SFTP receive buffer grows by concatenation
(quadratic for large reads), and a rekey may hold up to 4096 deferred packets.
Both are bounded; they belong with group 4's streaming SFTP work.

## Group 3: timeouts and the error model

- **No deadline after connect, no keepalive, no cancellation.** `timeout_ms`
  covers only TCP connect. A silent server hangs the coroutine forever (one bad
  host stalls a fleet sweep). `hl_net_stream_deadline` has no callers and
  stores a relative time where both backends expect an absolute one. Add a
  per-read idle timeout, a whole-handshake deadline, per-`exec` deadline, and
  `keepalive@openssh.com`.
- **One error shape everywhere.** Today only `connect` returns `{code=...}`:
  - `exec` / `sftp` / `rekey` raise on I/O failure; SFTP returns bare strings;
  - `privatekey.load` runs outside `step()` and raises out of `connect`;
  - `accept_host` raises when a key already exists;
  - a host that is down is reported as `denied`, because the binding returns a
    plain string and `ssh.lua` labels any code-less failure `denied`. Add
    `connect_failed` and `timeout`.

**How group 3 was fixed.**

- **Transport** (`cap/net_stream`): two bounds on a parked read or write,
  neither fatal. `hl_net_stream_deadline` (absolute, for a phase; it existed
  unused and stored a relative time where the backends expect an absolute
  one) and the new `hl_net_stream_wait` (per park, for liveness). An expiry is
  reported once, as `HL_NET_E_DEADLINE` / `HL_NET_E_TIMEOUT`, and only if the
  retry would otherwise wait again.
- **Binding**: `stream:deadline(ms)` / `stream:wait(ms)`; every error carries a
  short code (`denied`, `connect_failed`, `timeout`, `deadline`, `closed`,
  `io_error`); ws-stream passes both through.
- **Liveness** (`Transport:quiet`): each read waits at most `keepalive_ms`
  (30 s); after authentication a silence sends `keepalive@openssh.com`, whose
  reply is traffic and resets the count; `idle_ms` (60 s) of nothing at all
  ends the connection with `timeout`, and `keepalive_max` (3) does when
  `idle_ms = 0`. A dead connection fails every later call at once.
- **Deadlines**: `timeout_ms` (default 30 s) now covers connect, key exchange
  and userauth; `exec` takes `timeout_ms` and on expiry closes the channel
  under its own 5 s bound, keeping the connection.
- **One error shape**: `exec`, `sftp`, every SFTP operation, `rekey` and
  `accept_host` return `nil, {code, detail}`; a bad key file is `bad_key` /
  `bad_passphrase` / `passphrase_required`; SFTP statuses by name; a host that
  is down is `connect_failed`, not `denied`. The caller's own callback error is
  still raised, unchanged. `docs/ssh.md` lists every code.

Behaviour changes: `exec` / `sftp` / `rekey` no longer raise on I/O failure;
SFTP errors are tables, not strings; `accept_host` returns `already_trusted`
instead of raising; the default connect budget covers the whole handshake and
is 30 s (it was 10 s for the TCP connect alone); a TLS certificate refusal is
`connect_failed` (the e2e expected `denied`).

## Group 4: usability

- **SFTP coverage.** Expose `stat`, `lstat`, `mkdir`, `rmdir`, `remove`,
  `rename`, `setstat` (the codec already builds them). Streaming read/write
  instead of whole-file-in-memory (read cap is 16 MiB). Pipeline requests;
  today it is one 32 KiB read or 16 KiB write per round trip.
- **Trust store.** Key by `[host]:port` like OpenSSH; ship a persistence
  helper and `known_hosts` / `ssh-keyscan` import; align the store interface
  with `hull.kv` (`get` / `set` / `delete`) so a kv handle can back it.
- **Algorithms.** `rsa-sha2-256/512` user keys (many fleets need them);
  certificates.
- **Document** parallel connections (is concurrent `connect` in `app.main`
  supported?), and add PTY/shell/env, jump hosts and certificates to "Not
  implemented". Document `list`'s second return and `read`'s `max`.

**How group 4 was done so far.** Decisions taken with the maintainer: SFTP as
file handles, the trust store in `known_hosts` format, RSA in its own PR.

- The SFTP client moved to `hull.ssh.sftp_client` (the first of group 5's
  `transport.lua` splits).
- Operations: `stat`, `lstat`, `mkdir`, `rmdir`, `remove`, `rename`,
  `chmod`, `setstat`; modes as octal strings.
- `sftp:open(path, mode, opts)` -> file with `read` / `write` / `seek` /
  `tell` / `stat` / `close`. Requests are pipelined (32 KiB, a sliding
  window of up to 64 in flight, a read ramping from 1) and matched by id; whole-file `read` / `write`
  are loops over a handle.
- `ssh.file_store(path, { hash })` over `hull.ssh.known_hosts`: plain and
  hashed entries, patterns and markers skipped. Matched against a line hashed
  by `ssh-keygen -H` through the real HMAC-SHA1.
- `docs/ssh.md` "Not implemented" now also lists RSA user keys, jump hosts,
  PTY / shell / env and certificates.

Also done: `ssh.kv_store(kv)` over `hull.kv`, with atomic first use (the
store-interface alignment above), and `hull.kv` encryption at rest
(`kv_encryption_design.md`), which lets `ssh.kv_store` resist a backend writer
without the key.

Still open in group 4 (RSA user keys are done: `crypto.sign` + `crypto.rsa_private_pem`):
documenting parallel connections.

## Group 5: architecture, DRY, clean code

- **Split `transport.lua`** - done. The `Sftp` class was already in
  `sftp_client.lua`; the channel layer (`open_session`, the routing in
  `read_for`, `drain_channel` / `close_channel`, `exec`) is now
  `session.lua`, installed onto the Transport class so no caller changes.
  `transport.lua` keeps the connection: handshake, KEX and rekey, packets,
  liveness (1095 -> 754 lines).
- **Move capability logic out of the binding.** Done: `hl_cap_ssh_open`
  (`cap/ssh.c`) checks the destination and relay grants, refuses a TLS relay
  with no trust anchor rather than downgrading, and dials; `lua_ssh_connect`
  only parses and reports, so a JS binding cannot decide any of it
  differently. Unit-tested without a network (`tests/hull/cap/test_ssh.c`).
  Still open with the `ws-stream` move: renaming the binding to a generic
  byte-stream one (`mod_net_stream.c`).
- **Make the net policy generic.** Done: `cap/net_policy.c` is only the
  reach check (`hl_net_check_reach` -> `HlNetReach`, `HL_NET_REACH_*`, no
  wording); the SSH grants and their messages are `cap/ssh_policy.c`
  (`hl_ssh_check_connect` / `hl_ssh_check_tunnel` -> `HlSshAuth`,
  `HL_SSH_DENY_*`, `hl_ssh_auth_reason`), which maps each reach reason to the
  list that refused.
- **One connect adapter.** The `getaddrinfo` loop: done - `hl_net_resolve`
  (`cap/net_resolve.c`) is the one copy `net_stream.c`, `smtp_transport.c`
  and `db_transport.c` call (each keeps its test seam in front of it). The
  copies had drifted: `net_stream.c` dropped the IPv6 scope id, so a
  link-local destination could not connect, and only the DB copy skipped the
  resolver for a literal. Deliberately NOT merged: the KlConnectOp hooks and
  socket-provider wrappers. They sit on different loops (the async backend
  vs. a private KlEventCtx), and the SMTP and DB wrappers route through
  per-transport test providers; a shared adapter would have to abstract both,
  for ~60 lines each. The TLS rc mapping is one line per file. The CA
  bundle ladder: done - `hl_ca_trust_resolve` (`ca_trust.c`) is the one copy
  `serve.c` and `serve_cli.c` call.
- **DRY in the Lua stack.**
  - hex: done - `hull.encoding` is the one home for hex and base64;
  - fingerprint: done - `privatekey.fingerprint` delegates to `hostkey`;
  - the length-prefix peeks: done - `wire.peek_uint32`, used by the packet,
    cipher, sftp and transport framing;
  - close-and-drain: done - every path ends a channel through
    `close_channel`;
  - `userauth.sanitize_text`: done - it was an alias; `wire.safe_text` is the
    one sanitizer;
  - the short-read gatherer: done - `wire.gather` serves `Transport:fill`
    and `ws_stream`'s `_need`; each keeps its own policy (the transport
    raises at EOF and keepalives between timeouts, the tunnel reports EOF).
- **Facade hygiene.** Done: `ssh.lua` no longer re-exports `hostkey` /
  `privatekey`; `ssh.fingerprint(blob)` takes just the blob (`opts.crypto`
  is the test seam); `hostkey.verify` takes one crypto adapter (`sha256`,
  `ed25519_verify`) instead of a separate digest function. The WebSocket
  tunnel stream: done - `hull/web/ws-stream` (published, but no app could
  obtain a stream to give it, not a web concern, no JS twin) is now
  `hull.ssh.ws_stream`, part of `hull/ssh`; declaring the old name gets a
  fix-it. The binding keeps its name (`mod_ssh.c`): it enforces the
  `ssh.connect` grant and serves only `hull.ssh`, so a generic byte-stream
  name would describe it wrongly. The net cap bit: done -
  `hull/ssh` requires `HL_MOD_CAP_NET` (the outbound socket layer) instead of
  borrowing `HL_MOD_CAP_HTTP_CLIENT`; the compose gates (`needs_http`,
  `needs_tls`) and the pure-compute preset treat it as they treated the
  HTTP bit, since the binding and Keel still compose back the same way.
- **Dead code / wrong comments.** Done or overtaken:
  `hl_net_stream_deadline` now backs the SFTP reply deadline;
  `hl_net_stream_cancel` is used by close and by tests; `HL_NET_E_DENIED` is
  part of the error-string table. Fixed: the `net_stream.c` header (`cap/net.c`,
  "read and write land next"), the `cancel_resolve` comment (resolution runs on
  a worker before the op starts), and a `next_message` doc comment stranded
  above `defer_message` in `transport.lua` (it describes `read_message`).

## Group 6: tests and docs

Done. Live OpenSSH interop (`tests/e2e_ssh_tunnel.sh`) now covers stdin,
3 MB of streamed stdout with a key exchange sshd starts in the middle of it
(`RekeyLimit 1M`), the `max_output` refusal, a client-initiated rekey, an
SFTP round trip (write, read back, stat, list, remove), the direct path
without the relay, ECDSA / RSA host keys and ECDSA / RSA user keys. A real
`host_changed_midsession` - a rekey presenting another key - is a scripted
transport test, since sshd cannot swap its key mid-session. `ssh.md` lists
every code the stack emits; `ssh_module_design.md` §6 / §9 / §11 match the
code; `stdlib/context/ssh.md` exists and BOOTSTRAP.md / AGENTS.md list
`hull/ssh`. The items as originally recorded:

- **Tests.**
  - The SFTP *client* has no tests (only the codec does); the live sshd in CI
    enables the subsystem and never uses it.
  - Live OpenSSH interop covers only `echo` / `exit 3` through the relay: add
    the direct path, streaming `on_stdout`, `stdin`, `max_output`, a real
    rekey (`rekey_limit` exists for this), `host_changed_midsession`.
  - Done: fuzz the Lua parsers fed by the peer (`fuzz/fuzz_ssh.c`, targets
    in `fuzz/ssh_driver.lua`). Every parser listed here, plus identification,
    host key and signature blobs, known_hosts and the terminal sanitizers;
    and whole scripted connections (handshake, auth, exec, a mid-session
    rekey, SFTP, SSH over the WebSocket tunnel), which is where `cipher:open`
    runs. Runs in the `fuzz-core-security` CI job.
- **Docs.**
  - `ssh.md` error table lists 11 codes; the code emits about 25.
  - `ssh.md` says `exec` returns `nil, reason` only when the command could not
    run; I/O failure raises.
  - `ssh_module_design.md` section 9 error vocabulary, section 6 `ports` /
    `users` defaults, and the promised fuzz targets do not match the code.
  - No `stdlib/context` topic; SSH absent from BOOTSTRAP.md / AGENTS.md.

## Order

1. Group 1, each item with a regression test.
2. Group 2.
3. Group 3.
4. Group 4.
5. Group 5.
6. Group 6 (the tests may land alongside the group they cover).
