/*
 * module_registry.c — Canonical first-party Hull module table.
 *
 * Sorted by `name` in `strcmp` order so lookups can binary-search.
 * Adding a new module: insert in sort order, then bump the sort-order
 * sanity check at the bottom of `module_registry_self_check()` if any.
 *
 * The set of modules here is the v1 universe of names an app manifest
 * may reference. Whether a given module's runtime bindings actually
 * compile in is gated separately by HL_ENABLE_* flags (the resolver
 * checks `required_caps` against those).
 *
 * v0.2.0 reorganization (§1.3): strictly-web modules moved under
 * the hull/web/ namespace — see docs/roadmap_next.md §1.3 for the
 * move table.
 * Modules kept flat: hull/jwt (cross-cutting), hull/http-server
 * (foundational primitive), hull/template (content-type agnostic),
 * hull/http-client / hull/email / hull/smtp (cross-cutting non-web).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/module_registry.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ── The canonical table ───────────────────────────────────────────── */

/*
 * Field shorthand:
 *   .name = "hull/foo", .api_major = 1, .intrinsic = 0|1, .pure = 0|1,
 *   .required_caps = HL_MOD_CAP_*, .deps = {"hull/bar", NULL}
 *
 * Entries MUST stay sorted by `name` (strcmp). Insertions in the
 * middle of the table are intentional — keep them sorted.
 */
static const HlModuleSpec REGISTRY[] = {
    /* ── Intrinsic core (always present, no declaration needed) ───── */
    {
        .name = "hull/app",
        .api_major = 1, .intrinsic = 1, .pure = 0,
        .required_caps = 0, .deps = {0},
    },

    /* ── C-native side-effect modules + cross-cutting utilities ───── */
    {
        /* ustar archive parse/create/extract/pack. Namespaced under
         * hull/archive/ as the container-format family (zip/etc. would
         * be siblings; stream codecs live under a separate hull/compress/).
         * parse/create are pure byte<->table transforms (no authority);
         * extract/pack compose the fs capability, so they need
         * manifest.fs.write / fs.read at call time (gated independently,
         * not a module dep). */
        .name = "hull/archive/tar",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {0},
    },
    {
        /* Attachment storage: content-addressed disk via hull/blob,
         * metadata + dedup-by-refcount via hull/db. Flat top-level
         * (not under hull/web/) — the core API (store/read/metadata/
         * delete) is FS+DB only and works in CLI tools. The web-
         * specific auth-gated serve(req, res, id) lives in a separate
         * hull/web/attachment-serve@1 module. */
        .name = "hull/attachment",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0,
        /* hull/fs is needed by attachment.read_to_file (PR 2 of
         * §1.5.b-4) which streams a blob.reader through fs.write. */
        .deps = {"hull/blob", "hull/crypto", "hull/db", "hull/fs",
                 "hull/mime", "hull/time", 0},
    },
    {
        /* Content-addressed blob storage. Caller declares a fs.write
         * path; blob.init() validates against it. No SQLite dep —
         * works under HL_ENABLE_DB=0 (compute-only builds). */
        .name = "hull/blob",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_FS, .deps = {0},
    },
    {
        /* In-memory key/value cache with TTL + get-or-compute memoization.
         * Bounded (LRU on cap), lazily-expiring, process-local. Pure Lua/JS
         * over time; no persistence, no new authority. */
        .name = "hull/cache",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {"hull/time", 0},
    },
    {
        .name = "hull/compute",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_WASM, .deps = {0},
    },
    {
        /* Typed, fail-fast config over the env capability. The env allowlist
         * still gates each read (config just wraps env.get). load_dotenv reads
         * an optional .env via fs (needs manifest.fs.read) and applies only
         * allowlisted keys, so hull/fs is a dep. */
        .name = "hull/config",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {"hull/env", "hull/fs", 0},
    },
    {
        .name = "hull/crypto",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {0},
    },
    {
        /* HMAC-signed, JSON-payload, stateless token framing
         * (base64url(payload) "." hex(hmac)). Used internally by
         * hull/web/auth-flows tokens and hull/web/middleware/oauth
         * state cookies; the pattern is intentionally pulled out
         * so the signature framing — including the pcall around
         * hmac_sha256_verify that turns malformed-hex into a clean
         * "bad tag" — lives in one place. */
        .name = "hull/crypto/envelope",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0,
        .deps = {"hull/crypto", "hull/json", 0},
    },
    {
        .name = "hull/csv",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        .name = "hull/db",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_DB, .deps = {0},
    },
    {
        .name = "hull/email",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        /* email dispatches to either SMTP or HTTPS API providers — both
         * are reachable from the same email.send() entry point, so both
         * are hard deps. Auto-pulls hull/log + hull/json for structured
         * error logging and provider payload marshaling. */
        .required_caps = 0,
        .deps = {"hull/http-client", "hull/smtp", "hull/log", "hull/json", 0},
    },
    {
        .name = "hull/env",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_ENV, .deps = {0},
    },
    {
        .name = "hull/fs",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_FS, .deps = {0},
    },
    {
        .name = "hull/gpu",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_GPU, .deps = {0},
    },
    {
        /* Outbound HTTPS client — http.fetch. Renamed from
         * hull/http@1 (which was misleading; it was always the
         * client). The server-side counterpart is hull/http-server@1.
         * Stays flat in v0.2.0: cross-cutting (CLI tools, batch
         * jobs, and service-to-service calls all fetch URLs). */
        .name = "hull/http-client",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HOSTS | HL_MOD_CAP_HTTP_CLIENT,
        .deps = {0},
    },
    {
        /* Inbound HTTP server — REST verbs + middleware + router.
         * Declaring this module decorates the `app` intrinsic with:
         *   app.get / .post / .put / .delete / .patch / .options
         *   app.use / .use_post
         *   app.router(prefix, opts)
         * Without it, those methods don't exist on `app` at all
         * (attempt to call a nil value / not a function). Replaces
         * the vestigial hull/server@1 module — middleware modules
         * now depend on hull/http-server.
         *
         * Stays flat in v0.2.0: foundational primitive that the
         * hull/web/ namespace is built on top of. Convention is that
         * hull/web/X modules are CONSUMERS of http-server, not
         * http-server itself. */
        .name = "hull/http-server",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER, .deps = {0},
    },
    {
        .name = "hull/i18n",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        .name = "hull/image",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_IMAGE, .deps = {0},
    },
    {
        .name = "hull/inspect",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        /* Durable DB-backed background job queue. Design: docs/jobs_design.md.
         * The DB requirement rides in via the hull/db dep (HL_MOD_CAP_DB), so a
         * DB-less build rejects a hull/jobs declaration with the right message. */
        .name = "hull/jobs",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0,
        /* crypto: the per-claim token nonce (crypto.random). */
        .deps = {"hull/db", "hull/time", "hull/json", "hull/crypto", 0},
    },

    /* ── Pure data codec ──────────────────────────────────────────── */
    {
        /* json was intrinsic in early v0.1.0; demoted to declared so
         * the intrinsic core is just `app`. The runtime's response
         * helpers (res:json / res.json) keep working at the C level
         * without requiring this declaration — apps only need to
         * declare it when they call json.encode/decode directly. */
        .name = "hull/json",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        /* JWT stays flat in v0.2.0: cross-cutting (API auth, CLI
         * tokens, service-to-service signing — not strictly web). */
        .name = "hull/jwt",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0,
        /* HMAC needs crypto; payload encoding needs json; exp/iat checks need time. */
        .deps = {"hull/crypto", "hull/json", "hull/time", 0},
    },
    {
        /* Portable key/value STORE. Distinct from hull/cache (ephemeral +
         * evicting): hull/kv is externally-meaningful state with no eviction
         * unless explicitly requested. Backends: memory (in-process), sqlite /
         * postgres (durable, over an EXISTING hull/db connection passed by the
         * app). Pure Lua/JS over hull/time; it carries no new authority of its
         * own - a SQL backend's fs/network access is the db capability's,
         * already gated, so hull/db is the app's declaration, not a dep here. */
        .name = "hull/kv",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {"hull/time", 0},
    },

    /* ── Logger ───────────────────────────────────────────────────── */
    {
        /* log was intrinsic in early v0.1.0; demoted to declared. It
         * does stderr I/O, so the declared-modules story is cleaner
         * with it explicit. Runtime's internal error logging stays
         * intrinsic at the C level. */
        .name = "hull/log",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {0},
    },
    {
        /* Contextual logging: logx.with{fields} binds logfmt fields onto a
         * child logger over hull/log. Pure Lua/JS, no new authority. */
        .name = "hull/logx",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {"hull/log", 0},
    },
    {
        /* Content-MIME sniffer. Thin binding around hl_cap_mime_sniff:
         * mime.sniff(bytes) returns the sniffed MIME string or nil.
         * Pure — no fs/network/db access — usable in CLI tools as
         * well as servers. */
        .name = "hull/mime",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },

    /* ── Pure stdlib + remaining side-effect ─────────────────────── */
    {
        /* QR Code generator (ISO/IEC 18004). Pure transformation of
         * text → matrix → SVG. Used by hull/web/middleware/totp for
         * enrollment QR codes, but content-agnostic so it stays flat
         * (not under hull/web/). Also useful for WiFi sharing codes,
         * contact cards, payment links, etc. */
        .name = "hull/qrcode",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        /* Generic retry-with-backoff for any fallible op (http/compute/gpu/db).
         * retry.run(fn, opts) + retry.backoff. Sleeps via the hull.sleep
         * intrinsic; crypto is used only for optional jitter. */
        .name = "hull/retry",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {"hull/crypto", 0},
    },
    {
        .name = "hull/search",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0,
        .deps = {"hull/db", 0},
    },
    {
        .name = "hull/smtp",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        /* SMTP client — outbound mail delivery. */
        .required_caps = HL_MOD_CAP_HOSTS | HL_MOD_CAP_HTTP_CLIENT,
        .deps = {0},
    },
    {
        /* Template engine stays flat in v0.2.0: content-type
         * agnostic. The engine itself doesn't know HTML — apps render
         * any text (emails, configs, codegen). */
        .name = "hull/template",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        /* The `| json` filter encodes data as JSON in templates. */
        .required_caps = 0, .deps = {"hull/json", 0},
    },
    {
        .name = "hull/time",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {0},
    },
    /* Timers — declaring this module decorates the `app` intrinsic
     * with `app.every(ms, fn)` and `app.daily(hhmm, fn)`. Without it
     * those methods don't exist on `app` at all (calling them raises
     * "attempt to call a nil value"). The methods grant no new
     * authority — the handler runs in the same sandbox with the
     * same declared caps — but the declaration makes background
     * activity visible to a reviewer reading the manifest. */
    {
        .name = "hull/timers",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {0},
    },
    {
        .name = "hull/tui",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        /* Terminal UI capability. Requires HL_ENABLE_TUI at build AND
         * tui = true in the manifest. */
        .required_caps = HL_MOD_CAP_TUI, .deps = {0},
    },
    {
        /* RFC 9562 UUID v4/v7. Pure composition over crypto.random + time;
         * no new authority. */
        .name = "hull/uuid",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {"hull/crypto", "hull/time", 0},
    },
    {
        .name = "hull/validate",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },

    /* ── Web stdlib namespace (v0.2.0) ────────────────────────────────
     * Strictly-web modules: HTTP-protocol concerns (cookie, form),
     * HTML rendering (htmx), real-time delivery (sse, ws-client,
     * ws-server), flash messages, and the 14 hull/web/middleware
     * entries. Sort note: hull/web/<name> sorts before hull/worker
     * because at position 6 'e' < 'o'. */
    {
        /* Auth-gated HTTP response helper for hull/attachment. Thin
         * web layer: takes (req, res, id, opts) and returns a 4xx
         * (no auth or missing) or 200 with Content-Type, RFC 5987
         * Content-Disposition, strong ETag (=blob_id), and If-None-
         * Match → 304. Default-deny auth_check semantics. */
        .name = "hull/web/attachment-serve",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0,
        .deps = {"hull/attachment", "hull/blob", "hull/http-server", 0},
    },
    {
        /* Auth-flow recipes: registration, email-verify, login,
         * password-reset, magic-link, email-change. HMAC-signed
         * single-use tokens (state_secret), PBKDF2 password hash
         * (crypto.hash_password / verify_password), JSON or
         * url-encoded request bodies. App provides user-storage
         * callbacks + email_send + templates; the module owns the
         * auth-internal bookkeeping tables (_hull_auth_used_tokens,
         * _hull_auth_pending_email_changes). TOTP / 2FA is the app's
         * concern (compose with hull/web/middleware/totp). */
        .name = "hull/web/auth-flows",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER | HL_MOD_CAP_DB,
        /* hull/crypto/envelope transitively brings hull/crypto +
         * hull/json — both are still imported directly by the
         * module (hash_password, base64url, json) but the
         * resolver admits them via the dep chain so they don't
         * need re-declaration here.
         *
         * hull/web/pwned + hull/web/middleware/audit-log are now
         * eager top-level requires on both runtimes (Lua dropped
         * its pcall(require, ...) pattern so the transitive
         * declaration footprint is symmetric — QuickJS can't
         * dynamic-import). The check_pwned_passwords / sign_in_log
         * opts still gate whether those modules' side-effects are
         * triggered at request time. The HIBP host is gated at
         * call time via manifest.hosts; audit-log only writes
         * to the local DB. */
        .deps = {"hull/http-server", "hull/db",
                 "hull/crypto/envelope", "hull/crypto",
                 "hull/time", "hull/web/pwned",
                 "hull/web/middleware/audit-log",
                 "hull/web/middleware/ratelimit",
                 /* Round-8 MEDIUM-9: emit_event uses hull/log to warn
                  * on a swallowed audit-log failure so the symptom
                  * surfaces somewhere instead of being silent. */
                 "hull/log", 0},
    },
    {
        /* Runtime health probes for the auth stack. Read-only —
         * checks the shape of _hull_* tables, pwned.health() state,
         * audit-log cleanup scheduling. Apps wire auth_health.routes
         * to expose a JSON endpoint that the `hull agent auth-status`
         * CLI consumes. Statically imports audit-log + pwned so the
         * `cleanup_scheduled` and `pwned.health()` probes work
         * without dynamic require (QuickJS limitation). */
        .name = "hull/web/auth-health",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_DB,
        .deps = {"hull/db", "hull/web/middleware/audit-log",
                 "hull/web/pwned", 0},
    },
    {
        .name = "hull/web/cookie",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        /* One-shot user notifications across POST/redirect/GET. Two
         * emission paths: session-backed (set/consume — needs
         * middleware/session) and HTMX HX-Trigger (trigger — pure
         * header set, no session). Session is a hard dep so the
         * session-backed path always works once the module is
         * declared. */
        .name = "hull/web/flash",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0,
        .deps = {"hull/json", "hull/web/middleware/session", 0},
    },
    {
        .name = "hull/web/form",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        /* HTMX request inspection + response-header helpers. Pure
         * functions; no I/O. Reads request headers, mutates response
         * headers. Apps using it almost always also need
         * hull/http-server but that's a soft dep (htmx works inside
         * any handler). */
        .name = "hull/web/htmx",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        /* HTMX confirm dialog widget — server-side helper that
         * renders the attribute string the shipped client JS at
         * /static/hull/htmx/confirm/confirm.js consumes. The
         * client intercepts htmx:confirm, shows a native <dialog>
         * in place of window.confirm(), and either calls
         * issueRequest() on yes or drops the request on cancel /
         * dismiss. Structural CSS only.
         *
         * Pure attribute-string builder; no I/O. No runtime
         * dependencies (the json/htmx modules aren't touched
         * server-side — the entire dialog flow is client-side). */
        .name = "hull/web/htmx/confirm",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0,
        .deps = {"hull/web/htmx", 0},
    },
    {
        /* HTMX form widget — server-side helpers for rendering
         * field-level validation errors (consuming a hull/validate
         * result) plus shipped client JS that puts the submit
         * button into aria-busy / disabled / optional label-swap
         * during htmx requests. Structural CSS only.
         *
         * Pure HTML-string builder; no I/O. Soft-coupled to
         * hull/validate at the data shape level (a field-name ->
         * message map) but doesn't require it — helpers accept
         * the bare table and degrade cleanly when nil. */
        .name = "hull/web/htmx/form",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0,
        .deps = {"hull/web/htmx", 0},
    },
    {
        /* HTMX inline-edit widget — canonical click-to-edit
         * pattern. Two server helpers (cell + editor) emit the
         * htmx attributes for the GET→PATCH round trip. Two
         * server endpoints per editable field; pure hypermedia
         * mode switch (no client JS for that). A tiny shipped
         * JS handles input focus after the editor swaps in
         * (autofocus attr doesn't fire on dynamic inserts).
         * Esc-to-cancel is wired via hx-trigger on the Cancel
         * button. Structural CSS only.
         *
         * Pure HTML-string builder; no I/O. */
        .name = "hull/web/htmx/inline-edit",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0,
        .deps = {"hull/web/htmx", 0},
    },
    {
        /* HTMX pagination widget — htmx-attributed nav rendered
         * over the existing hull/web/pagination@1 page-math.
         * Replaces the ~30-line _pagination.html partial every
         * hypermedia app writes by hand. Pure HTML-string builder;
         * delegates page-math to the base module. */
        .name = "hull/web/htmx/pagination",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0,
        .deps = {"hull/web/htmx", "hull/web/pagination", 0},
    },
    {
        /* HTMX search widget — server-side helpers that emit
         * htmx attributes for a debounced search input and the
         * a11y attributes for the results container. Pure
         * htmx-native; no shipped client JS or CSS — the whole
         * interaction is hx-trigger="input changed delay:..."
         * + server-rendered results swapped into the container.
         *
         * Pure attribute-string builder; no I/O. */
        .name = "hull/web/htmx/search",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0,
        .deps = {"hull/web/htmx", 0},
    },
    {
        /* HTMX sort widget — server-side helpers for sortable
         * column headers driven by ?sort=col[:asc|desc]. Two
         * functions: parse(req, opts) reads + allowlist-validates
         * the param; header_attrs(col, current, opts) returns the
         * htmx attrs + class for an app-owned <th>. Toggle
         * cycles asc -> desc -> asc on subsequent clicks.
         *
         * Pure attribute-string builder; no I/O. */
        .name = "hull/web/htmx/sort",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0,
        .deps = {"hull/web/htmx", 0},
    },
    {
        /* HTMX table widget — schema-driven <table> renderer that
         * composes sort + inline-edit per column. The grid pattern
         * of every admin / data UI in one helper. Apps wire search
         * input + pagination separately. Pure HTML-string builder;
         * no I/O. */
        .name = "hull/web/htmx/table",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0,
        .deps = {"hull/web/htmx", "hull/web/htmx/sort",
                 "hull/web/htmx/inline-edit", 0},
    },
    {
        /* HTMX toast widget — server-side helper that emits the
         * HX-Trigger header consumed by the shipped client JS at
         * /static/hull/htmx/toast/toast.js. Structural CSS only;
         * apps style per-level appearance via [data-level] attrs.
         *
         * First widget in the §1.5.g htmx tier. Pure header-setter,
         * no I/O. Wraps htmx.trigger; needs json for the payload
         * encoder hull.web.htmx itself uses internally. */
        .name = "hull/web/htmx/toast",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0,
        .deps = {"hull/web/htmx", "hull/json", 0},
    },

    /* ── Web middleware ──────────────────────────────────────────────
     * Every middleware consumes KlRequest/KlResponse and registers via
     * app.use() / app.use_post(), all of which need Keel's HTTP server.
     * They share HL_MOD_CAP_HTTP_SERVER — apps targeting an
     * HL_ENABLE_HTTP_SERVER=0 build can't declare any of them. */
    {
        /* Append-only sign-in / auth event log + per-device
         * grouping. Composable with hull/web/auth-flows (which
         * emits events when opts.sign_in_log is wired), with
         * hull/web/middleware/session, or standalone for app-
         * recorded events. Owns _hull_audit_log. */
        .name = "hull/web/middleware/audit-log",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_DB,
        /* hull/timers powers the auto-daily cleanup scheduled by
         * audit_log.init() (opt-out via opts.cleanup = false). Apps
         * that don't want the schedule still need it admitted because
         * the module-conditional decoration that adds app.daily fires
         * at app startup, not lazily.
         *
         * hull/log is used to surface init-time cleanup failures and
         * the CLI-flavor "app.daily not available" warning. */
        .deps = {"hull/db", "hull/crypto", "hull/time", "hull/json",
                 "hull/timers", "hull/log", 0},
    },
    {
        .name = "hull/web/middleware/auth",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        /* Wraps cookie + jwt + session: session-cookie or bearer-token auth. */
        .deps = {"hull/http-server", "hull/db", "hull/crypto", "hull/web/cookie", "hull/jwt",
                 "hull/web/middleware/session", 0},
    },
    {
        .name = "hull/web/middleware/cors",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = HL_MOD_CAP_HTTP_SERVER, .deps = {"hull/http-server", 0},
    },
    {
        /* Content-Security-Policy with per-request nonce. Pure
         * header-setter middleware; reads no state. Depends on
         * hull/crypto for the nonce RNG + base64url encoder. The
         * htmx() profile is Pico-compatible; strict() is the no-
         * inline-style variant. Apps that need a different shape
         * compose their own using csp.nonce(). */
        .name = "hull/web/middleware/csp",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        .deps = {"hull/http-server", "hull/crypto", 0},
    },
    {
        .name = "hull/web/middleware/csrf",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        /* HMAC token (crypto + time). hull/web/cookie for the session-id
         * cookie fallback: both runtimes parse it now (previously only JS,
         * which broke a JS app that declared only csrf - the cookie import
         * wasn't auto-admitted). */
        .deps = {"hull/http-server", "hull/crypto", "hull/time",
                 "hull/web/cookie", 0},
    },
    {
        .name = "hull/web/middleware/etag",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        /* SHA-256 of body + json encode for the response payload. */
        .deps = {"hull/http-server", "hull/crypto", "hull/json", 0},
    },
    {
        .name = "hull/web/middleware/health",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        /* health pings the DB and inspects server stats; both are hard
         * deps even when db_check is disabled at runtime, because the
         * required modules need to be importable. */
        .deps = {"hull/db", "hull/http-server", "hull/time", 0},
    },
    {
        .name = "hull/web/middleware/idempotency",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        /* Key fingerprint (crypto), JSON payload caching, TTL clock. */
        .deps = {"hull/http-server", "hull/db", "hull/crypto", "hull/json", "hull/time", 0},
    },
    {
        .name = "hull/web/middleware/inbox",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        .deps = {"hull/http-server", "hull/db", "hull/time", 0},
    },
    {
        .name = "hull/web/middleware/logger",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        .deps = {"hull/http-server", "hull/log", 0},
    },
    {
        /* OIDC / OAuth 2.0 Authorization Code + PKCE. Owns three
         * routes: /auth/:provider/{login,callback}, /auth/logout.
         * State envelope is HMAC-signed (crypto), encoded as JSON,
         * timestamp-bounded (time). Token exchange + JWKS fetch use
         * http-client. ID token verify uses jwt + crypto.verify
         * (asym) + crypto.x509_pubkey_pem for the x5c bridge.
         * Login failures are logged via hull/log. */
        .name = "hull/web/middleware/oauth",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER | HL_MOD_CAP_HTTP_CLIENT,
        /* hull/log is implicitly available (log.X works without
         * declaration); not in deps because we're at the 8-dep cap.
         * Also relies on hull/time at runtime via jwt.verify's exp
         * check but jwt itself declares hull/time, so the dep
         * transitively resolves through hull/jwt. */
        /* hull/json is dropped from direct deps — hull/crypto/envelope
         * (used by state_sign / state_verify) brings it transitively
         * via the resolver, freeing the slot needed for envelope
         * without losing the implicit `local json = require(...)`
         * inside this module. */
        .deps = {"hull/http-server", "hull/http-client", "hull/crypto",
                 "hull/crypto/envelope", "hull/web/cookie", "hull/jwt",
                 "hull/time", 0},
    },
    {
        .name = "hull/web/middleware/outbox",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        /* Webhook payload as JSON, retry backoff via time. */
        .deps = {"hull/http-server", "hull/db", "hull/http-client", "hull/json", "hull/time", 0},
    },
    {
        .name = "hull/web/middleware/ratelimit",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        /* hull/cache backs the per-key bucket store (LRU-bounded). */
        .deps = {"hull/http-server", "hull/time", "hull/cache", 0},
    },
    {
        .name = "hull/web/middleware/rbac",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        .deps = {"hull/http-server", "hull/db", 0},
    },
    {
        .name = "hull/web/middleware/session",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        /* Session payload is JSON; ID is crypto.random; expiry needs
         * time. hull/timers backs the auto-daily cleanup (round-7
         * item 3, mirroring audit-log). hull/log surfaces the
         * cleanup-failed warnings. */
        .deps = {"hull/http-server", "hull/db", "hull/crypto",
                 "hull/json", "hull/time", "hull/timers",
                 "hull/log", 0},
    },
    {
        /* RFC 6238 TOTP (2FA). HMAC-SHA1 via crypto.hmac_sha1 (the
         * vtable-backed cap added earlier in this branch); secret +
         * recovery-code storage in db; enrollment QR via hull/qrcode.
         * Standalone middleware that gates routes when the session's
         * pending_2fa flag is set. */
        .name = "hull/web/middleware/totp",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER | HL_MOD_CAP_DB,
        /* Round-8 LOW-13: hull/timers + hull/log power the auto-
         * daily prune of orphaned pending-enrollment rows. Module-
         * conditional decoration in hull/timers adds app.daily,
         * which init() schedules unless cleanup = false is passed. */
        .deps = {"hull/http-server", "hull/db", "hull/crypto",
                 "hull/qrcode", "hull/time",
                 "hull/timers", "hull/log", 0},
    },
    {
        .name = "hull/web/middleware/transaction",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        .deps = {"hull/http-server", "hull/db", 0},
    },
    {
        /* Server-side offset-based pagination helper. Pure: reads
         * req.query, builds offset/limit + a nav structure with
         * windowed links and ellipses. Useful for REST endpoints,
         * server-rendered pages, and HTMX fragment swaps alike. */
        .name = "hull/web/pagination",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        /* k-anonymity pwned-password check via the HIBP range
         * API. Hashes the password (SHA-1) client-side, sends
         * only the first 5 hex chars over the wire, scans the
         * returned suffix list locally. Apps that enable
         * check_pwned_passwords in hull/web/auth-flows.init()
         * must add api.pwnedpasswords.com to manifest.hosts.
         * Fail-open on HIBP outage (logged via hull/log). */
        .name = "hull/web/pwned",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_CLIENT | HL_MOD_CAP_HOSTS,
        .deps = {"hull/http-client", "hull/log", "hull/crypto",
                 "hull/time", 0},
    },

    /* ── Web real-time + WebSocket ──────────────────────────────────── */
    {
        /* Server-Sent Events — declaring this decorates the `app`
         * intrinsic with app.sse(path, handler). Server-push protocol
         * only; no client API in Hull. */
        .name = "hull/web/sse",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER, .deps = {0},
    },
    {
        /* WebSocket client — outbound ws.connect(). Requires a
         * non-empty hosts allowlist (capability gate enforced at
         * call time inside ws.connect). */
        .name = "hull/web/ws-client",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HOSTS | HL_MOD_CAP_HTTP_SERVER, .deps = {0},
    },
    {
        /* WebSocket server — declaring this decorates the `app`
         * intrinsic with app.ws(path, handlers), and exposes
         * ws.broadcast(path, msg) and ws.connections(path) helpers
         * for managing connected clients. Split from the old
         * hull/ws@1 which mixed server and client. */
        .name = "hull/web/ws-server",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER, .deps = {0},
    },

    {
        .name = "hull/worker",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {0},
    },
};

#define REGISTRY_COUNT (sizeof(REGISTRY) / sizeof(REGISTRY[0]))

/* The module resolver indexes a fixed-width bitset (HlResolvedModuleSet,
 * HL_MODULE_BITSET_WORDS*64 bits) by registry position, and
 * hl_module_set_required_caps walks the whole registry through that set. If the
 * registry ever outgrew the bitset, indices past the width would be dropped:
 * gating fails closed (modules never admitted) but the required-caps count
 * would UNDER-count, which feeds hl_build_flavor_auto — a fail-OPEN on flavor
 * selection. Catch that at compile time instead. (C-audit 2026-07, Low-1.) */
_Static_assert(REGISTRY_COUNT <= HL_MODULE_BITSET_WORDS * 64,
               "module registry outgrew the resolved-set bitset width; "
               "raise HL_MODULE_BITSET_WORDS in include/hull/limits/core.h");

/* ── Binary search by name ─────────────────────────────────────────── */

const HlModuleSpec *hl_module_registry_find(const char *name)
{
    if (!name) return NULL;

    size_t lo = 0, hi = REGISTRY_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(name, REGISTRY[mid].name);
        if (cmp == 0) return &REGISTRY[mid];
        if (cmp < 0)  hi = mid;
        else          lo = mid + 1;
    }
    return NULL;
}

const HlModuleSpec *hl_module_registry_find_short(const char *short_name)
{
    if (!short_name) return NULL;

    /* Names that already start with the "hull/" canonical prefix are
     * looked up as-is. Anything else — including names with embedded
     * slashes like "web/middleware/session" — is treated as a short
     * alias inside the hull/ namespace and gets the prefix prepended.
     *
     * Once third-party vendors are supported the rule will need to be
     * "starts with any recognized vendor prefix"; for v1, hull/ is the
     * only first-party namespace. */
    if (strncmp(short_name, "hull/", 5) == 0)
        return hl_module_registry_find(short_name);

    char buf[HL_MODULE_NAME_MAX];
    size_t n = strlen(short_name);
    if (n + 5 + 1 > sizeof(buf)) return NULL;
    memcpy(buf, "hull/", 5);
    memcpy(buf + 5, short_name, n + 1);
    return hl_module_registry_find(buf);
}

const HlModuleSpec *hl_module_registry_find_runtime(const char *runtime_name,
                                                     char separator)
{
    if (!runtime_name) return NULL;
    if (separator == '/' || separator == '\0')
        return hl_module_registry_find(runtime_name);

    /* Convert in-place into a local buffer: every `separator` becomes
     * '/'. Bail if the name overflows our small canonical-name budget. */
    char buf[HL_MODULE_NAME_MAX];
    size_t n = strlen(runtime_name);
    if (n + 1 > sizeof(buf)) return NULL;
    for (size_t i = 0; i < n; i++)
        buf[i] = (runtime_name[i] == separator) ? '/' : runtime_name[i];
    buf[n] = '\0';
    return hl_module_registry_find(buf);
}

const HlModuleSpec *hl_module_registry_all(size_t *out_total)
{
    if (out_total) *out_total = REGISTRY_COUNT;
    return REGISTRY;
}

int hl_module_registry_index(const HlModuleSpec *spec)
{
    if (!spec) return -1;
    if (spec < &REGISTRY[0] || spec >= &REGISTRY[REGISTRY_COUNT]) return -1;
    return (int)(spec - &REGISTRY[0]);
}

size_t hl_module_registry_count(void)
{
    return REGISTRY_COUNT;
}

void hl_module_registry_format_deps(const HlModuleSpec *spec,
                                     char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!spec) return;

    size_t pos = 0;
    int first = 1;
    for (int i = 0; i < HL_MODULE_MAX_DEPS && spec->deps[i]; i++) {
        const char *name = spec->deps[i];
        /* Strip "hull/" prefix in the short form: easier to type back. */
        if (strncmp(name, "hull/", 5) == 0) name += 5;
        const char *sep = first ? "" : ", ";
        int n = snprintf(out + pos, cap - pos, "%s%s", sep, name);
        if (n < 0 || (size_t)n >= cap - pos) {
            /* Out of room — leave whatever fit, ensure NUL. */
            out[cap - 1] = '\0';
            return;
        }
        pos += (size_t)n;
        first = 0;
    }
}

/* ── Suggestion (Levenshtein) ───────────────────────────────────────── */

/* Longest registry name today is "web/middleware/idempotency" (26 chars) —
 * pad generously so future entries don't silently bust the bound. */
#define SUGGEST_MAX_LEN 64

/* Two-row Levenshtein. O(m*n) time, O(min(m,n)) extra space. */
static int levenshtein(const char *a, size_t alen,
                       const char *b, size_t blen)
{
    if (alen == 0) return (int)blen;
    if (blen == 0) return (int)alen;

    /* Swap so b is the shorter side — bounds the row to SUGGEST_MAX_LEN+1. */
    if (blen > alen) {
        const char *t = a; a = b; b = t;
        size_t tl = alen; alen = blen; blen = tl;
    }
    if (blen > SUGGEST_MAX_LEN) return (int)alen;  /* would overflow row */

    int prev[SUGGEST_MAX_LEN + 1];
    int curr[SUGGEST_MAX_LEN + 1];

    for (size_t j = 0; j <= blen; j++) prev[j] = (int)j;

    for (size_t i = 1; i <= alen; i++) {
        curr[0] = (int)i;
        for (size_t j = 1; j <= blen; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del  = prev[j] + 1;
            int ins  = curr[j - 1] + 1;
            int sub  = prev[j - 1] + cost;
            int m    = del < ins ? del : ins;
            curr[j]  = m < sub ? m : sub;
        }
        for (size_t j = 0; j <= blen; j++) prev[j] = curr[j];
    }

    return prev[blen];
}

/* Normalize an input into a short form: strip "hull/" and "web/"
 * prefixes so the v0.2.0 namespace shift doesn't make typos of old
 * short names (e.g. "middleware/sesssion") fall out of suggestion
 * range. Lowercase nothing — registry names are all lowercase, so
 * we match case-sensitively. Output buffer must be SUGGEST_MAX_LEN+1
 * bytes. Returns the strlen of out, or SIZE_MAX if input is too
 * long / NULL. */
static size_t normalize_input(const char *input, char *out)
{
    if (!input) return (size_t)-1;
    if (strncmp(input, "hull/", 5) == 0) input += 5;
    if (strncmp(input, "web/", 4) == 0)  input += 4;
    size_t len = strlen(input);
    if (len > SUGGEST_MAX_LEN) return (size_t)-1;
    memcpy(out, input, len);
    out[len] = '\0';
    return len;
}

const HlModuleSpec *hl_module_registry_suggest(const char *input)
{
    char norm[SUGGEST_MAX_LEN + 1];
    size_t in_len = normalize_input(input, norm);
    if (in_len == (size_t)-1 || in_len == 0) return NULL;

    /* Length-scaled threshold:
     *   ≤ 4 chars → 1 edit
     *   5–8       → 2 edits
     *   ≥ 9       → 3 edits
     * Avoids matching e.g. "fs" → "ws" on a single edit when the user
     * really did mean "fs"; and lets longer names ("middleware/sesssion")
     * tolerate the typo budget they need. */
    int threshold;
    if      (in_len <= 4) threshold = 1;
    else if (in_len <= 8) threshold = 2;
    else                  threshold = 3;

    int best_dist = threshold + 1;
    const HlModuleSpec *best = NULL;

    for (size_t i = 0; i < REGISTRY_COUNT; i++) {
        const char *cand = REGISTRY[i].name;
        if (strncmp(cand, "hull/", 5) == 0) cand += 5;
        if (strncmp(cand, "web/", 4) == 0)  cand += 4;
        size_t clen = strlen(cand);

        /* Cheap reject: if the length difference alone exceeds the
         * threshold, no single-substitution/insert/delete budget can
         * bridge the gap. */
        size_t diff = (clen > in_len) ? (clen - in_len) : (in_len - clen);
        if ((int)diff > threshold) continue;

        int d = levenshtein(norm, in_len, cand, clen);
        if (d < best_dist) {
            best_dist = d;
            best = &REGISTRY[i];
            if (d == 0) break;  /* exact (shouldn't happen — caller checks) */
        }
    }

    return best;
}
