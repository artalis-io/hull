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
| Before download | Inspect capabilities | `verify/index.html` (offline) | Gethull platform-sig, per-app platform sig, app sig, manifest |
| Before install | Verify integrity | `hull verify --developer-key` (CLI) | All three layers + file hashes |
| At startup | Runtime check | `--verify-sig` flag | All three layers + file hashes |

The platform layer split into two sub-layers in v0.1.3:

- **Gethull layer** (`package.sig.platform.gethull`) — the per-arch
  SHA-256 manifest of `libhull_platform.a`, signed at release time by
  `HULL_PLATFORM_KEY` and verified by every consumer against the
  hard-coded `HL_PLATFORM_PUBKEY_HEX`. This is the proof that the
  platform bytes baked into a built app are the ones gethull.dev
  published.
- **Per-app platform layer** (`package.sig.platform.{platforms,
  signature, public_key}`) — the developer's own
  `hull sign-platform` output, kept for back-compat and for forks
  that pin a non-gethull platform pubkey. Verified for
  self-consistency only — no upstream-key pinning, since that role
  moved to the gethull layer.

### Trust Anchors

**Gethull platform public key:**
- Hardcoded in Hull CLI as `HL_PLATFORM_PUBKEY_HEX` in
  `include/hull/signature.h`
  (`2a5461235aa51bbbe1e9cbc462e6a63f37d099f5ad17646a8f3a67db2f3a4fad`,
  active since v0.1.3)
- Hardcoded in the browser verifier (`GETHULL_DEV_PLATFORM_KEY` in
  `site/verify.html`) — must rotate in lockstep
- Override at compile time with
  `-DHL_PLATFORM_PUBKEY_HEX="<your hex>"` for forks running their
  own platform-signing key (Section 3.D); pass
  `--no-verify-platform` to skip the gethull check entirely

**Developer public key:**
- Published in app repository (`.pub` file)
- Manually cross-referenced by user against trusted source
- Passed explicitly: `hull verify --developer-key dev.pub`

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
  key mismatch — the gethull-signed layer is missing or signed by an
  unexpected key, the user sees a warning in both UIs.
- **Out of scope:** Post-install binary integrity (an attacker with
  local write access to the on-disk `hull` binary editing
  `HL_PLATFORM_PUBKEY_HEX` and re-signing). This is the same threat
  class as any local malware with file-system write access — not
  something the signature scheme can prevent. Defense lives at the
  OS layer (signed system updates, FIM, SELinux/AppArmor, etc.).
  Reproducible builds (Phase 9) make the bytes-on-disk
  cross-checkable against the published source.

**Attack: Declare minimal manifest but access more at runtime**

- **Prevention:** Manifest is signed in `package.sig`. At runtime, pledge/unveil enforce the declared capabilities at the kernel level. Accessing undeclared paths triggers SIGKILL (Linux/Cosmo).
- **Remaining risk:** On macOS, Seatbelt returns EPERM (operation denied) rather than SIGKILL. The app continues running after a violation — the forbidden operation simply fails. On Linux/Cosmo, the process is killed on violation. The practical security is equivalent (the operation is denied either way), but the failure mode differs.

**Attack: Call `app.manifest()` again at runtime to escalate capabilities**

- **Prevention:** Three independent barriers make this a non-issue:
  1. **One-shot enforcement:** `app.manifest()` errors on second call in both Lua and JS runtimes. The first call writes to a registry key; any subsequent call raises a runtime error (`"app.manifest() can only be called once"`).
  2. **Startup-only extraction:** The manifest is read from the runtime into a C struct (`HlManifest`) once during startup (step 10 of the boot sequence). C-level capabilities (`rt->env_cfg`, `rt->http_cfg`, `rt->csp_policy`) are wired from this struct and never re-read from the runtime state.
  3. **Kernel seal:** `unveil(NULL, NULL)` seals filesystem visibility and `pledge()` restricts syscall families. Both are one-way operations — the kernel refuses to add permissions after sealing, regardless of what the runtime state says.
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
  - **WASM:** WAMR is compiled without `WASM_ENABLE_JIT` and `WASM_ENABLE_FAST_JIT` (the C source carries `#error` directives so the policy fails at build time if either is re-enabled). Modules run via the fast interpreter or as AOT artifacts produced at `hull build` time and embedded in the VFS — never JIT-compiled at runtime. `init_args.running_mode = Mode_Interp` is set explicitly in `hl_cap_wasm_init`.
  - **Kernel (Linux/Cosmopolitan):** seccomp-bpf via the jart/pledge polyfill denies `mmap` with `PROT_WRITE|PROT_EXEC`, denies `mprotect` adding `PROT_EXEC` (including `pkey_mprotect`), returns `ENOSYS` from `memfd_create`, and kills the process on `execve`/`execveat`/`ptrace`/`process_vm_readv`/`process_vm_writev` — none of those promises are granted in the phase-2 promise set.
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
  2. **Content-Security-Policy (CSP):** Hull injects a strict CSP header on every `res:html()` / `res.html()` response by default: `default-src 'none'; style-src 'unsafe-inline'; img-src 'self'; form-action 'self'; frame-ancestors 'none'`. This blocks inline scripts, external script loads, `eval()`, object embeds, and iframe embedding — even if an attacker bypasses template escaping, the browser refuses to execute injected scripts.
- **Remaining risk:** Raw output (`{{{ }}}`) and the `| raw` filter bypass escaping. Developers must only use raw output with trusted content. Templates don't escape for JavaScript string contexts (e.g. inline `<script>` blocks) — use `{{ var | json }}` to safely embed data in JS contexts. Apps that require client-side JavaScript must customize the CSP (e.g. `app.manifest({ csp = "default-src 'self'; script-src 'self'" })`).

**Attack: Clickjacking — embedding the app in a malicious iframe**

- **Prevention:** The default CSP includes `frame-ancestors 'none'`, which instructs the browser to refuse rendering the page inside any `<iframe>`, `<frame>`, or `<object>` tag. This prevents UI redress attacks where a malicious site overlays invisible frames over the app to trick users into clicking hidden elements.
- **Actor:** Any third-party website operator. Does not require compromising the app — just embedding it.

**Attack: MIME type confusion / content sniffing**

- **Prevention:** The default CSP's `default-src 'none'` prevents the browser from loading any sub-resources (scripts, stylesheets, fonts, media) that an attacker might inject via reflected content. Combined with `Content-Type: text/html; charset=utf-8` set by `res:html()`, the browser cannot misinterpret response content.
- **Actor:** Network MITM or injection via stored user content.

**Attack: Template injection (server-side template injection / SSTI)**

- **Prevention:** Template compilation uses `luaL_loadbuffer` / `JS_Eval` in the C bridge, which is only callable from embedded stdlib code (not user application code). The code generator produces deterministic output from the AST — user data is never interpolated into the generated source code. User data flows through the `__d` (data) parameter at render time, not at compile time. There is no `eval()` or `load()` in the sandboxed runtimes.

**Attack: Session hijacking via cookie theft**

- **Prevention:** `hull.cookie` defaults to `HttpOnly=true`, `Secure=true`, `SameSite=Lax`. HttpOnly prevents JavaScript access (XSS-based theft). Secure prevents plaintext transmission. SameSite=Lax blocks cross-origin POST requests from carrying session cookies.
- **Remaining risk:** Same-origin XSS can still read `req.ctx.session` data. Hull's template engine (`hull.template`) auto-escapes all `{{ }}` output by default (`& < > " '` → HTML entities), which prevents most reflected and stored XSS vectors. Raw output via `{{{ }}}` or the `| raw` filter bypasses escaping and should only be used with trusted content.

**Attack: CSRF — forged state-changing requests from another origin**

- **Prevention:** `hull.middleware.csrf` middleware generates HMAC-based tokens tied to the session ID and timestamp. State-changing methods (POST/PUT/DELETE/PATCH) require a valid CSRF token in the `X-CSRF-Token` header or `_csrf` form field. Tokens expire (default 1h). Safe methods (GET/HEAD/OPTIONS) are automatically skipped. Constant-time comparison prevents timing attacks.
- **Remaining risk:** If the CSRF secret is leaked, tokens can be forged. The secret must be stored securely (e.g., `env.get("SECRET_KEY")`).

**Attack: JWT token forgery**

- **Prevention:** `hull.jwt` uses HS256 with HMAC-SHA256 (no "none" algorithm, no algorithm negotiation). Signature verification uses constant-time comparison. Expired tokens are rejected.
- **Remaining risk:** JWT secrets must be strong. JWTs are stateless — they cannot be revoked until they expire. For revocation, use sessions instead.

**Attack: Session fixation / brute-force session IDs**

- **Prevention:** `hull.middleware.session` generates 32 random bytes (256-bit entropy) via `crypto.random()` for session IDs. IDs are hex-encoded (64 chars). Sessions are server-side (SQLite) with sliding expiry. Expired sessions are automatically pruned.

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
| `app.manifest({ csp = "custom..." })` | Custom CSP string |
| `app.manifest({ csp = false })` | CSP disabled (opt-out) |

**Where CSP is injected:** At the C level in `lua_res_html()` and `js_res_html()`, not in application code. This means the CSP cannot be forgotten, bypassed, or misconfigured by app developers — it's structural, like parameterized SQL. Only `res:json()` and `res:text()` skip CSP (non-HTML content types are not vulnerable to script injection).

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
| W^X / dynamic code | seccomp-bpf denies `mmap` and `mprotect` adding `PROT_EXEC`; `memfd_create` returns ENOSYS; `execve`/`execveat`/`ptrace`/`process_vm_*` are not in any granted promise group | SIGKILL or ENOSYS |
| Mode | `__pledge_mode = KILL_PROCESS \| STDERR_LOGGING` | Process killed + diagnostic to stderr |

**Allowed pledge promises:** `stdio inet rpath wpath cpath flock dns` (dns only if hosts declared). Notably absent: `prot_exec`, `exec`, `proc` — these grant the very syscalls Hull's W^X policy forbids.

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
- `(deny default)` — deny-by-default SBPL profile generated dynamically from manifest
- App directory: read-only access to app files
- Database directory: read-write access to SQLite files (db, WAL, SHM, journal)
- Manifest `fs` paths: unveil-equivalent via `(allow file-read* (subpath ...))` / `(allow file-read* file-write* ...)`
- Network: TCP allowed only when manifest declares `hosts`
- GPU: `iokit-open` + MTLCompilerService mach-lookup allowed only when `manifest.gpu` is set
- CA bundle + TLS paths: read-only when HTTPS client is used
- System frameworks + dyld cache: read-only (required for process operation)
- Parameter substitution for paths — avoids escaping issues with special characters
- Irreversible — `sandbox_init` cannot be modified or removed after application

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
continue to function — the Seatbelt `(deny dynamic-code-generation)`
clause and the C-level capability layer still apply.

**Active C-level defenses (defense-in-depth):**
- `hl_cap_fs_validate()` rejects path traversal (absolute paths, `..`, symlink escapes via realpath)
- `hl_cap_env_get()` enforces allowlist
- `hl_cap_db_query()` uses parameterized binding
- `hl_cap_http_request()` validates host allowlist

**Difference from Linux/Cosmo:** Seatbelt returns EPERM on violation (the operation fails with a permission error) rather than SIGKILL (the process is killed). The app stays alive but the operation is denied. The C capability layer returns errors on violation in all cases, so the practical behavior is identical — the forbidden operation fails.

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

**No manifest declared?** Even if the app doesn't call `app.manifest()`, the default-deny posture is identical to `app.manifest({})`:
- **Kernel sandbox is applied** — pledge/unveil restrict to only the database file and TLS paths
- **CSP is active** — the default strict policy is injected on all HTML responses
- **C-level capabilities deny all** — env returns NULL, HTTP requests fail, filesystem operations fail
- Signature still covers the absence of manifest (`"manifest": null`)

**Escape-hatch keys (off by default, surfaced as risky by `hull inspect`):**

| Key | Type | Default | Meaning |
|-----|------|---------|---------|
| `allow_dynamic_code` (Lua) / `allowDynamicCode` (JS) | bool | `false` | Opt-in to JIT / runtime codegen. Rejected by the sandbox unless `--no-sandbox` is passed. |
| `allow_dynamic_libraries` (Lua) / `allowDynamicLibraries` (JS) | bool | `false` | Opt-in to runtime native library loading (dlopen). Rejected by the sandbox unless `--no-sandbox` is passed. |

Setting either key emits a warning at manifest-extract time and causes the kernel sandbox to refuse to start. There is no in-policy downgrade — the operator must explicitly disable the sandbox with `--no-sandbox` (development only).

All example apps declare `app.manifest()` explicitly, even when the empty `{}` is sufficient, as a best practice.

---

## 5b. Module Declaration

The manifest's `modules` table is the app's **declared module surface** — a strict allowlist of which first-party Hull stdlib modules the app may import at runtime. It complements the capability fields described above: capability sections (`fs`, `hosts`, `env`) say *what ambient authority* the app uses; `modules` says *which library names* it imports.

```lua
app.manifest({
    modules = {
        "hull/crypto@1",
        "hull/db@1",
        "hull/time@1",
        "hull/validate@1",
        "hull/middleware/auth@1",
        "hull/middleware/session@1",
    },
    fs    = { read = {"data/"} },
    hosts = {"api.stripe.com"},
})

-- import paths use the standard Lua/JS forms — name the local
-- variable whatever you want:
local crypto = require("hull.crypto")
local fetcher = require("hull.http-client")           -- bind to any local name
```

Each entry is a canonical spec `"<vendor>/<name>@<major>"`. First-party modules live under `hull/`; future third-party packages would follow the same pattern (`"acme/widgets@2"`). The manifest declares *what's in scope*; the `require()` / `import` call site picks *what to call it locally*.

**Design principles:**

| Principle | What it means |
|-----------|---------------|
| **Every external capability is declared** | Lua/JS intrinsics (string, table, math, JSON in JS) plus a minimal Hull core are always available. The intrinsic core is just `hull/app` — the registration API; it must be intrinsic because the manifest is expressed via `app.manifest(...)`. Every other first-party module — `hull/log`, `hull/json`, `hull/crypto`, `hull/db`, `hull/http`, every middleware, every stdlib helper — must be in `modules` or imports fail. |
| **Import-only exposure** | Declared modules are reached via `require("hull.X")` (Lua) / `import "hull:X"` (JS). They are NOT globals. Apps that don't declare a module cannot use it even by accident. |
| **Capability + module separate gates** | Declaring `hull/http-client@1` does not also open the network. Apps still need a non-empty `hosts` allowlist. The resolver rejects `hull/http` declared without `hosts`. Same for `hull/fs` (needs `fs.read`/`write`) and `hull/env` (needs `env`). |
| **Explicit dependencies** | If `hull/middleware/session` internally uses `hull/db`, `hull/crypto`, and `hull/time`, the app must declare *all four*. No silent pull-in. The resolver lists the missing dep in its error. |
| **Sealed at startup, no runtime install** | The resolved module set is computed once after manifest extraction, frozen for the lifetime of the process. Runtime code cannot install, fetch, discover, or load new modules. The set's contents are signed into `package.sig` as `modules_resolved`. |
| **Build-time subsystems gate too** | Modules whose backing C is compile-time-optional (`hull/db`, `hull/compute`, `hull/gpu`) are rejected if the build wasn't compiled with the corresponding `HL_ENABLE_*` flag. The resolver reports the missing build flag by name. |

**What this prevents:**
- **Capability expansion via stdlib upgrade.** A new module added to `hull/*` cannot magically become available to existing apps. The app's declaration is its admit list.
- **Hidden imports in transitive code.** If a vendored helper `require("hull.crypto")` without the app declaring it, the require fails at runtime. The static-analysis pass (planned, see roadmap) surfaces this at build time.
- **Ambient stdlib drift.** The set of admitted modules is auditable from `package.sig` alone — no need to load the app to know what it imports.

**What it does NOT do:**
- Replace the capability sections. `modules = { http = "hull/http-client@1" }` doesn't grant network — the app still needs a `hosts` allowlist. The resolver enforces this pairing.
- Defeat C-level escape via embedded WASM or compute. Compute modules still execute inside the WAMR sandbox with their own gas + memory limits.

**Inspection:**

| Command | Output |
|---------|--------|
| `hull modules available` | Print the full first-party registry — names, deps, capability requirements |
| `hull modules list [APP_DIR]` | What the app declares |
| `hull modules explain <NAME>` | One spec, including deps and required capabilities |
| `hull --json modules ...` | Same, machine-readable |
| `hull agent modules [APP_DIR]` | Agent-friendly JSON: `{declared, intrinsic, build_caps, registry_count}` |
| `hull doctor` | Reports which `HL_ENABLE_*` build-time subsystems are linked in |

**This is not npm/pip/cargo.** There is no remote registry, no fetching, no runtime install, no third-party packages, no version resolution beyond the single API-major. v1 is capability-aware dependency declaration for sealed Hull apps.

---

## 6. Verification Tools

**Status:** Shipped in full as of v0.1.3 — all three verifiers check
the gethull platform-sig layer, the per-app self-consistency layer,
and the app developer signature.

### A. Browser Verifier (`site/verify.html`)

- Self-contained HTML file, zero server dependencies
- Runs entirely in browser — no data sent anywhere
- Inlined tweetnacl-js (public domain) for Ed25519
- Web Crypto API for SHA-256
- CSP: `default-src 'none'` — no network except optional key fetch via `connect-src https:`
- Gethull platform pubkey hardcoded (matches `HL_PLATFORM_PUBKEY_HEX`)
- Release pubkey hardcoded (matches `HL_RELEASE_PUBKEY_HEX`) — covers
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
- **Per-app platform layer:** self-consistency check only — no
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

The Keel HTTP server library (vendored at `vendor/keel/`, upstream at
[github.com/artalis-io/keel](https://github.com/artalis-io/keel)) is
maintained as a separate project with its own audit cycle. The
findings that were live when Hull first vendored Keel — kqueue
READ|WRITE bitmask handling, WebSocket and HTTP/2 partial-write
issues, TLS private-key zeroization, `writev_all` busy-spin on EAGAIN
— are all resolved upstream and reflected in the current submodule
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
| Canary is not foolproof | Attacker could embed magic bytes in custom binary | Reproducible builds (Phase 9) eliminate this |
| `realpath()` is TOCTOU | Race between check and use | Kernel unveil prevents actual access |
| Default CSP blocks client-side JS | Apps needing fetch/AJAX must customize CSP | `app.manifest({ csp = "default-src 'self'; connect-src 'self'" })` |
| 32-entry limit per manifest category | Large apps may hit ceiling | Sufficient for most production apps |
| `req.ctx` uses raw malloc (not tracked) | ctx JSON bypasses runtime memory limits | Capped at 64KB; bounded by runtime heap indirectly |
| HMAC-SHA256 binding returns hex string | Callers must use constant-time comparison | `hull.jwt` and `hull.middleware.csrf` stdlib use constant-time internally |
| `--no-sandbox` is the only W^X downgrade | Apps run under `--no-sandbox` lose every kernel-enforced guarantee, not only W^X | Use only for local development; production startup fails closed when the manifest opts into dynamic code / dynamic libraries. The Hardened-Runtime `csops` probe under `HL_RELEASE_BUILD` and the WAMR-JIT `#error` build assertions add additional fail-loud layers. |
