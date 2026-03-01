# Hull — Security Model

This document is brutally explicit about what Hull protects against, what it doesn't, and where trust anchors lie.

---

## 1. Trust Model Overview

### Parties

| Party | Controls | Must Trust |
|-------|----------|------------|
| **Platform publisher** (gethull.dev) | Platform library, signing key, build service | Nothing (self-sovereign) |
| **App developer** | Application code, signing key, manifest | Platform publisher (or vendor their own) |
| **End user** | Which apps to run, which keys to trust | App developer + platform publisher |
| **Third-party auditor** | Nothing — read-only verification | Cryptographic math (Ed25519, SHA-256) |

### Key Insight

The system is designed so that **no party requires blind trust**:
- Users can verify the platform (signature + canary + source audit)
- Users can verify the app (signature + file hashes + manifest inspection)
- Users can eliminate gethull.dev entirely (self-host, self-sign)

---

## 2. Signature Verification Chain

Three verification points, each catching different attack vectors:

| When | What | Tool | Checks |
|------|------|------|--------|
| Before download | Inspect capabilities | `verify/index.html` (offline) | Platform sig, app sig, manifest, canary |
| Before install | Verify integrity | `hull verify --developer-key` (CLI) | Both sigs + file hashes |
| At startup | Runtime check | `--verify-sig` flag | App sig + file hashes, platform key pin |

### Trust Anchors

**Platform public key:**
- Hardcoded in Hull CLI (`HL_PLATFORM_PUBKEY_HEX` in `signature.h`)
- Hardcoded in browser verifier (`GETHULL_DEV_PLATFORM_KEY` in `verify/index.html`)
- Published at `https://gethull.dev/.well-known/platform.pub`
- Overridable via `--platform-key <file>` for self-hosted platforms

**Developer public key:**
- Published in app repository (`.pub` file)
- Manually cross-referenced by user against trusted source
- Passed explicitly: `hull verify --developer-key dev.pub`

---

## 3. Attack Model

### A. Malicious App Developer

This is the primary threat model. Hull exists to make it possible to trust apps from unknown developers.

**Attack: Ship a binary without the Hull platform (custom runtime, no sandbox)**

- **Prevention:** Platform signature + canary. Browser verifier checks platform sig against pinned gethull.dev key. Canary scanner finds `HULL_PLATFORM_CANARY` in the binary and verifies its integrity hash against the signed value.
- **Remaining risk:** Developer could build a custom binary that includes the canary bytes at the right offset. Reproducible builds (Phase 9) close this gap entirely — a rebuild from source proves the binary is just "Hull platform + declared source."

**Attack: Declare minimal manifest but access more at runtime**

- **Prevention:** Manifest is signed in `package.sig`. At runtime, pledge/unveil enforce the declared capabilities at the kernel level. Accessing undeclared paths triggers SIGKILL (Linux/Cosmo).
- **Remaining risk:** macOS has no kernel sandbox — pledge/unveil are no-ops. C-level validation in capability functions is the only defense. A bug in `hl_cap_fs_validate()` on macOS would allow bypass. Linux and Cosmopolitan are kernel-enforced.

**Attack: SQL injection through user input**

- **Prevention:** All database access goes through `hl_cap_db_query()` / `hl_cap_db_exec()` which use SQLite parameterized binding (`sqlite3_bind_*`). SQL is always a literal string from app code. No string concatenation, ever. SQL injection is structurally impossible.

**Attack: Path traversal to read /etc/passwd**

- **Prevention:** `hl_cap_fs_validate()` rejects:
  - Absolute paths (starts with `/`)
  - `..` components
  - Any path that resolves outside the app's base directory via `realpath()` ancestor check
  - Symlink escapes (realpath resolves symlinks before checking)
- Kernel unveil() also blocks access to undeclared paths.

**Attack: Memory exhaustion / DoS via infinite allocation**

- **Prevention:**
  - Lua: Custom allocator enforces 64 MB heap limit. Exceeding → NULL allocation → script error, not crash.
  - QuickJS: `JS_SetMemoryLimit()` enforces 64 MB. Exceeding → allocation failure → JS exception.

**Attack: Infinite loop / CPU exhaustion**

- **Prevention:**
  - QuickJS: Instruction-count interrupt handler. Configurable `max_instructions` limit. Exceeding → JS exception.
  - Lua: Memory limit eventually triggers (loops allocate stack frames). Less precise than instruction counting.

**Attack: Exfiltrate data to unauthorized hosts**

- **Prevention:** `hl_cap_http_request()` validates the target host against the manifest's `hosts` allowlist. Only declared hosts are reachable. Kernel pledge includes `inet` + `dns` only if hosts are declared.

**Attack: Read environment variables (API keys, secrets)**

- **Prevention:** `hl_cap_env_get()` checks against the manifest's `env` allowlist (max 32 entries). Undeclared variables return NULL.

**Attack: Session hijacking via cookie theft**

- **Prevention:** `hull.cookie` defaults to `HttpOnly=true`, `Secure=true`, `SameSite=Lax`. HttpOnly prevents JavaScript access (XSS-based theft). Secure prevents plaintext transmission. SameSite=Lax blocks cross-origin POST requests from carrying session cookies.
- **Remaining risk:** Same-origin XSS can still read `req.ctx.session` data. Hull doesn't have built-in XSS output escaping (application responsibility).

**Attack: CSRF — forged state-changing requests from another origin**

- **Prevention:** `hull.csrf` middleware generates HMAC-based tokens tied to the session ID and timestamp. State-changing methods (POST/PUT/DELETE/PATCH) require a valid CSRF token in the `X-CSRF-Token` header or `_csrf` form field. Tokens expire (default 1h). Safe methods (GET/HEAD/OPTIONS) are automatically skipped. Constant-time comparison prevents timing attacks.
- **Remaining risk:** If the CSRF secret is leaked, tokens can be forged. The secret must be stored securely (e.g., `env.get("SECRET_KEY")`).

**Attack: JWT token forgery**

- **Prevention:** `hull.jwt` uses HS256 with HMAC-SHA256 (no "none" algorithm, no algorithm negotiation). Signature verification uses constant-time comparison. Expired tokens are rejected.
- **Remaining risk:** JWT secrets must be strong. JWTs are stateless — they cannot be revoked until they expire. For revocation, use sessions instead.

**Attack: Session fixation / brute-force session IDs**

- **Prevention:** `hull.session` generates 32 random bytes (256-bit entropy) via `crypto.random()` for session IDs. IDs are hex-encoded (64 chars). Sessions are server-side (SQLite) with sliding expiry. Expired sessions are automatically pruned.

### B. Malicious Third Party (MITM / CDN Compromise)

**Attack: Replace binary on CDN with modified version**

- **Prevention:** `binary_hash` in `package.sig` is signed by the developer's Ed25519 key. Changed binary → hash mismatch. Browser verifier catches this immediately when binary is uploaded.

**Attack: Replace `package.sig` with forged one**

- **Prevention:** Signature is Ed25519 over the canonical payload. Forging requires the developer's 32-byte private key. Ed25519 is considered secure against all known attacks.

**Attack: Replace both binary and `package.sig`**

- **Prevention:** User verifies the developer's public key against a trusted source (e.g., GitHub repo, personal website). If the attacker doesn't have the developer's private key, they can't produce a valid signature for any payload.

**Attack: Replace platform libraries in a self-hosted Hull**

- **Prevention:** Platform signature in `package.sig` is verified against the pinned gethull.dev key. Swapped platform → platform signature mismatch.

### C. Compromised gethull.dev (Platform Publisher)

**Attack: Ship malicious platform libraries**

- **Prevention:** Platform signing key is published. Users can:
  1. Audit Hull source (AGPL-3.0)
  2. Build their own platform: `make platform`
  3. Sign with own key: `hull sign-platform mykey`
  4. Pin their own key in apps

  The architecture is designed so you **don't have to trust gethull.dev**.

**Attack: Backdoor the build service**

- **Prevention:** Reproducible builds. Anyone can rebuild from source with the recorded `cc_version` + `flags` and compare `binary_hash`. The build service is a convenience, not a trust requirement.

### D. End User Who Doesn't Trust Anyone

Complete trust elimination path:

1. Download Hull source from GitHub (AGPL-3.0)
2. Audit the code
3. Build platform yourself: `make platform`
4. Sign with your own key: `hull sign-platform mykey`
5. Distribute to customers with your key pinned
6. Customers verify against YOUR key, not gethull.dev's

Trust chain: Customer → You (platform builder) → App developer. gethull.dev is completely out of the picture.

---

## 4. Sandbox Enforcement by Platform

### Linux (gcc/clang + jart/pledge polyfill)

| Mechanism | Implementation | Violation |
|-----------|---------------|-----------|
| Syscall filter | seccomp-bpf via jart/pledge | SIGKILL (unbypassable, kernel-enforced) |
| Filesystem restriction | Landlock via jart/pledge | EACCES or SIGKILL |
| Mode | `__pledge_mode = KILL_PROCESS \| STDERR_LOGGING` | Process killed + diagnostic to stderr |

**Allowed pledge promises:** `stdio inet rpath wpath cpath flock dns` (dns only if hosts declared)

**CVE classes prevented:**
- Arbitrary file access outside declared paths
- Privilege escalation via undeclared syscalls
- Shell escape / command injection (no `exec` pledge)
- Network exfiltration to undeclared hosts
- Device access, mount, ptrace, raw sockets

### Cosmopolitan APE (cosmocc)

| Mechanism | Implementation | Violation |
|-----------|---------------|-----------|
| Syscall filter | Native pledge() in cosmocc libc | SIGKILL |
| Filesystem restriction | Native unveil() | ENOENT |
| Static binary | No dynamic linking | N/A |

**Additional protections:**
- Works on Linux, FreeBSD, OpenBSD, Windows (via NT security)
- No dynamic linking → no LD_PRELOAD attacks
- No DLL injection
- No dynamic linker attacks
- W^X enforcement by Cosmopolitan runtime

### macOS (gcc/clang)

| Mechanism | Implementation | Violation |
|-----------|---------------|-----------|
| Kernel sandbox | **Not available** | N/A |
| C-level validation | Capability functions | Returns error |

**Active defenses:**
- `hl_cap_fs_validate()` rejects path traversal
- `hl_cap_env_get()` enforces allowlist
- `hl_cap_db_query()` uses parameterized binding
- `hl_cap_http_request()` validates host allowlist

**Honest limitation:** If a bug exists in the C validation layer, macOS has no kernel backup. A vulnerability in `hl_cap_fs_validate()` would allow filesystem access. On Linux/Cosmo, the kernel catches it anyway. This is a known, documented limitation.

---

## 5. What the Manifest Tells You

The manifest is the app's **declared behavior contract**:

```lua
app.manifest({
    fs = { read = {"data/"}, write = {"data/uploads/"} },
    env = {"PORT", "DATABASE_URL", "API_KEY"},
    hosts = {"api.stripe.com", "hooks.slack.com"}
})
```

**What this tells an auditor:**
- This app reads from `data/`, writes to `data/uploads/`
- It reads 3 environment variables
- It makes HTTP calls to Stripe and Slack only
- It has NO other filesystem, environment, or network access

**How the system enforces it:**

| Level | Enforcement | Bypass |
|-------|-------------|--------|
| Kernel | unveil() seals filesystem to declared paths | SIGKILL on violation (Linux/Cosmo) |
| Kernel | pledge() restricts to declared syscall families | SIGKILL on violation (Linux/Cosmo) |
| C | Every capability function validates against manifest | Returns error on violation |
| Signature | Manifest is signed — tampering invalidates signature | Ed25519 forgery required |

**No manifest declared?** If the app doesn't call `app.manifest()`:
- No kernel sandbox is applied
- C-level capabilities still function (with defaults)
- Signature still covers the absence of manifest (`"manifest": null`)

---

## 6. Verification Tools

### A. Browser Verifier (`verify/index.html`)

- Self-contained HTML file, zero server dependencies
- Runs entirely in browser — no data sent anywhere
- Inlined tweetnacl-js (public domain) for Ed25519
- Web Crypto API for SHA-256
- CSP: `default-src 'none'` — no network except optional key fetch via `connect-src https:`
- gethull.dev platform key hardcoded — auto-verifies against known key
- Canary scanner: searches uploaded binary for `HULL_PLATFORM_CANARY` magic bytes

**Checks performed:**
1. Platform signature validity (Ed25519)
2. Platform key match against pinned gethull.dev key
3. App signature validity (Ed25519)
4. Developer key match (if provided)
5. Binary hash match (if binary uploaded)
6. Platform canary presence + integrity (if binary uploaded)
7. Source file hash verification (if files uploaded)
8. Manifest capability display with risk levels

### B. CLI Verifier (`hull verify`)

```
hull verify [--platform-key <file|url>] [--developer-key <file|url>] [app_dir]
```

- Reads `package.sig` (or `hull.sig` for backwards compat)
- Verifies platform layer Ed25519 signature
- Verifies app layer Ed25519 signature
- Recomputes SHA-256 of all declared files
- Reports mismatches, missing files, key mismatches
- Exit code 0 = all checks passed, 1 = failure

### C. Runtime Verifier (`--verify-sig`)

```
./myapp --verify-sig dev.pub
```

- Checks on every startup before accepting connections
- Platform key pinned at compile time (`HL_PLATFORM_PUBKEY_HEX`)
- Verifies both signature layers
- Verifies file hashes against embedded entries
- Refuses to start if any check fails

---

## 7. Trusted Rebuild Infrastructure (Future — Phase 9)

**Service:** `api.gethull.dev/ci/v1`

### Flow

1. Developer pushes source to GitHub
2. CI calls `api.gethull.dev/ci/v1/build`
3. Service rebuilds with exact `cc_version` + `flags` from `package.sig`
4. Compares `binary_hash`
5. If match → issues "Reproducible Build Verified" attestation
6. Attestation is an Ed25519 signature over `{binary_hash, timestamp, builder_version}`

### What This Proves

A passing reproducible build check proves the developer **could not have** injected custom native code. The binary is provably just "Hull platform + declared source files."

### Why It Works

1. App developers cannot write C — only Lua/JS source
2. Platform binary is hash-pinned — `platform.sig` locks exact bytes
3. Trampoline (`app_main.c`) is deterministic — generated from template
4. Cosmopolitan produces deterministic output — static linking, no timestamps
5. Build metadata is signed — `cc_version` + `flags` attested by developer

### Self-Hosted Alternative

Run your own rebuild service. Pin your own platform key. Your customers trust you, not gethull.dev.

---

## 8. Keel HTTP Server Audit

The Keel HTTP server library (vendored at `vendor/keel/`) has been audited for memory safety, input validation, resource management, and network security. Full report: [keel_audit.md](keel_audit.md).

**Key findings relevant to security:**

| Severity | Issue | Impact |
|----------|-------|--------|
| Critical | kqueue `kl_event_mod` doesn't handle READ\|WRITE bitmask | HTTP/2 write starvation on macOS |
| Critical | WebSocket `ws_send_frame` partial writes | Frame corruption on non-blocking sockets |
| High | HTTP/2 and WebSocket 101 upgrade partial writes | Protocol stream corruption |
| High | TLS private key material not zeroed before free | Key residue in heap memory |
| Informational | Request smuggling mitigation present | `Transfer-Encoding: chunked` zeroes `Content-Length` (RFC 7230 §3.3.3) |
| Informational | Header injection guard present | `contains_crlf()` rejects `\r`/`\n` in header names/values |

**Build hardening verified:**
- `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror` in production
- `-fstack-protector-strong` (non-Cosmopolitan builds)
- ASan + UBSan debug build (`make debug`)
- Two libFuzzer targets (HTTP parser + multipart parser)
- 229 unit tests across 13 suites

---

## 9. Known Limitations

These are real, not theoretical:

| Limitation | Impact | Mitigation |
|------------|--------|------------|
| macOS has no kernel sandbox | C-level validation bugs allow bypass | Use Linux or Cosmo for production |
| Lua lacks instruction-count metering | Infinite loops are only caught by memory limit | QuickJS has precise gas metering |
| Canary is not foolproof | Attacker could embed magic bytes in custom binary | Reproducible builds (Phase 9) eliminate this |
| `realpath()` is TOCTOU | Race between check and use | Kernel unveil prevents actual access |
| No manifest = no kernel sandbox | Unsigned apps have reduced protection | Signature verification catches missing manifest |
| 32-entry limit per manifest category | Large apps may hit ceiling | Sufficient for most production apps |
| `req.ctx` uses raw malloc (not tracked) | ctx JSON bypasses runtime memory limits | Capped at 64KB; bounded by runtime heap indirectly |
| HMAC-SHA256 binding returns hex string | Callers must use constant-time comparison | `hull.jwt` and `hull.csrf` stdlib use constant-time internally |
