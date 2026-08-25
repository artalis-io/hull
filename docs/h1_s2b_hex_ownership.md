# H1 / S2b - hex-encoding caller inventory + ownership recommendation (design-only)

Status: **DESIGN-ONLY. ZERO CODE CHANGE.** S2b of the ratified
[H1 cleanup freeze](h1_cleanup_inventory.md), and the deferred half of the
freeze's 1.2a hex observation. It produces the exhaustive hex-caller inventory,
a caller-by-caller link-closure/dependency table, and an ownership
recommendation. **No code is consolidated here.** The recommendation is offered
for review; a future execution slice (if approved) must prove zero link-delta
per composition before landing anything. ("Finding no safe cleanup is a valid
result.")

**DECISION (ratified):** **Option A** - a private, dependency-neutral
`src/hull/utils/hex.{c,h}` leaf - is chosen (see 4.A). The consolidation lands
as a SEPARATE reviewed execution slice under the acceptance in section 5. Scope:
only the seven verified buffer encoders (P1-P7); the cache helper, the
`hl_release_io_sha256_hex` hash+hex composite, and the `FILE*`-stream encoder are
left unchanged unless a later ownership audit justifies moving them.

**EXECUTED:** the consolidation slice landed `src/hull/utils/hex.{c,h}` and
routed P1-P7 onto it, one TU per commit, under section 5's acceptance. Proof of
unchanged dependency closure: `hex.o` has zero undefined symbols, so every
consumer's link closure grows by exactly `hl_hex_encode` and no composition
gains a crypto/mbedtls reference (`nm` verified per consumer). No material size
regression: the base binary is 96 bytes SMALLER (seven statics removed, one
152-byte leaf added). Linked and validated across base (unit tests 91/91),
pure-compute, libhull (C embedder links), and PostgreSQL; cosmo inclusion is
structural (`PLATFORM_CAP_OBJS` is the full `CAP_OBJS` on cosmo, which carries
`hex.o`) and CI-covered.

## 1. Why this slice is design-only

The freeze deferred hex consolidation specifically because "route the generic
copies through the capability layer" is the wrong instinct: `hl_cap_crypto_hex_encode`
lives in `cap_crypto.o`, and C links at OBJECT granularity, so pulling that one
function pulls the whole crypto object and everything it references. Measured:

```
$ nm build/cap_crypto.o | grep -iE 'U mbedtls_|tweet' | head
                 U _crypto_hash_sha512_tweet
   ... (mbedtls_* undefined references present)
```

So the real question is not "is there duplication" (there is) but "which callers
can share ONE implementation without gaining a dependency they do not already
have." That is a link-closure question, answered below.

## 2. Exhaustive hex-caller inventory

Recorded commands (fail-closed; run from repo root):

```
set -e
# byte->hex-BUFFER encoders (the consolidation candidates)
grep -rnE '0123456789abcdef' src/hull --include='*.c'
# streamed/formatted hex (a DIFFERENT shape - excluded, see 2.3)
grep -rnE '%02[xX]' src/hull --include='*.c'
# the three already-named encoder homes
grep -rn 'hl_cap_crypto_hex_encode\|hl_runtime_cache_hex_encode\|hl_release_io_sha256_hex' \
     src/hull include/hull --include='*.c' --include='*.h'
```

### 2.1 The three existing NAMED encoder homes

| Symbol | Home TU | Shape | Callers |
|---|---|---|---|
| `hl_cap_crypto_hex_encode` | `cap/crypto.c` | pure byte->hex buffer | `runtime/{lua,js}/mod_crypto.c` (the public `crypto.hexEncode` bindings) |
| `hl_runtime_cache_hex_encode` | `runtime/cache_common.c` | pure byte->hex buffer | `runtime/{lua,js}/bytecode_cache.c`, `runtime/{lua,js}/template_cache.c`, `commands/cache.c` |
| `hl_release_io_sha256_hex` | `release_io.c` | **SHA-256 + hex combined** (not a pure encoder) | `commands/{flavor,feature,update}.c`, `release_io.c` |

`hl_release_io_sha256_hex` is a hash-then-hex composite, not a byte->hex
primitive; it is out of scope for a pure-hex consolidation (kept regardless).

### 2.2 The private / inline byte->hex-BUFFER copies (the duplication)

Each is an ~6-8 line `static` encoder or an inline lookup-table loop over a
`"0123456789abcdef"` table. All produce the identical lowercase-hex byte->buffer
transform.

| # | Site | Form | Purpose |
|---|---|---|---|
| P1 | `signature.c:37` `hex_encode` | `snprintf %02x` loop | display app/hash hex at verify time |
| P2 | `release.c:26` `hex_encode` | lookup-table | encode the ed25519 signature to hex |
| P3 | `sbom.c:72` `hex_encode_sha256` | lookup-table (fixed 32->64) | SBOM digest strings |
| P4 | `shared/blob_store.c:88` `hex_encode` | lookup-table | blob IDs + random tmp names |
| P5 | `cap/db_postgres.c:90` inline | lookup-table | SCRAM client/server proof hex |
| P6 | `commands/verify_self.c:110` inline | lookup-table | self-verify digest display |
| P7 | `runtime/lua/mod_tool.c:1557` inline | lookup-table | build-tool hex output |

(The two named narrow homes in 2.1 - `hl_runtime_cache_hex_encode`,
plus `release_io.c:382`'s inline table inside `hl_release_io_sha256_hex` -
are the same transform but already have owners.)

### 2.3 Deliberately EXCLUDED (different shape, not a buffer encoder)

`tool.c:65,82` `fprintf(f, "%02x", pk[i])` stream hex directly to a `FILE*`;
`mod_crypto.c` `%02x` sites are inside the public bindings that already delegate
to `hl_cap_crypto_hex_encode`. Neither is a byte->hex-buffer encoder, so neither
is a consolidation candidate (mirrors the freeze's exclusion of the parity dup).

## 3. Link-closure / dependency table

The pivotal column is the last one: does the caller's object ALREADY reference
`cap_crypto.o` (so routing to `hl_cap_crypto_hex_encode` adds nothing), or would
routing pull crypto (mbedtls + tweetnacl) in as a NEW object?

Recorded command:

```
for o in signature release release_io sbom verify_self cache_common blob_store; do
  obj=$(ls build/${o}.o build/*${o}.o 2>/dev/null | head -1) || continue
  nm "$obj" | grep -qE 'U _?hl_cap_crypto_' \
    && echo "$o: already links cap_crypto" \
    || echo "$o: does NOT link cap_crypto"
done
```

| Site | Home TU | Already co-links `cap_crypto.o`? | Route to `hl_cap_crypto_hex_encode` = | Narrow-config TU? |
|---|---|---|---|---|
| P1 signature | `signature.c` | **yes** (uses `hl_cap_crypto_sha256`/`ed25519_verify`/`hex_decode`) | no new dep | no (verify path already has crypto) |
| P2 release | `release.c` | **no** (links tweetnacl directly) | **NEW crypto dep** | signing path |
| P3 sbom | `sbom.c` | **no** | **NEW crypto dep** | `hull sbom` command |
| P4 blob_store | `shared/blob_store.c` | **no** | **NEW crypto dep** | **yes** - shared infra used in cache / libhull / no-runtime configs |
| P5 db_postgres | `cap/db_postgres.c` | (feature; SCRAM co-links crypto) | no new dep (feature only) | feature TU |
| P6 verify_self | `commands/verify_self.c` | **yes** | no new dep | no |
| P7 mod_tool | `runtime/lua/mod_tool.c` | (tool VM; links crypto bindings) | no new dep | tool-only |
| (cache) `hl_runtime_cache_hex_encode` | `cache_common.c` | **no** (deliberately) | **NEW crypto dep** | **yes** - the cache is intentionally crypto-free |

**The finding.** Routing every copy to the cap-layer encoder is a dependency
regression for exactly the TUs that must stay narrow (`release`, `sbom`,
`blob_store`, `cache_common`). It is free for `signature`, `verify_self`,
`db_postgres`, `mod_tool`. A single cap-layer home is therefore the WRONG target.
The cache's separate `hl_runtime_cache_hex_encode` is not an oversight - it is the
correct pattern (a narrow home for a crypto-free consumer), and it is evidence
that the right shape is a dependency-neutral primitive, not the crypto object.

## 4. Ownership recommendation

Two dispositions are on the table. Per the freeze, "no consolidation" is a valid
outcome; the recommendation below is offered for the reviewer to choose between.

### Option A (recommended) - one dependency-neutral hex leaf

Introduce a zero-dependency leaf (candidate: `src/hull/utils/hex.{c,h}`, alongside
the other Hull-domain-free leaves in `utils/`) exporting the pure byte->hex-buffer
transform, and route P1-P7 + `hl_runtime_cache_hex_encode`'s body onto it.

- Removes ~7 near-identical copies (~50 lines) behind ONE tested implementation.
- **Zero dependency regression for every caller** - the leaf pulls nothing, so the
  narrow TUs (`blob_store`, `cache_common`, `release`, `sbom`) stay crypto-free.
- `hl_cap_crypto_hex_encode` keeps its public signature (it MAY delegate to the
  leaf internally, or stay verbatim - an optional internal refinement, not required).
- `hl_release_io_sha256_hex` is unchanged (hash+hex composite; its internal hex
  step MAY call the leaf, optional).
- Strengthens the "one obvious home" principle without expanding public API
  (the leaf is `src/hull/`-private, not `include/hull/**` - same ownership
  discipline S2a applied to `asset_checksum`).

Cost: touches 7-8 TUs across `cap/`, `commands/`, `shared/`, `runtime/`. Moderate
churn for a low-severity duplication. Justified mainly by the "single tested home"
win, not by any correctness or size pressure.

### Option B - retain as-is

The copies are tiny, correct, and independently obvious at each call site; the
narrow-config ones exist for a real (now-measured) reason. If the reviewer judges
the churn not worth ~50 lines of benign duplication, retaining is the honest
result and consistent with the freeze principle.

### Explicitly NOT recommended

Routing the generic copies through `hl_cap_crypto_hex_encode` (the crypto layer).
The table shows it regresses four narrow TUs' link closure. This is the instinct
the freeze flagged and S2b confirms is wrong.

## 5. Acceptance for a future execution slice (only if Option A is chosen)

This slice writes no code. If Option A is approved, the execution slice must:

1. Add `utils/hex.{c,h}` (zero deps) with unit tests (empty input, 1 byte,
   32-byte digest, 64-byte, output NUL-termination, buffer-size guard).
2. Route P1-P7 + the cache body onto it, one TU per reviewable commit.
3. **Prove zero link-delta**: for each affected composition (base, pure-compute,
   libhull, cosmo, the postgres feature), diff `nm`-visible new undefined symbols
   and `size` before/after - assert no new `cap_crypto` / mbedtls / tweetnacl
   reference enters any TU that lacked it. A measured regression aborts the route
   for that caller (it stays on its private copy).
4. Leave `hl_release_io_sha256_hex` and the public `crypto.hexEncode` bindings
   behaviourally identical.

Until that slice is separately approved, all sites remain as-is.
