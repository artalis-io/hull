# `hull.encoding`: one home for byte <-> text codecs

Status: **implemented**, in two PRs: phase 1 (the module and the stdlib), then
phases 2 and 3 (no codecs left in `hull.crypto`, UTF-8, the examples, and C).

## Why

A review of the stdlib found the same codecs written out again and again:
about a dozen hex encoders and six hex decoders, five standard-base64
implementations, a second base64url decoder, and a private base32 in TOTP.
They agreed on their output, but not on what they accepted: one base64 decoder
stopped at the first `=` and ignored the rest, another skipped `=` anywhere;
one hex decoder took `" f"` as `0x0f`, another took `"-1"` as `0xFFFF`; one JS
base64url decoder accepted padding and `+/` that the Lua side rejected, so a
token could verify in one runtime and not the other. And the JS helpers
silently truncated a character above 0xFF to its low byte, so two different
secrets could hex to the same HMAC key.

## The module

`hull/encoding@1`: Lua `require("hull.encoding")`, JS `import { encoding }
from "hull:encoding"`. One module, one namespace per codec:

| | encode | decode |
|---|---|---|
| `hex` | lowercase | either case; strict |
| `base64` | standard alphabet, padded; `{ url = true }` for `-_`, unpadded; `pad` overrides | strict; `url` takes no padding; `{ lenient = true }` skips whitespace |
| `base32` | RFC 4648, uppercase, unpadded | either case; strict; `lenient` skips whitespace and `=` |
| `utf8` | text to its UTF-8 bytes (JS: throws on a lone surrogate; Lua: the identity) | bytes to text, or nil/null for malformed UTF-8 (overlong, surrogates, above U+10FFFF, truncated) |
| `bytes` (JS only) | `toU8(byteString)` for the C bindings | `fromBuffer(buf)` to a byte string |

- **Values are bytes**: Lua byte strings; in JS a byte string (one character
  per byte), an ArrayBuffer, a typed array or a DataView. A JS string with a
  character above 0xFF is refused, never truncated.
- **Decoding is strict**: a character outside the alphabet, wrong or misplaced
  padding, or an impossible length returns nil (Lua, with a reason:
  `invalid_char`, `bad_padding`, `bad_length`) or null (JS). Only a
  non-string argument raises.
- **Pure**: no capability and no other module, so SSH and ws-stream, which
  are capability-free by design, can require it. `hull/crypto` lists it as a
  dependency, so every module that uses crypto has it admitted without
  spending a slot of its own (several are near the limit of 10).
- Tests: RFC 4648 vectors and the strictness rules in both runtimes
  (`stdlib/{lua,js}/hull/tests/test_encoding.*`, asserting identical values),
  and `tests/e2e_hex_parity.sh`, which requires Lua and JS to produce the
  same text for all 256 byte values in every codec and to read it back.

## Decisions

Taken with the maintainer on 2026-09-28:

1. **Converge base64url on strict.** JS jwt's own decoder accepted padding and
   `+/`; it now uses the strict decoder, the same rule the Lua side gets from
   C. A token that only one runtime accepted is now rejected by both.
2. **Session ids are lowercase hex in both runtimes.** Lua accepted uppercase,
   JS did not; both now accept exactly what they generate.
3. **The name is `hull/encoding`, with no alias.** The earlier
   `hull/encoding/base64` (Lua-only, used by SSH and ws-stream) is removed, as
   is the internal `hull.crypto._hex`.

## Kept exactly as they were

A merge must not change anything stored or sent. These stay byte-identical:

- kv keys are lowercase hex (prefix scans on Postgres are case-sensitive, and
  existing rows are lowercase); kv values are padded standard base64. kv keeps
  its own `corrupt base64 in store` / `corrupt hex in store` errors.
- JS secrets are hexed as one byte per character, as before: every HMAC key in
  jwt, csrf, auth-flows and oauth is unchanged.
- pwned keeps uppercase hex (the HIBP wire format).
- audit-log's device fingerprint hexes a hex digest before truncating it, so
  it covers 32 bits of the digest. Coarse, but stored in `_hull_audit_log`;
  changing it would make every device look new. Documented at the function.

## Phase 1 (done)

- `hull.encoding` in both runtimes, registered, with the tests above.
- SSH (`kex`, `hostkey`, `known_hosts`, `privatekey`, `transport`, `ssh.lua`)
  and ws-stream moved off `hull.encoding.base64` and their private hex.
- `hull.crypto._hex` removed; jwt, csrf, auth-flows, oauth, audit-log, totp,
  sealbox and otp use `hull.encoding.hex`.
- kv's base64 and hex (`_util`, both runtimes) and Valkey's namespace hex are
  thin wrappers that add kv's error codes.
- TOTP's base32, jwt.js's base64url decoder, sealbox.js's buffer adapters and
  the session / attachment id loops are gone.
- Stale comments fixed: the JS `crypto.hexEncode` binding claimed string input
  was binary-safe (it is UTF-8 encoded), and others that pointed at removed
  helpers.

## Phase 2 (done): no codecs in `hull.crypto`

`crypto.hex_encode` / `hex_decode` / `base64url_encode` / `base64url_decode`
and the JS `hexEncode` / `hexDecode` / `base64urlEncode` / `base64urlDecode` /
`base64urlDecodeBytes` are removed. In JS they took strings as UTF-8, the root
of every byte-string workaround the stdlib had grown. What they did for text
now has a name, `hull.encoding.utf8`: jwt and envelope encode their JSON as
UTF-8 bytes before base64url, and decode a segment as base64url, then UTF-8 -
strictly, so a segment that is not valid UTF-8 is refused in both runtimes
rather than turned into U+FFFD in JS. jobs, auth-flows, csp, oauth (PKCE,
state, and the JWKS `x5c` certificate, now decoded directly as standard
base64), pwned, sealbox and otp moved too.

## Phase 3 (done): examples and C

- The examples (`irc_chat`, `todo`, `webhooks`, `image_processing`) use
  `hull.encoding` and declare `hull/encoding@1`. The JS webhooks example hexes
  its text secret as UTF-8 bytes, so it derives the same key as the Lua one.
- C has one base64: `src/hull/utils/base64.{c,h}` (`hl_base64_encode` /
  `hl_base64_decode`, standard or url alphabet, padding optional, strict
  decode). It replaces the copies in SMTP AUTH PLAIN, PostgreSQL SCRAM, the
  terminal's OSC 52 clipboard write, and the base64url pair in `cap/crypto.c`.
  Like `utils/hex` (see `h1_s2b_hex_ownership.md`) it is a leaf with no
  undefined symbols, so it widens no consumer's link.
- C has one hex decoder: `hl_hex_decode` joins `hl_hex_encode` in
  `utils/hex`, replacing `hl_cap_crypto_hex_decode` (release, signature,
  verify-release, verify-self, the crypto bindings). `hl_cap_crypto_hex_encode`,
  unused once the bindings went, is removed. Release and signature code no
  longer reach into the crypto object just to decode hex.
- Left alone on purpose: mbedTLS's own base64 for PEM output (the library's
  code, inside the TLS feature), and the one-character hex-digit parsers in the
  wire protocols (Postgres `bytea`, MySQL, Valkey), which parse protocol text
  rather than encode bytes.
