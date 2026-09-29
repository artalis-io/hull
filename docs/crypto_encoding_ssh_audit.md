# Audit: hull.encoding, hull.crypto, encrypted kv and hull.ssh

Date: 2026-09-28, after #587-#589. A C audit (the new `utils/base64` and
`utils/hex`, the crypto bindings, the SSH stream layer and the callers moved
onto the shared codecs) and a Lua/JS audit (`hull.encoding`,
`hull.crypto.{sealbox,otp,envelope}`, encrypted kv, `hull.ssh`). Every finding
below was checked against the code. Items are ticked as their fixes merge.

Fix plan: **PR 1** crypto/encoding, **PR 2** SSH, **PR 3** missing C pieces.

## Checked and clean

- `utils/base64` and `hl_hex_decode`: capacity and overflow math, padding
  rules, every caller's buffer size; SMTP and PostgreSQL still scrub their
  secret buffers.
- SSH C layer: close and GC while a read is parked, read-size cap, write bytes
  anchored across a park, host / port / user / relay checked against the
  manifest before dialing. The SSH crypto bindings borrow their key arguments
  (no C-side copy to scrub), and a failed `gcm_open` is zeroed by mbedTLS.
- SSH protocol: host key verified before authentication, strict KEX and
  sequence reset, the GCM counter never repeats, every peer length bounded
  before a read, `passphrase_env` never becomes a Lua value.
- Lua/JS codec parity on the tricky inputs (padding, `=` mid-string,
  surrogates, overlong / truncated / 4-byte UTF-8, empty strings); constant-time
  tag comparisons in jwt, csrf, totp and envelope; registry closure for every
  user of `hull.encoding`; no references left to the removed crypto codecs.

## PR 1: crypto and encoding

- [x] **High** - JS `hull:encoding` builds output with `out +=`, which is
  O(n^2) in the vendored QuickJS: `OP_add_loc` duplicates the string before
  `JS_ConcatString`, so the in-place append (refcount 1) never runs. Hits every
  JS kv/cache value on the SQL backends, every encrypted value, attachments.
  Collect pieces and join once.
- [x] **Medium** - `jwt.lua` decodes the unauthenticated header (and payload)
  with `json.decode`, which raises on bad JSON; `jwt.verify` then raises and
  the auth middleware answers 500 to any client sending `ew.e30.AA`. JS
  rejects cleanly. Same in `envelope.lua` (after the tag check).
- [x] **Medium** - JS `sealbox.keyring` accepts `current: "1"`; `isCurrent`
  compares with `===` against the numeric version, so `rekey()` re-seals
  everything every time. Key ids from env are strings. Also: ids go through
  `Number()`, so `""`, `" 1"`, `"0x10"`, `"1e0"` are accepted.
- [x] **Medium** - `kv:rekey()` passes `opts.ttl` (normally nil) to `cas`, so
  a re-sealed value loses its TTL. The design says TTL is unchanged.
- [x] **Medium** - `cache.open{ encrypt = ... }` is silently ignored (plaintext).
  Refuse it with `invalid_argument`.
- [x] **Medium** - JS `hmacSha256`, `hmacSha256Verify`, `ed25519Sign`,
  `ed25519Verify`, `sha512`, `auth`, `authVerify`, `box`, `boxOpen` and
  `constantTimeEq` read the message only with `JS_ToCStringLen`: binary bytes
  are UTF-8-inflated (a different result from Lua) and an ArrayBuffer becomes
  `"[object ArrayBuffer]"`. Take buffers like `sha256` / `hmacSha1` /
  `secretbox` do.
- [x] **Low** - base64 decoding (C, Lua, JS) accepts non-zero trailing bits
  (`Zh==` = `Zg==`), which makes asymmetric JWS signatures malleable; base32
  accepts impossible lengths.
- [x] **Low** - `sealbox` does not type-check `value` / `context` / `blob`
  (JS seals a Uint8Array as the text `"1,2,3"`); "no context" is not bound
  into the frame (document: never share a keyring between context and
  no-context use, or tag it).
- [x] **Low** - parity: JS `otp.step` does no validation (`period || 30`);
  OAuth state compared with `~=` / `!==`; encrypted `cas` compares plaintext
  with `~=`; corrupt kv rows reported as `invalid_argument`.
- [x] **Low** - stale comments (`encoding.lua` "as hull.crypto's decoder",
  `jwt.js` "Lua sibling gets from C", the registry comment omits utf8,
  `hostkey.lua` mentions `kex.to_hex`); TOTP re-implements keyring validation.
- [x] **Doc** - JS secrets are bytes (one character per byte), Lua secrets are
  their bytes: `"pässword"` gives different HMAC keys, and a JS secret with a
  character above 0xFF now throws (it used to be truncated). Correct the claim
  in `auth-flows.lua` and the plan doc; point text secrets at `utf8.encode`.
- [x] **Doc** - `crypto.constant_time_eq` / `constantTimeEq` is missing from
  `docs/app_api_reference.md`.

## PR 2: hull.ssh

- [x] **Medium** - `file_store` treats any read error as an empty file and
  rewrites the whole file from that. Can wipe the trust store. Now only
  `not_found` is an empty store; any other read error raises `store_failed`.
  The write is atomic too, now that `fs.write` is (temp file + rename).
- [x] **Medium** - `@revoked` lines are skipped, not enforced: a key revoked
  and also on a plain line is trusted. Now refused as `host_revoked`, on
  connect and on `accept_host`.
- [x] **Medium** - keepalive replies count toward `read_message`'s 256-message
  "no progress" limit: a silent exec / SFTP wait dies after about 128 minutes.
  The count is gone; the idle and keepalive bounds already end a dead wait.
- [x] **Medium** - a streamed `exec` falls through to success after 1,000,000
  messages. The loop is now bounded by `timeout_ms` alone.
- [x] **Medium** - `sftp_client.open` leaves the channel open when SFTP fails
  to start.
- [x] **Low** - the deferred queue during a rekey is capped by count, not
  bytes (now 8 MiB, and only channel messages may be deferred); strict KEX is
  enabled on the server's marker alone and a caller can drop `kex-strict-c`
  (the marker is now always sent); exit-signal / `error_message` / SFTP
  `longname` reach the app unsanitised; `accept_host` can raise outside
  `guard`; key-error codes guessed from message text (now raised with a code);
  `opts.software` unvalidated (now `bad_software`, via `build_ident`); `list`
  has no total cap and stops silently (now `too_large` past 100000 entries, and
  `bad_reply` for an empty non-EOF reply); ws-stream output buffer copies
  quadratically (reads are now served from an offset); `drain_channel` gives up
  silently (it now closes the connection).

## PR 3: missing C pieces

- [x] Script bindings to the C codecs (`hl_base64_*`, `hl_hex_*`) for large
  values (also fixes Lua heap use: a ~3 MB kv value exceeds the 64 MB heap).
  Internal `hull.encoding._native` / `hull:encoding:_native`; `hull.encoding`
  takes them for hex and strict base64 and keeps its pure codecs for reasons,
  lenient decoding and a vanilla Lua state. A differential test holds the C
  and pure decoders to the same accepted set.
- [x] The two remaining hex encoders (`release_io.c`, `cache_common.c`) and
  the four private hex-digit parsers (Postgres `bytea`, three DSN
  percent-decoders) onto `utils/hex` (`hl_hex_digit` is now public).
- [x] A fuzz target for `hl_base64_decode` / `hl_hex_decode`
  (`fuzz/fuzz_encoding.c`, asserting canonical re-encoding and round trips).

## Review 2: duplication, placement, SSH throughput

A second pass after #590-#592: is any crypto or encoding implemented outside
`hull.crypto` / `hull.encoding` (and their C homes, `cap/crypto*` and
`utils/`), is anything duplicated, and is the SSH stack fast where it matters.
Findings come from reading the code; SSH throughput was not measured.

**Crypto placement: clean.** Every stdlib caller (pwned, csrf, totp, jwt,
sealbox, the SSH stack) delegates to `hull.crypto`; no primitive is
reimplemented in Lua, JS or C outside `cap/crypto*`.

Fix plan: **PR 4** encoding consolidation, **PR 5** SFTP pipelining, **PR 6**
a byte-only crypto API (a deliberate breaking change).

### PR 4: encoding consolidation

- [x] Percent-encoding has no home. Seven encoders (oauth, totp and
  attachment-serve in Lua and JS, plus `examples/hypermedia_photos`) and six
  decoders (`form`, plus private parsers in `csrf` and `auth-flows`, in each
  runtime). Add `hull.encoding.url` and move them all onto it. Done; the
  JS twin of attachment-serve also lost a private UTF-8 encoder.
- [x] Five percent-decoders in C: two identical query-string decoders in
  `runtime/{lua,js}/bindings.c` (each with its own inline hex-digit parse)
  and the Postgres, MySQL and Valkey DSN decoders. Add `utils/url.h` (header-only) with a
  strict mode (DSNs) and a form mode (`+` is a space, a malformed escape is
  kept), and move all five onto it.
- [x] `base64url(random(n))` / `hex(random(n))` is written out at 14 sites
  (jobs, auth-flows, attachment, session, oauth, uuid, an example). Added
  `crypto.random_token(n [, "hex"])` (JS `randomToken`), in C over the shared
  codecs.
- [x] `hostkey.fingerprint` and `privatekey.fingerprint` are the same
  function; `uuid` formats its own hex; two SSH test files carry private hex
  helpers.

### PR 5: SFTP throughput

- [x] At most 8 x 32 KiB requests in flight, sent in lock-step batches (send
  all, wait for all), so the pipe drains between batches: under 256 KiB per
  round trip, about 5 MB/s over a 50 ms relay. OpenSSH keeps 64 in flight and
  the 2 MiB channel window already allows it. Move reads and writes to a
  sliding window. Done: up to 64 in flight, topped up per reply; a READ reply
  longer than the request is now `bad_reply`.

### PR 6: byte-only crypto API (breaking)

- [x] The older bindings (`hmac_sha256`, `hmac_sha1`, secretbox, box,
  ed25519, x25519) take keys and return results as hex; the newer ones
  (`gcm_seal` / `gcm_open`, `aes256ctr`, bcrypt) take bytes. 17 stdlib files
  hex-encode keys going in and hex-decode results coming out, and the SSH
  stack carries adapters to undo it. Make the whole API take and return
  bytes, and delete the round trips. A clean break, with no hex fallback.
  Done in both runtimes (JS outputs are ArrayBuffers; keypairs
  `{ publicKey, secretKey }`). Stored and on-the-wire hex formats (token
  hashes, CSRF MACs, envelope tags, ETags, idempotency fingerprints, audit
  fingerprints, package.sig / platform.sig) are unchanged: those callers now
  encode explicitly. The private `snprintf` hex loops in both bindings and
  `kex.raw_hash` are gone.

## Missing, not yet planned

- **Crypto / encoding:** HKDF is done (`hull.crypto.hkdf`, RFC 5869
  vectors in both runtimes). Open: a keyring from env (a C-held keyring, see
  `kv_encryption_design.md`); reason strings on JS decode failures; one JS error shape (encoding
  returns `null`, envelope `[v, err]`, sealbox `{ ok, ... }`).
- **SSH, roadmap still open:** RSA user keys; documenting parallel
  connections; group 5 (split `transport.lua`, fingerprint duplicated,
  two buffered readers, raw `string.unpack(">I4")`, facade clutter, dead code);
  group 6 (live rekey / stdin interop tests, context docs). The group 6 fuzz
  targets are done: `fuzz/fuzz_ssh.c`.
- **SSH, not on the roadmap:** only ed25519 host keys (servers with only RSA
  or ECDSA host keys are unreachable); one cipher; one key per connection with
  no fallback; one operation at a time per connection (an exec and an open
  SFTP session share it safely, but two coroutines driving one connection at
  once are not supported); exec stdin capped at 128 KiB and not streamable; no
  time-based rekey (deliberate, but unrecorded).

See also [`encoding_consolidation_plan.md`](encoding_consolidation_plan.md),
[`kv_encryption_design.md`](kv_encryption_design.md) and
[`ssh_cleanup_roadmap.md`](ssh_cleanup_roadmap.md).
