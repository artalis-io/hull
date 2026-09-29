# `hull.kv` encryption at rest: design

Status: **implemented.** `hull.crypto.sealbox` (Lua + JS), the `encrypt`
option of `hull.kv.open`, and TOTP on the shared module. The decisions taken on
the open questions are recorded in section 11; the user-facing description is
[`kv_cache.md`](kv_cache.md#encryption-at-rest).

## 1. Why

`hull.kv` stores values as plain bytes in whatever backend the app chose:
process memory, a SQLite file, a Postgres table, Valkey. Anyone who can read
that backend - a database administrator, a backup, another app sharing the
Postgres - reads every value, and anyone who can write it can change any
value undetected.

Some of what apps put in `hull.kv` is secret (session payloads, API tokens,
agent state) and some of it must not be substituted (SSH host keys in
`ssh.kv_store`, where a writer who swaps a key makes Hull trust a
man-in-the-middle). One opt-in feature covers both: **authenticated**
encryption of values, so a reader without the key sees noise and a writer
without the key cannot produce a value that decrypts.

## 2. Threat model

**Protects against** someone with read and/or write access to the backend but
not the key:

- reading a value;
- changing a value (it no longer decrypts: `decrypt_failed`);
- writing a value of their own (cannot be produced without the key);
- moving a genuine value from one key to another, or between namespaces
  (the key name and namespace are sealed inside it; see section 4).

**Does not protect against**, and says so in the user docs:

- **Deletion.** A writer can remove any entry. For `ssh.kv_store` that yields
  `host_unknown`, which stops and asks, so it fails safe; for other data it is
  loss, not forgery.
- **Rollback.** A writer who kept an OLD genuine ciphertext for a key can put
  it back; it still decrypts. Detecting that needs a monotonic counter per key
  that the attacker cannot reset, which a shared backend cannot provide.
  Consequence for SSH: after `forget_host` + a new `accept_host`, a restored
  old entry would be trusted again. Documented; an app that must rule this out
  keeps a version number outside the store.
- **Key names and access patterns.** Keys stay plaintext: `scan` needs their
  prefixes and SQL backends index them. Only values are encrypted. Names are
  visible (`hostkey:web1` reveals that web1 is a trusted host).
- **Anyone who holds the key**, including the app itself and its process
  memory.

## 3. Where it lives

In the **handle layer** (`stdlib/lua/hull/kv/_handle.lua` and its JS mirror),
not in the backends. Backends keep storing opaque bytes, so every backend -
including ones not written yet - gets encryption without change, and a backend
never sees plaintext.

```lua
local kv = require("hull.kv").open{
    backend = "postgres", database = db, namespace = "sessions",
    encrypt = { keys = { [1] = key_v1, [2] = key_v2 }, current = 2 },
}
```

`encrypt` absent means today's behaviour exactly. With it, every value written
is sealed with the `current` key, and a value is opened with whichever key its
version prefix names.

## 4. Wire format

One format, shared with `hull/web/middleware/totp` (section 8):

```
version  u32 BE      which key sealed it (the id in `keys`)
nonce    24 bytes    random per write
box      secretbox(key, nonce, frame)          -- XSalsa20-Poly1305
frame    u32 len(namespace) || namespace
         u32 len(key)       || key
         value
```

NaCl `secretbox` has no associated-data input, so the namespace and the key
name go **inside** the sealed frame and are checked on open. Without that, a
writer could copy the genuine sealed value of `session:alice` over
`session:bob` and it would decrypt cleanly. On open, any mismatch is
`decrypt_failed` - the same code as a bad MAC, on purpose, so the store does
not tell an attacker which check failed.

Overhead per value: 4 + 24 + 16 bytes plus the two names.

## 5. Operation semantics

| op | with `encrypt` |
|---|---|
| `get` | read, open, check namespace and key; `nil` on a miss |
| `set` | seal with the current key, write |
| `delete`, `exists`, `scan`, `clear`, TTL | unchanged: they touch keys, not values |
| `cas(k, expected, new)` | read the current sealed value, open it, compare the plaintext with `expected`; then the backend's `cas(k, current_sealed, new_sealed)`. Still atomic: the backend compares the exact bytes it holds. `expected = nil` stays a plain set-if-absent. |
| `incr` | **`unsupported`**. The backend does integer arithmetic on the stored bytes, which it cannot do on ciphertext. A counter that must be secret is stored as a value and updated with `cas`. |

A value without a valid version prefix fails with `decrypt_failed`, except
under `encrypt.allow_plaintext = true` (section 7), which exists only for
migrating an existing namespace.

## 6. Keys

- **32 random bytes**, not a passphrase. A passphrase would need a KDF with a
  per-namespace salt stored somewhere, and a weak one would make the feature
  decorative. `hull.crypto.random(32)` produces one; operators store it hex or
  base64 in their secret manager.
- **Never in source.** The docs show `env.get("KV_KEY")` through the manifest's
  `env` allowlist, as TOTP's docs already do.
- **First version: keys are Lua strings**, consistent with TOTP, and simple.
  The cost is that key bytes live in the Lua heap, where they cannot be wiped.
- **Then: a C-held keyring** (built), following `passphrase_env` in
  `hull/ssh`. `crypto.key_from_env(var)` (JS `crypto.keyFromEnv`) reads the
  variable under the manifest's `env` allowlist, decodes 64 hex digits or
  base64 into a buffer `cap/crypto_key.c` owns, and returns a handle whose
  only methods are `secretbox`, `secretbox_open` and `destroy`; the buffer is
  zeroed on `destroy` or collection. Sealbox keyrings take a handle anywhere
  they take a 32-byte string (`sealbox.keyring_from_env` builds a whole
  keyring from variable names), so `hull/kv`'s `encrypt` and TOTP use it with
  no change of their own. The sealed bytes are identical either way. The
  limit is the one `bcrypt_pbkdf_env` states: the value is still in the
  process environment; what is gone is the unscrubbable copy in the script
  heap.

## 7. Rotation and migration

- **Rotation:** add the new key under a new id, make it `current`. Old values
  still open under their own id; each `set` re-seals under the new one.
  `kv:rekey(prefix?)` re-seals everything (by `scan`), idempotent and safe to
  interrupt, so an operator can then drop the old key.
- **Migrating a plaintext namespace:** open it with
  `encrypt = { ..., allow_plaintext = true }`. Plaintext values are read as
  they are and re-sealed on their next write; `kv:rekey()` converts the rest.
  Then drop `allow_plaintext`. A value that happens to begin with bytes that
  look like a version prefix is tried as sealed first; the namespace/key check
  makes a false positive fail cleanly rather than return garbage.

## 8. Reuse, not a second format

TOTP already implements versioned `secretbox` with a key map, a `current` id
and a 4-byte version prefix (`stdlib/lua/hull/web/middleware/totp.lua`). The
first step is to **extract that into one shared module** -
`hull.crypto.sealbox` (Lua and JS): `seal(keyring, frame)`,
`open(keyring, blob)`, the version handling, and the frame binding of section
4 as an option. TOTP then uses it (its existing on-disk format is exactly the
same shape), and `hull.kv` uses it. One implementation of the part that is
easy to get subtly wrong.

`hull.crypto.secretbox` takes and returns raw bytes (the crypto API has no
hex anywhere since the byte-only change), so a value crosses no encoding.

## 9. What changes for `ssh.kv_store`

Nothing in `hull.ssh`. An app that opens its kv handle with `encrypt` gets
host keys that cannot be substituted by a backend writer without the key.
Deletion still yields `host_unknown` (fail-safe) and rollback remains possible
(section 2), and `docs/ssh.md` says both.

## 10. Tests

- Round trip on every backend the suite can reach (memory, SQLite; Postgres
  and Valkey in their e2e jobs).
- Tamper: flip a byte of a stored value -> `decrypt_failed`.
- Swap: copy key A's stored bytes to key B, and across namespaces ->
  `decrypt_failed`.
- Forge: a value written without the key -> `decrypt_failed`.
- `cas` on sealed values: succeeds on a plaintext match, fails on a mismatch,
  stays atomic under a concurrent writer.
- `incr` -> `unsupported`.
- Rotation: values sealed under key 1 open after `current = 2`; `rekey`
  converts them; dropping key 1 afterwards still opens everything.
- Migration: a plaintext namespace under `allow_plaintext` reads, re-seals on
  write, and `rekey` finishes the job.
- TOTP's existing rows open unchanged through the shared module.
- JS parity for all of the above.

## 11. Decisions

Taken with the maintainer on 2026-09-27:

1. **One PR:** the shared `hull.crypto.sealbox`, `hull.kv` encryption, and
   TOTP moved onto the shared module, together - so the extraction is proven
   by two users from the start, and TOTP's existing rows open unchanged
   through it (a test in section 10).
2. **Keys as Lua strings first**, as TOTP does. The C-held keyring of
   section 6 is a later step, not part of the first version.
3. **Key names stay plaintext.** No keyed hashing of names; `scan` by prefix
   keeps working. The docs say names are visible, and an app that needs them
   hidden hashes them itself.
4. **`hull.cache` is left as it is** until someone needs encrypted cache
   entries.
