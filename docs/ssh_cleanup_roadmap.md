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
| 1 | Memory safety and data integrity | in progress (`fix/ssh-lifetimes-and-sftp`) |
| 2 | Protocol correctness | open |
| 3 | Timeouts and the error model | open |
| 4 | Usability: SFTP, trust store, algorithms | open |
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

## Group 5: architecture, DRY, clean code

- **Split `transport.lua`** (939 lines): packets + KEX stay; move the `Sftp`
  class to `sftp_client.lua` and `open_session` / `drain` / `exec` to
  `session.lua`.
- **Move capability logic out of the binding.** `lua_ssh_connect` composes the
  two relay grants and selects the TLS trust anchor; that belongs in a
  `cap/` function (e.g. `hl_cap_ssh_open`) so a JS binding does not copy it.
  The binding is then a generic byte-stream binding (`mod_net_stream.c`) that
  `ws-stream` also uses.
- **Make the net policy generic.** `hl_cap_net_auth_reason` returns SSH
  wording for generic codes; `net_policy.c` holds the `hl_ssh_*` rules. Move
  those to `cap/ssh_policy.c`; unify naming (`hl_net_*` / `hl_ssh_*` /
  `hl_cap_net_*`); rename the `HL_NET_DENY_*` enum so it does not share a
  prefix with the `HL_NET_E_*` error macros.
- **One connect adapter.** `net_stream.c` copies the KlConnectOp adapter,
  socket-provider wrappers, the `getaddrinfo` loop and the TLS rc mapping from
  `smtp_transport.c` / `tls_client.c` / the DB transport (~300 lines). The CA
  bundle ladder is also duplicated between `serve.c` and `serve_cli.c`.
- **DRY in the Lua stack.**
  - hex: `kex.lua`, `examples/ssh_fleet/app.lua`, and `crypto/_hex.lua` (which
    calls itself the one home; add `from_hex` there);
  - fingerprint: `hostkey.lua` and `privatekey.lua`;
  - the buffered short-read gatherer: `Transport:fill` and `ws-stream`'s
    `_need`;
  - `string.unpack(">I4")` in ~8 places instead of `wire.uint32`;
  - close-and-drain written twice inside `exec`;
  - `wire.safe_text` / `safe_name` vs `userauth.sanitize_text`: pick one.
- **Facade hygiene.** `ssh.lua` re-exports `hostkey` / `privatekey` despite
  its "internal" header; `ssh.fingerprint(crypto, blob)` makes callers pass a
  crypto module; `hostkey.verify` takes eight positional parameters (build one
  crypto adapter). `hull/web/ws-stream` is published but no app can obtain a
  stream to give it, it is not a web concern, and it has no JS twin: move it
  under `hull/ssh/` or a future `hull/net/`. The registry uses
  `HL_MOD_CAP_HTTP_CLIENT` as a stand-in for a net cap bit.
- **Dead code / wrong comments.** `hl_net_stream_deadline` (unused, and wrong),
  `HL_NET_E_DENIED`, `hl_net_stream_cancel` (could be static); `net_stream.c`
  header still says `cap/net.c` and "read and write land next";
  `.cancel_resolve = NULL /* resolution is inline */` (it runs on a worker);
  `net_stream.h` "graceful close: drain"; the `mod_ssh.c` "nothing suspended to
  unwind" comment; the `poll.c` POLLNVAL comment.

## Group 6: tests and docs

- **Tests.**
  - The SFTP *client* has no tests (only the codec does); the live sshd in CI
    enables the subsystem and never uses it.
  - Live OpenSSH interop covers only `echo` / `exit 3` through the relay: add
    the direct path, streaming `on_stdout`, `stdin`, `max_output`, a real
    rekey (`rekey_limit` exists for this), `host_changed_midsession`.
  - Fuzz the Lua parsers fed by the peer: `packet.parse`, `cipher:open`,
    `kexinit.parse`, `kex.parse_ecdh_reply`, `channel.parse`, `sftp.parse` /
    `parse_frame` / `decode_attrs`, `userauth.parse_response`,
    `privatekey.parse_container`, ws-stream `decode` / `parse_response`.
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
