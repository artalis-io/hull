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

- [ ] **High** - JS `hull:encoding` builds output with `out +=`, which is
  O(n^2) in the vendored QuickJS: `OP_add_loc` duplicates the string before
  `JS_ConcatString`, so the in-place append (refcount 1) never runs. Hits every
  JS kv/cache value on the SQL backends, every encrypted value, attachments.
  Collect pieces and join once.
- [ ] **Medium** - `jwt.lua` decodes the unauthenticated header (and payload)
  with `json.decode`, which raises on bad JSON; `jwt.verify` then raises and
  the auth middleware answers 500 to any client sending `ew.e30.AA`. JS
  rejects cleanly. Same in `envelope.lua` (after the tag check).
- [ ] **Medium** - JS `sealbox.keyring` accepts `current: "1"`; `isCurrent`
  compares with `===` against the numeric version, so `rekey()` re-seals
  everything every time. Key ids from env are strings. Also: ids go through
  `Number()`, so `""`, `" 1"`, `"0x10"`, `"1e0"` are accepted.
- [ ] **Medium** - `kv:rekey()` passes `opts.ttl` (normally nil) to `cas`, so
  a re-sealed value loses its TTL. The design says TTL is unchanged.
- [ ] **Medium** - `cache.open{ encrypt = ... }` is silently ignored (plaintext).
  Refuse it with `invalid_argument`.
- [ ] **Medium** - JS `hmacSha256`, `hmacSha256Verify`, `ed25519Sign`,
  `ed25519Verify`, `sha512`, `auth`, `authVerify`, `box`, `boxOpen` and
  `constantTimeEq` read the message only with `JS_ToCStringLen`: binary bytes
  are UTF-8-inflated (a different result from Lua) and an ArrayBuffer becomes
  `"[object ArrayBuffer]"`. Take buffers like `sha256` / `hmacSha1` /
  `secretbox` do.
- [ ] **Low** - base64 decoding (C, Lua, JS) accepts non-zero trailing bits
  (`Zh==` = `Zg==`), which makes asymmetric JWS signatures malleable; base32
  accepts impossible lengths.
- [ ] **Low** - `sealbox` does not type-check `value` / `context` / `blob`
  (JS seals a Uint8Array as the text `"1,2,3"`); "no context" is not bound
  into the frame (document: never share a keyring between context and
  no-context use, or tag it).
- [ ] **Low** - parity: JS `otp.step` does no validation (`period || 30`);
  OAuth state compared with `~=` / `!==`; encrypted `cas` compares plaintext
  with `~=`; corrupt kv rows reported as `invalid_argument`.
- [ ] **Low** - stale comments (`encoding.lua` "as hull.crypto's decoder",
  `jwt.js` "Lua sibling gets from C", the registry comment omits utf8,
  `hostkey.lua` mentions `kex.to_hex`); TOTP re-implements keyring validation.
- [ ] **Doc** - JS secrets are bytes (one character per byte), Lua secrets are
  their bytes: `"pässword"` gives different HMAC keys, and a JS secret with a
  character above 0xFF now throws (it used to be truncated). Correct the claim
  in `auth-flows.lua` and the plan doc; point text secrets at `utf8.encode`.
- [ ] **Doc** - `crypto.constant_time_eq` / `constantTimeEq` is missing from
  `docs/app_api_reference.md`.

## PR 2: hull.ssh

- [ ] **Medium** - `file_store` treats any read error as an empty file and
  rewrites the whole file from that; the write is not atomic. Can wipe the
  trust store. Distinguish "not found", raise `store_failed`, write + rename.
- [ ] **Medium** - `@revoked` lines are skipped, not enforced: a key revoked
  and also on a plain line is trusted.
- [ ] **Medium** - keepalive replies count toward `read_message`'s 256-message
  "no progress" limit: a silent exec / SFTP wait dies after about 128 minutes.
- [ ] **Medium** - a streamed `exec` falls through to success after 1,000,000
  messages.
- [ ] **Medium** - `sftp_client.open` leaves the channel open when SFTP fails
  to start.
- [ ] **Low** - the deferred queue during a rekey is capped by count, not
  bytes; strict KEX is enabled on the server's marker alone and a caller can
  drop `kex-strict-c`; exit-signal / `error_message` / SFTP `longname` reach
  the app unsanitised; `accept_host` can raise outside `guard`; key-error codes
  guessed from message text; `opts.software` unvalidated (`build_ident` unused);
  `list` has no total cap and stops silently; ws-stream output buffer copies
  quadratically; `drain_channel` gives up silently.

## PR 3: missing C pieces

- [ ] Script bindings to the C codecs (`hl_base64_*`, `hl_hex_*`) for large
  values (also fixes Lua heap use: a ~3 MB kv value exceeds the 64 MB heap).
- [ ] The two remaining hex encoders (`release_io.c`, `cache_common.c`) and
  the four private hex-digit parsers (Postgres `bytea`, three DSN
  percent-decoders) onto `utils/hex`.
- [ ] A fuzz target for `hl_base64_decode` / `hl_hex_decode`.

## Missing, not yet planned

- **Crypto / encoding:** a random-token helper (`base64url(random(n))` is
  written out about 8 times); byte-level (non-hex) crypto API; HKDF; a keyring
  from env; reason strings on JS decode failures; one JS error shape (encoding
  returns `null`, envelope `[v, err]`, sealbox `{ ok, ... }`).
- **SSH, roadmap still open:** RSA user keys; documenting parallel
  connections; group 5 (split `transport.lua`, fingerprint duplicated,
  two buffered readers, raw `string.unpack(">I4")`, facade clutter, dead code);
  group 6 (fuzz targets, live rekey / stdin interop tests, context docs).
- **SSH, not on the roadmap:** only ed25519 host keys (servers with only RSA
  or ECDSA host keys are unreachable); one cipher; one key per connection with
  no fallback; no channel multiplexing (exec while SFTP is open breaks); exec
  stdin capped at 128 KiB and not streamable; no SFTP deadlines; no time-based
  rekey (deliberate, but unrecorded).

See also [`encoding_consolidation_plan.md`](encoding_consolidation_plan.md),
[`kv_encryption_design.md`](kv_encryption_design.md) and
[`ssh_cleanup_roadmap.md`](ssh_cleanup_roadmap.md).
