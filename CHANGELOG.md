# Changelog

All notable changes to Hull are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); Hull adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html) on the
public surface (`hull` CLI flags, embedded stdlib API, manifest schema,
release-artifact layout).

## [Unreleased]

### Security / build flavors

- **Build-time re-verify of installed per-flavor platform libs (closes the
  install-to-build TOCTOU).** `hull platform install` now caches the signed
  manifest (`hull.sha256` + `.sig`) in `~/.hull/platform/`, and `hull build
  --flavor` re-verifies any lib it pulls from that cache before linking, fully
  offline, via `hl_release_io_verify_local_asset` (`tool.platform_verify`): the
  manifest signature is re-checked against the EMBEDDED release pubkey (the
  trust anchor, not the writable cache dir), then the lib's SHA-256 is matched
  to the signed manifest. A tampered cached lib or a swapped/absent manifest
  fails the build with a reinstall hint; a lib the developer built locally
  (`make platform-<flavor>`) is trusted as-is. Verified end-to-end against the
  v0.6.0 release (valid lib links, tampered lib rejected).
- **`crypto.constant_time_eq` / `crypto.constantTimeEq`** (Lua / JS): a
  constant-time byte-string equality primitive compared in C. `hull/jwt`
  (HS256) and `hull/web/middleware/csrf` now route their HMAC-signature compare
  through it instead of an in-interpreter loop, matching `hull/crypto/envelope`
  and removing interpreter-timing variance. Functionally identical (both were
  already non-early-exit); this is a defense-in-depth consistency change.
- **Fix:** the JS `csrf` middleware's session-cookie fallback defaulted to
  `"hull.sid"` instead of the session/auth middleware's `"hull_session"`, so
  the fallback could never find the cookie. Aligned to `"hull_session"`.

## [0.6.0] — 2026-07-10

### Build flavors

- **Signed published cosmo per-flavor platform libs + cosmo `hull platform
  install`.** The release pipeline now builds each cosmo flavor lib
  (server-only / client-only / pure-compute) on its **own fresh runner** and
  publishes the dual-arch pair
  `libhull_platform-<flavor>.{x86_64,aarch64}-cosmo.a`, covered by the
  Ed25519-signed `hull.sha256`. `hull platform install <flavor>` on a cosmo
  hull fetches + verifies the pair into `~/.hull/platform/`, and `hull build
  --flavor` finds it there (previously cosmo flavor libs were build-from-source
  only). Isolated producer: a second cosmo platform build in one CI job
  corrupts the dynamic loader (system cc/sh can't mmap libc; not a resource
  limit), so each flavor builds alone. `platform.c` was already cosmo-ready;
  `build.lua`'s cosmo `--flavor` branch gained the `~/.hull/platform` lookup.

## [0.5.0] — 2026-07-10

### Build flavors

- **All four HTTP build flavors now link clean and are CI-covered.**
  Fixed the `HL_ENABLE_HTTP_CLIENT=0` (server-only) and `HL_ENABLE_HTTP=0`
  (pure-compute) link breaks and added both to the `flavors` matrix in
  `.github/workflows/ci.yml` (each builds, runs `hull version`, and runs
  an `app.main` exit-code smoke). Server-only: `release_io.c`'s offline
  helpers (used unconditionally by `hull verify-self`) are no longer gated
  out alongside the HTTPS fetch path. Pure-compute: with both HTTP halves
  off, mbedTLS is dropped entirely, so Hull's own hashing now runs on
  in-tree implementations (hand-rolled SHA-1, the cap layer's
  self-contained SHA-256, TweetNaCl SHA-512, and a portable HMAC backend)
  instead of `mbedtls_sha*` / `mbedtls_md_hmac`. Default and HTTP-on builds
  are byte-for-byte unchanged.
- Documented the pure-compute flavor in `CLAUDE.md` ("Pure-compute builds")
  with the mbedtls-free hashing path, the contributor invariant (core code
  must hash via the cap layer, never mbedTLS directly), and refreshed the
  flavor binary-size table to measured values.
- **`hull build --flavor=full|server-only|client-only|pure-compute`** builds
  a flavored app binary from a full hull, linking a locally-built
  `libhull_platform-<flavor>.a` (`make platform-<flavor>`) and validating the
  app's manifest against the TARGET flavor's caps (a module needing a dropped
  subsystem is rejected at build time). **`--flavor=auto`** infers the
  minimal flavor from the declared modules. Design: `docs/build_flavors.md`.
  Along the way: fixed the no-keel platform archive (missing `sh_seal_arena`),
  a `tool.modules_resolve` parser that ignored the canonical array module
  form, and `serve_cli.c` not detecting a built binary's embedded app.
- **`hull platform install <flavor>` + signed published per-flavor platform
  libs.** Releases now publish `libhull_platform-<flavor>-<arch>.a` for the
  three native arches, covered by the Ed25519-signed `hull.sha256`. `hull
  platform install` fetches + verifies them into `~/.hull/platform/`, and
  `hull build --flavor` uses the cached lib, so end users no longer have to
  build the platform lib from source. Same trust chain as `hull tools
  install`, factored into one shared `hl_release_io_fetch_verified_manifest`.
  Cosmo flavor libs stay build-from-source for now (`make
  platform-cosmo-<flavor>`).

## [0.4.0] — 2026-06-17

HTMX widget tier completed (8 widgets total: toast, confirm, form,
search, inline-edit, sort, pagination, table) and shipped with
browser-driven Playwright E2E coverage. Three production-affecting
bug fixes in the `hull build` + sandbox pipeline: any app declaring
`manifest.fs.write` was silently broken when shipped via standalone
binary, and `static/vendor/*` assets weren't getting embedded.
Confirm widget fixed twice (open only on `hx-confirm`; resume via
`htmx.ajax` to work around htmx 2.0.9's deferred-`issueRequest`
no-op). 31 commits since v0.3.0.

### Added — HTMX widget tier (§1.5.g)

- **`hull/web/htmx/toast@1`** (Lua + JS). Server-fired toasts via
  `HX-Trigger` header — `toast.success(res, msg)` /
  `toast.info(res, msg)` / `toast.warning` / `toast.error` /
  `toast.show(res, msg, opts)`. Auto-dismiss with configurable
  duration, deduplication by id (server-controllable), single-
  instance container appended lazily on first toast. ~150 LOC of
  structural CSS + client JS.
- **`hull/web/htmx/confirm@1`** (Lua + JS). `confirm.attrs(question,
  opts?)` produces `hx-confirm` + `data-confirm-*` attribute string
  for splicing into delete buttons. Replaces the browser's
  `window.confirm()` with a styled native `<dialog>`. ~140 LOC.
  Single-instance dialog (second prompt overwrites first), Escape
  + backdrop click both map to cancel.
- **`hull/web/htmx/form@1`** (Lua + JS). Validation-error rendering
  + auto loading state. `form.errors(validation_result) -> errs`,
  `form.field_error(errs, "name") -> "<small class=...>" or ""`,
  `form.field_attrs(errs, "name") -> "aria-invalid=true ..." or ""`.
  Client JS toggles `aria-busy="true"` + `disabled` on submit
  buttons during in-flight htmx requests; restores on
  `htmx:afterRequest`. ~75 LOC.
- **`hull/web/htmx/search@1`** (Lua + JS). `search.input_attrs(opts)`
  produces a debounced `<input>` attribute string
  (`hx-get` + `hx-trigger="input changed delay:Nms"` +
  `hx-target` + optional `hx-push-url`).
  `search.results_attrs(opts)` for the result container.
- **`hull/web/htmx/inline-edit@1`** (Lua + JS). Click-to-edit single
  fields: `inline_edit.cell(opts) -> read view`,
  `inline_edit.editor(opts) -> edit form`. Two routes + one PATCH
  pattern; ~30 LOC of server helpers.
- **`hull/web/htmx/sort@1`** (Lua + JS). Sortable column headers
  driven by `?sort=col[:asc|desc]`. `sort.parse(req, opts)` reads
  + allowlist-validates the query param;
  `sort.header_attrs(col, current, opts)` returns the htmx attrs
  for a `<th>`. Configurable `opts.swap` for the swap target.
  Client JS (sort.js) adds Enter / Space keyboard activation
  without relying on htmx trigger filters (which need
  `unsafe-eval` and conflict with the `csp = "htmx"` preset).
  Native `<th>` semantics + `tabindex="0"` + `aria-sort` — no
  `role="button"` override (would invalidate `aria-sort`).
  ~30 LOC.
- **`hull/web/htmx/pagination@1`** (Lua + JS). `pagination.nav(total,
  opts)` wraps existing `hull/web/pagination@1` page-math and
  emits htmx-attributed nav HTML. Replaces hand-written nav
  partials.
- **`hull/web/htmx/table@1`** (Lua + JS). `table.render(rows,
  schema, opts)` composes sort + inline-edit per column via a
  schema array. Custom `render(value, row)` callback for cells.
  ~155 LOC; the grid pattern of every admin / data UI in one
  helper.
- **CSP preset `csp = "htmx"`.** Manifest sugar — expands at
  startup to a known-good policy for SSR htmx apps with stdlib
  JS from `/static/`: `default-src 'none'; script-src 'self';
  style-src 'self' 'unsafe-inline'; img-src 'self' data:;
  connect-src 'self'; form-action 'self'; frame-ancestors 'none'`.
  Cheaper than the nonce-based `hull/web/middleware/csp@1` for
  apps that don't need per-request nonces.
- **Stdlib static + template VFS.** `stdlib/static/hull/<module>/*`
  files are embedded in the platform library and served at
  `/static/hull/<module>/*`. Widget CSS and client JS ship through
  this path so apps just add one `<link>` and one `<script>` per
  widget — no vendoring, no per-app bundling.
- **`htmx.trigger(res, name, payload, opts?)`** with `opts.timing`
  to pick `HX-Trigger` / `HX-Trigger-After-Settle` /
  `HX-Trigger-After-Swap`. Replaces the older
  `trigger_after_settle` / `trigger_after_swap` variants.
- **`docs/htmx_widgets.md`** — long-form usage guide for the eight
  widgets, the handler-pre-render pattern (templates can't call
  functions; widget helpers run once in the handler and the
  resulting strings flow into template data), setup checklist
  with htmx-config requirements.
- **`examples/htmx_widgets_register`** — exercises every widget in
  one CRUD page (asset register). Doubles as the UX-test surface
  and the playwright suite's primary target.

### Added — Browser E2E suite (Playwright + axe-core)

- **`tests/e2e_htmx_playwright.sh`** + **`tests/e2e_htmx_playwright.mjs`**.
  Headless Chromium driven by Playwright; tests
  `examples/htmx_widgets_register` (all 8 widgets) and
  `examples/hypermedia_photos` (both Lua AND JS runtimes —
  runtime-parity gaps surface as concrete FAIL lines). Catches
  things curl can't: CSS actually applies, htmx swaps fire,
  widget JS executes under the `csp = "htmx"` strict preset,
  sort widget Enter / Space keyboard activation works, CRUD
  round-trip with CSRF + session, confirm-yes-deletes-row.
- **Two modes.** `make e2e-htmx-playwright` (dev) launches
  `hull <app.lua>` so files come off disk;
  `make e2e-htmx-playwright-build` (build) runs `hull build`
  first and launches the standalone binary, exercising the
  embedded-VFS code path. 33 assertions in either mode
  (widgets 15 + photos[lua] 9 + photos[js] 9). Caught
  the `static/vendor/*` and `fs.write` resolution bugs.
- **`@axe-core/playwright` WCAG scan** on initial page load,
  per suite. FAILs on `critical` / `serious` violations; logs
  `moderate` / `minor` as informational. Caught three real
  bugs (sort widget `aria-allowed-attr`, two `color-contrast`
  cases, one `heading-order`) — all fixed in stdlib + the
  example app.
- **Failure artifacts.** Each suite writes a Playwright
  `trace.zip` (per-action DOM snapshots, network log, console,
  screenshots) and a `final.png` to `build/playwright-artifacts/`
  on failure only (no spurious uploads on success). CI uploads
  via `actions/upload-artifact@v4` gated on `if: failure()`.
  Local runs print the `npx playwright show-trace` invocation.
- **CI matrix.** New `htmx-browser` job runs both dev and build
  modes on Linux + macOS. Playwright + Chromium install cached
  via `actions/cache@v4`; skips cleanly when node / npm absent.

### Added — Build / runtime infrastructure

- **`hl_tool_find_files_ex(..., include_vendor)`** in the C tool
  cap. `hl_tool_find_files` kept as a back-compat wrapper. Lua
  binding accepts `opts.include_vendor = true` (default false)
  so static-asset walks no longer silently drop `static/vendor/*`.
- **`sandbox_resolve_manifest_path(app_dir, relpath, ...)`** —
  shared cross-platform helper that joins app_dir + relpath,
  pre-mkdirs, then `realpath()`s. Used by both seatbelt SBPL
  build (macOS) and the unveil setup (Linux / OpenBSD / Cosmo)
  so the sandbox allows the SAME absolute directory the
  capability layer (`hl_cap_blob_init`, `hl_cap_fs_validate`)
  resolves to.
- **`hl_serve_resolve_entry` canonicalizes `app_dir` to an
  absolute path** when the entry point has no slash (the standard
  case for `hull build` standalone binaries). Downstream
  `HlFsConfig.base_dir` is now always absolute, which
  `hl_blob_store_open` requires.

### Fixed

- **`hull build` silently dropped `static/vendor/*` assets.**
  `hl_tool_find_files`'s recursive walker hardcoded a skip on
  any directory named `vendor` — correct for source-file walks
  but wrong for `static/`, where apps put vendored CSS/JS
  (pico, htmx, etc.). Standalone binaries returned 404 for
  every `/static/vendor/*` request; pages rendered unstyled.
  `hull dev` was unaffected (reads from disk, bypasses the
  recursive walk). Caught by the new MODE=build playwright pass.
- **Standalone binaries with `manifest.fs.write` failed
  `blob.init`** (and any other capability that resolves paths
  via `HlFsConfig.base_dir`). The sandbox processed
  `policy->fs_write[i]` (e.g. `"data/"`) as cwd-relative for
  unveil / realpath / seatbelt, but `hl_cap_blob_init` resolved
  the same name against `app_dir`. When `cwd != app_dir` (always
  true in CI / production), the two interpretations disagreed
  and the app's writes were blocked at syscall time, with a
  confusing "blob.init: failed (check fs.write declares
  'data/blobs')" error that pointed at the wrong layer. Affects
  every app that declares `manifest.fs.write` — including
  `examples/hypermedia_photos` — when shipped via `hull build`.
  Fixed by resolving manifest paths against `app_dir` in the
  sandbox setup (both macOS seatbelt and Linux unveil paths).
- **Confirm widget popped its dialog on every htmx request.**
  `handleConfirm` did not check whether the triggering element
  actually had `hx-confirm`, so any htmx swap (search keystroke,
  form submit, sort header click) triggered the delete-confirm
  dialog. Early-return when `evt.detail.question` is empty
  (htmx sets it only when `hx-confirm` is present). Caught by
  the playwright suite while typing into the search input.
- **Confirm widget couldn't resume hx-delete requests.** In
  htmx 2.0.9, `evt.detail.issueRequest()` is a silent no-op
  for verbs like `hx-delete` on a standalone `<button>` — both
  sync inside the event AND deferred after the dialog closes.
  Verified by capturing the event externally and calling
  `issueRequest()` at various points: no path makes it fire.
  Workaround: `close()` now uses `htmx.ajax(verb, path, {source,
  target})` with the same config the original click would have
  produced, temporarily stripping `hx-confirm` from the source
  element so the resumed request doesn't re-trigger the dialog.
- **Sort widget broke keyboard accessibility under
  `csp = "htmx"`.** Used `hx-trigger="click, keyup[key=='Enter']
  from:this, keyup[key==' '] from:this"`. The `[expr]` filter
  syntax goes through `new Function()` eval, which the strict
  preset rejects (`allowEval:false`). htmx then dropped the
  entire trigger string — so Enter / Space AND the bare `click`
  both stopped working. Moved keyboard activation to a new
  `sort.js` client file that listens for keydown Enter / Space
  on `.hull-sort-header` and dispatches a real click. The
  `hx-trigger` collapses to just `click`.
- **Sort widget hard-coded `hx-swap="outerHTML"`.** When
  `opts.target` was a wrapping container (e.g. `#grid` holding
  table + nav + actions), outerHTML replaced the container
  itself with the response, destroying the target for any
  subsequent request that named `#grid`. Made `hx-swap`
  configurable via `opts.swap`; omitted by default so htmx
  uses its own default (`innerHTML`), which is correct for
  the common container-wrapping pattern.
- **Sort widget invalidated `aria-sort`.** Put `role="button"`
  on `<th>`, which overrode the implicit `columnheader` role.
  `aria-sort` is only valid on `columnheader` / `rowheader` /
  `gridcell`, so axe-core flagged it as `aria-allowed-attr`
  critical. Worse, screen readers said "Name, button" instead
  of "Name, column header, sorted ascending". Dropped
  `role="button"`; `tabindex="0"` + `sort.js` keyboard handler
  preserves activation.
- **`examples/hypermedia_photos/app.js` was never migrated to
  the §1.5.g widget tier.** Lua sibling had been; JS rendered
  `<input id="search-input">` with no `hx-*` attrs and delete
  buttons without `hx-confirm`. Mirrored the Lua wiring
  line-for-line: imports, manifest entries, pre-rendered
  attribute constants, plumbing through `rowData()` /
  `feedData()` / form re-render path.
- **A11y polish in `examples/htmx_widgets_register`** (all
  axe-surfaced): explicit text color on table headers so contrast
  stays WCAG-AA in both light and dark Pico; `.badge-retired`
  bumped from grey-500/grey-100 (~3.4:1, fail) to grey-700/grey-200
  (~7.5:1, AAA); `_form.html` `<h3>` → `<h2>` so heading order
  doesn't skip levels.

### Changed

- **`htmx.trigger(res, name, payload, opts?)`** with
  `opts.timing` is the canonical surface for firing client
  events. The earlier
  `trigger_after_settle` / `trigger_after_swap` /
  `triggerAfterSettle` / `triggerAfterSwap` variants are
  removed. Migration is mechanical:
  `htmx.trigger_after_settle(res, "foo", {x=1})` →
  `htmx.trigger(res, "foo", {x=1}, {timing="after-settle"})`.

## [0.3.0] — 2026-06-15

Production-grade authentication & authorization stack, streaming multipart upload, content-addressed blob storage with runtime caches, and asymmetric crypto. The auth stack alone went through **13 iterative security-review rounds** (rounds 1–13); the final round returned zero findings across three independent reviewers, marking convergence. Eight new stdlib modules ship in this release: `hull/web/auth-flows@1`, `hull/web/middleware/totp@1`, `hull/web/middleware/oauth@1`, `hull/web/middleware/audit-log@1`, `hull/web/auth-health@1`, `hull/web/pwned@1`, `hull/qrcode@1`, `hull/attachment@1`, `hull/blob@1`, `hull/mime@1`. (132 commits since v0.2.0.)

### Added — Authentication & authorization stack

- **`hull/web/auth-flows@1`** (Lua + JS). End-to-end auth flows on top of HMAC-signed envelopes: registration, email verification, login, password reset, magic link, email change (with revoke from old address), optional TOTP 2FA. Owns `_hull_auth_used_tokens` (single-use replay protection), `_hull_auth_pending_email_changes`, `_hull_auth_login_attempts` (per-user lockout). Pluggable user-storage callbacks; turnkey `standard_users` adapter for the common schema. Per-recipient email-storm rate limit, `public_origin` / `trusted_hosts` URL-origin gate (host-header injection class closed), strict user-sanitize allowlist with optional `user_sanitize(user)` hook. Settled API after 13 audit rounds.
- **`hull/web/middleware/totp@1`** (Lua + JS). RFC 6238 TOTP. Enroll → confirm → verify lifecycle with dual-row staging (pending vs confirmed) so re-enrollment doesn't lock out users who lost the new recovery codes mid-flow. Recovery codes single-use, PBKDF2-hashed, normalized (case + non-alphanumerics) so "ABCD-EFGH" and "abcdefgh" match. Constant-time digest compare. At-rest secrets stored under a versioned NaCl-secretbox scheme with multi-key rotation (`encryption_keys = {[1]=OLD, [2]=NEW}`); lazy-rekey on verify, batch `totp.rekey()` for active migration. Per-user brute-force lockout (`max_failed_attempts=5`, `lockout_duration=15min`); per-IP lockout opt-in via `trust_xff` flag. Auto-daily prune of orphaned pending rows via `app.daily`.
- **`hull/web/middleware/oauth@1`** (Lua + JS). OIDC Authorization Code + PKCE. Verifies ID-token signatures against the IdP's JWKS (RS256/384/512, PS256, ES256/384). Presets for Google and Microsoft Entra; Microsoft `tenant=common` now passes via `issuer_pattern` (regex match) since the actual ID-token issuer is `/{tenant-guid}/v2.0`, not `/common/`. HMAC-signed state cookie binds (provider, state, nonce, PKCE verifier, return_to). Owns `/auth/:provider/login`, `/auth/:provider/callback`, `/auth/logout`. `on_login(req, res, provider, claims, tokens) -> path?` and `on_logout(req, res) -> path?` callbacks.
- **`hull/web/middleware/audit-log@1`** (Lua + JS). Append-only sign-in / auth-event log with per-device fingerprint (HMAC-salted hash of UA + IP-prefix, deployment-private salt). Records `login_success`, `login_failure`, `password_reset_completed`, `email_changed`, `email_change_revoked`, custom kinds. `audit_log.list(user_id, opts)` and `list_devices(user_id)` for /devices UI. `is_new_device(user_id, req)` for new-device notifications. Auto-daily cleanup (`opts.retain_days = 365`); manual `audit_log.cleanup()`. Migration helper `recompute_fingerprints()` for salt rotation (paged + bounded by `max(id)` snapshot, in-process mutex with 1h staleness, breaks only on empty page). `cleanup_status() -> "scheduled" | "external" | "missing"` tri-state probe.
- **`hull/web/auth-health@1`** (Lua + JS). Runtime health probes for the auth stack: session table presence + count, audit-log cleanup status, pwned reachability (`not_yet_checked` / `reachable` / `fail_open` tri-state), TOTP enrollment count, RBAC tables. `auth_health.check({include_counts = false})` for the JSON output (counts gated since enumeration is recon material). `auth_health.routes(app, {auth_check})` mounts `/admin/auth-status` behind a required gate (admit on literal `true` only; thenable returns 500; throw returns 401; falsy returns 403). Backs the `hull agent auth-status` CLI.
- **`hull/web/pwned@1`** (Lua + JS). HIBP k-anonymity check (`api.pwnedpasswords.com/range/<first-5-hex-of-SHA1>`). 80KB embedded SecLists-10K blocklist for offline / air-gapped operation (binary-searched). Single-flight request cache, fail-open on network error with a one-shot warn. `pwned.health()` surfaces `{ok, status, last_check_at, last_error}` for the auth-health probe. Apps must add `api.pwnedpasswords.com` to `manifest.hosts`.
- **`hull/qrcode@1`** (Lua + JS). Pure-Lua / pure-JS QR Code generator (ISO/IEC 18004). SVG output with EC level + scale options. Used by `totp.enroll()` to render the authenticator-app QR. Color allowlist on `opts.dark` / `opts.light` so user-input wiring doesn't break out into arbitrary SVG.

### Added — Multipart upload, attachments, blob storage

- **Streaming multipart iterator (`req:multipart()` Lua / `req.multipart()` JS).** Routes opt in via `app.<verb>(..., { multipart = {...} })`. Iterator-shaped: each part yields `{name, filename, content_type}` plus `part:chunks(n)` / `part.chunks(n)` for byte-streaming reads and `part:read()` / `part.read()` for whole-part buffering. Binary-safe — Lua returns byte strings, JS returns `ArrayBuffer`. Caps (`max_part_size`, `max_total_size`, `max_parts`, `max_headers_size`, `max_input_buffer`) surface as parser errors the handler can `pcall` / try-catch to write structured 4xx responses. 94/94 e2e cases in `tests/e2e_multipart.sh` across both runtimes. See [docs/multipart.md](docs/multipart.md). Roadmap §1.5.b-2.
- **Incremental SHA-256 hasher.** `crypto.create_sha256()` / `crypto.createSha256()` returns an object with chainable `:update(bytes)` / `.update(bytes)` and a one-shot `:digest()` / `.digest()` (lowercase hex). Update-after-digest and double-digest both raise. Designed for streaming multipart digests but works for any incremental hashing use.
- **`hull/attachment@1`** (Lua + JS). Store, retrieve, delete file attachments backed by `hull/blob@1`. `attachment.store(part)` ingests a multipart `Part` via `blob.writer()` (content-addressed, dedupes identical bytes). `attachment.metadata(id)`, `attachment.read(id)`, `attachment.read_to_file(id, path)`, `attachment.delete(id)`. Companion `hull/web/attachment-serve@1` for serving with Content-Type + ETag + Range. Reference apps: `examples/hypermedia_photos` (photo gallery), `examples/hypermedia_todo` (photo uploads). Roadmap §1.5.b-4.
- **`hull/blob@1`** (Lua + JS). Content-addressed blob storage. `blob.writer()` returns a streaming writer (`:write(bytes)` / `.write(bytes)` + `:finalize()` / `.finalize()`) that returns the content-hash id. `blob.reader(id)` for streaming reads. Used as the low-level CAS layer for attachments, runtime bytecode caches, template caches, AOT compute caches, and `hull tools install`. Per-blob hard-link layout so duplicate writes are free. See `docs/blob.md`. Roadmap §1.5.b-3.5.
- **`hull/mime@1`** (Lua + JS). MIME type sniffer (magic-bytes + extension fallback). Used by attachment-serve. Roadmap §1.5.b-3.

### Added — Crypto cap + asymmetric verification

- **Asymmetric signature verification in the crypto cap.** RS256 / RS384 / RS512 / PS256 / ES256 / ES384 via mbedTLS. `cap/crypto_asym_mbedtls.c` backend behind a vtable; surfaces in stdlib as `crypto.verify_asym(alg, pem, message, sig)`. JWT module (`hull/jwt@1`) now dispatches on alg: HS256 (HMAC) AND RS/ES (asymmetric). Allowlist enforced BEFORE key resolution to defeat alg-confusion (e.g. token claims RS256 against an HS256-only allowlist → rejected before the key is ever hashed). Used by OAuth's OIDC ID-token verification.
- **`crypto.x509_pubkey_pem(cert_der_or_pem)` helper.** Extracts the SPKI public key in PEM from an x509 cert. Used to convert IdP JWKS `x5c` entries to PEM for the asymmetric verifier.
- **HMAC-SHA256 routed through a backend vtable** (`HlHmacBackend`). Same shape as the asymmetric and database vtables; mbedTLS is the default. Closes the round-1 audit finding that crypto primitives should follow Hull's vtable convention.
- **HMAC-SHA1 added** (HOTP/TOTP prerequisite).
- **`crypto.hex_encode` / `crypto.hex_decode` in the cap layer.** Lifted from per-module ad-hoc implementations; now a single binary-safe helper. (Stdlib modules that need byte-safe hex for non-ASCII bytes still use a local `bytesToHex` / `bytes_to_hex_local` because `crypto.hex_encode` for `string` input goes through QuickJS's UTF-8-inflating C boundary; documented in the relevant modules.)
- **`crypto.sha256` SHA-NI runtime dispatch** on Linux/Cosmo for arm64 + x86_64 (`cap/crypto_sha256_*`). Falls back to mbedTLS when the CPU doesn't support the extensions.

### Added — Runtime caches + CAS infrastructure

- **`hull/blob@1` low-level CAS** (`hl_blob_store_*`) powering everything content-addressed in Hull: attachment storage, Lua + JS bytecode caches, Lua + JS template caches, compute AOT cache (`wamrc` output memoization), `hull tools install`. Per-blob hard-link layout so duplicate writes are free; LRU/FIFO eviction via `hull cache prune`.
- **`hull cache` CLI surface.** `hull cache list` (per-kind entry count + size), `hull cache prune --max-size=N --max-age=N --kind=K [--dry-run]`, `hull cache clear --yes [--kind=K]`, `hull cache verify`. Plus `--json` output. Cache registry is a single source of truth shared between `hull doctor` and `hull inspect`.
- **`HULL_CACHE_DIR` env var** for per-app cache isolation in multi-tenant deployments. The `tools/` store stays in `$HOME/.hull/` regardless (signed durable downloads).
- **Persistent Lua + JS bytecode caches** at `~/.hull/blobs/runtime/{lua-bytecode,js-bytecode}/`. Reduce cold-start time on `hull dev` / re-init.
- **Persistent Lua + JS template caches** at `~/.hull/blobs/runtime/{lua-template,js-template}/`. Templates compile once, reuse across processes.
- **Compute AOT cache** keyed on the WASM bytes + `wamrc` invocation hash.
- **`hull tools install`** now routes through `blob_store` with a symlink-as-tool layout so a single binary backs multiple symlink names.
- **`stdlib/context/`** task-discoverable docs: `multipart.md`, `blob.md`, plus existing entries surface via `hull agent context --task=<name>`.

### Added — Streaming-handler routes (Keel v2.1+ pre-body dispatch)

- **`kl_server_route_streaming_async()` integration** lets Lua/JS handlers run BEFORE the body is fully buffered, enabling the multipart iterator and mid-stream early-exit on parser caps. Hull's body cap (`HL_ENABLE_HTTP_SERVER`'s 16 MiB default) is no longer the only knob — per-route multipart caps surface as structured 4xx without consuming server memory.

### Added — Build / agent / CI

- **`hull agent auth-status`** subcommand backs the auth-health probe over the CLI. Used by ops dashboards.
- **`hull agent attachment`**, **`hull agent blob`**, **`hull agent mime`** subcommands surface the new modules.
- **`hull modules available`** four-section grouping (Intrinsic / Core / Web / Web middleware) updated with the eight new modules.

### Changed (BREAKING)

- **`hull/web/auth-flows@1` API converged after 13 audit rounds.** Apps written against pre-v0.3.0 snapshots of any of `auth-flows` / `audit-log` / `oauth` / `totp` / `session` should re-read the module docstrings; option names and shapes were unified across runtimes during the audit cadence. Notable changes:
  - `auth_flows.init` now REQUIRES one of `public_origin` (single canonical URL), `trusted_hosts` (allowlist array of bare hostnames), or `trust_request_host = true` (explicit dev/test escape, init-time warn). Pre-v0.3.0 silently honored `req.headers.host` — host-header injection class. See round-9 HIGH-1.
  - `session.init({absolute_ttl = 86400})` is the new default (24 h hard cap on top of sliding TTL). Pass `false` to opt out; non-positive numeric values are coerced to nil with a warn.
  - `audit_log.init({fingerprint_salt = ...})` is REQUIRED (was silently `""` if missing). Run `audit_log.recompute_fingerprints()` ONCE during deploy if you had pre-v0.3.0 rows. Round-6 hardening.
  - `totp.verify(user_id, code, req)` now accepts an optional `req` for the per-IP gate. Apps wiring `totp_verify` callback in `auth-flows.init` should pass `req` through.
  - `strip_user_secrets` flipped from 1-field denylist (`password_hash`) to strict allowlist (`{id, user_id, email, email_verified}`) with optional `user_sanitize(user) -> safe_user` callback. Apps reading custom user fields in templates / `on_login` must wire the callback. Round-9 MEDIUM-6.
  - `is_email_ish` rejects bytes < 0x20 or == 0x7f.
- **Vendored Keel bumped through v2.0.0 → v2.1.0 → v2.1.1 → v2.1.2 → v2.2.0.** Streaming handler routes, mid-stream early-exit on parser errors, streaming-async pre-body dispatch, state-honoring fix on leftover-rc<0 / KL_PARSE_ERROR branches, synchronous-completion keep-alive force-off. Closes the previously-documented single-read `max_total_size` known limitation end-to-end.
- **DB SQL dialect helpers moved into the `HlDbBackend` vtable.** `db.table_columns()` and other dialect-aware queries dispatch via the backend rather than per-call SQL strings. SQLite is the only shipped backend today; PostgreSQL backend lands behind the same vtable.

### Fixed

- **Auth-stack: 13 rounds of iterative fixes.** Full per-round breakdown lives in the `git log --grep='^auth: round-'` history. Highlights:
  - Round 9 HIGH-1 — host-header injection in 5 click-through URL builders. Phishing-grade takeover via attacker-controlled `Host`. Pre-existing since auth-flows shipped.
  - Round 9 HIGH-2 — lockout counter never reset after window expiry; user got 1 attempt per 15min for 24h, not 5.
  - Round 9 HIGH-3 — `session.absolute_ttl = 0` killed every session on Lua (`0` is truthy in Lua, falsy in JS).
  - Round 10 HIGH-3 — Microsoft `tenant=common` preset rejected every login; strict `iss` equality failed against per-tenant issuer.
  - Round 10 HIGH-4 — `extract_ip` unconditionally honored X-Forwarded-For; trivially spoofable. `trust_xff` opt-in (default false).
  - Round 10 HIGH-5 — round-9 timing-collapse dummy PBKDF2 turned lockout into a CPU amplifier. Reverted.
  - Round 11 HIGH-1 — async `userSanitize` returning a Promise bypassed the round-10 allowlist (Promise is truthy object). Explicit thenable reject.
  - Plus ~50 MEDIUM / LOW items across rounds 5-12 covering parity gaps, operator-visibility, edge cases.
- **`hull manifest` and `hull inspect` on JS apps.** Previously the JS-runtime manifest-extractor was only reachable via the JS-bindings TU, so `hull inspect` (a build-tool command that doesn't link the JS runtime) failed on `app.js`. Extractor now lives in a runtime-neutral TU (`src/hull/manifest_extract_file.c`) reachable from both runtimes and from the build-tool commands.
- **`cap/crypto` swapped K constants in SHA-NI rounds 16-19.** SHA-256 produced wrong digests on Linux/x86_64 with SHA-NI enabled. Caught by the SBOM `binary_sha256` cross-check.
- **Multipart `multipart_config` zero-init on every route alloc** (Lua + JS). Stale config from a previous route could leak in if the struct wasn't zeroed. Caught by static analysis.
- **`blob_store` O_NOATIME on Linux for `track_access=0` reads.** Cache reads no longer perturb atime → no spurious LRU touches → eviction policy behaves as specified.
- **`hull/web/middleware/outbox` `cleanup({alsoFailed=true})` actually deletes failed rows** (JS only). Pre-fix `WHERE delivered_at <= ?` never matched failed rows (delivered_at IS NULL); now branched into `(state='delivered' AND delivered_at <= ?) OR (state='failed' AND created_at <= ?)`.
- **`hull/web/middleware/health` `/ready` actually emits the `stats` block** (Lua). Pre-fix `if server and http_server.stats then` — `server` was an undefined free global, branch was dead since the code was written.
- **`hull/qrcode` rejects malformed `opts.dark` / `opts.light`** so user-input wiring (e.g. `opts.dark = req.query.color`) can't break out into arbitrary SVG. Validates against a small color literal allowlist.
- **`hull/web/cookie` rejects `;` / CRLF / NUL in `opts.path`** to defeat Set-Cookie directive injection.
- **`hull/web/middleware/csrf` per-form-pair cap** (JS, parity with Lua). 256 pairs max.
- **`auth-flows.js`: avoid for-of destructuring** (`for (const [k, b] of map)`). Triggers a MSan use-of-uninit in QuickJS's `js_parse_destructuring_element`; surfaced repeatedly on CI MSan job. Workaround: `.forEach((b, k) => ...)`. Same pattern dropped in totp.js, qrcode.js.
- **MSan + UBSan CI job timeout** bumped from 10 → 15 min to fit the expanded auth-stack test surface.
- **14 scan-build / `-Wformat-truncation` findings** closed in one analysis pass.
- **2 cppcheck findings** closed; `HL_QJS_VERSION` propagated to cppcheck so QuickJS-version-dependent code stops being flagged.
- **JSON serialization moved to `sh_json` writer** in `cache`, `sbom`, `doctor`, `tools`, `version`, `serve` for streaming-safe output (no buffer-the-world).
- **Various e2e fixture fixes** for Linux Landlock (`data/` pre-creation), dash bashisms, and CI flakes exposed by first-time runs of new test suites.

### Security

- **13 iterative rounds of auth-stack security review** ran against `auth-flows` + `session` + `audit-log` + `totp` + `oauth` + `pwned` + `auth-health` between rounds 5 (initial) and 13 (convergence). Round 13 returned zero findings across three independent reviewers; the surface has converged. See `git log --grep='^auth: round-'` for the full per-round history.
- **Single-pass `/c-audit` + `/js-audit` + `/lua-audit` ran on the non-auth surface** after round 13. Two MEDIUM findings (outbox cleanup no-op; health.lua dead branch) shipped along with five LOW hardening items.
- **`/auth-audit` skill** (local, in `.claude/skills/`) encodes the iterative audit pattern: parallel-agent fan-out over three slices, git-log round-awareness, explicit "drop weak findings; zero is OK" instruction, report-only output. Not committed (`.claude/` is gitignored, matching `/c-audit` etc.); see the existing skill files for the convention.

## [0.2.0] — 2026-05-31

Hypermedia profile + web stdlib namespace reorganization. `hull init --profile htmx` now produces a complete HTMX + Pico app with CSP nonce, CSRF, session, flash, pagination, search, inline edit, loading indicator, form re-population, and idempotency wiring out of the box. Strictly-web stdlib modules moved under `hull/web/*` to parallel the existing `hull/middleware/*` precedent (full release notes: the GitHub release page or the GitHub release).

### Added

- **HTMX hypermedia profile.** `hull init --profile htmx` scaffolds a complete working app: HTMX 2.0.9 + Pico v2.1.1 classless (vendored, SHA-pinned), per-request CSP nonce, CSRF, session, flash messages, pagination, search-with-debounce, inline edit, loading indicator, form re-population, idempotency. 8/8 scaffold tests pass.
- **`hull/web/flash@1`** (Lua + JS). One-shot user notifications. Session-backed `flash.set` / `flash.consume` for POST/redirect/GET; `flash.trigger` for HTMX `HX-Trigger: {flash:...}` events. Hard dep on `hull/web/middleware/session@1`.
- **`hull/web/pagination@1`** (Lua + JS). Offset-based pagination. `pagination.from_query(req)` parses `?page=N&per_page=M`; `pagination.render(total, opts)` produces a windowed-links nav structure with ellipses for big page counts. Snake_case keys in both runtimes so the same template works for both.
- **`idempotency.respond_html(req, res, status, html, headers?)`** parallels existing `respond()` for HTML fragments. Replay path now reads content-type from cached headers blob instead of hardcoding `application/json` — HTMX double-clicks against the cached-response path now actually work for HTML.
- **`docs/htmx.md`** (~700 lines). Long-form pattern guide: architecture, CSP, CSRF, fragment-vs-page rendering, flash, delete confirmation, search + debounce, inline edit, loading indicator, form re-population, idempotency, empty states, testing.
- **`stdlib/context/htmx.md`** — agent-discoverable task-context doc; `hull agent context --task=htmx` returns it.
- **Resolver fix-it hint for renamed modules.** Apps declaring old names get an explicit `module 'hull/X@1' was renamed to 'hull/web/X@1' in v0.2.0` message before the fuzzy-suggest path. `hull_module_registry_suggest` also strips `web/` from candidates so typos of old short names still resolve to the new canonical.
- **`hull modules available`** now groups output into four sections (Intrinsic / Core / Web / Web middleware). `--json` output unchanged.
- **README + completions** surface `hull sbom`, `hull verify-self`, `hull verify-release`, `hull sign-release`, `hull modules` (with subcommands) — these existed but were missing from the README table and the bash/zsh/fish completion files.

### Changed (BREAKING)

- **20 strictly-web stdlib modules moved under `hull/web/*`.** Full move table in the GitHub release page. Modules that stay flat: `hull/jwt`, `hull/http-server`, `hull/http-client`, `hull/template`, `hull/email`, `hull/smtp`, plus all general utilities. Migration: see the perl one-liner in the release notes — idempotent, run from repo root. Old names emit a fix-it hint at app load.

### Fixed

- **Pre-existing template scoping bug in `examples/hypermedia_todo`** caught by the new pagination tests: `partials/todo_row.html` used `{{ id }}` as a bare name, which resolved to outer-scope nil when included inside `{% for t in todos %}{% include %}{% end %}`. The fragment-render path worked because POST handlers passed `{id, title, done}` directly. Now uses `{{ t.X }}` consistently; both contexts work.
- **Stale module-name references** in `src/hull/commands/doctor.c` and `stdlib/context/quickstart-cli.md` — `hull/http`, `hull/ws`, `hull/server` (deprecated names that pre-dated v0.2.0) cleaned up alongside the rename.
- **HTMX dev-mode session cookies** across `examples/{auth, todo, hypermedia_todo}`: `secure = true` was browser-dropping the cookie on plain-HTTP `hull dev`, breaking the CSRF binding. Now explicit `secure = false` with a doc comment about flipping for HTTPS.
- **CSRF placeholder warning** at app startup if scaffold-generated apps still have the literal `"CHANGE-ME-IN-PRODUCTION"` secret.
- **JS sessionBootstrap dead-code removal** — `req.headers["cookie"] = ...` patch was load-bearing before v0.1.8's `csrf.sessionKey` and became dead afterwards.

## [0.1.6] — 2026-05-29

Trust chain end-to-end verifiable, three independent ways. Closes the cheap and medium-cheap items from `roadmap_next.md` §0.3 (trust-chain hardening). After this release, anyone can prove a hull binary came from a specific commit, was built by a specific workflow, hasn't been tampered with, and that its SBOM was signed by the same release process. No single-party trust required.

### Added

- **`hull verify-self` subcommand.** One-shot replacement for the three-step `sha256sum` + `grep` + `hull verify-release` manual flow. Resolves running binary path (`/proc/self/exe` / `_NSGetExecutablePath` / argv[0] fallback), streams SHA-256, fetches manifest from cwd or alongside argv[0], verifies Ed25519 signature against the embedded release pubkey, extracts the expected hash for this platform's asset, constant-time compares. Flags for explicit / offline / custom-release use: `--manifest`, `--signature`, `--asset`, `--pubkey`. Roadmap §0.3.8.
- **Sigstore + Rekor transparency log.** Every release publishes `(release_tag, hull.sha256_digest, signature)` to `rekor.sigstore.dev` via `cosign sign-blob`. Two new release assets: `hull.sha256.cosign.sig` (96 bytes, Sigstore signature) and `hull.sha256.cosign.pem` (3.3 KB, Fulcio short-lived cert). Verifies independently of `HULL_RELEASE_KEY`: anyone runs `cosign verify-blob hull.sha256 --certificate hull.sha256.cosign.pem --signature hull.sha256.cosign.sig --certificate-identity-regexp='^https://github.com/artalis-io/hull/' --certificate-oidc-issuer='https://token.actions.githubusercontent.com'`. Roadmap §0.3.4.
- **SLSA build provenance.** Every shipped binary now has a Sigstore-signed attestation tying it to the specific commit, workflow run, and runner image. Generated by `actions/attest-build-provenance@v2` (SHA-pinned), covers all 7 binaries + 4 manifest/SBOM files. Verifiable with `gh attestation verify hull-linux-x86_64 --repo artalis-io/hull`. Independent of `HULL_RELEASE_KEY`. Roadmap §0.3.5.
- **Signed SBOM as release artifact.** Three new release files: `hull.sbom.json`, `hull.sbom.cdx.json` (CycloneDX 1.5), `hull.sbom.spdx.json` (SPDX 2.3). Generated by the linux-x86_64 binary before `hull.sha256` is computed, so all three are covered by the existing Ed25519 manifest signature AND the new Rekor signature AND a SLSA attestation. The SBOM is now triply attested. Roadmap §0.3.9.
- **`hull sbom` output includes `binary_sha256`.** New top-level field in JSON, `metadata.component.hashes[0]` in CycloneDX, `documentDescribes` + `SPDXRef-Package-hull-binary.checksums` in SPDX. The running binary's SHA-256 is computed lazily on first format call (cached, invalidated when `hl_sbom_set_binary_path` is called with a different path). Lets consumers cross-reference SBOM output against the signed release manifest in one comparison. Roadmap §0.3.11.
- **`docs/fork_playbook.md` (new, 259 lines).** Authoritative guide for forking Hull to ship with your own trust root: when to fork, what it gets you and doesn't (including trademark caveat), a substantial "why most organisations shouldn't fork" section (AGPL §13 propagation, security posture inheritance, super-linear divergence cost, trust non-transitivity, three lighter-weight alternatives), the five-step procedure, verification checklist, maintenance strategies, honest threat-model notes. Roadmap §0.3.15.

### Changed

- **All GitHub Actions invocations pinned to commit SHA.** Workflows previously used `actions/checkout@v4`-style mutable tags. Now pinned with format `actions/checkout@34e114876b0b11c390a56381ad16ebd13914f8d5  # v4`. Closes the "compromised action publisher slips code under existing tag" attack path. Four distinct actions pinned across `ci.yml`, `release.yml`, `deploy-site.yml`, `dco.yml`. Dependabot supports SHA-pinned actions natively and will PR updates. Roadmap §0.3.6.
- **Release workflow grants `id-token: write` and `attestations: write`** to the Create Release job (required for Sigstore OIDC and SLSA attestations).
- **`hull.sha256` manifest now covers SBOM files** in addition to binaries; the existing Ed25519 manifest signature transitively signs them.
- **`docs/roadmap_next.md` §0.3** updated to mark 7 of 15 trust-chain hardening items shipped. Remaining 8 are either organisational (Tier 1: pin CI environment, signing-key custody, key rotation) or polish (Tier 3: CPE strings, vendored archive repro check, build-env manifest).

### Fixed

- **`src/hull/sbom.c` magic-constant cleanup** during the §0.3.11 work: `HL_SHA256_DIGEST_BYTES` / `HL_SHA256_HEX_LEN` / `HL_SHA256_HEX_BUF` / `HL_SBOM_BINARY_MAX_BYTES` named instead of raw 32 / 64 / 65 / 256\*1024\*1024 sprinkled through the file.
- **`test_sbom`'s `binary_sha256_present_when_path_set` test** uses `__has_feature(memory_sanitizer)` to skip cleanly under MSan, where MSan's stdio interception silently short-circuits the `fopen` + `fread` + `mbedtls_sha256` chain. The feature is verified on every other CI variant (Linux, macOS, ASan, UBSan, cosmocc, GPU, reproducibility) plus a local smoke check that compares `hull sbom`'s `binary_sha256` to `shasum -a 256 ./build/hull` byte-for-byte. Not a Hull bug; a test-method incompatibility with MSan's libc-stdio wrappers.

## [0.1.5] — 2026-05-29

Live verifiable SBOM, macOS reproducibility CI gate, canonical positioning surfaced across every Hull-mentioning surface, site self-hosted with no third-party CDN dependencies, em-dash sweep across all prose.

### Added

- **`hull sbom` and `hull agent sbom`.** Every Hull binary self-describes its bill of materials via four output formats: human (default table), JSON (flat array; same shape as `hull agent sbom`), CycloneDX 1.5 (NTIA-aligned), SPDX 2.3. Per-build auto-refresh via Makefile `-DHULL_VENDOR_*_COMMIT=$(git -C vendor/<name> rev-parse HEAD)` injects vendored submodule SHAs into the binary at compile time. Build-flag gated: `make HL_ENABLE_DB=0` correctly omits SQLite. Runtime SHA-256 over the embedded Mozilla CA bundle (via mbedTLS, cached) exposed as `embedded_blob_sha256`. Orthogonal to the rest of the runtime: src/hull/sbom.c depends only on `cacert.h` + mbedTLS; test_sbom links against the minimal surface and would fail loudly if the SBOM module accidentally pulled in anything else. 16 test cases including format-determinism (two calls produce byte-identical output) + defensive error paths. Documented in `docs/sbom.md`.
- **Byte-reproducible builds, three layers, CI-gated.** Same source tree → byte-identical `build/hull` between rebuilds. Same source + same hull version → byte-identical app binary from `hull build`. Plus `make self-build` proves hull is self-hostable across all platforms. New CI job `reproducibility` enforces all three on every commit. Mechanism: deterministic ar archives via `ZERO_AR_DATE=1`; `-ffile-prefix-map=<srcdir>=.` in `sys_compile` to strip per-build tempdir paths from .o file content; same-path methodology for the `hull build` test (macOS `ld64` hashes output path into LC_UUID, so end-user same-target-name behavior is what's tested). Documented in `docs/MANIFESTO.md` "Reproducible builds" and `docs/POSITIONING.md` precision notes; full investigation arc preserved in `docs/roadmap_next.md §0.2` as lessons-learned.
- **`docs/POSITIONING.md` (new, 153 lines).** Standalone messaging-style guide encoding the canonical thesis, descriptor, target audiences, tone, vocabulary table, geographic positioning, typography rules, surface-update cadence, and authoring checklist. Lives separately from `MANIFESTO.md` (which is narrative WHY); POSITIONING is operational reference for anyone writing about Hull (humans or AI agents).
- **New canonical thesis surfaced across every Hull-mentioning surface:** *"Code became disposable. Trust is not."* Hero on gethull.dev, README opener, MANIFESTO callout, INVESTORS callout, GitHub repo About. Canonical descriptor unified to *"Hardened, capability-secure runtime infrastructure for AI-native systems."*
- **Self-hosted site assets.** gethull.dev no longer fetches from third-party CDNs. Tailwind, Lucide, Inter + JetBrains Mono fonts all vendored under `site/vendor/` and `site/fonts/`. Verify.html CSP tightened to `default-src 'none'` with `'self'` for everything except `connect-src https:` (for the dev-key fetch path).
- **OG card PNG variant.** `site/og-card.png` (1200×630, rendered from the SVG via `rsvg-convert`) added so LinkedIn and Facebook social previews render correctly (those platforms don't render SVG OG images).
- **OG metadata on `verify.html`.** Previously had only `<title>` + `<meta name="description">`; social shares of the verifier URL showed blank/scraped previews. Now has full `og:*` + `twitter:*` block with the canonical thesis.
- **`Engineered in Europe for sovereign, deploy-anywhere AI infrastructure.`** Imprint-style line added to site footer and README license area. Restrained provenance, no flag.
- **LICENSING.md vendored-dependency table.** Every static-linked third-party library listed with license (mostly MIT/BSD/Apache/ISC/PD). Explicit TinyCC note: LGPL-2.1+ is the only non-permissive dependency; compliance with §6 is satisfied by Hull shipping full vendored source under AGPL.

### Changed

- **README, MANIFESTO, AGENTS, INVESTORS positioning aligned** to the new canonical thesis. README's lead tagline + Why section rewritten. MANIFESTO title vocabulary updated (`Agent-Native, Local-First Applications` → `Hardened, Capability-Secure Runtime for AI-Native Systems`). AGENTS.md opening line uses canonical descriptor. INVESTORS adds thesis callout at top and refreshes Status section (was stuck at "approaching v0.1.0" — now correctly states v0.1.4 shipped 2026-05-28 with detailed shipped-features list). All `agent-native` → `AI-native`, `secure runtime` → `hardened, capability-secure runtime`. "Zero system dependencies" wording corrected: embedded TinyCC removes the system-compiler requirement, but the system linker is still used.
- **`docs/release_signing.md` status line** updated from `Design draft — pre-v0.1.0` to `Shipped (as of v0.1.3). The embedded gethull release key is live; every release manifest carries an Ed25519 signature verified by hull verify-release and by hull update before atomic install.`
- **Em-dash sweep across all prose files.** 1100+ em-dashes replaced with period + capitalize for between-clause breaks, parentheses for parentheticals, colons for elaborations. Same pattern as commit 6b6f394 precedent. Applies to README, AGENTS, CLAUDE, MANIFESTO, INVESTORS, PERSONAS, POSITIONING, all `docs/*.md`, and site HTML. Two UI-placeholder em-dashes in `site/verify.html` status badges became middle dots (`·`).
- **Deploy-site workflow CF-invalidation bug fixed.** Conditional was `if: ${{ env.CF_DISTRIBUTION_ID != '' }}` but `env:` was set in the same step (applied after `if:`), so the invalidation step never ran. Promoted to job-level env; now fires correctly when the secret is configured.
- **Documentation archive cleanup.** Five superseded docs moved to `docs/archive/`: three Phase-6 audits, `ASSESSMENT.md` ("approaching v0.1.0" snapshot), `api_review.md` (pre-v0.1.0 public-surface review). Archive index (`docs/archive/README.md`) updated with new entries and reorganized sections.

### Fixed

- **macOS sandbox claim precision.** Hero/site tech-stack chip clarified to `pledge + unveil (Linux/OpenBSD/cosmo) · Seatbelt (macOS)` so the platform split is visible without restructuring copy. Aligns the marketing claim with the truth disclosed deeper in the "Hull is not" section.

## [0.1.4] — 2026-05-28

§3.1, §3.2, §3.3 from `docs/roadmap_next.md`. Closes the v0.1.4 batch from the platform-sign-chain follow-ups roadmap.

### Added

- **§3.1 — Cosmo APE `hull build` works on Linux.**
  - Tool-mode sandbox unveils widened with `/opt` + `$HOME/.cosmocc` + `$HOME/cosmocc` so the cosmocc toolchain's install locations aren't sealed off (`src/hull/sandbox.c`).
  - `hl_compiler_select()` auto-detects cosmocc when running as a cosmo APE: checks `$HOME/.cosmocc/bin/cosmocc`, `$HOME/cosmocc/bin/cosmocc`, `/opt/cosmo/bin/cosmocc`, then `$PATH`. No need to pass `--compiler cosmocc` explicitly (`src/hull/compiler.c`).
  - Makefile `WAMR_INVOKE_SRC` selection reordered: target-compiler checks (cosmocc / `*-unknown-cosmo-cc`) precede `UNAME_S=Darwin` so cross-building a cosmo APE from a macOS host picks the correct asm invoker (was previously failing with "missing elf symbol table").
  - Known limitation: cosmo platform-sig E2E smoke test in `release.yml` stays SKIPPED. The jart pledge polyfill on Linux rejects child `mmap(PROT_EXEC)` independently of unveil policy, so spawning `/bin/sh` or `/usr/bin/cc` from inside the polyfill always fails on Linux runners. Native build jobs (darwin-arm64, linux-x86_64, linux-aarch64) gate the gethull signature chain end-to-end.

- **§3.2 — `hull eject` and `hull sign-platform` work on installed binaries.**
  - Both commands auto-extract the embedded `libhull_platform.a` to a tempdir when no `build/` tree is present. End-users who only have the binary can now eject or sign-platform without first cloning the repo (`stdlib/cli/lua/hull/{eject,sign_platform,build}.lua`).
  - New Lua bindings: `tool.extract_platform()` (single-arch) and `tool.extract_platform_cosmo()` (multi-arch).

- **§3.3 — Platform-sign chain polish (four follow-ups).**
  - Variant A (`--no-sandbox`) + Variant B (with sandbox active) for the native platform-sig E2E smoke test in `release.yml`. Variant A is the primary gate; Variant B re-runs the same `myapp` with the full pledge/unveil sandbox active and confirms `--verify-sig` still works under it.
  - Key-rotation story added as a new section in `docs/security.md` §2. Covers scheduled rotation, post-compromise rotation, the non-cross-validity property (intentional), and the impersonation gate.
  - `hull verify --gethull-key <file>` parity in Lua (`stdlib/cli/lua/hull/verify.lua`). CROSS-CHECK semantics: the file's pubkey must match this hull's embedded `HL_PLATFORM_PUBKEY_HEX` AND the signature must verify against it. Documented asymmetry with `verify.js` (JS uses override semantics because it can't reach the embedded key from QuickJS).
  - `tests/release_smoke.sh` extended with platform-sig E2E section. Post-publish on the actually-uploaded artifact, runs sign-platform + build --sign + --verify-sig. Catches CDN-tampering / upload-integrity surprises the in-CI smoke can't see.

### Fixed

- **Cosmo platform-sig E2E re-disabled in `release.yml`.** Round-1 audit attempt re-enabled it under the §3.1 sandbox/compiler fixes; CI demonstrated the deeper polyfill mmap restriction (`/bin/sh: libc.so.6: failed to map segment from shared object`) is independent of unveil policy. Re-skipped with honest comment naming the actual mmap-vs-unveil distinction. Native build jobs continue to gate the gethull signature chain.

## [0.1.3] — 2026-05-27

> The first published v0.1.3 build was found to ship with a broken
> gethull layer (the `libhull_platform.a` embedded into the hull
> binary had different bytes than the .a `sign-platform-manifest`
> hashed, so `hull build --sign` against that binary failed the
> cross-check). The original tag + GitHub release were deleted and
> v0.1.3 was retagged with the release-process fix described under
> "Fixed — release process" below. End-users who installed the
> first v0.1.3 should `hull update` to pick up the corrected build.

### Added

- **Platform-sig chain end-to-end (v0.1.3).** The
  `HL_PLATFORM_PUBKEY_HEX` placeholder is replaced with the real
  gethull.dev platform Ed25519 public key
  (`2a5461235aa51bbbe1e9cbc462e6a63f37d099f5ad17646a8f3a67db2f3a4fad`),
  and the full release → build → verify chain is wired:
  - **Release CI:** `release.yml` reorg splits platform building
    into a matrix, then a single Linux job
    (`sign-platform-manifest`) downloads the per-arch `.a`
    artifacts, computes SHA-256s, signs the canonical text
    manifest (`<64-hex>  <arch>\n`, LC_ALL=C sorted by arch) with
    `HULL_PLATFORM_KEY`, and emits
    `build/embedded_platform_sig.h` for the downstream
    `build-native` / `build-cosmo` jobs to embed.
  - **`hull build`:** computes SHA-256 of the
    `libhull_platform.a` being embedded, cross-checks against the
    inherited signed manifest, hard-rejects on mismatch unless
    `--no-verify-platform` is set. Writes the inherited
    `{manifest, signature, arch_hashes}` into
    `package.sig.platform.gethull`.
  - **`--verify-sig` runtime:** verifies the gethull manifest
    signature against `HL_PLATFORM_PUBKEY_HEX`; hard-rejects on
    missing block unless `--no-verify-platform`. Apps built by
    dev hulls (placeholder pubkey) skip the check silently with
    a one-line warning — same bootstrap shape as `hull update`'s
    release-pubkey placeholder.
  - **`hull verify` (CLI):** matching `--no-verify-platform`
    flag, new `tool.platform_pubkey()` Lua binding exposing the
    embedded pubkey, gethull-layer block in the report.
  - **Browser verifier (`site/verify.html`):**
    `verifyGethullLayer()` checks `platform.gethull.signature`
    against the hardcoded gethull pubkey; renders the layer
    explicitly. Per-app platform layer is now self-consistency
    only (the canary scanner is gone — the signed per-arch
    SHA-256 does all the integrity work it was hypothesized for).
- **`--no-verify-platform`** flag on both `hull build` and the
  runtime serve path. Documented escape valve for dev-built hulls
  and forks signing with their own key.
- **`tool.platform_pubkey()`** Lua tool-mode binding — returns the
  embedded `HL_PLATFORM_PUBKEY_HEX` or `nil` when the all-zeros
  placeholder is active.
- **`hull sign-release` / `hull verify-release`** still cover the
  release-binary path; the new gethull layer is signed by a
  separate key (`HULL_PLATFORM_KEY`) so a release-key compromise
  cannot forge platform-sigs and vice-versa.

### Changed

- **`signature.c` §5 reduced to self-consistency.** The v0.1.2
  per-app platform layer no longer pins
  `platform.public_key_hex` against `HL_PLATFORM_PUBKEY_HEX` —
  that role moved to the new §5b gethull layer. Forks signing
  platform with their own developer key continue to work without
  conflicting with the upstream gethull pubkey. `hull verify`
  matches the same shape; the
  `Platform layer: WARNING — key mismatch` path becomes a soft
  comparison against `--platform-key` only.
- **Docs/security.md §2 and §6** rewritten to reflect the
  three-layer structure (gethull, per-app, app developer) and
  the v0.1.3 status flip from "wired but inactive" to "shipped
  and enforced." Section 3.A and 3.B replace the canary-based
  prevention story with the manifest-signature prevention story.
- **Honest scorecard on `gethull.dev`** moves the platform-sig
  bullet from "Not yet" to "Ships," and replaces the old gap
  text with an explicit out-of-scope note for post-install
  binary integrity (OS layer's job).

### Removed

- **Platform canary scanner** in the browser verifier. The
  canary was the placeholder integrity signal while the
  signed-manifest path was missing; v0.1.3's per-arch SHA-256s
  cover the same property without Makefile post-link gymnastics.

### Fixed — release process

- **Platform-sig embed drift in `release.yml`.** `make EMBED_PLATFORM=1`
  was rebuilding `libhull_platform.a` from source during the
  `build-native` / `build-cosmo` jobs, replacing the downloaded
  artifact (the one `sign-platform-manifest` had hashed) with a
  freshly-built .a that produces different bytes on a different
  GH Actions VM. Result: the hull binary embedded one set of bytes
  and a manifest signature pointing at a different set, breaking
  the gethull cross-check for every app built with v0.1.3.
  - Fixed by switching to `make -o build/libhull_platform.a` —
    GNU make's `--assume-old` flag tells make to treat the .a as
    pristine and never rebuild it.
- **Verify-before-embed step** in every build job: sha256sum the
  downloaded .a, compare against its line in the signed manifest,
  fail loudly on mismatch. Defense in depth if the rebuild guard
  ever regresses.
- **Platform-sig E2E smoke test inside CI** (`release.yml`): each
  `build-native` / `build-cosmo` job now does `hull keygen` +
  `hull sign-platform` + `hull build --sign` + run with
  `--verify-sig` on the freshly-baked hull binary. Both signature
  layers (per-app v0.1.2 + gethull v0.1.3) are exercised end-to-end
  before the artifact is uploaded. Tests `tests/release_smoke.sh`
  still cover the release-sig path (post-publish, manual); the new
  in-CI test covers the gap that let v0.1.3-rc ship broken.

### Fixed

CI matrix repairs — four jobs that had been red since v0.1.2 went
green during the v0.1.3 stabilization pass:

- **Cosmopolitan (APE)** — test fixtures used `system("rm -rf ...")`
  for cleanup, which Cosmo's toybox `rm` rejects (the `-r` flag fails
  in any combined form: `-r`, `-rf`, `-fr`, `-Rf`). The failed cleanup
  left tmpdirs intact and the next `mkdir` hit `EEXIST`, cascading-
  failing the rest of the suite. Replaced with in-process
  `nftw(FTW_DEPTH | FTW_PHYS)` recursive delete — POSIX, works on
  Linux/macOS/cosmo uniformly, no flag-parsing dependency. Gated the
  `_XOPEN_SOURCE` define on `__linux__` so the change doesn't hide
  Darwin extensions on macOS.
- **Static Analysis (scan-build + cppcheck)** — scan-build flagged
  `agent/compute.c`'s `const char *platform = "unknown"` as a
  dead-store on Linux x86_64 because subsequent `#ifdef` branches
  reassigned it. Restructured as one `#if/#elif/#else` chain so
  exactly one assignment runs per build. cppcheck flagged four false
  positives — `agent/overview.c` `knownArgument`, three
  `commands/tools.c` `knownConditionTrueFalse` on version-string
  prefix-stripping — added suppressions for the defensive code
  cppcheck couldn't model.
- **Code Coverage** — `test_lua`'s link line referenced `$(PLEDGE_OBJS)`,
  `$(BUILDDIR)/tool.o`, `$(BUILDDIR)/sandbox.o`, and several other
  objects without listing them as prerequisites. Serial builds picked
  them up via earlier test binaries' chains; the coverage and ASan
  parallel builds didn't. Added explicit prereqs so make schedules
  them unconditionally.
- **MSan + UBSan** — vendor `*_CFLAGS` carried
  `-fsanitize=memory,undefined`; UBSan flagged well-known
  "technically UB but works on every target" patterns in
  `tweetnacl` (left shift of negative values in field arithmetic)
  and `quickjs` (function-pointer casts in `cutils`, signed-overflow
  left shifts). Dropped `,undefined` from QJS/LUA/SQLITE/LOG/
  SH_ARENA/SH_JSON/TWEETNACL/STB CFLAGS so vendor TUs get MSan
  shadow tracking (the actual point of the job) but not UBSan.
  Added MSan instrumentation to `MBEDTLS_CFLAGS` so MSan tracks
  mbedtls writes to caller buffers (e.g. `mbedtls_sha256` →
  caller-provided `digest[32]`); without it MSan reported every
  subsequent read as use-of-uninitialized. Hull's own CFLAGS still
  carry `-fsanitize=memory,undefined`, so Hull-code UBSan errors
  still fail the build loudly.

## [0.1.2] — 2026-05-26

Two themes: (1) side-loaded Hull-native tools (`hull tools install`),
opening the door for optional executables like `wamrc` without bloating
the main binary; (2) a complete agent-onboarding surface so AI coding
agents can bootstrap against a fresh hull install without scraping
docs from elsewhere.

### Added

- **`hull tools install <name> [--all]` / `tools list [--json]` /
  `tools uninstall <name>`** — Side-load optional tools (currently
  `wamrc`, the WAMR AOT compiler) from GitHub releases into
  `$HOME/.hull/tools/`. Reuses the same Ed25519-signed `hull.sha256`
  manifest that protects `hull update` — no new keys, no new
  ceremonies. Version-coupled: pulls from the SAME release as the
  running hull binary so e.g. wamrc stays at the WAMR commit hull was
  compiled against. Cosmo unsupported for tools that need LLVM. Full
  design: [`docs/tools_install.md`](docs/tools_install.md).
- **CI `build-wamrc` matrix** (linux-x86_64, linux-aarch64,
  darwin-arm64) in the release workflow — produces signed
  `hull-wamrc-<platform>` assets listed in the same `hull.sha256`
  manifest as the hull binaries.
- **`tool.find_tool(name)` Lua binding** (build-tool VM) — generic
  4-step lookup (`~/.hull/tools/` → sibling-of-hull → `$PATH`).
  Replaces the inline `find_wamrc()` heuristic in `build.lua`.
- **`hull --help` / `hull help` / `hull -h`** — Top-level usage
  printer grouping every registered subcommand by purpose. Suppresses
  build-time-gated commands when their `HL_ENABLE_*` flag is off so
  help only advertises what this binary can do. Symmetric with
  `--version`/`-v`/`version`.
- **`hull agent tools`** — Generic JSON dump of the tool registry
  crossed with the install state on this host. Independent of
  `HL_ENABLE_HTTP_CLIENT` — CLI-flavor builds without the installer
  can still enumerate what's registered.
- **`hull agent compute` gains a `wamrc` block** — installed/path/
  managed/available_for_platform/install_hint, so agents seeing
  modules with no AOT artifacts can recommend
  `hull tools install wamrc` in one call.
- **`hull agent context --list`** — Enumerate every embedded topic
  doc plus which level markers (minimal/compact/full) each populates.
  Cold-start agents call this first to discover the topic registry.
- **`hull agent overview [app_dir]`** — Single-shot composite
  summary (runtime, routes, compute, gpu, migrations, declared
  modules, tests, build_ready). One call to orient an agent dropped
  into an unfamiliar tree.
- **Six new context docs** in `stdlib/context/`: `orientation.md`
  (the AI-agent cold-start primer), `quickstart-{web,cli,tui}.md`
  (opinionated bootstraps per app shape), `gpu.md` (WGSL shaders +
  GPU vs WASM AOT crossover guidance), `tools.md` (`hull tools
  install` from the app-dev perspective). All three levels.
- **Discoverability breadcrumbs** in `hull --help`, bare-`hull`
  usage, and `install.sh` postscript — every entry point now points
  at `hull agent context --task=orientation --level=minimal` so an
  agent can bootstrap by reading any one of them.
- **Shared `release_io.{c,h}` helpers** extracted from
  `commands/update.c` — HTTPS GET, JSON-string extraction, SHA-256
  hex, manifest-line lookup, atomic install. Used by both `hull
  update` and `hull tools install` so the trust chain is implemented
  once.
- **`tests/release_smoke.sh`** — Post-`gh release create` smoke
  script that exercises the live install path: download wamrc from
  the just-published release, verify SHA-256, run `wamrc --help`,
  uninstall. The only end-to-end test of the live install codepath
  (the rest is covered by unit + e2e suites).
- **Shell completions** (bash, zsh, fish) for the new surface:
  `tools list/install/uninstall`, `agent overview`, `agent context
  --list`, all 19 context tasks.

### Fixed

- **`hl_release_io_find_checksum` OOB-read defense** — Reordered the
  exact-match guard's OR chain so the bounds check runs before the
  byte dereference. Dormant in practice (Keel allocates body+1 and
  writes a trailing NUL), but the helper is now safe regardless of
  caller. Surfaced by the post-implementation audit.
- **`atomic_write` checks `fsync` + `close` return values** — Aborts
  + unlinks the `.new` sidecar on either failure. Prevents a
  power-loss window where rename could happen over a half-flushed
  file (ENOSPC, EIO on network mounts).
- **SHA-256 hex compare is now constant-time** in both `hull update`
  and `hull tools install` paths (via `mbedtls_ct_memcmp`). The
  compared values are public, so the practical timing leak is nil,
  but the codebase now uniformly uses constant-time comparison for
  hash equality.
- **JSON escaper for `hull tools list --json`** — Tool descriptions
  now flow through a proper RFC 8259 escaper. Today's static
  registry is clean (no embedded quotes/backslashes), but future
  tool descriptions can't accidentally produce malformed JSON.
- **Makefile xxd hyphen handling** — The stdlib registry generator's
  sed expression now translates hyphens to underscores in symbol
  names, matching xxd's own normalization. Without this, context
  docs with hyphens in their filenames (`quickstart-web.md`) would
  fail to link.

### Changed

- **`build.lua` wamrc lookup simplified** — Replaces the inline
  `find_wamrc()` (PATH → `./build/wamrc` → sibling-of-hull) with a
  call to `tool.find_tool("wamrc")` plus a single `./build/wamrc`
  dev fallback. Same effective lookup order, implemented once in C.
- **`hull doctor` wamrc row** — Reports `installed` /
  `managed via hull tools` / `not installed` states. Hint text now
  recommends `hull tools install wamrc` (signed download) over
  `make wamrc` (build from source) as the first option.
- **`hull agent context` bare-command error** — Now points users at
  `--list` instead of hard-coding a stale topic enumeration.
- **`hull update` refactored to use `release_io`** — Same external
  behavior; the HTTPS + SHA-256 + manifest plumbing it relied on is
  now shared with `hull tools install`. Net diff: 252 lines deleted
  from `update.c`, same lines added to `release_io.{c,h}` plus
  reused in `tools.c`.

## [0.1.1] — 2026-05-25

Patch release. Two reproducible v0.1.0 bugs that broke first-time
user experience on Linux:

### Fixed

- **`hull init` scaffolded app crashed at load.** The generated
  `app.lua` / `app.js` declared `modules = ["hull/time@1"]` and
  imported `hull.log` + `hull.time`, but never declared
  `hull/http-server@1`. The runtime gate refused `app.get` (which
  the http-server module installs on the `app` intrinsic) and
  emitted "module 'hull/log' was imported at top-of-file but is not
  declared". Templates now declare every module they import
  (`hull/http-server@1`, `hull/log@1`, `hull/time@1`); the JS
  template also gains its missing `import { app, log } from
  "hull:..."` lines.
- **Released native binaries shipped without the embedded platform
  library.** `hull doctor` on a freshly-installed v0.1.0
  `hull-linux-x86_64` / `hull-linux-aarch64` / `hull-darwin-arm64`
  reported `Platform library: embedded no` and `hull build: not
  ready`. Cause: the release-workflow native-build step ran plain
  `make` while the Cosmopolitan job correctly ran `make CC=cosmocc
  EMBED_PLATFORM=cosmo`. Now the native step runs `make platform`
  followed by `make EMBED_PLATFORM=1`, with a smoke-test check that
  fails the release if doctor doesn't report `embedded yes`. Binary
  size grows ~5.5 MB → ~12 MB per native platform; still well under
  the cosmo APE.

### Improved

- `HL_PLATFORM_PUBKEY_HEX` and `HL_RELEASE_PUBKEY_HEX` macros are
  now `#ifndef`-guarded in their headers so tests, self-hosted
  forks, and future rotation tooling can override them at compile
  time via `-DHL_*_PUBKEY_HEX=...`. Production builds inherit the
  embedded values unchanged.

### Reverted

- `HL_PLATFORM_PUBKEY_HEX` is back to the all-zeros placeholder.
  v0.1.0 shipped a real value, but the rest of the platform-sig
  chain (canary emission in `libhull_platform.a`, release-time
  signed manifest, `hull build` pass-through, runtime
  enforcement — tracked as item 2 in `docs/roadmap_next.md`)
  isn't in place yet. With a real pubkey embedded and no actual
  signed platform artefact, the pinning bypass in
  `hl_verify_startup` turns off and rejects every `hull build`
  output. The placeholder restores the documented "wired but
  inactive" state until the full chain ships. The platform key
  itself is still backed up offline and in the
  `HULL_PLATFORM_KEY` GitHub secret — no key regeneration needed
  when the chain is ready.

### Trust model

No changes. Existing v0.1.0 binaries continue to verify their own
updates against the same embedded release pubkey; v0.1.1 is a
drop-in replacement.

## [0.1.0] — 2026-05-25

First publicly tagged release. The goal of `v0.1.x` is to lock the
distribution and capability model so apps and tooling built against
`v0.1` can rely on a stable shape; performance, internal architecture,
and the WASM / GPU surface continue to evolve under that contract.

### Distribution

- **One binary per platform.** `hull` ships as a self-contained
  executable with no runtime dependencies. Bundled: Lua 5.4, QuickJS
  (ES2023), SQLite, mbedTLS, WAMR, TweetNaCl, the embedded Mozilla CA
  bundle, the platform standard library, and the build toolchain.
- **Four release artifacts per tag.**
  - `hull-linux-x86_64` — native ELF (~5 MB).
  - `hull-linux-aarch64` — native ELF for Graviton / NVIDIA DGX /
    Ampere / Raspberry Pi 4+ (~5 MB).
  - `hull-darwin-arm64` — native Mach-O for Apple Silicon (~5 MB).
  - `hull-cosmo` — Cosmopolitan APE (fat x86_64 + aarch64, ~30 MB)
    that runs on Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD from
    a single file.
- **Signed releases.** Every release manifest (`hull.sha256`) is
  Ed25519-signed (`hull.sha256.sig`). The release public key is
  embedded in every Hull binary as `HL_RELEASE_PUBKEY_HEX`; `hull
  update` verifies the signature against the embedded key before
  atomically replacing itself via `rename(2)`. See
  [docs/release_signing.md](docs/release_signing.md).
- **One-line installer.** `curl -fsSL https://gethull.dev/install.sh | sh`
  detects OS+arch, downloads the matching native binary (or `hull-cosmo`
  on unsupported combinations), verifies SHA-256 against `hull.sha256`,
  and installs to `~/.local/bin/hull`.
- **Self-update.** `hull update` fetches the latest release via
  `api.github.com`, verifies both the signature and the SHA-256, and
  atomically replaces the running binary.
- **Shell completions.** Bash, zsh, fish completions ship in
  `completions/`.

### Runtimes

- **Lua 5.4** sandbox: `io` and `os` removed, custom `require` resolves
  only from the embedded stdlib registry, 64 MB memory cap, configurable
  per-request instruction limit (default 100 M instructions).
- **QuickJS** sandbox (ES2023): `eval` removed, `std`/`os` not loaded,
  64 MB memory cap, 1 MB stack cap, configurable per-request instruction
  limit.
- **One runtime per app** — entry point extension (`app.lua` or
  `app.js`) selects.
- **Polymorphic vtable.** Both runtimes go through the same
  `HlRuntimeVtable`; the capability layer (`hl_cap_*`) is shared C and
  is the only path to system resources.

### Capabilities

All system access is mediated by the C capability layer:

- **Database** — SQLite, parameterized queries, transactions, batch,
  user-defined functions (Lua/JS or WASM), async pool.
- **Filesystem** — manifest-allowlisted read/write paths, traversal
  rejection, symlink-escape rejection, mmap.
- **HTTP client / server** — separately gateable
  (`HL_ENABLE_HTTP_CLIENT`, `HL_ENABLE_HTTP_SERVER`). Server: routes,
  middleware, SSE, WebSockets, body limits, ETag, static files. Client:
  host-allowlisted `http.fetch`, SMTP, follow-redirects, mTLS via
  embedded mbedTLS.
- **Crypto** — SHA-256/512, HMAC, PBKDF2, Ed25519, NaCl box/secretbox,
  random. Constant-time comparison helpers.
- **Time / env / WebSockets / static files / image codecs**.
- **Compute (WASM)** — WAMR runtime, AOT compilation, gas metering,
  persistent instances, shared read-only data segments, streaming I/O.
  ~256 KB binary cost, gateable.
- **Compute (GPU)** — wgpu-native (Vulkan / Metal / DX12), WGSL
  compute shaders, pipelines, persistent buffers, GPU↔CPU and GPU↔GPU
  copies, textures. Opt-in (`HL_ENABLE_GPU=1`).
- **Tool mode (Lua-only)** — `hull build`, `hull deploy`, etc. run in
  a sandboxed Lua VM via `hl_tool_spawn` with a compiler allowlist.
  No `system()` / `popen()`.

### Capability sandbox

- **Linux / Cosmopolitan** — `pledge(2)` syscall filter + `unveil(2)`
  filesystem cap. Violation → SIGKILL.
- **macOS** — Seatbelt SBPL profile built dynamically from the
  manifest. Violation → EPERM.
- **OpenBSD** — native pledge/unveil.

### Module declaration

- `manifest.modules` is the canonical declaration mechanism. Apps list
  every first-party stdlib module they use, with API version
  (`"hull/crypto@1"` etc.).
- Two independent gates: **module resolver** (build-time / declaration
  check) and **per-call capability cap** (manifest `fs`/`env`/`hosts`
  allowlist enforced at the C boundary).
- `hull/app` is the only intrinsic — `app.manifest`, `app.get`,
  `app.post`, `app.use`, `app.router`, `app.ws`, `app.sse`, `app.main`.
  Other modules (timers, db, fs, http, …) must be declared.

### Build modes

- `make` — default build, ~5 MB on darwin/arm64.
- `make HL_ENABLE_DB=0` — compute-only build, ~3.9 MB on darwin/arm64
  (no SQLite, no DB-dependent stdlib modules).
- `make HL_ENABLE_HTTP_SERVER=0` — CLI-only build (no inbound HTTP).
  Apps may use `app.main(fn)` plus `http.fetch` outbound.
- `make HL_ENABLE_HTTP=0` — pure compute/CLI, no Keel, no mbedTLS,
  smallest possible build.
- `make CC=cosmocc EMBED_PLATFORM=cosmo` — fat APE binary with multi-arch
  embedded platform library.
- `make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu` — adds GPU compute.

### Application surface

- **Modular layouts** for REST / CLI / TUI apps: `hull new --type rest`
  / `--type cli` / `--type tui` scaffold a `routes/` + `models/` +
  `middleware/` + `lib/` skeleton instead of a single `app.lua`.
- **Background timers** — `app.every(ms, fn)` and `app.daily(hhmm,
  fn)` run on the event-loop thread, support full async runtime,
  return `false` to self-cancel.
- **Migrations** — versioned SQL migrations in `migrations/*.sql`,
  embedded in built binaries, auto-applied on startup, tracked by
  checksum.

### Standard library

Embedded `hull/*` modules (Lua + JS, identical semantics where the
languages allow):

- **Middleware** — cors, ratelimit, csrf, auth (session + JWT), session,
  logger, transaction, idempotency, outbox, inbox, rbac, health, etag.
- **Helpers** — cookie, jwt, template (with inheritance + filters),
  validate, form, i18n, csv, search (SQLite FTS5), image, db.udf.
- **CSRF wire format** is fixed and identical across Lua and JS
  (`tsHex.hmac_hex` over `session_id ":" tsHex`); a cross-runtime
  reference token is asserted by unit tests in both runtimes.

### Tooling

- **`hull agent`** — JSON introspection commands for AI coding agents:
  `routes`, `db schema`, `db query`, `request`, `status`, `errors`,
  `test`, `context`, `migrate`, `deploy`, `manifest`, `endpoint`,
  `middleware`, `capabilities`, `modules`, `validate`, `vfs`, `compute`,
  `gpu`, `perf`, `logs`, `eval`, `template`, `schema_diff`, `sql`.
- **`hull mcp`** — MCP server exposing the same introspection surface
  to MCP-aware clients (Claude Code, Cursor, OpenCode).
- **`hull doctor`** — environment-readiness report (platform embedded,
  compilers available).
- **`hull check`** — manifest + module-graph validation before tests.
- **`hull dev`** — watch-and-reload server with structured error
  sidecars (`--agent` writes `.hull/dev.json` + `.hull/last_error.json`).
- **`hull init`** — in-place project initialization (git-init-style;
  doesn't overwrite existing files).

### Distribution site

- `https://gethull.dev/` — landing page + `install.sh`. Hosted on S3
  behind CloudFront, deployed automatically from `site/**` changes via
  `.github/workflows/deploy-site.yml`.

### Licensing

Dual-licensed:
- **AGPL-3.0** — free for AGPL-compatible use.
- **Commercial** — closed-source embedding and proprietary
  distribution. Contact `licensing@artalis.io`.
- See [LICENSING.md](LICENSING.md) for which license applies to your
  use case.

### Known limitations in 0.1.0

- macOS x86_64 (Intel) is not a native target — Intel Mac users get
  `hull-cosmo` via the installer.
- Windows native binary not in the matrix yet; Windows users get
  `hull-cosmo` via WSL or directly (APE runs natively on Windows).
- HTTP/2 is supported via Keel, but production hardening of HTTP/2
  server is still maturing (see `vendor/keel/CLAUDE.md`).
- PostgreSQL backend for `hull/db` is on the post-0.1 roadmap; v0.1
  ships SQLite only.

[Unreleased]: https://github.com/artalis-io/hull/compare/v0.1.1...HEAD
[0.1.1]: https://github.com/artalis-io/hull/releases/tag/v0.1.1
[0.1.0]: https://github.com/artalis-io/hull/releases/tag/v0.1.0
