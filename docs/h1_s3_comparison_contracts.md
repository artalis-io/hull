# H1 / S3 - comparison / constant-time contract matrix (design-only)

Status: **DESIGN-ONLY. ZERO CODE CHANGE.** S3 of the ratified
[H1 cleanup freeze](h1_cleanup_inventory.md), completing the deferred half of the
freeze's 1.2b observation ("the constant-time comparisons are not one semantic
family - 8 sites, contract matrix"). This record inventories **every**
security-relevant comparison site (not just the eight previously surveyed),
gives each a full contract, classifies them into equivalence classes, and makes
a per-class recommendation. **No new helper and no implementation change land
here.** "No safe consolidation" is an acceptable outcome and, as recorded in
section 6, is the outcome for the comparison logic itself.

## 1. Scope and reproducible inventory

"Security-relevant" = a comparison whose result gates authentication, integrity,
signature, checksum, or MAC verification, OR that compares secret-derived bytes.
Ordinary string/byte compares (argument parsing, filename suffixes, MIME magic,
sort keys, DSN `sslmode` tokens) are enumerated in section 3 and excluded with
rationale.

Recorded commands (run from repo root):

```
# A. explicit constant-time accumulator loops (XOR-accumulate):
grep -rnE 'diff[[:space:]]*\|=' src/hull --include='*.c'
# B. named equality helpers + mbedTLS constant-time:
grep -rnE 'ct_hex_eq|_checksum_eq|constant_time_eq|mbedtls_ct_memcmp' src/hull --include='*.c'
# C. non-CT compares on verification material:
grep -rnE 'memcmp|strncmp|strcmp' src/hull --include='*.c' \
  | grep -iE 'hash|token|mac|sig|digest|secret|tag|hmac|expect|verif|pubkey|proof'
```

Command A returns 9 CT loops; B returns the named helpers + the one
`mbedtls_ct_memcmp`; C returns the non-CT verification compares plus the benign
matches triaged in section 3. The union is the 14 security-relevant sites below.

## 2. Master contract matrix (14 security-relevant sites)

Columns: **Repr** (byte array / NUL-terminated hex string / mbedtls bytes);
**Extent** (trusted length bound); **Len** (Fixed / Variable); **LenChk**
(does it validate the operand lengths); **Null** (nullability handling);
**Secret** (is either operand secret-derived); **LeakOK** (is leaking the
compare's timing/length acceptable); **CT** (constant-time?).

### 2A. Secret-derived 32-byte compares (must be constant-time)

| # | Site | Function | Repr | Extent | Len | LenChk | Null | Secret | LeakOK | CT |
|---|------|----------|------|--------|-----|--------|------|--------|--------|----|
| 1 | `cap/crypto.c:915` | `hl_cap_crypto_hmac_sha256_verify` | byte[32] | 32 | Fixed | n/a (param `expected[32]`) | NULL-guards inputs | **yes** (HMAC) | no | **yes** |
| 2 | `runtime/lua/mod_crypto.c:197` | `verify_password` | byte[32] | 32 | Fixed | hex_decode enforces 64->32 | rejects bad hex | **yes** (PBKDF2) | no | **yes** |
| 3 | `runtime/js/mod_crypto.c:244` | `verify_password` (JS) | byte[32] | 32 | Fixed | hex_decode enforces 64->32 | rejects bad hex | **yes** (PBKDF2) | no | **yes** |

Sites 1-3 all XOR-accumulate 32 secret-derived bytes under `volatile`, then
`secure_zero` the scratch. Site 1 is the cap-layer primitive; sites 2/3 inline
the identical 32-byte loop rather than calling a shared compare.

### 2B. Public-checksum hex compares (defensive constant-time)

| # | Site | Function | Repr | Extent | Len | LenChk | Null | Secret | LeakOK | CT |
|---|------|----------|------|--------|-----|--------|------|--------|--------|----|
| 4 | `release_io.c:255` | `local_ct_hex_eq` | NUL-term hex str | 64+NUL | Variable-guarded | **strict**: rejects short (`ca==0`) AND long (`a[64]!='\0'`) | reads only within 0..64 | no (public SHA) | yes | yes |
| 5 | `commands/asset_checksum.c:14` | `hl_asset_checksum_eq` | `char[64]` array | 64 | Fixed | none (caller guarantees 64-byte arrays) | no terminator dependency | no (public SHA) | yes | yes |
| 6 | `commands/verify_self.c:297` | self-verify inline | hex chars | 64 | Fixed | operands are `HL_SHA256_HEX_BUF` (65) | n/a | no (public SHA) | yes | yes |
| 7 | `commands/update.c:237` | `mbedtls_ct_memcmp` | mbedtls bytes | 64 | Fixed | fixed 64 arg | n/a | no (public SHA) | yes | yes (mbedTLS) |

Sites 4-7 compare a computed SHA-256 hex to an expected checksum from a
release/feature manifest. The manifest is itself Ed25519-signed, so the expected
value is trusted-but-public; CT here is belt-and-suspenders, not load-bearing.
**Their contracts differ materially**: #4 is a length-validating NUL-terminated
string compare; #5 is a fixed-width array compare with no length check (the
S2a-ratified separation); #6 is an inline fixed-64 over 65-byte buffers; #7 is
mbedTLS bytes. This split is the crux of the "not one family" finding.

### 2C. Public generic constant-time-eq (app-facing primitive)

| # | Site | Function | Repr | Extent | Len | LenChk | Null | Secret | LeakOK | CT |
|---|------|----------|------|--------|-----|--------|------|--------|--------|----|
| 8 | `runtime/lua/mod_crypto.c:830` | `crypto.constant_time_eq` | bytes (Lua string) | caller | Variable | length-equality first (leaks length by design) | luaL_checklstring | caller may pass a secret | length only | yes |
| 9 | `runtime/js/mod_crypto.c:1168` | `crypto.constantTimeEq` (JS) | bytes (JS string) | caller | Variable | length-equality first | JS_ToCStringLen | caller may pass a secret | length only | yes |

Sites 8/9 are the **public** `crypto.constant_time_eq` primitive apps use for
their own token/MAC compares. Length inequality short-circuits (a deliberate,
standard length leak). Per-runtime by necessity (Lua string vs JS string).

### 2D. Auth-protocol compare (backend-local, constant-time)

| # | Site | Function | Repr | Extent | Len | LenChk | Null | Secret | LeakOK | CT |
|---|------|----------|------|--------|-----|--------|------|--------|--------|----|
| 10 | `cap/pg_conn.c:512` | SCRAM SASL-final | byte[32] | 32 | Fixed | base64-decode asserts `gl==32` | rejects bad b64/len | **yes** (ServerSignature) | no | **yes** |

Site 10 verifies the SCRAM ServerSignature inside the Postgres SASL state
machine (feature TU). Auth-protocol-specific; MySQL auth has its own path.

### 2E. Public-value integrity compares (non-constant-time, acceptable)

| # | Site | Function | Repr | Extent | Len | LenChk | Null | Secret | LeakOK | CT |
|---|------|----------|------|--------|-----|--------|------|--------|--------|----|
| 11 | `signature.c:465` + `:532` | app/platform file-hash verify | NUL-term hex | 64 | Variable (`strcmp`) | relies on NUL | computed side terminated | no (both public) | yes | **no** |
| 12 | `signature.c:636` | embedded-pubkey compare | hex | 64 | Fixed (`strncmp`,64) | fixed 64 | n/a | no (pubkey is public) | yes | **no** |
| 13 | `platform_sig.c:229` | platform-asset hash compare | hex | 64 | Fixed (`strncmp`,64) | fixed 64 | n/a | no (public SHA) | yes | **no** |
| 14 | `shared/blob_store.c:363` | content-addressed id verify | hex | `HL_BLOB_STORE_ID_HEX_LEN` | Fixed (`memcmp`) | fixed len | n/a | no (content id is public) | yes | **no** |

Sites 11-14 compare public values (signed-manifest hashes, a public key, a
content-addressed blob id). Neither operand is secret, so non-CT is acceptable.

## 3. Excluded benign compares (enumerated for completeness)

Not verification of secret/auth material; excluded from the matrix:
`static.c:136` (ETag, public), `cap/mime.c:236` (magic bytes),
`cap/fs.c:430` (VFS name sort), `cap/valkey.c:272` (RESP token),
`platform_sig.c:74,100` (arch sort/dup-detect),
`signature.c:411,434,437` (`.lua` suffix / entry-name match),
`pg_conn.c:669`, `mysql_conn.c:388` (DSN `sslmode` tokens),
and the `--flag` argument parses in `tool.c`, `serve.c`, `update.c`,
`verify_release.c`, `sign_release.c`, `mod_tool.c`.

## 4. Threat context per equivalence class

| Class | Sites | Threat context | Secret operand? | CT required? |
|-------|-------|----------------|-----------------|--------------|
| **A** secret 32-byte | 1,2,3 | secret MAC / password-hash verification | yes | **yes** (load-bearing) |
| **B** public checksum | 4,5,6,7 | integrity of a signed-but-public download/asset | no | defensive only |
| **C** public CT-eq API | 8,9 | app-supplied token/MAC compare primitive | caller-dependent | yes (it IS the primitive) |
| **D** auth protocol | 10 | SCRAM ServerSignature (server authenticates to client) | yes | **yes** |
| **E** public integrity | 11,12,13,14 | signature/manifest/content-id equality | no | not required |

## 5. Callers, link/composition ownership, and current tests

| # | Callers | Link domain | Test proving the contract |
|---|---------|-------------|---------------------------|
| 1 | HMAC-verify consumers (jwt/envelope via bindings) | base (`cap_crypto.o`) | `test_lua.c` / `test_js.c` (binding-level); **no direct C unit test** |
| 2,3 | `crypto.verify_password` (Lua/JS) | per-runtime binding | `test_lua.c` / `test_js.c` |
| 4 | `release_io.c:335` (asset SHA verify) | base (`release_io.o`, HTTP-client) | **none direct** (exercised via `e2e_update.sh`) - GAP |
| 5 | `feature.c:174`, `flavor.c:133` | commands (private helper) | `test_dispatch.c::asset_checksum` |
| 6 | `verify_self` self-hash check | commands | **none direct** - GAP (integration via `hull verify-self`) |
| 7 | `update.c` release-binary SHA | commands (HTTP-client, links mbedTLS) | **none direct** - GAP (integration via `e2e_update.sh`) |
| 8,9 | app code via `crypto.constant_time_eq` | per-runtime binding | `test_lua.c` / `test_js.c` |
| 10 | `pg_conn.c` SASL final | postgres feature TU | `test_pg_conn.c` (`server_sig`), `e2e_postgres.sh` |
| 11 | `hl_sig_verify` file loop | base (`signature.o`) | `test_signature.c`, `e2e_build.sh`, `e2e_composed_sig.sh` |
| 12 | `hl_sig_verify` pubkey gate | base (`signature.o`) | `test_signature.c` |
| 13 | `hl_platform_sig_verify_composed` | base (`platform_sig.o`) | `test_platform_sig.c`, `e2e_composed_sig.sh` |
| 14 | `blob_store` tmp/id verify | base/shared (also libhull) | blob/cache suites (indirect) |

**Recorded test gaps** (contracts not directly unit-tested): #4 `local_ct_hex_eq`,
#6 verify_self compare, #7 `mbedtls_ct_memcmp` call. All are exercised through
integration (`e2e_update`, `hull verify-self`) but none has a focused
compare-contract unit test.

## 6. Equivalence classes and per-class recommendation

| Class | Recommendation | Rationale |
|-------|----------------|-----------|
| **A** secret 32-byte (1,2,3) | **Retain locally** (consolidation deferred, out of S3 scope) | Contract IS identical (32 secret bytes, CT, `secure_zero`), so a shared cap-crypto CT-eq primitive would fit 2/3 onto site 1's kind. But that means **creating a new helper**, which S3 forbids and this slice does not do. Sites are already correct + CT; flag as a candidate for a future hardening slice, not a defect. |
| **B** public checksum (4,5,6,7) | **Retain locally - no consolidation** | The contracts genuinely differ: #4 is a length-validating NUL-terminated string compare; #5 a fixed-width array compare with no length check (S2a already ratified these two as distinct); #6/#7 are inline-fixed / mbedTLS-bytes. A single home would either drop #4's strict length validation or over-constrain the fixed-width callers. This is the freeze's "not one family" finding, confirmed. |
| **C** public CT-eq API (8,9) | **Retain** | These are the public `crypto.constant_time_eq` surface, required per-runtime (Lua/JS parity). Not internal duplication. |
| **D** auth protocol (10) | **Retain locally** | Belongs with the SCRAM codec in the postgres feature TU; no cross-backend sharing (MySQL auth is separate). Correct as-is. |
| **E** public integrity (11-14) | **Retain locally**; ONE optional harden-for-consistency note | Non-CT is acceptable (both operands public). Observation: #11 (`signature.c` strcmp of computed file-hash vs signed-manifest hash) is structurally identical to #6 (`verify_self`, which is CT). Both are defensible; if uniformity is wanted, hardening #11 to CT is low-risk/low-value. **Not required, not a security defect.** |

## 7. Outcome and (non-)acceptance

**No safe consolidation of the comparison logic is recommended.** Every class
resolves to *retain*: the contracts differ (Class B), are per-runtime public
surface (C), are protocol- or link-local (A-cap, D), or compare public values
(E). This confirms and closes the freeze's 1.2b hypothesis that the constant-time
sites are not one family.

Two observations are recorded for a POSSIBLE future **hardening** slice (never a
consolidation, and each optional, each out of S3's design-only scope):

1. **Class A** verify_password (2,3) inlines the same 32-byte secret CT compare
   that the cap layer already performs in `hl_cap_crypto_hmac_sha256_verify`
   (1). A future slice *could* add a private cap-crypto CT-eq primitive both
   share - contract-identical, but it creates a helper, so deferred.
2. **Class E** `signature.c` file-hash verify (11) is non-CT while the
   structurally-identical `verify_self` (6) is CT. Optional harden-for-consistency;
   both are currently correct because the operands are public.

3. **Test gaps** (section 5): #4/#6/#7 lack a focused compare-contract unit test.
   Adding those is test-only and independent of any consolidation decision; it is
   noted here, not actioned in this design slice.

Until a future slice is separately proposed and approved, **all 14 sites remain
as-is.**
