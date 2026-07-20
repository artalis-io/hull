# Hull. Security Model

This document is brutally explicit about what Hull protects against, what it doesn't, and where trust anchors lie.

---

## 1. Trust Model Overview

### Parties

| Party | Controls | Must Trust |
|-------|----------|------------|
| **Platform publisher** (gethull.dev) | Platform library, signing key, build service | Nothing (self-sovereign) |
| **App developer** | Application code, signing key, manifest | Platform publisher (or vendor their own) |
| **End user** | Which apps to run, which keys to trust | App developer + platform publisher |
| **Third-party auditor** | Nothing. Read-only verification | Cryptographic math (Ed25519, SHA-256) |

### Key Insight

The system is designed so that **no party requires blind trust**:
- Users can verify the platform (signature + canary + source audit)
- Users can verify the app (signature + file hashes + manifest inspection)
- Users can eliminate gethull.dev entirely (self-host, self-sign)

---

## 2. Signature Verification Chain

Verification points across the lifecycle. As of v0.1.6, every release is
covered by **three independent trust roots, any one sufficient** to prove
a binary's authenticity. A compromise of any single trust root leaves the
other two intact.

| When | What | Tool | Trust root |
|------|------|------|------------|
| Before download | Inspect capabilities | `verify/index.html` (offline) | Gethull platform-sig + app sig + manifest |
| Before install | Verify integrity | `hull verify --developer-key` (CLI) | All three Ed25519 layers + file hashes |
| At startup | Runtime check | `--verify-sig` flag | All three Ed25519 layers + file hashes |
| **Post-install (v0.1.6)** | **Self-check binary** | **`hull verify-self`** | **Embedded gethull release pubkey + manifest hash for this asset** |
| **Any time (v0.1.6)** | **Independent verify** | **`cosign verify-blob hull.sha256 --certificate hull.sha256.cosign.pem --signature hull.sha256.cosign.sig`** | **Sigstore Fulcio CA + GitHub OIDC + Rekor transparency log entry. No gethull-managed key in the chain** |
| **Any time (v0.1.6)** | **Per-binary provenance** | **`gh attestation verify hull-linux-x86_64 --repo artalis-io/hull`** | **GitHub Actions OIDC + Sigstore Fulcio + SLSA build-provenance attestation** |

**Auth-stack audit convergence (v0.3.0).** Beyond the trust roots,
the v0.3.0 web/auth surface (`auth-flows`, `session`, `audit-log`,
`auth-health`, `totp`, `oauth`, `pwned`) went through **13 rounds
of iterative parallel-reviewer audits**. Convergence was reached
when three independent reviewer slices each returned zero findings
across the auth-stack source. The convergence marker is reachable
post-hoc via `git log --grep='^auth: round-'` on the main branch.
See §3.A for the per-attack defense list this body of work landed.

The platform layer split into two sub-layers in v0.1.3:

- **Gethull layer** (`package.sig.platform.gethull`). The per-arch
  SHA-256 manifest of `libhull_platform.a`, signed at release time by
  `HULL_PLATFORM_KEY` and verified by every consumer against the
  hard-coded `HL_PLATFORM_PUBKEY_HEX`. This is the proof that the
  platform bytes baked into a built app are the ones gethull.dev
  published.
- **Per-app platform layer** (`package.sig.platform.{platforms,
  signature, public_key}`). The developer's own
  `hull sign-platform` output, kept for back-compat and for forks
  that pin a non-gethull platform pubkey. Verified for
  self-consistency only. No upstream-key pinning, since that role
  moved to the gethull layer.

### Trust Anchors

**Gethull platform public key:**
- Hardcoded in Hull CLI as `HL_PLATFORM_PUBKEY_HEX` in
  `include/hull/signature.h`
  (`2a5461235aa51bbbe1e9cbc462e6a63f37d099f5ad17646a8f3a67db2f3a4fad`,
  active since v0.1.3)
- Hardcoded in the browser verifier (`GETHULL_DEV_PLATFORM_KEY` in
  `site/verify.html`). Must rotate in lockstep
- Override at compile time with
  `-DHL_PLATFORM_PUBKEY_HEX="<your hex>"` for forks running their
  own platform-signing key (Section 3.D); pass
  `--no-verify-platform` to skip the gethull check entirely

**Developer public key:**
- Published in app repository (`.pub` file)
- Manually cross-referenced by user against trusted source
- Passed explicitly: `hull verify --developer-key dev.pub`

### Key rotation

`HL_PLATFORM_PUBKEY_HEX` and `HL_RELEASE_PUBKEY_HEX` are baked into
every hull binary at compile time, so rotation is **release-driven,
not in-place**. There is no key-distribution service to ping; users
get a new key by installing a new hull binary, and old hulls keep
working under the old key they shipped with.

**Scheduled rotation** (planned key change for hygiene):

1. Generate a new keypair offline. Store the secret half in the
   same locations as the current key: `~/.hull/keys/<key>.key`
   (with offline backup) and the `HULL_PLATFORM_KEY` /
   `HULL_RELEASE_KEY` GitHub Actions secret.
2. Update `HL_PLATFORM_PUBKEY_HEX` in
   `include/hull/signature.h` (and `HL_RELEASE_PUBKEY_HEX` in
   `include/hull/release.h` if rotating that key). Update
   `GETHULL_DEV_PLATFORM_KEY` / `GETHULL_DEV_RELEASE_KEY` in
   `site/verify.html` to match.
3. Ship a hull release. The release-time signing job uses the new
   secret; the binary embeds the new pubkey. Every artifact in this
   release (the hull binary itself + every app built with it) is
   signed under the new key.
4. Document the rotation in `CHANGELOG.md`. The note should name
   the new pubkey hex so users with `gpg --verify`-style workflows
   can cross-check.

**Post-compromise rotation** (suspected or confirmed secret-key
disclosure):

Same procedure, expedited, plus:

5. Revoke the compromised key by publishing a notice on
   `gethull.dev` and the GitHub repo. The honest disclosure is
   "any hull binary built with secret `<old key>` between
   `<dates>` may have been signed by an unknown party."
6. Users running the affected hull binaries should `hull update`
   (which fetches the new release, verified with the *old* embedded
   release pubkey since the user's still running the old hull). If
   the compromise was of the release key itself, users have to
   download a known-good hull manually and verify it out-of-band
   (e.g. compare SHA-256 against the published value on a separate
   trusted channel).

**Non-cross-validity is the design.** A new hull cannot verify a
gethull-layer signature produced by the old key, because the new
hull only has the new pubkey embedded. This is intentional:
old-app + new-hull combinations correctly fail
`--verify-sig`, surfacing the rotation rather than silently
accepting both keys. Users who need to keep verifying old apps
should keep the matching old hull binary around, OR rebuild the
app with the new hull.

**Why we can't impersonate ourselves.** An attacker who publishes
a hull binary claiming to be gethull-built must produce a
`hull.sha256.sig` that verifies against the embedded
`HL_RELEASE_PUBKEY_HEX` in the user's current hull. Without the
release secret half, this is computationally infeasible. The
release-sig chain protects against impersonation of the
distribution channel; the gethull layer adds the per-platform
binary-integrity property on top of it.

---

## 3. Attack Model

### A. Malicious App Developer

This is the primary threat model. Hull exists to make it possible to trust apps from unknown developers.

**Attack: Ship a binary without the Hull platform (custom runtime, no sandbox)**

- **Prevention (v0.1.3+):** Gethull platform-sig. At `hull build`
  time the SHA-256 of the `libhull_platform.a` being embedded is
  cross-checked against the per-arch entry in the signed manifest
  inherited from the building hull binary; mismatch hard-rejects
  unless `--no-verify-platform` is set. At runtime `--verify-sig`
  re-verifies the manifest signature against `HL_PLATFORM_PUBKEY_HEX`
  and refuses to start on missing/invalid block. Browser verifier
  performs the same gethull-layer check.
- **Remaining risk:** Developer could rebuild Hull from source with a
  different `HL_PLATFORM_PUBKEY_HEX` (their own key) and re-sign the
  manifest themselves. Browser verifier and CLI both detect this as a
  key mismatch. The gethull-signed layer is missing or signed by an
  unexpected key, the user sees a warning in both UIs.
- **Out of scope:** Post-install binary integrity (an attacker with
  local write access to the on-disk `hull` binary editing
  `HL_PLATFORM_PUBKEY_HEX` and re-signing). This is the same threat
  class as any local malware with file-system write access. Not
  something the signature scheme can prevent. Defense lives at the
  OS layer (signed system updates, FIM, SELinux/AppArmor, etc.).
  Reproducible builds (`make reproducible-check`, CI-gated) make
  the bytes-on-disk cross-checkable against the published source.

**Attack: Declare minimal manifest but access more at runtime**

- **Prevention:** Manifest is signed in `package.sig`. At runtime, pledge/unveil enforce the declared capabilities at the kernel level. Accessing undeclared paths triggers SIGKILL (Linux/Cosmo).
- **Remaining risk:** On macOS, Seatbelt returns EPERM (operation denied) rather than SIGKILL. The app continues running after a violation. The forbidden operation simply fails. On Linux/Cosmo, the process is killed on violation. The practical security is equivalent (the operation is denied either way), but the failure mode differs.

**Attack: Call `app.manifest()` again at runtime to escalate capabilities**

- **Prevention:** Three independent barriers make this a non-issue:
  1. **One-shot enforcement:** `app.manifest()` errors on second call in both Lua and JS runtimes. The first call writes to a registry key; any subsequent call raises a runtime error (`"app.manifest() can only be called once"`).
  2. **Startup-only extraction:** The manifest is read from the runtime into a C struct (`HlManifest`) once during startup (step 10 of the boot sequence). C-level capabilities (`rt->env_cfg`, `rt->http_cfg`, `rt->csp_policy`) are wired from this struct and never re-read from the runtime state.
  3. **Kernel seal:** `unveil(NULL, NULL)` seals filesystem visibility and `pledge()` restricts syscall families. Both are one-way operations. The kernel refuses to add permissions after sealing, regardless of what the runtime state says.
- Even without the one-shot guard, a second `app.manifest()` call would only overwrite the Lua/JS registry key with no effect on the already-wired C capabilities or the sealed kernel sandbox. The guard exists to make the immutability explicit and prevent developer confusion.

**Attack: SQL injection through user input**

- **Prevention:** All database access goes through `hl_cap_db_query()` / `hl_cap_db_exec()` which use SQLite parameterized binding (`sqlite3_bind_*`). SQL is always a literal string from app code. No string concatenation, ever. SQL injection is structurally impossible.

**Attack: Path traversal to read /etc/passwd**

- **Prevention:** `hl_cap_fs_validate()` rejects:
  - Absolute paths (starts with `/`)
  - `..` components
  - Any path that resolves outside the app's base directory via `realpath()` ancestor check
  - Symlink escapes (realpath resolves symlinks before checking)
- Kernel unveil() also blocks access to undeclared paths.

**Attack: Inject native code at runtime via JIT, dlopen, or mmap(W|X)**

- **Prevention:** Hull's W^X policy makes guest-controlled memory non-executable and writable→executable transitions impossible. Executable memory only comes from Hull itself or from predeclared, signed build-time artifacts. The policy is enforced at four layers:
  - **Lua VM:** `package`, `package.loadlib`, `package.cpath`, `debug`, `os`, `io`, `load`, `loadfile`, `dofile` are absent from the sandbox. The Lua interpreter has no JIT.
  - **JS VM:** `eval` and `Function` are removed from the global; the `std`/`os` QuickJS modules are never registered; module resolution is whitelist-only. QuickJS has no JIT.
  - **WASM:** WAMR is compiled without `WASM_ENABLE_JIT` and `WASM_ENABLE_FAST_JIT` (the C source carries `#error` directives so the policy fails at build time if either is re-enabled). Modules run via the fast interpreter or as AOT artifacts produced at `hull build` time and embedded in the VFS. Never JIT-compiled at runtime. `init_args.running_mode = Mode_Interp` is set explicitly in `hl_cap_wasm_init`.
  - **Kernel (Linux/Cosmopolitan):** seccomp-bpf via the jart/pledge polyfill denies `mmap` with `PROT_WRITE|PROT_EXEC`, denies `mprotect` adding `PROT_EXEC` (including `pkey_mprotect`), returns `ENOSYS` from `memfd_create`, and kills the process on `execve`/`execveat`/`ptrace`/`process_vm_readv`/`process_vm_writev`. None of those promises are granted in the phase-2 promise set.
  - **Kernel (macOS):** The signed release binary uses Hardened Runtime (`codesign --options=runtime`) and deliberately does NOT carry `com.apple.security.cs.allow-jit` or `com.apple.security.cs.allow-unsigned-executable-memory`. The kernel refuses any RWX mapping under these conditions. When `HL_RELEASE_BUILD` is set, Hull verifies via `csops(CS_OPS_STATUS)` that Hardened Runtime is active and fails closed if it is not. (We deliberately do not add a Seatbelt `(deny dynamic-code-generation)` clause: WAMR uses `MAP_JIT` for non-executable linear-memory housekeeping, so the SBPL clause would block legitimate memory allocation even though no code is being generated. The defense at this layer is the entitlement-and-Hardened-Runtime check, not SBPL.)
- **Remaining risk:** Apps that explicitly opt in via `app.manifest({ allow_dynamic_code = true })` or `allow_dynamic_libraries = true` are rejected by the sandbox unless the operator additionally passes `--no-sandbox` (development only). There is no second downgrade flag and no silent fallback. Documented downgrade is the existing `--no-sandbox` flag.

**Attack: Memory exhaustion / DoS via infinite allocation**

- **Prevention:**
  - Lua: Custom allocator enforces 64 MB heap limit. Exceeding → NULL allocation → script error, not crash.
  - QuickJS: `JS_SetMemoryLimit()` enforces 64 MB. Exceeding → allocation failure → JS exception.

**Attack: Infinite loop / CPU exhaustion**

- **Prevention:**
  - QuickJS: Instruction-count interrupt handler via `JS_SetInterruptHandler`. Configurable `max_instructions` limit (default 100M). Exceeding → JS exception.
  - Lua: Instruction-count hook via `lua_sethook(LUA_MASKCOUNT)`. Same configurable `max_instructions` limit (default 100M). Exceeding → `luaL_error("instruction limit exceeded")`. Hook is re-applied on every dispatch, coroutine resume, and async continuation.
  - Both: Override with `--max-instructions N` or `HULL_MAX_INSTRUCTIONS` env var.

**Attack: Exfiltrate data to unauthorized hosts**

- **Prevention:** `hl_cap_http_request()` validates the target host against the manifest's `hosts` allowlist. Only declared hosts are reachable. Kernel pledge includes `inet` + `dns` only if hosts are declared.

**Attack: Read environment variables (API keys, secrets)**

- **Prevention:** `hl_cap_env_get()` checks against the manifest's `env` allowlist (max 32 entries). Undeclared variables return NULL.

**Attack: Cross-site scripting (XSS) via template output**

- **Prevention:** Two layers of defense:
  1. **Template auto-escaping:** Hull's template engine (`hull.template`) HTML-escapes all `{{ }}` output by default. The five dangerous characters (`& < > " '`) are replaced with HTML entities. This prevents reflected and stored XSS from user-controlled data rendered into HTML templates.
  2. **Content-Security-Policy (CSP):** Hull injects a strict CSP header on every `res:html()` / `res.html()` response by default: `default-src 'none'; style-src 'unsafe-inline'; img-src 'self'; form-action 'self'; frame-ancestors 'none'`. This blocks inline scripts, external script loads, `eval()`, object embeds, and iframe embedding. Even if an attacker bypasses template escaping, the browser refuses to execute injected scripts.
- **Remaining risk:** Raw output (`{{{ }}}`) and the `| raw` filter bypass escaping. Developers must only use raw output with trusted content. Templates don't escape for JavaScript string contexts (e.g. inline `<script>` blocks). Use `{{ var | json }}` to safely embed data in JS contexts. Apps that require client-side JavaScript must customize the CSP (e.g. `app.manifest({ csp = "default-src 'self'; script-src 'self'" })`).

**Attack: Clickjacking. Embedding the app in a malicious iframe**

- **Prevention:** The default CSP includes `frame-ancestors 'none'`, which instructs the browser to refuse rendering the page inside any `<iframe>`, `<frame>`, or `<object>` tag. This prevents UI redress attacks where a malicious site overlays invisible frames over the app to trick users into clicking hidden elements.
- **Actor:** Any third-party website operator. Does not require compromising the app. Just embedding it.

**Attack: MIME type confusion / content sniffing**

- **Prevention:** The default CSP's `default-src 'none'` prevents the browser from loading any sub-resources (scripts, stylesheets, fonts, media) that an attacker might inject via reflected content. Combined with `Content-Type: text/html; charset=utf-8` set by `res:html()`, the browser cannot misinterpret response content.
- **Actor:** Network MITM or injection via stored user content.

**Attack: Template injection (server-side template injection / SSTI)**

- **Prevention:** Template compilation uses `luaL_loadbuffer` / `JS_Eval` in the C bridge, which is only callable from embedded stdlib code (not user application code). The code generator produces deterministic output from the AST. User data is never interpolated into the generated source code. User data flows through the `__d` (data) parameter at render time, not at compile time. There is no `eval()` or `load()` in the sandboxed runtimes.

**Attack: Session hijacking via cookie theft**

- **Prevention:** `hull.web.cookie` defaults to `HttpOnly=true`, `Secure=true`, `SameSite=Lax`. HttpOnly prevents JavaScript access (XSS-based theft). Secure prevents plaintext transmission. SameSite=Lax blocks cross-origin POST requests from carrying session cookies.
- **Remaining risk:** Same-origin XSS can still read `req.ctx.session` data. Hull's template engine (`hull.template`) auto-escapes all `{{ }}` output by default (`& < > " '` → HTML entities), which prevents most reflected and stored XSS vectors. Raw output via `{{{ }}}` or the `| raw` filter bypasses escaping and should only be used with trusted content.

**Attack: CSRF. Forged state-changing requests from another origin**

- **Prevention:** `hull.web.middleware.csrf` middleware generates HMAC-based tokens tied to the session ID and timestamp. State-changing methods (POST/PUT/DELETE/PATCH) require a valid CSRF token in the `X-CSRF-Token` header or `_csrf` form field. Tokens expire (default 1h). Safe methods (GET/HEAD/OPTIONS) are automatically skipped. Constant-time comparison prevents timing attacks.
- **Remaining risk:** If the CSRF secret is leaked, tokens can be forged. The secret must be stored securely (e.g., `env.get("SECRET_KEY")`).

**Attack: JWT token forgery**

- **Prevention:** `hull.jwt` ships HS256/384/512 (HMAC) plus
  asymmetric verify for RS256/384/512, PS256, ES256/384 via the C
  capability layer (mbedTLS-backed `hl_cap_asym_*` vtable). The
  `none` algorithm is rejected unconditionally, and the verify path
  rejects any token whose `alg` header doesn't match the algorithm
  the caller asked for. There is no algorithm negotiation. HMAC
  comparisons are constant-time; asymmetric verifications run
  through the backend's own constant-time primitives. Expired and
  not-yet-valid tokens are rejected.
- **Remaining risk:** Symmetric secrets must be strong (≥32 random
  bytes). Public-key JWTs need the IdP's signing certs pinned via
  JWKS lookup (the `web/middleware/oauth` flow does this for you;
  bare `hull.jwt.verify` callers must supply the pubkey
  themselves). JWTs are stateless and cannot be revoked until they
  expire. For revocation, use sessions instead.

**Attack: OAuth / OIDC ID-token forgery or replay**

- **Prevention:** `hull.web.middleware.oauth` runs a full
  Authorization-Code-plus-PKCE flow with IdP-pinning at every step:
  it fetches the IdP's JWKS, materializes the signing certificate
  chain via `crypto.x509_pubkey_pem`, and only allows
  `RS256/RS384/RS512/PS256/ES256/ES384` for ID-token verification
  (HS256 is excluded because no OIDC IdP signs ID tokens with HMAC).
  The per-request `state`, `nonce`, and PKCE verifier are HMAC-bound
  into a single HttpOnly cookie keyed by `state_secret` so a
  CSRF'd callback or a cross-provider replay can't forge a session.
  Allowed Microsoft multi-tenant configs may set `issuer_pattern`
  for the `/{tenant-guid}/v2.0` shape; without it Microsoft
  `tenant=common` is rejected because issuer would never equal
  `/common/v2.0`.
- **Remaining risk:** Trust in the IdP's CA / cert-chain is
  inherited. Operators who don't want this exposure should pin the
  pubkey directly via the explicit-provider form instead of the
  `preset = "google" | "microsoft"` shortcut.

**Attack: TOTP code brute-force or replay**

- **Prevention:** `hull.web.middleware.totp` enforces a small drift
  window (±1 step by default), uses constant-time HMAC comparison
  on every code, persists the last-used step per user, and refuses
  any code at or below the high-water mark — so a single TOTP code
  cannot be reused within its 30-second window even by the
  legitimate user. Failed attempts are counted; the auth-flows
  helper folds TOTP failures into the same lockout machinery as
  password failures.
- **Remaining risk:** Shared-secret leakage at enrollment time
  (e.g. screenshot of the QR code) is out of scope. The dual-row
  enrollment design exists specifically so the secret only persists
  after a verified first code, capping the window where an
  unconfirmed secret sits in the database.

**Attack: Credential stuffing with a known-breached password**

- **Prevention:** `hull.web.pwned` performs the Have-I-Been-Pwned
  k-anonymity check at password set / change / reset time: it hashes
  the candidate, sends only the first five hex characters of the
  SHA-1 to `api.pwnedpasswords.com`, and rejects passwords that
  appear in the breach corpus. The capability is **fail-open** by
  design when the upstream service is unreachable, so an outage at
  the HIBP API doesn't lock every user out of their account; the
  `hull agent auth-status` JSON surfaces whether the most recent
  check succeeded (`reachable | fail_open | not_yet_checked`).
- **Remaining risk:** Fail-open trades off "block all password
  changes during an outage" for "let users through with an
  uncheckable password." Operators that prefer fail-closed should
  wrap the call site themselves and reject when `pwned.check`
  returns `nil, "unreachable"`.

**Attack: Online password brute-force / account guessing**

- **Prevention:** `hull.web.auth-flows` tracks per-account failure
  counts in the same DB-backed table that drives the registration
  / login / verify / magic-link / password-reset / email-change
  flow. After N consecutive failures (configurable, default 5) the
  account is locked for a backoff window. Token / verification
  tables use cryptographically random IDs with a TTL; replay of a
  consumed token is rejected because the consume path deletes the
  row atomically.
- **Remaining risk:** A patient attacker who knows the exact lockout
  policy can still mount slow-rate guessing if rate-limit middleware
  isn't also wired. The recommended stack is `ratelimit + auth-flows`
  with the rate-limit key being the source IP or session id (see
  `examples/auth_flows_*`).

**Attack: Multipart upload payload injection / type confusion**

- **Prevention:** The streaming multipart iterator (`req:multipart()`
  / `req.multipart()`) decodes parts incrementally without
  materializing the full body in memory; the per-part filename is
  validated by `hull.web.attachment` (rejecting `..`, absolute
  paths, control chars, and reserved Windows names) and the
  declared Content-Type is cross-checked with `hull.mime`'s magic-
  byte sniffer — a part claiming `image/png` whose bytes don't
  start with the PNG signature is rejected before it reaches blob
  storage. Bytes are streamed straight into the content-addressed
  blob store (`hull.blob`), which keys by SHA-256, so two uploads
  of the same content collapse to one on-disk file.
- **Remaining risk:** Magic-byte sniffing is best-effort. Formats
  with no fixed leading bytes (raw `application/octet-stream`,
  some text formats) fall back to the declared Content-Type. Apps
  that handle arbitrary uploads should pair the iterator with their
  own per-format validator before exposing the blob to other
  systems.

**Attack: Audit-log tampering or evidence destruction**

- **Prevention:** `hull.web.middleware.audit-log` writes a strict-
  append-only event stream into a dedicated SQLite table. The
  schema has no `UPDATE` path exposed; deletion is reserved to the
  scheduled cleanup window which retains events for a configurable
  window (default 90 days). Events carry actor, action, resource,
  IP, user-agent, and request ID; the resource column is the
  primary index. `hull agent auth-status` reports whether cleanup
  is scheduled (`scheduled | external | missing`) so ops can
  detect a misconfigured retention policy from the CLI.
- **Remaining risk:** The audit log lives in the same SQLite file
  as the app's other tables. Operators who need stronger non-
  repudiation should ship the events to an external WORM store
  (e.g. via the outbox helper) — set the `external` cleanup mode
  to silence the auth-status warning.

**Attack: Session fixation / brute-force session IDs**

- **Prevention:** `hull.web.middleware.session` generates 32 random bytes (256-bit entropy) via `crypto.random()` for session IDs. IDs are hex-encoded (64 chars). Sessions are server-side (SQLite) with sliding expiry. Expired sessions are automatically pruned.

### Browser-Level Security Headers

Hull injects security headers automatically at the C level to provide defense in depth:

**Content-Security-Policy (CSP):**

Default policy (applied to all `res:html()` / `res.html()` responses):
```
default-src 'none'; style-src 'unsafe-inline'; img-src 'self'; form-action 'self'; frame-ancestors 'none'
```

| Directive | Value | Blocks |
|-----------|-------|--------|
| `default-src` | `'none'` | All resource types not explicitly allowed (scripts, fonts, media, objects, workers, WebSockets) |
| `style-src` | `'unsafe-inline'` | External stylesheets (inline styles allowed for SSR convenience) |
| `img-src` | `'self'` | Images from external origins |
| `form-action` | `'self'` | Form submissions to external origins (data exfiltration via `<form action="evil.com">`) |
| `frame-ancestors` | `'none'` | Embedding in iframes on any origin (clickjacking) |

**What the default CSP mitigates:**

| Attack | Actor | How CSP Blocks It |
|--------|-------|-------------------|
| Reflected XSS (injected `<script>`) | Any user who can craft a malicious URL | `default-src 'none'` blocks inline script execution |
| Stored XSS (persisted `<script>`) | Authenticated user who stores malicious content | `default-src 'none'` blocks inline script execution |
| External script injection (`<script src="evil.js">`) | Attacker who bypasses template escaping | `default-src 'none'` blocks all external script loads |
| `eval()`-based XSS | Attacker who injects data into JS eval context | `default-src 'none'` implicitly disables `eval()` and `Function()` |
| Clickjacking (iframe embedding) | Any third-party site operator | `frame-ancestors 'none'` refuses rendering in iframes |
| Form action hijacking | Attacker who injects `<form action="evil.com">` | `form-action 'self'` restricts form targets to same origin |
| Data exfiltration via `<img src="evil.com/steal?data=...">` | Attacker with XSS who tries to leak data via image tags | `img-src 'self'` blocks images from external origins |
| Keylogging via injected external JS | Attacker who loads a remote keylogger script | `default-src 'none'` blocks all external resource loads |

**CSP configuration:**

| Manifest | Behavior |
|----------|----------|
| No `app.manifest()` | Default strict CSP (defense in depth) |
| `app.manifest({})` | Default strict CSP |
| `app.manifest({ csp = "htmx" })` | Named preset expanded at startup: `default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; connect-src 'self'; form-action 'self'; frame-ancestors 'none'`. Right for htmx-driven SSR apps with stdlib JS / CSS served from `/static/`. `data:` images are allowed for inline SVG favicons + Pico-style inline icons (XSS-safe; browsers don't execute scripts inside `<img>` content). Apps needing nonce-based stricter CSP should use `hull/web/middleware/csp@1` with the htmx profile instead of this static preset. |
| `app.manifest({ csp = "custom..." })` | Custom CSP string (unknown preset names pass through as literal policies — no risk of typos silently becoming `default-src 'none'`). |
| `app.manifest({ csp = false })` | CSP disabled (opt-out) |

**Where CSP is injected:** At the C level in `lua_res_html()` and `js_res_html()`, not in application code. This means the CSP cannot be forgotten, bypassed, or misconfigured by app developers. It's structural, like parameterized SQL. Only `res:json()` and `res:text()` skip CSP (non-HTML content types are not vulnerable to script injection).

### B. Malicious Third Party (MITM / CDN Compromise)

**Attack: Replace binary on CDN with modified version**

- **Prevention:** `binary_hash` in `package.sig` is signed by the developer's Ed25519 key. Changed binary → hash mismatch. Browser verifier catches this immediately when binary is uploaded.

**Attack: Replace `package.sig` with forged one**

- **Prevention:** Signature is Ed25519 over the canonical payload. Forging requires the developer's 32-byte private key. Ed25519 is considered secure against all known attacks.

**Attack: Replace both binary and `package.sig`**

- **Prevention:** User verifies the developer's public key against a trusted source (e.g., GitHub repo, personal website). If the attacker doesn't have the developer's private key, they can't produce a valid signature for any payload.

**Attack: Replace platform libraries in a self-hosted Hull**

- **Prevention (v0.1.3+):** The gethull-signed manifest pins the
  exact per-arch SHA-256 of `libhull_platform.a`.
  `hull build` cross-checks the on-disk `.a` against the manifest
  before signing. `--verify-sig` re-checks the manifest signature
  at startup. Swapping the platform library → SHA-256 mismatch at
  build, or signature failure at startup.

### C. Compromised gethull.dev (Platform Publisher)

**Attack: Ship malicious platform libraries**

- **Prevention:** Platform signing key is published. Users can:
  1. Audit Hull source (AGPL-3.0)
  2. Build their own platform: `make platform`
  3. Sign with own key: `hull sign-platform mykey`
  4. Pin their own key in apps

  The architecture is designed so you **don't have to trust gethull.dev**.

**Attack: Backdoor the build service**

- **Prevention:** Reproducible builds (CI-gated on Linux via `make reproducible-check`). Anyone can rebuild from source with the recorded `cc_version` + `flags` and compare `binary_hash`. The build service is a convenience, not a trust requirement. See §7 for the Tier 4 verification surface.

**Attack: Compromise app developer's machine (sign a binary that doesn't match the published source)**

- **Prevention:** Same mechanism, different attacker. The signature chain alone cannot detect this (the signature is valid; the developer's identity is intact). Reproducible builds close it: anyone re-deriving from source sees the hash mismatch immediately. This is the attack Tier 4 of the trust chain exists for; see §7.1.

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
| W^X / dynamic code | seccomp-bpf denies `mmap` and `mprotect` adding `PROT_EXEC`; `memfd_create` returns ENOSYS; `execve`/`execveat`/`ptrace`/`process_vm_*` are not in any granted promise group | SIGKILL or ENOSYS |
| Mode | `__pledge_mode = KILL_PROCESS \| STDERR_LOGGING` | Process killed + diagnostic to stderr |

**Allowed pledge promises:** `stdio inet rpath wpath cpath flock dns` (dns only if hosts declared). Notably absent: `prot_exec`, `exec`, `proc`. These grant the very syscalls Hull's W^X policy forbids.

**CVE classes prevented:**
- Arbitrary file access outside declared paths
- Privilege escalation via undeclared syscalls
- Shell escape / command injection (no `exec` pledge)
- Network exfiltration to undeclared hosts
- Device access, mount, ptrace, raw sockets
- Runtime native code injection (no `prot_exec` pledge; `memfd_create` blocked)

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
- W^X enforcement by Cosmopolitan runtime (APE loader uses fixed code segments; `MAP_JIT` and equivalent OS-specific JIT APIs are unavailable to the guest)

### macOS (gcc/clang)

| Mechanism | Implementation | Violation |
|-----------|---------------|-----------|
| Kernel sandbox | Seatbelt via `sandbox_init_with_parameters()` | EPERM (kernel-enforced) |
| Dynamic SBPL profile | Built from manifest at startup | Deny-default with selective allows |
| W^X / no JIT | WAMR built without `WASM_ENABLE_JIT` / `WASM_ENABLE_FAST_JIT` (`_Static_assert` in `src/hull/cap/wasm.c`); QuickJS / Lua have no JIT; Hardened Runtime + absent `cs.allow-jit` / `cs.allow-unsigned-executable-memory` entitlements | Build fails / kernel refuses RWX |
| Hardened Runtime probe | `csops(CS_OPS_STATUS)` at startup under `HL_RELEASE_BUILD` | Hull exits before sandbox if not active |
| C-level validation | Capability functions | Returns error |

**Seatbelt enforcement:**
- `(deny default)`. Deny-by-default SBPL profile generated dynamically from manifest
- App directory: read-only access to app files
- Database directory: read-write access to SQLite files (db, WAL, SHM, journal)
- Manifest `fs` paths: unveil-equivalent via `(allow file-read* (subpath ...))` / `(allow file-read* file-write* ...)`
- Network: TCP allowed only when manifest declares `hosts`
- GPU: `iokit-open` + MTLCompilerService mach-lookup allowed only when `manifest.gpu` is set
- CA bundle + TLS paths: read-only when HTTPS client is used
- System frameworks + dyld cache: read-only (required for process operation)
- Parameter substitution for paths. Avoids escaping issues with special characters
- Irreversible. `sandbox_init` cannot be modified or removed after application

**Hardened Runtime (out-of-repo signing concern):**

The released `hull` binary on gethull.dev is signed with `codesign
--options=runtime`, notarized, and intentionally does NOT carry the
`com.apple.security.cs.allow-jit`,
`com.apple.security.cs.allow-unsigned-executable-memory`,
`com.apple.security.cs.allow-dyld-environment-variables`, or
`com.apple.security.cs.disable-library-validation` entitlements. This
is enforced in the release pipeline, not in the repo build files.

Release builds are compiled with `-DHL_RELEASE_BUILD`. At startup Hull
calls `csops(CS_OPS_STATUS)` to verify the `CS_HARD` flag is set; if it
is not (e.g. the binary has been re-signed without `--options=runtime`,
or stripped of its signature), the sandbox refuses to start.

Self-built binaries from `make` are unsigned and lack Hardened Runtime.
In that case the `csops` check downgrades to a warning so dev workflows
continue to function. The Seatbelt `(deny dynamic-code-generation)`
clause and the C-level capability layer still apply.

**Active C-level defenses (defense-in-depth):**
- `hl_cap_fs_validate()` rejects path traversal (absolute paths, `..`, symlink escapes via realpath)
- `hl_cap_env_get()` enforces allowlist
- `hl_cap_db_query()` uses parameterized binding
- `hl_cap_http_request()` validates host allowlist

**Difference from Linux/Cosmo:** Seatbelt returns EPERM on violation (the operation fails with a permission error) rather than SIGKILL (the process is killed). The app stays alive but the operation is denied. The C capability layer returns errors on violation in all cases, so the practical behavior is identical. The forbidden operation fails.

---

## 4b. Sealed runtime tables (read-only memory protection)

The kernel sandbox stops the app from *calling* dangerous things. The
sealed-arena layer stops a memory-corruption bug from *rewriting the
policy itself* to make dangerous things look benign, or from
pivoting a function pointer to attacker-controlled code.

### Threat model

An attacker who lands a linear-write heap-corruption bug in Hull
post-boot (buffer overflow in a parser, double-free, use-after-free
in WAMR / mbedTLS / SQLite, anything that yields "I can write N bytes
to address X") has two distinct escalation paths into the policy
plane that this layer closes.

**Policy tamper (silent capability escalation).** Overwriting an
allowlist byte to relax a gate:

- The manifest's `fs_write` allowlist: change `"data/"` to `"/etc/"`.
- The manifest's `hosts` allowlist: add their C2 host.
- The manifest's `env` allowlist: add `AWS_SECRET_ACCESS_KEY`.
- The mTLS `authmode` byte: flip `VERIFY_REQUIRED` to `VERIFY_NONE`.
- A DoS cap like `max_body_size`: raise to `SIZE_MAX`.
- A CA chain root pointer: swap a Let's Encrypt root for an attacker
  cert and turn every outgoing HTTPS into a silent MITM.

The capability layer (`hl_cap_fs_validate`, `hl_cap_http_request`,
`hl_cap_env_get`) trusts the values it was handed; it has no way to
detect "this allowlist was tampered with after boot."

**Function-pointer pivot (ROP/JOP data-plane variant).** Overwriting a
function pointer in a dispatch table or callback slot to redirect a
call site to an attacker-chosen address. This is the data-plane
counterpart to the classic instruction-pointer hijack covered in §4c:
instead of corrupting a return address on the stack, the attacker
corrupts a vtable entry on the heap. The compiler-level mitigations
(PAC, BTI, CFI, stack canaries) don't see this. The call is a
normal indirect dispatch; the pointer just happens to point at a
ROP/JOP gadget chain. Sealing the tables that *hold* the function
pointers closes the primitive at the source.

High-value pivot targets that are sealed today:

- Router handler `vtable[i].handler` (per-route C fn ptr)
- Pre/post middleware function pointers
- `mbedtls_pk_info` dispatch table (every pk op routes through it)
- `KlConfig.parser` factory (invoked on every accepted connection)
- `KlConfig.log_fn` / `KlConfig.access_log` (invoked on every request)
- `mbedtls_ssl_config` RNG callback + cert / CA chain pointers
- WAMR `host_symbols` table (mapped on every WASM call)

Sealing turns the attempted write (whether to allowlist data or to
a function-pointer slot) into `SIGSEGV` at the syscall level. The
OS page table says read-only, the CPU faults, the process dies. The
attacker gets a crash instead of a silent capability escalation or
control-flow hijack.

### Two protection mechanisms

**`.rodata` (compile-time constants).** Every `static const` table
lands in the read-only data segment. The linker maps that segment with
PROT_READ; the kernel rejects writes. No runtime mprotect needed.
Hull's dispatch tables (vtables, command tables, module registry,
CSP presets, MIME table, embedded CA bundle, embedded stdlib entries)
are all `static const` and get this protection for free.

**`sh_seal_arena` (boot-built data, shared with Keel).** What's left
is data that *can't* be `static const` because it's built at boot
from app input: the manifest from `app.manifest({...})`, the
sandbox policy derived from it, the router's per-route handler
table, the parsed CA chain inside an mbedTLS context. Hull and Keel
both allocate this kind of data into a page-aligned `mmap` arena,
populate it during the boot phase, then call `mprotect(PROT_READ)`
once. Same protection as `.rodata` but for runtime-built data.

The arena primitive itself lives in `vendor/keel/vendor/sh_seal_arena/`
as a shared `sh_*`-family utility (`sh_seal_arena.h` /
`sh_seal_arena.c`, sibling to `sh_arena` and `sh_json`). Both Hull and
Keel link it; Hull additionally builds a sanitizer-instrumented
in-tree copy so ASan/MSan see arena allocations correctly when running
the Hull test suite. The API is one-way: `init` → `alloc/strdup/memdup`
→ `seal` → reads only; there's deliberately no unseal because the
whole point is irreversibility.

### What's sealed today

The sealed surface spans both halves of the C boundary. Hull-side
covers app policy + runtime hookpoints; Keel-side covers HTTP +
TLS state. The two halves shipped over Keel v2.3.0 → v2.5.1.

| Layer | Subsystem | Mechanism | Shipped in |
|---|---|---|---|
| Hull | Embedded CA bundle (Mozilla roots, `embedded_cacert`) | `.rodata` (`const`-qualified via Makefile post-process of `xxd -i` output) | (existing) |
| Hull | All dispatch tables (`HlRuntimeVtable`, `HlDbBackend`, `HlAsyncBackend`, command table, module registry, MIME table, CSP presets) | `.rodata` (`static const`) | (existing) |
| Hull | Embedded stdlib entries (`hl_stdlib_entries[]`, ~130 files) | `.rodata` (generated `build/stdlib_registry.c`) | (existing) |
| Hull | Manifest `fs_read` / `fs_write` / `hosts` / `env` / `csp` / `cors_*` / `modules[].name` strings | `sh_seal_arena` | `hl_serve_wire_caps` (`src/hull/serve.c`) post-extract, pre-resolver |
| Hull | WAMR `host_symbols` table (WASM-side native imports) | `sh_seal_arena` | `hl_cap_wasm_init` (`src/hull/cap/wasm.c`) |
| Hull | CORS config (per-route allowlists) | `sh_seal_arena` | Allocated inside the manifest arena |
| Keel | Router routes + pre-middleware + post-middleware tables | `sh_seal_arena` via `kl_router_freeze` | Keel v2.3.0 |
| Keel | mbedTLS top-level policy: `ssl_config`, `cert`, `pkey`, `ca_cert`, `authmode`, cipher allowlist, RNG callback | `sh_seal_arena` (per-context) | Keel v2.3.3 |
| Keel | `KlServer.config` mirror: `parser`, `log_fn`, `access_log`, `max_body_size`, `max_header_size`, `bind_addr`, TLS / H2 / compress storage | `sh_seal_arena` via `kl_server_freeze` + `kl_server_config(s)` accessor | Keel v2.4.0 |
| Keel | mbedTLS deep allocations (client TLS): parsed DER bytes, RSA bignum limbs + Montgomery `RN` cache, EC `grp->T` comb tables, `pk_info` dispatchers, full CA chain link nodes | `sh_seal_arena` via `mbedtls_platform_set_calloc_free` arena hook + pre-warm walker | Keel v2.5.0 / fixes v2.5.1 |

### Companion: phase-gated registration

The C-side seal is paired with a script-side gate. After
`hl_serve_wire_routes` flushes the runtime registry into the C router
and before `kl_server_freeze` mprotects the tables, Hull sets
`HlRuntime.registration_closed = 1`. From that point onward every
`app.X()` registration binding (`app.get`, `app.post`, `app.use`,
`app.use_post`, `app.ws`, `app.sse`, `app.every`, `app.daily`)
raises a structured error:

> `app.get` can only be called at app startup (top-level code or
> inside `app.main`). Hull seals the router after wire-up so dynamic
> registration from request handlers / timer callbacks is
> intentionally not supported. Move the registration to top level,
> or to an `app.main(fn)` that runs before the serve loop starts.

This is a defense-in-depth pairing: the C-side seal makes
`kl_router_add` fault on heap-write tamper, the script-side gate
gives the developer a clear, actionable error if they try the same
operation through the legitimate API path. Without the gate, a
handler that called `app.get(...)` would silently push into a Lua/JS
table that no consumer reads post wire-up.

### What's NOT sealed (by design)

| Subsystem | Why mutable |
|---|---|
| Auth secrets (`auth-flows.state_secret`, `totp.encryption_keys`, `csrf.secret`, `jwt.secret`, `oauth.state_secret`, `audit-log.fingerprint_salt`) | Live in Lua/JS `_state` tables, passed to C as per-call stack-locals with `secure_zero` after the operation. No long-lived C-side cache exists. Rotation is a feature; sealing would block it. |
| JWKS cache (oauth) | Lua-side `_state._jwks_cache`; PEM strings, not parsed mbedTLS contexts. No C-side cache. |
| Session table, idempotency table, audit log | SQLite-backed; mutated every request by design. |
| WASM module instances, GPU buffer pools, DB connection pool | Mutated every request by design. |
| **Server-side own RSA private key (`mbedtls_rsa_context` for own_pkey)** | mbedTLS's `rsa_prepare_blinding` (Kocher 1996 timing-attack defense) rewrites `ctx->Vi` / `ctx->Vf` on every signature. The cached path squares both blinding values in place; the no-blinding path is a security regression. Pre-warm doesn't help: blinding has to be fresh per sign. Server own_pkey therefore stays heap-mutable (the top-level `pk_context` struct is still sealed via v2.3.3's policy half; only the bignum limbs the context points at remain heap-resident and mutable). Mitigated by the kernel sandbox + capability layer. Documented in [`roadmap_next.md` § 8](roadmap_next.md). |
| `mbedtls_ctr_drbg` + `mbedtls_entropy` state | DRBG advances on every random draw; entropy accumulator updates per pool. Both structurally cannot live on RO pages. Initialised outside Keel's deep-seal arena window so their internal allocations stay on the heap and `mbedtls_*_free` at destroy can zero them normally. |
| Per-handshake `mbedtls_ssl_context` (per-connection session state) | Mutated continuously during the handshake state machine and the post-handshake record layer. Each connection allocates a fresh one on the heap. The shared `ssl_config` it points to is sealed (above table). |

### Convention for new code

If you're adding a new C-level structure that:

1. Is **built at boot** from configuration (manifest, env, file
   parse, or anything else not compile-time-constant), AND
2. Is **read-only at runtime** (consumed but not mutated after
   the boot phase ends), AND
3. **Affects security policy** (allowlists, capability gates,
   trust anchors, dispatch tables that route to sensitive code,
   function-pointer slots invoked on every request),

then it's a candidate for `sh_seal_arena`. The pattern:

```c
ShSealArena arena;
sh_seal_arena_init(&arena, 16 * 1024, "what-this-protects");
/* ...allocate + populate structures via sh_seal_arena_alloc / strdup / memdup... */
if (sh_seal_arena_seal(&arena) != 0) FATAL("seal failed");
/* ...subsequent reads are RO; writes SIGSEGV; allocation returns NULL... */
```

Seal failure should be fatal: the alternative is shipping with
unsealed policy, which silently weakens the hardening posture in a
way nobody notices until they're being exploited.

If you're adding a generated table via `xxd -i`, post-process the
output to prepend `const` (see the `EMBEDDED_CACERT_H` rule in the
Makefile for the pattern). Default `xxd -i` emits writable arrays
that land in `.data`, not `.rodata`.

**The `/c-audit` skill checks for both patterns.** Run it before
shipping any C runtime changes that touch security policy.

### Tests prove the OS protection actually fires

Sealed-arena protection is only worth anything if the `mprotect` call
actually faults on write. Every layer ships a fork+SIGSEGV death test
that proves it does.

| Test | Covers |
|---|---|
| `vendor/keel/vendor/sh_seal_arena/tests/test_sh_seal_arena.c::write_after_seal_faults` | Bare arena: write to a sealed page → child dies with SIGSEGV/SIGBUS. Foundation. |
| `tests/hull/test_manifest_seal.c::write_to_sealed_string_faults` | Hull manifest allowlist string write → SIGSEGV. |
| `tests/hull/test_manifest_seal.c::write_to_sealed_csp_faults` | Hull CSP / CORS config write → SIGSEGV. |
| `vendor/keel/tests/test_router.c::frozen_table_write_faults` | Router handler vtable entry overwrite → SIGSEGV (Keel v2.3.0). |
| `vendor/keel/tests/test_server_freeze.c::frozen_config_write_faults` | `KlConfig.parser` function pointer overwrite → SIGSEGV (Keel v2.4.0). |
| `vendor/keel/tests/test_server_freeze.c::frozen_max_body_size_write_faults` | DoS cap raise via heap-write → SIGSEGV. |
| `vendor/keel/tests/test_server_freeze.c::frozen_bind_addr_write_faults` | Duplicated `bind_addr` string mutation → SIGSEGV. |
| `tests/hull/cap/test_tls_mbedtls_multi_ctx.c` | Multi-context lifecycle for the v2.5.0 deep mbedTLS seal under ASan: confirms registry walk + branched destroy stay consistent across LIFO / FIFO / interleaved create-destroy patterns. |

Each child process resets `SIGSEGV` AND `SIGBUS` to `SIG_DFL` before
the offending write so sanitizer runtimes don't swallow the signal,
and the parent's `WIFSIGNALED` check accepts both signals (macOS
prefers SIGBUS for mprotect violations; Linux prefers SIGSEGV).

If `mprotect` were ever a no-op (kernel bug, platform
incompatibility, configuration mistake), the child would write
happily and exit 0, and the parent's `WIFSIGNALED` check would fail.
The tests are the only way to guarantee the protection isn't
theoretical.

---

## 4c. Compiler/linker hardening (binary-level ROP/JOP resistance)

§4 covers what a misbehaving app *can do*; §4b covers what a memory-
corruption bug *can rewrite*. This section covers the rung below:
what an attacker can do once they already control the instruction
pointer.

The capability layer cannot defend against an attacker who has
already hijacked control flow. Compiler and linker hardening flags
shrink the window between "bug present" and "exploit landed" by
making the standard ROP/JOP chains used to escalate a memory-safety
bug into RCE substantially harder to construct.

Full reference: `Makefile` lines ~69-180 (the probe-based hardening
layer) and `scripts/check_hardening.sh` (the post-build verifier).
`make hardening` prints the resolved flag list for the current
toolchain; `make check-hardening` runs the verifier against
`build/hull`.

### Threat model

Hull's design already rules out the easiest escalation paths:

- **No JIT.** Neither Lua 5.4 nor QuickJS JIT. WAMR is interpreter or
  AOT (statically compiled at build time, never written at runtime).
- **No RWX memory.** No mmap/mprotect path takes both `PROT_WRITE`
  and `PROT_EXEC` simultaneously. The sealed-manifest arena
  (`sh_seal_arena`, §4b) flips RW → RO via `mprotect` and never the
  reverse.
- **No writable function-pointer tables.** All dispatch vtables
  (`HlRuntimeVtable`, `HlDbBackend`, `HlAsyncBackend`, etc.) live in
  `.rodata` via `const` qualification (§5b of the C audit skill).

Hardening flags push the residual surface further down: a memory bug
that lands inside the `hl_cap_*` boundary still has to chain ROP
gadgets through hardened text segments to do anything useful.

### Flag set, by platform (release build)

| Flag | Linux | macOS | Cosmo | Effect |
|---|---|---|---|---|
| `-fstack-protector-strong` | ✓ | ✓ | skip | Canaries on every function with a stack buffer or `&local` taken. |
| `-fPIE` + `-pie` | ✓ | ✓ (MH_PIE) | skip | Position-independent → ASLR. |
| `-D_FORTIFY_SOURCE=3` | ✓ | ✓ | skip | Compile-time bounds checks; runtime `*_chk` variants on `memcpy`/`strcpy`/`sprintf`/etc. |
| `-Wl,-z,relro` + `-Wl,-z,now` | ✓ | n/a | skip | Full RELRO — GOT/PLT marked read-only after bind. |
| `-Wl,-z,noexecstack` | ✓ | n/a | skip | PT_GNU_STACK without X. |
| `-fstack-clash-protection` | ✓ (probed) | reject | skip | Per-frame probe; defeats stack-clash pivot. gcc 8 / clang 11+. |
| `-fno-plt` | ✓ (probed) | ✓ (probed) | skip | Direct GOT calls — shrinks ROP gadget surface and lets RELRO+BIND_NOW eliminate writable function pointers. |
| `-fno-common` | ✓ (probed) | ✓ (probed) | skip | Reject tentative definitions. |
| `-ftrivial-auto-var-init=zero` | ✓ (probed) | ✓ (probed) | skip | Zero-init stack vars — mitigates info-leak primitives. clang 8 / gcc 12+. |
| `-fzero-call-used-regs=used-gpr` | ✓ (probed) | ✓ (probed) | skip | Zero scratch GPRs on return — defeats register-based ROP gadgets. gcc 11 / clang 15+. |
| `-fcf-protection=full` (x86_64) | ✓ (probed) | n/a | skip | Intel CET: ENDBR for IBT + shadow-stack note. gcc 8 / clang 7+. |
| `-mbranch-protection=standard` (arm64) | ✓ (probed) | ✓ (probed) | skip | ARMv8.3 pac-ret + BTI. clang 14 / gcc 9+. |
| `-Wl,-z,separate-code` | ✓ (probed) | n/a | skip | Separate code/data pages — write primitive can't land in executable memory. GNU ld 2.30+. |
| `-Wl,--as-needed` | ✓ (probed) | n/a | skip | Drop unused DT_NEEDED entries — shrinks loaded-library surface. |

"probed" means the Makefile runs a tiny compile/link test against
`$(CC)` and only adds the flag if the toolchain accepts it cleanly
(with `-Werror` so warn-then-pass flags are correctly rejected). The
result is that the same Makefile produces a maximally hardened build
on a modern Linux toolchain and a still-correct build on an older
one without breaking either.

### Cosmopolitan trade-off

APE binaries skip the entire hardening layer. The format constraints
(custom bootloader, no GNU dynamic linker, must run on pre-CET CPUs
by design) make most ELF-specific options either inapplicable or
actively break the linker script. `make CC=cosmocc hardening` prints
each property as `skipped` with the reason. Operators choosing the
cosmo build for portability accept this trade explicitly.

### Build-time verification

`make hardening` prints the resolved set:

```
$ make hardening
Hull hardening summary (cc on Linux/x86_64):
  stack canary:     -fstack-protector-strong
  PIE:              -fPIE (linked with -pie)
  fortify:          -D_FORTIFY_SOURCE=3
  probed CFLAGS:    -fstack-clash-protection -fno-plt -fno-common
                    -ftrivial-auto-var-init=zero
                    -fzero-call-used-regs=used-gpr
                    -fcf-protection=full
  Linux LDFLAGS:    -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
                    -Wl,-z,separate-code
  link-time:        -Wl,--as-needed
```

### Post-build verification

`scripts/check_hardening.sh build/hull` inspects the binary and
reports PASS/FAIL/SKIP per property. Format-aware (ELF / Mach-O /
APE). Exits non-zero only when a *required* protection for that
platform is missing — never for "not applicable to this format"
properties. The CI matrix runs `make check-hardening` after every
build (Linux x2, Linux aarch64, macOS). The release workflow runs it
on every release native target. A regression that strips a required
protection fails the release.

Verified properties:

- **PIE / ASLR**: ELF type `DYN` / Mach-O `MH_PIE`.
- **RELRO + BIND_NOW**: PT_GNU_RELRO segment + DT_BIND_NOW or
  DT_FLAGS_1 NOW.
- **NX stack**: PT_GNU_STACK without X flag.
- **Stack canaries**: `__stack_chk_fail` symbol referenced.
- **W^X**: no LOAD segment with W+E permissions.
- **FORTIFY**: any `*_chk` symbol present (informational; missing in
  debug builds is expected).
- **CET note** (x86_64): NT_GNU_PROPERTY x86 feature 1 IBT/SHSTK.
- **BTI/PAC note** (arm64): NT_GNU_PROPERTY AArch64 feature 1.
- **No RPATH/RUNPATH** (informational): runtime library path
  embedded in the binary is an attack surface.
- **Hardened Runtime** (macOS): `codesign -dv` runtime flag.

### What's intentionally NOT added (yet)

| Considered | Decision | Reason |
|---|---|---|
| `-flto` / `-flto=thin` | **Shipped** as opt-in `HL_ENABLE_LTO=1` (2026-06-19). Per-vendor compatibility audit clean across mbedtls, sqlite, lua, qjs, miniz, tweetnacl, stb, wamr, log.c, sh_*. Reproducible build holds byte-identical under LTO. Build time roughly 2x, binary size +3.5%. Default off because the value is only realised together with CFI (which is now documented as not-pursued, see next row). | n/a |
| `-fsanitize=cfi-icall` (LLVM) | **Shipped** as opt-in `HL_ENABLE_CFI=1` (2026-06-20, follow-up 2026-06-21). Auto-enables `HL_ENABLE_LTO=1`. Linux clang only: probe-skips cleanly on Apple clang (no Darwin CFI runtime), gcc (no CFI), and cosmocc (no CFI). The original 2026-06-19 spike thought adoption needed a 3-5 week refactor of six polymorphic vtables; the second look found five of the six (`HlAsyncBackend`, `HlNetBackend`, `HlCompilerVtable`, `HlRuntimeVtable`, plus Keel's `KlBodyReader`) were already CFI-compatible via opaque-forward typed struct pointers, and only `HlDbBackend` actually used `void *ctx` type erasure. `HlGpuBackend` was the sixth holdout (also `void *backend_ctx` / `void *backend_device`) and was retyped 2026-06-21 once Metal validation became available; all 18 `test_gpu` cases pass under real Metal on Apple M1 Max, plus 48/48 unit + 22/22 e2e under live CFI on Lima Ubuntu 25.04 aarch64. All six Hull vtables now use typed-handle method signatures. QuickJS and WAMR vendor TUs stay excluded (their internal callback dispatch uses cast-through-generic-prototype patterns Hull can't patch without forking the vendors). See `roadmap_next.md` §9 for the full empirical history. |
| `-fsanitize=safe-stack` | Deferred | clang-only, splits stacks. Runtime cost and incompatible with `setjmp` / coroutine patterns Hull uses extensively for Lua + JS async. |
| `-fsanitize=shadow-call-stack` | Deferred | aarch64-only. Requires a free register reservation (`-ffixed-x18`). Worth measuring on Linux aarch64 release as a follow-up. |
| `-fhardened` (GCC 14+) | Skipped | Meta-flag that conflicts with explicit overrides. We deliberately probe individual flags so the build still works on toolchains 5+ years old. |
| Windows CFG | N/A | Hull doesn't currently target MSVC / clang-cl. |
| Apply hardening CFLAGS to vendor TUs (mbedtls, sqlite, lua, qjs, miniz, tweetnacl, wamr) | Deferred | Vendor TUs use their own `CFLAGS := … -w …` arrays that clobber the global set. Worth doing per-vendor (mbedtls first; security-critical and well-tested under hardening flags elsewhere). |
| Apply hardening to `libkeel.a` | Deferred | Keel has its own Makefile; needs separate opt-in or env-var passthrough. |

### Relationship to §4b (data-plane vs. instruction-plane)

§4c protects the instruction-plane: PAC, BTI, CFI-equivalents (via
RELRO + `-fno-plt`), stack canaries, and shadow-stack notes all aim
at the moment the CPU loads a corrupted address into PC. They don't
see what's INSIDE the dispatch table being loaded from.

§4b's `sh_seal_arena` mechanism covers the data-plane: it makes the
dispatch tables themselves (router handler vtable, mbedTLS
`pk_info`, `KlConfig.parser` factory, `KlConfig.log_fn`, the
sealed mbedTLS chain) physically read-only via `mprotect`. A
heap-write primitive that targets one of those function-pointer
slots faults at the write itself, before PC ever loads the
poisoned value. Together the two layers defend the same ROP/JOP
escalation chain at different stages.

### Residual ROP/JOP risk

What an attacker still gets, even with this layer in place:

- **Vendor TUs are not yet hardened.** Gadgets in mbedTLS, sqlite,
  lua, qjs, miniz, tweetnacl, wamr text segments are reachable.
  Tracked above.
- **CFI shipped on Linux clang only.** Indirect-call sites are now
  protected by FOUR layers:
    1. `const`-qualification of the dispatch vtables (compile-time
       baseline; vtables can't be overwritten via a wild pointer
       into `.rodata`).
    2. RELRO + BIND_NOW (the static vtables themselves stay
       read-only after loader fixups).
    3. `sh_seal_arena` mprotect-RO on boot-built tables that can't
       be `static const` (§4b).
    4. `-fsanitize=cfi-icall` (opt-in `HL_ENABLE_CFI=1`): at every
       indirect call, clang verifies the loaded function pointer's
       runtime type matches the call site's expected signature.
       Fake-vtable-pointer-in-unsealed-object attacks trap at the
       call site instead of pivoting control flow.
  CFI covers ~85% of indirect-call sites: every Hull TU (including
  the GPU backend after the 2026-06-21 HlGpuBackend retype), Keel,
  plus mbedtls, sqlite, lua, tweetnacl, miniz, log.c, sh_*. All six
  Hull polymorphic vtables (HlDbBackend, HlGpuBackend, HlAsyncBackend,
  HlNetBackend, HlCompilerVtable, HlRuntimeVtable) now use typed-
  handle method signatures, so CFI matches type-ids at every Hull
  dispatch site. Vendor exclusions: QuickJS and WAMR (their internal
  callback dispatch uses cast-through-generic-prototype patterns Hull
  can't patch without forking). CFI is Linux-clang-only by toolchain
  constraint: macOS Apple clang has no CFI runtime; gcc and cosmocc
  reject the flag. On those platforms the four-layer defense is
  reduced to three (layers 1-3 still apply). The death test in
  `tests/hull/test_cfi.c` proves CFI traps wrong-typed indirect
  calls under release-mode build. See `roadmap_next.md` §9 for
  the empirical history.
- **Server-side own private-key bignums.** Documented in §4b's
  "What's NOT sealed" table. mbedTLS rewrites RSA blinding state
  (`ctx->Vi` / `ctx->Vf`) on every signature as a Kocher 1996
  timing-attack defense, which prevents sealing the per-server
  signing key's deep allocations. Top-level `pk_context` struct is
  still sealed (v2.3.3 policy half); only the bignum limbs the
  context points at remain heap-mutable. Mitigated by the kernel
  sandbox + the rest of the seal coverage.
- **No kernel-enforced shadow stack on Linux x86_64** unless the CPU
  supports CET *and* the kernel enables it
  (`arch_prctl(ARCH_SHSTK_ENABLE)`). Hull emits the GNU property
  note; runtime enforcement is the kernel's call.
- **Cosmopolitan binaries have effectively no compiler-level
  hardening**, as noted above. The `sh_seal_arena` data-plane
  protection in §4b DOES apply to APE builds (pure POSIX
  `mmap` + `mprotect`, no toolchain dependency), so cosmo users
  still get the seal coverage even when they're skipping PAC /
  BTI / CFI.

### Opt-out

`HULL_DISABLE_HARDENING=1 make` skips the entire block. Debug-only;
release CI fails if the verifier doesn't find the required
protections — so this flag can't accidentally ship.

---

## 4d. Runtime enforcement (kernel + loader knobs)

§4c emits the markers; the kernel and dynamic loader enforce them
at runtime. The compiler can't turn that enforcement on from
inside the process; the operator picks it up via deployment
configuration. The defaults are conservative across distributions,
so a Hull binary shipped without explicit runtime configuration
may be running with weaker enforcement than the binary metadata
suggests.

This section is the **operator-facing companion** to §4c. Read
§4c to understand what the binary advertises; read this section
to make sure the platform actually honors those advertisements.

### What's already automatic (no operator action)

| Platform | Marker | Enforcement | Status |
|---|---|---|---|
| Linux aarch64 | BTI (Branch Target Identification) | Kernel ≥ 5.8 + CPU support. The GNU property note is read by `ld.so` at load time; if both the kernel and the CPU support BTI, every indirect branch must land on a `bti` instruction or the kernel raises SIGILL. | Automatic, no per-process opt-in. |
| Linux aarch64 | PAC (Pointer Authentication Codes) | Kernel ≥ 5.0 + CPU support (Apple Silicon, Cortex-A78+). Return-address signing is automatic when `-mbranch-protection=standard` is set at compile time. | Automatic. |
| macOS arm64 | BTI + PAC + Hardened Memory | Apple Silicon CPUs + macOS kernel enforce both unconditionally. Hardened Runtime adds W^X + library validation when codesigned with the `runtime` flag. | Automatic for SIP-protected paths; `codesign -o runtime` for distributed binaries. |
| Linux any | NX stack | Universal on modern kernels. PT_GNU_STACK without the X flag (Hull emits) is honored by every loader. | Automatic. |
| Linux any | ASLR (PIE) | Kernel default (`/proc/sys/kernel/randomize_va_space=2`). | Automatic; verify via `cat /proc/sys/kernel/randomize_va_space`. |
| Linux any | RELRO + BIND_NOW | `ld.so` resolves all relocations at load and marks `.got` read-only. Hull emits both flags. | Automatic. |

### What needs operator opt-in

| Platform | Marker | Operator action |
|---|---|---|
| Linux x86_64 | CET / Intel SHSTK (shadow stack) | Kernel ≥ 6.6 + CPU support (Intel Tiger Lake+, AMD Zen 3+). Process opts in via `arch_prctl(ARCH_SHSTK_ENABLE)`. **Hull does NOT call this today.** Currently best done via glibc's `GLIBC_TUNABLES=glibc.cpu.x86_shstk=on` env var (glibc 2.39+) or via systemd's per-unit `Environment=GLIBC_TUNABLES=glibc.cpu.x86_shstk=on`. Future Hull may add a `--enable-shadow-stack` CLI flag that calls `arch_prctl` directly. |
| Linux x86_64 | CET / Intel IBT (indirect branch tracking) | Same kernel + CPU requirement as SHSTK. The `endbr64` instructions are emitted by `-fcf-protection=full` (Hull does this); the kernel enforces them on every indirect branch. Enabled at process start via the same loader path as SHSTK. |
| macOS arm64 (distributed) | Hardened Runtime | `codesign --options runtime --sign "Developer ID Application: ..."` at distribution time. Required for notarisation. `hull` is unsigned in dev builds; release `.tar.gz` should be signed and notarised. |

### Recommended systemd unit

For Linux deployments, this unit applies every kernel-side hardening
knob that complements §4c. Designed for a Hull web app deployed as
`hull-app.service`; adjust `User=`/`Group=`/`WorkingDirectory=` to taste:

```ini
[Unit]
Description=Hull application
After=network.target

[Service]
Type=simple
ExecStart=/opt/hull-app/bin/myapp
WorkingDirectory=/opt/hull-app
User=hull
Group=hull
Restart=on-failure

# Memory hardening
MemoryDenyWriteExecute=yes
LockPersonality=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectKernelLogs=yes
ProtectControlGroups=yes

# Filesystem hardening
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
PrivateDevices=yes
ReadWritePaths=/var/lib/hull-app /var/log/hull-app

# Network hardening (drop AF_NETLINK, AF_PACKET, raw sockets, etc.)
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
RestrictNamespaces=yes
RestrictRealtime=yes
RestrictSUIDSGID=yes
NoNewPrivileges=yes
LockPersonality=yes

# Capability drop (Hull doesn't need any after bind)
CapabilityBoundingSet=
AmbientCapabilities=

# x86_64 CET enablement (glibc 2.39+, kernel 6.6+)
Environment=GLIBC_TUNABLES=glibc.cpu.x86_shstk=on:glibc.cpu.x86_ibt=on

# Optional: stack-clash + UMA protection at the kernel level
ProcSubset=pid

[Install]
WantedBy=multi-user.target
```

`MemoryDenyWriteExecute=yes` is the most important line: it makes
the kernel reject any `mprotect(PROT_WRITE|PROT_EXEC)` /
`mmap(PROT_WRITE|PROT_EXEC)` at the syscall boundary. Hull's
code path never asks for W^X memory (no JIT, no runtime codegen
(see §4b), so the policy holds at the kernel layer too.

`LockPersonality=yes` blocks `personality(2)` writes, which would
otherwise let a compromised process disable ASLR for itself.

### Docker / Kubernetes

Equivalent knobs for container deployments:

```yaml
# Kubernetes Pod spec
spec:
  securityContext:
    runAsNonRoot: true
    runAsUser: 1000
    fsGroup: 1000
    seccompProfile:
      type: RuntimeDefault
  containers:
    - name: hull-app
      image: myapp:latest
      securityContext:
        readOnlyRootFilesystem: true
        allowPrivilegeEscalation: false
        capabilities:
          drop: ["ALL"]
        seccompProfile:
          type: RuntimeDefault
      env:
        - name: GLIBC_TUNABLES
          value: "glibc.cpu.x86_shstk=on:glibc.cpu.x86_ibt=on"
      volumeMounts:
        - name: data
          mountPath: /var/lib/hull-app
        - name: tmp
          mountPath: /tmp
  volumes:
    - name: data
      persistentVolumeClaim:
        claimName: hull-app-data
    - name: tmp
      emptyDir: {}
```

Docker equivalent:

```sh
docker run --rm \
  --read-only \
  --tmpfs /tmp \
  --cap-drop=ALL \
  --security-opt=no-new-privileges \
  --security-opt=seccomp=default.json \
  --env GLIBC_TUNABLES=glibc.cpu.x86_shstk=on:glibc.cpu.x86_ibt=on \
  -v /var/lib/hull-app:/data \
  myapp:latest
```

### How to verify enforcement is active

Use these to confirm runtime enforcement is actually engaged, not
just available:

```sh
# Verify the binary's marker set (build-time)
make hardening                     # Hull-side flag summary
scripts/check_hardening.sh build/hull  # binary verifier (PIE, RELRO, etc.)
readelf -nW build/hull | grep -E "IBT|SHSTK|BTI|PAC"  # GNU property notes

# Verify ASLR is on
cat /proc/sys/kernel/randomize_va_space  # 2 = full ASLR

# Verify the running process has CET enabled (Linux x86_64)
# (requires kernel 6.6+; the PR_GET_SHADOW_STACK_STATUS prctl is glibc 2.39+)
gdb -p $(pgrep myapp) -batch -ex 'call (int)prctl(76, 0)' 2>&1 | grep -i shstk

# Verify systemd applied the unit hardening
systemd-analyze security myapp.service   # exposure score; aim < 1.0

# Confirm MemoryDenyWriteExecute holds
sudo strace -p $(pgrep myapp) -e trace=mmap,mprotect 2>&1 | grep -E "PROT_WRITE.*PROT_EXEC"
# (should produce zero matches; non-zero = audit needed)
```

### What we still can't do from inside the process

Some hardening requires kernel + loader cooperation that Hull
can't trigger from C:

- **`PROC_PDEATHSIG`** can't be set from inside without being a
  child of a known parent; Hull is typically PID 1 in a container
  so this is moot.
- **`PR_SET_NO_NEW_PRIVS`** Hull could call `prctl(PR_SET_NO_NEW_PRIVS, 1)`
  at startup; not done today. Low priority because the systemd /
  Docker layer typically sets it. Worth adding as a one-liner in
  `hl_serve_wire_caps` between phase 1 and phase 2 sandbox; tracked
  as a follow-up.
- **`landlock_*` syscalls (Linux ≥ 5.13)** Future direction for
  Hull's filesystem capability layer. Currently uses `unveil`
  (cosmocc polyfill) on Linux; landlock is the kernel-native
  equivalent and could supplement.

### Documenting the gap

This section is intentionally an operator guide, not a Hull-side
runtime feature. Adding `arch_prctl(ARCH_SHSTK_ENABLE)` at startup
would close the gap for distribution binaries that need to work
without operator config; tracked as a future enhancement (low
priority because the systemd / container deployment path is the
recommended one).

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
| Signature | Manifest is signed. Tampering invalidates signature | Ed25519 forgery required |

**No manifest declared?** Even if the app doesn't call `app.manifest()`, the default-deny posture is identical to `app.manifest({})`:
- **Kernel sandbox is applied**. Pledge/unveil restrict to only the database file and TLS paths
- **CSP is active**. The default strict policy is injected on all HTML responses
- **C-level capabilities deny all**. Env returns NULL, HTTP requests fail, filesystem operations fail
- Signature still covers the absence of manifest (`"manifest": null`)

**Escape-hatch keys (off by default, surfaced as risky by `hull inspect`):**

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `allow_dynamic_code` (Lua) / `allowDynamicCode` (JS) | bool | `false` | Opt-in to JIT / runtime codegen. Rejected by the sandbox unless `--no-sandbox` is passed. |
| `allow_dynamic_libraries` (Lua) / `allowDynamicLibraries` (JS) | bool | `false` | Opt-in to runtime native library loading (dlopen). Rejected by the sandbox unless `--no-sandbox` is passed. |

Setting either key emits a warning at manifest-extract time and causes the kernel sandbox to refuse to start. There is no in-policy downgrade. The operator must explicitly disable the sandbox with `--no-sandbox` (development only).

All example apps declare `app.manifest()` explicitly, even when the empty `{}` is sufficient, as a best practice.

---

## 5b. Module Declaration

The manifest's `modules` table is the app's **declared module surface**. A strict allowlist of which first-party Hull stdlib modules the app may import at runtime. It complements the capability fields described above: capability sections (`fs`, `hosts`, `env`) say *what ambient authority* the app uses; `modules` says *which library names* it imports.

```lua
app.manifest({
    modules = {
        "hull/crypto@1",
        "hull/db@1",
        "hull/time@1",
        "hull/validate@1",
        "hull/web/middleware/audit-log@1",
        "hull/web/middleware/oauth@1",
        "hull/web/middleware/session@1",
        "hull/web/middleware/totp@1",
        "hull/web/auth-flows@1",
        "hull/web/pwned@1",
    },
    fs    = { read = {"data/"} },
    hosts = {"accounts.google.com", "api.pwnedpasswords.com"},
})

-- import paths use the standard Lua/JS forms. Name the local
-- variable whatever you want:
local crypto = require("hull.crypto")
local oauth  = require("hull.web.middleware.oauth")
local flows  = require("hull.web.auth-flows")
```

Each entry is a canonical spec `"<vendor>/<name>@<major>"`. First-party modules live under `hull/`; future third-party packages would follow the same pattern (`"acme/widgets@2"`). The manifest declares *what's in scope*; the `require()` / `import` call site picks *what to call it locally*.

**Design principles:**

| Principle | What it means |
|-----------|---------------|
| **Every external capability is declared** | Lua/JS intrinsics (string, table, math, JSON in JS) plus a minimal Hull core are always available. The intrinsic core is just `hull/app` (the registration API; it must be intrinsic because the manifest is expressed via `app.manifest(...)`. Every other first-party module) `hull/log`, `hull/json`, `hull/crypto`, `hull/db`, `hull/http`, every middleware, every stdlib helper. Must be in `modules` or imports fail. |
| **Import-only exposure** | Declared modules are reached via `require("hull.X")` (Lua) / `import "hull:X"` (JS). They are NOT globals. Apps that don't declare a module cannot use it even by accident. |
| **Capability + module separate gates** | Declaring `hull/http-client@1` does not also open the network. Apps still need a non-empty `hosts` allowlist. The resolver rejects `hull/http` declared without `hosts`. Same for `hull/fs` (needs `fs.read`/`write`) and `hull/env` (needs `env`). |
| **Explicit dependencies** | If `hull/web/middleware/session` internally uses `hull/db`, `hull/crypto`, and `hull/time`, the app must declare *all four*. No silent pull-in. The resolver lists the missing dep in its error. |
| **Sealed at startup, no runtime install** | The resolved module set is computed once after manifest extraction, frozen for the lifetime of the process. Runtime code cannot install, fetch, discover, or load new modules. The set's contents are signed into `package.sig` as `modules_resolved`. |
| **Build-time subsystems gate too** | Modules whose backing C is compile-time-optional (`hull/db`, `hull/compute`, `hull/gpu`) are rejected if the build wasn't compiled with the corresponding `HL_ENABLE_*` flag **and** no matching composable feature was linked (`hull build --with=gpu` reports GPU at runtime via the `hl_gpu_feature_backends` hook, so a composed binary admits `hull/gpu`). The resolver reports the missing build flag by name. |
| **Optional modules fail *soft*, but only for absent build caps** | A trailing `?` (`"hull/gpu@1?"`) makes a declaration optional: if - and only if - the build lacks the required build-cap subsystem, the resolver *skips* it (records it in an `optional_absent` bitset) and require/import yields nil/null instead of failing app load, so the app can fall back. This is deliberately narrow: it changes **only** the "capability compiled out / not composed" branch. A **present** optional module is gated identically to a normal declaration (full resolver + per-call cap layer - zero new authority); a module absent for any *other* reason (e.g. an unconfigured DB runtime) still errors; and the manifest-side caps (`fs`/`hosts`/`env`) are never suppressed by `?` (they fail closed at call time regardless). So `?` cannot be used to paper over a missing allowlist - it only turns a hard "subsystem not in this binary" error into a graceful nil. |

**What this prevents:**
- **Capability expansion via stdlib upgrade.** A new module added to `hull/*` cannot magically become available to existing apps. The app's declaration is its admit list.
- **Hidden imports in transitive code.** If a vendored helper `require("hull.crypto")` without the app declaring it, the require fails at runtime. The static-analysis pass (planned, see roadmap) surfaces this at build time.
- **Ambient stdlib drift.** The set of admitted modules is auditable from `package.sig` alone. No need to load the app to know what it imports.

**What it does NOT do:**
- Replace the capability sections. `modules = { http = "hull/http-client@1" }` doesn't grant network. The app still needs a `hosts` allowlist. The resolver enforces this pairing.
- Defeat C-level escape via embedded WASM or compute. Compute modules still execute inside the WAMR sandbox with their own gas + memory limits.

**Inspection:**

| Command | Output |
|---------|--------|
| `hull modules available` | Print the full first-party registry. Names, deps, capability requirements |
| `hull modules list [APP_DIR]` | What the app declares |
| `hull modules explain <NAME>` | One spec, including deps and required capabilities |
| `hull --json modules ...` | Same, machine-readable |
| `hull agent modules [APP_DIR]` | Agent-friendly JSON: `{declared, intrinsic, build_caps, registry_count}` |
| `hull doctor` | Reports which `HL_ENABLE_*` build-time subsystems are linked in |

**This is not npm/pip/cargo.** There is no remote registry, no fetching, no runtime install, no third-party packages, no version resolution beyond the single API-major. v1 is capability-aware dependency declaration for sealed Hull apps.

---

## 6. Verification Tools

**Status:** Shipped in full as of v0.1.3. All three verifiers check
the gethull platform-sig layer, the per-app self-consistency layer,
and the app developer signature.

### A. Browser Verifier (`site/verify.html`)

- Self-contained HTML file, zero server dependencies
- Runs entirely in browser. No data sent anywhere
- Inlined tweetnacl-js (public domain) for Ed25519
- Web Crypto API for SHA-256
- CSP: `default-src 'none'`. No network except optional key fetch via `connect-src https:`
- Gethull platform pubkey hardcoded (matches `HL_PLATFORM_PUBKEY_HEX`)
- Release pubkey hardcoded (matches `HL_RELEASE_PUBKEY_HEX`). Covers
  both app-bundle and release-binary verification flows

**Checks performed:**
1. **Gethull platform-sig (v0.1.3):** `package.sig.platform.gethull.signature` verified against the hardcoded gethull pubkey; missing block flagged
2. Per-app platform layer self-consistency (Ed25519 over `platforms` object with the embedded per-app key)
3. App signature validity (Ed25519)
4. Developer key match (if provided)
5. Binary hash match (if binary uploaded)
6. Source file hash verification (if files uploaded)
7. Manifest capability display with risk levels

### B. CLI Verifier (`hull verify`)

```
hull verify [--no-verify-platform] [--platform-key <file|url>] \
            [--developer-key <file|url>] [app_dir]
```

- Reads `package.sig` (or `hull.sig` for backwards compat)
- **Gethull layer:** signature on `platform.gethull.manifest`
  verified against `HL_PLATFORM_PUBKEY_HEX` (queried via the
  `tool.platform_pubkey()` binding). Missing block on a hull with a
  real pubkey hard-rejects unless `--no-verify-platform`. Hulls
  built with the all-zeros placeholder skip the check silently
  (dev-hull bootstrap path).
- **Per-app platform layer:** self-consistency check only. No
  upstream-key pinning. `--platform-key <file>` is honored as an
  optional comparison against an expected developer pubkey.
- **App layer:** Ed25519 over the canonical payload of
  `{binary_hash, build, files, manifest, modules_resolved,
  platform, trampoline_hash}`.
- Recomputes SHA-256 of all declared files; reports mismatches,
  missing files, key mismatches.
- Exit code 0 = all checks passed, 1 = failure.

### C. Runtime Verifier (`--verify-sig`)

```
./myapp --verify-sig dev.pub [--no-verify-platform]
```

- Checks on every startup before accepting connections.
- Platform key pinned at compile time (`HL_PLATFORM_PUBKEY_HEX`,
  real key since v0.1.3).
- Verifies all three layers (gethull, per-app, app developer) in
  `signature.c` §4 → §5 → §5b.
- Verifies file hashes against embedded entries via VFS
  (`O(log n)` lookup).
- Refuses to start on any failure. `--no-verify-platform` skips
  the gethull layer (for dev hulls and forks). Builds with the
  all-zeros placeholder pubkey skip the gethull layer with a
  one-line warning.

---

## 7. Reproducibility and Trusted Rebuild Infrastructure

### 7.1. Byte-reproducible builds (shipped)

CI-gated on both Linux and macOS via `make reproducible-check`.
Three independent properties:

1. **`make` is deterministic.** Same source tree produces a
   byte-identical `build/hull` between rebuilds.
2. **`hull build` is deterministic.** Same source + same hull
   version + same output path produces a byte-identical app binary.
3. **`make self-build` proves hull is self-hostable.** Hull can
   build hull2 can build hull3 across all platforms.

#### What this proves

A passing reproducibility check proves the developer **could not
have** injected custom native code. The binary is provably just
"Hull platform + declared source files." Anyone with the source
can rebuild from the recorded `cc_version` + `flags` in
`package.sig` and compare hashes. If the hashes don't match, the
signing process was compromised or the developer lied about what's
in the binary.

This is the **Tier 4 verification surface** described on the
gethull.dev trust-chain panel: it complements the three signature
tiers (release, platform, app) by closing the one attack the
signature chain alone cannot catch. A compromise of the signer's
own machine (the developer's identity is intact, the signature is
valid, but the binary doesn't match the source they published).
Tier 4 makes that detectable as soon as anyone re-derives.

#### Why it works

1. App developers cannot write C. Only Lua/JS source.
2. Platform binary is hash-pinned. `platform.sig` locks exact bytes.
3. Trampoline (`app_main.c`) is deterministic. Generated from
   template.
4. Build inputs are deterministic. `ZERO_AR_DATE=1` makes ar
   archives mtime-free; `-ffile-prefix-map` strips per-build
   tempdir paths from `.o` file content; same-output-path
   methodology isolates macOS `ld64`'s path-hashed LC_UUID.
5. Build metadata is signed. `cc_version` + `flags` are attested
   by the developer in `package.sig`.
6. Cosmopolitan produces deterministic output. Static linking, no
   timestamps. CI-verified: the `reproducibility-cosmo` job in
   `.github/workflows/ci.yml` builds `hull-cosmo` on two independent
   runners and byte-compares them (identical across runners, not just
   same-machine).

#### Vendored-dependency reproducibility

Every dependency is vendored (submodules: keel, wamr, tcc; source
snapshots: lua, quickjs, sqlite, mbedtls, tweetnacl, miniz, stb,
sh_arena, sh_json, log.c, pledge). None ship as a prebuilt binary;
every vendored translation unit is compiled from the pinned source on
every `make`. That makes vendored-dependency reproducibility a **goal**,
and it is transitively enforced by the whole-build reproducibility gate
above: `make reproducible-check` runs `make` then `make clean` then
`make` then `cmp`, and `make clean` wipes `build/`, so the second `make`
recompiles every vendored TU from the pinned source. A byte-identical
`hull` therefore proves the vendored objects rebuilt byte-for-byte too.
There is no separate "did WAMR rebuild identically" question to answer:
if it hadn't, `hull` would not match. This holds for all three artifact
types (native Linux, native macOS, and the cosmo APE, the last
byte-compared across two independent runners by `reproducibility-cosmo`).

Load-bearing pieces:

- **Pinned source.** Submodules are locked to exact SHAs; snapshots are
  committed verbatim. The rebuild always starts from identical bytes.
- **`ZERO_AR_DATE=1`** makes the vendored archives (`libkeel.a`, the
  platform library) mtime-free.
- **`-ffile-prefix-map`** strips per-build tempdir paths out of vendored
  `.o` content.
- **Pinned CI runners** (`ubuntu-24.04` / `macos-15`, see roadmap
  §0.3.1) fix the toolchain version so the two rebuild passes share one
  compiler.

**Non-goal:** whether an upstream dependency (WAMR, Keel, mbedTLS, ...)
is itself byte-reproducible across *different* toolchain versions is the
dependency's own concern, not Hull's. Hull's reproducibility claim rests
on pinning both the dependency *source* (the SHA) and the *toolchain*
(the runner); with those two fixed, the rebuild is deterministic.
Reproducing Hull under a different compiler version is out of scope: that
is a property of each upstream project, and pinning the runner is
precisely how Hull sidesteps needing it.

#### Build-environment immutability (why a snapshot-pinned Dockerfile, not Nix)

The gate above proves the build is deterministic *given a fixed
toolchain*. What fixes the toolchain across time is the CI runner.
Pinning `runs-on:` to versioned labels (`ubuntu-24.04`, `macos-15`, see
roadmap §0.3.1) removed the largest source of drift, a `latest` label
silently jumping a major OS between a release and a rebuild, but
versioned images still receive GitHub's monthly patch updates. So the
toolchain is fixed only within a major-version window, not indefinitely.

The chosen mechanism to close that gap for the Linux and cosmo
build/release jobs is a **reproducible, snapshot-pinned Dockerfile built
in CI, with no image registry**. The Dockerfile pins its base by digest
(`FROM ubuntu:24.04@sha256:...`) and pins the apt archive to a fixed
point in time via the Ubuntu snapshot service (`snapshot.ubuntu.com`,
driven by a recorded `SOURCE_DATE_EPOCH` using the upstream
`repro-sources-list.sh` helper), so `apt-get install` resolves to
byte-identical package versions (gcc, glibc, binutils) on every rebuild,
anywhere, indefinitely. The SHA-pinned cosmocc is baked in the same way.
A verifier reproduces the toolchain by rebuilding the Dockerfile from
this repo at the recorded timestamp; nothing is pulled from a registry
Hull controls, so there is no hosted image to trust or to rot.

This was chosen over both a published+digest-pinned image and a Nix flake:

- **vs. a published image (GHCR + `container: @sha256`).** A pushed image
  pinned by digest is also immutable, but it moves the trust anchor to a
  registry artifact and adds hosting plus a publish workflow. The
  snapshot-pinned Dockerfile achieves the same fixed toolchain *from
  source in the repo*, which fits a project whose whole verification
  story is "rebuild from source and compare."
- **vs. Nix.** (1) The claim needs the environment *fixed*, not itself
  rebuildable-from-a-compiler-source: pinning the base digest plus the
  apt snapshot fixes the toolchain bytes, and nobody re-derives clang to
  verify Hull. (2) Hull's dependencies are already fetch-and-pin (cosmocc
  SHA-pinned, wgpu-native, the LLVM for `wamrc`, every vendored archive),
  which a Dockerfile matches and a Nix derivation would fight (cosmocc in
  particular is awkward to package). (3) macOS cannot be containerized
  regardless: `hull-darwin-arm64` builds on a real `macos-15` runner with
  Xcode, so the immutable base only ever covers `linux-x86_64`,
  `linux-aarch64`, and the cosmo APE. That caps the payoff and argues for
  the option that fits the existing grain.

The honest resulting claim: with the snapshot-pinned Dockerfile,
"reproducible across time" holds indefinitely for the Linux and cosmo
artifacts and within a major-version window for macOS (a real `macos-15`
runner with Xcode, which no container touches). BuildKit's
`rewrite-timestamp` can additionally make the produced image itself
byte-reproducible, but Hull does not depend on that: it rebuilds the
image and runs the build inside it rather than shipping the image.
Rollout status is tracked in roadmap §0.3.1.

#### Self-hosted alternative

Run your own build host. Pin your own platform key. Your customers
trust you, not gethull.dev.

#### Known follow-up

The `hull build --compiler=tcc` codepath still has per-tempdir
variance because TCC doesn't support `-ffile-prefix-map`. The
reproducibility CI test forces `--compiler=system` (the documented
production path). TCC determinism is a smaller separate work item;
tracked in `docs/roadmap_next.md §0.2`.

### 7.2. Sigstore + Rekor transparency log (shipped v0.1.6)

Every release publishes `(release_tag, hull.sha256_digest, signature)`
to Sigstore's public Rekor log via `cosign sign-blob`. Two release
artifacts:

- `hull.sha256.cosign.sig` (~96 bytes). Sigstore-format signature
  over the release manifest.
- `hull.sha256.cosign.pem` (~3.3 KB). Fulcio-issued short-lived
  code-signing certificate. The certificate's SAN identifies the
  GitHub Actions workflow + commit SHA + repository that produced it.

#### What this proves

A non-trivial subset of the trust story Hull cannot establish with its
own Ed25519 key alone:

- **Backdating defense.** Rekor is an append-only public log with
  Merkle-tree consistency proofs. Once a release entry is logged, no
  later actor (including a compromised gethull) can forge a backdated
  entry that pretends to predate the compromise.
- **Independent of HULL_RELEASE_KEY.** If gethull's long-lived Ed25519
  key is compromised, the Rekor entry still proves the release existed
  at this timestamp and was produced by this specific workflow under
  this specific OIDC identity.
- **Verifiable without trusting gethull.** The Fulcio root is in
  cosign's default trust store. Anyone runs:
  ```
  cosign verify-blob hull.sha256 \
    --certificate hull.sha256.cosign.pem \
    --signature hull.sha256.cosign.sig \
    --certificate-identity-regexp='^https://github.com/artalis-io/hull/' \
    --certificate-oidc-issuer='https://token.actions.githubusercontent.com'
  ```
  No gethull-controlled trust anchor is consulted; the verification
  chain terminates at the public Sigstore + GitHub roots.

#### What this does NOT prove

- That the binary matches an independent rebuild of the source. Sigstore
  + Rekor attest the workflow that produced the binary; they don't
  re-derive the binary from source. §7.4 below covers that gap.
- That the source itself isn't malicious. Rekor signs the artifact, not
  the source's intent.

### 7.3. SLSA build provenance (shipped v0.1.6)

Every shipped binary has a Sigstore-signed
[build-provenance attestation](https://slsa.dev/) tying it to the
specific commit, workflow run, runner image, and OIDC identity.
Generated by `actions/attest-build-provenance@v2` (SHA-pinned). Covers
all seven release binaries plus `hull.sha256` and the three SBOM
files.

Verify with the `gh` CLI:

```
gh attestation verify hull-linux-x86_64 --repo artalis-io/hull
```

This is the SLSA Level 3 build-track property: provenance is
generated by a hosted build platform (GitHub Actions) outside the
producer's direct control, signed by a separate trust root
(Sigstore Fulcio), and published to a transparency log. Independent
of HULL_RELEASE_KEY for the same reason §7.2 is.

#### Combined trust-chain claim (post-v0.1.6)

The three signatures over each release manifest line up as:

| Signature | Trust root | What it proves |
|-----------|------------|----------------|
| `hull.sha256.sig` (Ed25519) | Embedded `HL_RELEASE_PUBKEY_HEX` | Same gethull-managed key that signed every prior release. `hull update` verifies offline. |
| `hull.sha256.cosign.sig` (Sigstore) | Fulcio CA + GitHub OIDC | This release was produced by gethull's GitHub Actions workflow on the v0.1.x tag, at this Rekor-logged timestamp. |
| Per-binary SLSA attestation (Sigstore) | Fulcio CA + GitHub OIDC | This specific binary was produced by this specific workflow run + commit + runner image. |

The SBOM files (`hull.sbom.json`, `hull.sbom.cdx.json`, `hull.sbom.spdx.json`)
are included in `hull.sha256`, so they inherit all three signatures.

### 7.4. Hosted rebuild attestation service (Future. Phase 9)

The byte-reproducibility property above lets anyone with source
verify a binary matches its source. A hosted **rebuild attestation
service** would make that one HTTP call instead of a local rebuild,
producing a portable cryptographic statement third parties can rely on.

**Service:** `api.gethull.dev/ci/v1`

#### Flow

1. Developer pushes source to GitHub
2. CI calls `api.gethull.dev/ci/v1/build`
3. Service rebuilds with the recorded `cc_version` + `flags` from
   `package.sig`
4. Compares `binary_hash`
5. If match, issues a "Reproducible Build Verified" attestation
6. Attestation is an Ed25519 signature over
   `{binary_hash, timestamp, builder_version}`

#### What the service adds over Tier 4

Tier 4 lets a determined auditor re-derive locally. The hosted
service lets a non-auditor (a buyer, a regulator, a compliance team)
trust that someone independent did the re-derivation, with a signed
attestation they can attach to a SBOM or procurement record.

A self-hosted alternative is identical in shape: run your own
attestation service, sign with your own key, pin your own trust
root in your customers' apps.

---

## 8. Keel HTTP Server Audit

The Keel HTTP server library (vendored at `vendor/keel/`, upstream at
[github.com/artalis-io/keel](https://github.com/artalis-io/keel)) is
maintained as a separate project with its own audit cycle. The
findings that were live when Hull first vendored Keel. Kqueue
READ|WRITE bitmask handling, WebSocket and HTTP/2 partial-write
issues, TLS private-key zeroization, `writev_all` busy-spin on EAGAIN
. Are all resolved upstream and reflected in the current submodule
pin.

Keel ships the same hardening Hull does: `-Wall -Wextra -Wpedantic
-Wshadow -Wformat=2 -Werror`, `-fstack-protector-strong` (non-Cosmo
builds), ASan+UBSan debug build, libFuzzer targets for the HTTP parser
and multipart parser, request-smuggling and header-injection guards.
For the live audit history see the Keel repository.

---

## 9. Known Limitations

These are real, not theoretical:

| Limitation | Impact | Mitigation |
|------------|--------|------------|
| macOS Seatbelt returns EPERM, not SIGKILL | App survives sandbox violations (operation denied, process continues) | C-level caps also return errors; net effect is the same |
| Lua instruction hook is per-VM, not per-coroutine-instruction | Hook fires every N VM instructions globally; coroutine yields reset the counter | Both runtimes enforce the same default 100M instruction limit |
| Canary is not foolproof | Attacker could embed magic bytes in custom binary | Reproducible builds (`make reproducible-check`, CI-gated) eliminate this |
| `realpath()` is TOCTOU | Race between check and use | Kernel unveil prevents actual access |
| Default CSP blocks client-side JS | Apps needing fetch/AJAX must customize CSP | `app.manifest({ csp = "default-src 'self'; connect-src 'self'" })` |
| 32-entry limit per manifest category | Large apps may hit ceiling | Sufficient for most production apps |
| `req.ctx` uses raw malloc (not tracked) | ctx JSON bypasses runtime memory limits | Capped at 64KB; bounded by runtime heap indirectly |
| HMAC-SHA256 binding returns hex string | Callers must use constant-time comparison | `hull.jwt` and `hull.web.middleware.csrf` stdlib use constant-time internally |
| `--no-sandbox` is the only W^X downgrade | Apps run under `--no-sandbox` lose every kernel-enforced guarantee, not only W^X | Use only for local development; production startup fails closed when the manifest opts into dynamic code / dynamic libraries. The Hardened-Runtime `csops` probe under `HL_RELEASE_BUILD` and the WAMR-JIT `#error` build assertions add additional fail-loud layers. |
| `hull.web.pwned` is fail-open on upstream outage | If `api.pwnedpasswords.com` is unreachable, password changes proceed without the breach check rather than locking every user out | Surfaced in `hull agent auth-status` JSON (`pwned.status = "fail_open"`). Operators who need fail-closed wrap the call site and reject on `nil, "unreachable"`. |
| Multipart magic-byte sniffer is best-effort | Formats with no fixed leading bytes (raw `application/octet-stream`, plain text) fall back to the declared Content-Type | Apps handling arbitrary uploads should add per-format validators after the iterator and before exposing the blob to other systems. |
| `hull.web.middleware.audit-log` writes to the app's SQLite file | A compromised app DB could in principle be edited offline by an attacker with file-system write access | For stronger non-repudiation ship events to an external WORM store via the outbox helper; set `cleanup = "external"` to silence the `auth-health` warning. |
