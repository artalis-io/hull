# Hull Application Bootstrap Prompt

> **For AI coding agents** (Claude Code, Codex, OpenCode, Cursor, etc.) starting
> a new Hull application from a product spec. This file is the single canonical
> entry point - paste it (or its contents) into the agent along with the
> application spec, and the agent will follow the workflow below.

This bootstrap is **app-agnostic**. It assumes you have:

1. The `hull` binary installed (see [Install Hull](#install-hull) below).
2. A product spec (markdown or similar) describing **what** to build.
3. This file.
4. Either an internet connection (to fetch docs from
   `github.com/artalis-io/hull`) OR a local clone of the Hull source
   tree for offline reading.

---

## Install Hull

If `hull` isn't already on this machine:

```sh
curl -fsSL https://gethull.dev/install.sh | sh
```

This installs to `~/.local/bin/hull` (or `/usr/local/bin/hull` if root).
The install script detects OS/arch, verifies the SHA-256, and verifies
the Ed25519 release signature against the embedded public key. No
sudo prompt unless the prefix needs it. Pin a specific version with
`HULL_VERSION=v0.10.0 curl ... | sh`.

Verify:

```sh
hull version          # vM.N.P-...
hull doctor           # check toolchain readiness
hull modules available --json | head
```

If `hull doctor` reports `hull_build: ready`, you can also produce
standalone binaries (`hull build .`). For just developing apps via
`hull dev`, the default install is enough.

---

It produces, in order:

- **Phase 1 (Discovery)** - agent reads required Hull docs + the spec, then
  asks the human 5–10 clarifying questions.
- **Phase 2 (Plan)** - agent writes `PLAN.md` in the new app's repo for
  human review. **No code yet.**
- **Phase 3 (Implementation)** - only after the human approves `PLAN.md`.

And, on an ongoing basis:

- **`PLATFORM_GAPS.md`** in the app repo - agent appends here any time it
  encounters something Hull is missing, instead of coding around it.

---

## How to use this prompt (human instructions)

```sh
# Fresh start, in the new app's empty repo:
hull init                                # scaffolds app.lua, tests/, migrations/, .gitignore
curl -fsSL https://raw.githubusercontent.com/artalis-io/hull/main/BOOTSTRAP.md > BOOTSTRAP.md

# Then hand the agent both files:
cat BOOTSTRAP.md /path/to/your-spec.md | <your-agent-cli>
```

Or, if your agent supports file references / `@`-mentions and can
read URLs, point it at:

- This file (BOOTSTRAP.md) - local copy OR
  `https://github.com/artalis-io/hull/blob/main/BOOTSTRAP.md`.
- The product spec.
- The Hull source docs - either a local clone OR the GitHub URLs
  listed in [Phase 1: required reading](#phase-1-required-reading).

The agent needs **read** access to Hull's docs (web or local) and
**write** access to the new app's repo (where it will create
`PLAN.md`, source files, tests, and `PLATFORM_GAPS.md`). It does NOT
need to modify the Hull source tree.

---

## Agent: read this entire file before doing anything else

You are an AI coding agent. You are going to build a Hull application. You
likely have very little prior knowledge of Hull. **That is fine** - Hull is
self-describing, and this bootstrap will tell you exactly what to read,
what patterns to follow, and what to avoid.

### The non-negotiable workflow

You will work in four phases. **Do not skip any of them.** Even if
the spec looks clear, the discovery loop surfaces mismatches between
the spec author's intent and your reading of it BEFORE they're baked
into code.

**Phase 0 - Archetype.** Determine what KIND of app this is (web /
CLI / TUI / API / compute). The answer drives `hull init`'s flags
and which reading is actually required.
**Phase 1 - Discovery.** Required reading (archetype-filtered) +
clarifying questions about the spec.
**Phase 2 - Plan.** Write `PLAN.md` to the new app's repo. **Stop. Wait
for human approval before writing any code.**
**Phase 3 - Implement.** Build per `PLAN.md`, in small testable
increments.

### Phase 0: pick the app archetype

Hull scaffolds different shapes via `hull init` flags. **Before
reading anything else,** identify which archetype the product spec
implies - then run `hull init` with the matching flag set, so the
rest of your work happens on the right skeleton.

If the spec doesn't make the archetype obvious, **ask the human**
before proceeding. Don't pick by vibe.

| Archetype | Identifying signals in the spec | `hull init` flag | Reference examples |
|---|---|---|---|
| **HTMX web app** | Browser UI, forms, server-rendered HTML, "admin panel" / "dashboard" / "portal", users log in, server state. **Default for most "build me an app" briefs.** | `hull init --profile htmx` | `examples/hypermedia_photos`, `examples/htmx_widgets_register` |
| **REST / JSON API** | "API for clients to consume", mobile/SPA frontend, OpenAPI, no server-rendered UI | `hull init` (minimal server, then add JSON routes) | `examples/rest_api`, `examples/rest_api_modular`, `examples/jwt_api` |
| **CLI tool** | Runs once, does a thing, exits. Pipelines / scripts / one-shot operations. Returns shell exit codes. | `hull init --cli` | `examples/hello_cli`, `examples/cli_modular` |
| **TUI app** | Interactive terminal UI, "dashboard in the terminal", REPL, log tailer, picker. | `hull init` then add `hull/tui` bindings | `examples/tui_dashboard`, `examples/tui_chat`, `examples/tui_log_tailer`, `examples/tui_modular`, `examples/tui_picker`, `examples/tui_repl`. See `docs/cli_mode.md` for the `app.main` lifecycle TUI apps run on. |
| **Compute / WASM service** | Pure data transformation, scoring, ML inference, no persistent state. Often run as a sidecar. **Large-file (OSM PBF / Parquet / raster / model) scanning** without copying the file into WASM: map a window with `fs.mmap(path, {offset,length})` and attach it read-only to one call via `compute.call(m, in, {spans={{name,buffer}}})`; the plugin reads it in place through the `hull/wasm/span.h` SDK (zero-copy, no per-access host call, files bigger than WASM's 4 GiB space via windowing). | `hull init` + `compute/` modules; a compute app already links zero HTTP/Keel/TLS/SQLite/WASM it doesn't use (the SLIM base auto-composes), or add `--flavor=pure-compute` to also validate no HTTP/TLS is declared. GPU compute + DuckDB OLAP are opt-in composable features (`hull build --with=gpu\|duckdb`, native-only) - see `docs/features_and_flavors.md` | `examples/compute`, `examples/compute_gpu_chain`, `examples/mapped_spans`. See `docs/wamr_architecture.md`, CLAUDE.md § "WASM Compute Plugins" (→ "Mapped spans"), and `docs/wasm_mapped_spans_design.md`. |
| **Hybrid (app.main + serve loop)** | App needs startup migrations + a serve loop. App needs to run a one-shot then serve. | `hull init` (default) + register `app.main` alongside routes - both are supported simultaneously, see CLAUDE.md § "App Lifecycle" | Any of the above; the lifecycle page covers patterns. |

After picking, run the chosen `hull init` invocation (still in the
empty app repo). It creates the right `app.lua` / `app.js`, `tests/`,
`migrations/` (where relevant), and `.gitignore` without
overwriting any existing files.

The archetype also determines how lean the built binary is, for free:
`hull build` produces a binary tailored to the app. The distributed
hull's app-build base is **fully composable** - it drops EVERY reducible
subsystem (both interpreters, the HTTP core + web bindings, the Keel
event loop + server, mbedTLS + TLS, SQLite, WASM/WAMR, image codecs) and
composes back only what the app uses, statically at build time (a
whole-archive link, NOT `dlopen`). You get exactly one interpreter (Lua
or JS, from the entry file); the HTTP layer + Keel only when the manifest
declares an HTTP module; TLS only when the app needs it (HTTP or a
`--with=postgres`/`mysql` net-DB); SQLite only when it uses `db`; WASM
only when it uses `compute`; image codecs only when it declares
`hull/image`. A CLI / compute archetype (`app.main`, no HTTP/TLS/DB) links
**zero** Keel, mbedTLS, SQLite, or WASM (~2.1 MB, vs ~6.5 MB full);
`--flavor=pure-compute` additionally *validates* the app declares no
HTTP/TLS. This is automatic - every subsystem rides inside `hull` and
auto-composes; only large optional subsystems (gpu / duckdb / postgres /
mysql / tui) are `hull build --with=<name>`. Composition is
signature-aware: `hull build --sign` attests every archive it composes
(the runtime, HTTP core, Keel, TLS, SQLite, WASM, image, tui bridge, any
`--with` feature) inside `package.sig`, and `--verify-sig` proves the app
was built from genuine gethull.dev artefacts. You don't manage this - it
just happens. (Cosmo, the fat APE, is exempt and keeps everything
in-base.)

If the spec describes multiple shapes (e.g. an HTMX admin UI **plus**
a CLI for batch import), pick the PRIMARY one for `hull init` and
note the secondary shape as a "Phase 3 add-on" in your eventual
`PLAN.md`. Adding `app.main` to an HTMX app, or HTMX routes to a CLI
scaffold, is a few lines later; the initial scaffold just needs to
match the primary shape.

### When in doubt, ask hull

Hull is **self-introspecting**. Before guessing at an API, grepping
source, or asking the human, **try a `hull` command first**. Almost
every question you'll have during Phase 1–3 has a one-line answer
from the CLI:

| You want to know … | Run … |
|---|---|
| What stdlib modules exist + their deps + capability requirements | `hull modules available --json` |
| What ONE module does in detail | `hull modules explain hull/web/auth-flows@1` |
| What capabilities the current app uses (declared vs actually called) | `hull agent capabilities` |
| What docs exist for a topic (htmx, auth, blob, multipart …) | `hull agent context --task=<topic>` |
| What routes does my app currently expose | `hull agent routes` |
| What does my DB schema look like | `hull agent db schema` |
| Did my last reload succeed | `hull agent errors` |
| What does the manifest resolve to | `hull agent manifest` |
| Is my app ready to deploy | `hull agent deploy` |
| Run a request without booting the full server | `hull agent endpoint POST /items` |
| What `---@`-annotated declarations does my Lua source define (analyzed statically, no execution) | `hull agent inspect` |

There are dozens of `hull agent` subcommands - see [README.md § Using Hull
with AI Agents](README.md#using-hull-with-ai-agents) for the full
list. They emit JSON (or `--json`-flagged human output) designed for
agent consumption: no screen-scraping, no log-parsing. If you find
yourself reading source to answer a structural question about an app,
**stop and try `hull agent` instead.**

Other commands you'll lean on heavily during Phase 3:

- `hull dev` - run with file-watch, auto-reload on save.
- `hull test` - in-process HTTP test harness against `:memory:` DB
  with migrations applied. Fast (sub-second per test). Run after
  every change.
- `hull check` - validate the manifest before tests fire.
- `hull migrate [status|new <name>]` - apply / inspect / create
  migrations.
- `hull build` - ship a standalone binary with everything embedded.
- `hull doctor` - environment / toolchain sanity check.

### Phase 1: required reading (in this order)

You must read these files before asking questions or writing anything.
They are listed roughly in dependency order - each builds on the
previous. Paths are relative to the Hull repo root; if you don't have
a local clone, fetch from
`https://github.com/artalis-io/hull/blob/main/<path>` (raw form:
`https://raw.githubusercontent.com/artalis-io/hull/main/<path>`).

| # | Path | Why |
|---|------|-----|
| 1 | `CLAUDE.md` | The Hull project's own conventions for agents. Read it ALL - it covers build flags, manifest system, capability layer, runtime sandboxes, sub-commands, stdlib middleware, and the WASM/GPU compute story. Long but dense; every section matters. |
| 2 | `README.md` | High-level pitch and feature surface. Read for the worldview. |
| 3 | `docs/htmx.md` | The hypermedia pattern guide (~700 lines). Architecture, CSP, CSRF, fragment-vs-page rendering, flash, delete confirmation, search + debounce, inline edit, idempotency, empty states, testing. |
| 4 | `docs/htmx_widgets.md` | The §1.5.g widget tier. Eight widgets that handle the common UI patterns (toast, confirm, form, search, inline-edit, sort, pagination, table). **Read every section** - this is the answer to "should I build my own X?" 90% of the time. |
| 5 | `docs/security.md` | Capability model, sandbox enforcement, manifest gates. Critical for understanding what your app is allowed to do and how. |
| 6 | `examples/hypermedia_photos/` (Lua AND JS variants) | The reference HTMX app. Read `app.lua` and `app.js` side by side, plus `templates/`, `migrations/`, and `tests/`. This is what a real, production-shaped Hull app looks like. |
| 7 | `examples/htmx_widgets_register/` | Exercises all 8 widgets in one CRUD page. Use as a recipe for which widget fits which UX need. |
| 8 | `stdlib/context/*.md` | Task-discoverable docs (`multipart.md`, `blob.md`, `htmx.md`, `build.md`). Skim the index, read what's relevant to your spec. Available locally via `hull agent context --task=<name>` even without a source checkout. |

You may also benefit from running these commands against the Hull tree
to discover its current API surface (don't memorize, just know they exist):

```sh
hull modules available --json          # canonical first-party module list
hull modules explain <name>            # one module's full spec
hull agent capabilities                # what cap functions exist
hull agent context --task=<name>       # task-relevant docs (htmx, auth, etc.)
```

### Phase 1: clarifying questions

After reading, ask the human **5–10 clarifying questions** about the
spec. Good questions:

- Surface ambiguity in the spec ("Section 3.2 says 'admin can reassign
  assets'. Is admin a role per app, per user, or per environment?").
- Resolve unstated cross-cutting concerns ("Is this single-tenant or
  multi-tenant? If multi-tenant, is isolation per-database, per-row, or
  per-schema?").
- Force decisions the spec author may have deferred ("How should
  out-of-stock items render - separate table, struck-through, hidden?").
- Pick between Hull-supported alternatives ("Sessions: do you want
  sliding TTL (default) or hard 24h absolute? See `session.init` opts.").

Bad questions:

- "What is the database?" (Always SQLite for v1. Hull's docs say so.)
- "Which CSS framework?" (Hull ships Pico classless. Use it.)
- "How do I handle CSRF?" (`hull/web/middleware/csrf@1` is wired in
  scaffold output.)

If the spec is genuinely complete on a topic, don't fish for questions
to ask. Quality over quota.

### Phase 2: write PLAN.md

After the human answers, write `PLAN.md` in the new app's repo. Suggested
structure (adapt to the app):

```
# <App Name> - Implementation Plan

## 1. Scope
What's in v1, what's explicitly out, what's "v2 maybe".

## 2. Hull modules used
Each module from `app.manifest({ modules = {...} })` with the one-line
reason it's there. Goal: a reader can audit "do we need all of these
and only these?" in 60 seconds.

## 3. Manifest (proposed)
The actual table you'll pass to `app.manifest({...})` - modules, fs,
hosts, env, csp. Include comments for non-obvious entries.

## 4. Data model
Tables, columns, indexes, FK relationships. SQLite-flavored SQL.
Include the migration file(s) you'll create.

## 5. Routes
Method + path + brief description. Group by resource. Note which
return full pages vs HTMX fragments.

## 6. UI layout
Pages + the widgets used on each. Reference `docs/htmx_widgets.md`
sections.

## 7. Auth model
Who can do what. Sessions vs JWT vs OAuth. If `auth-flows@1` is in
play, the user storage callbacks you'll wire.

## 8. Tests
What you'll cover with `hull test`. Note any planned playwright E2E
(usually overkill for v1; revisit later).

## 9. Open questions
Anything you couldn't resolve in Phase 1 that needs human input
before you can finish phase 3.

## 10. Phase 3 sequencing
Numbered build order. Smallest first. Each step should end with a
runnable app + passing tests.
```

**Stop after writing PLAN.md.** Ask the human to review. Do not start
implementing until they say "go" or "approved" or equivalent.

### Phase 3: implement

Work through PLAN.md's sequencing in order. After each numbered step:

- Run `hull test` (and any other relevant `make` targets).
- Boot the app with `hull dev` and verify the new surface manually
  (or via `hull agent request` if you can't open a browser).
- Commit with a focused message (one logical change per commit).

When you encounter ambiguity, ask the human. **Do not invent
requirements.**

---

## Hull in one page (inline summary)

Read the canonical docs (above) for depth. This is the mental model
in five minutes.

### What Hull is

A single static binary (~6.5 MB full) that runs applications written in
**Lua 5.4** OR **QuickJS** (ES2023) - either HTTP-serving or `app.main`
CLI/compute. Apps are dynamically loaded from a source tree at startup OR
composed into a standalone binary via `hull build` (which produces a
binary tailored to the app - a compute-only app links ~2.1 MB). The `hull`
binary embeds the runtimes, the standard library, an HTTP server (Keel),
SQLite, mbedTLS, a WASM runtime (WAMR), image codecs, and a kernel-level
sandbox (Seatbelt on macOS, unveil/pledge polyfill on Linux/Cosmo, native
on OpenBSD) - and `hull build` composes back into the app only the pieces
it actually uses. A GPU runtime (wgpu-native) and the DuckDB / Postgres /
MySQL backends are opt-in composable features (`--with=`), native-only.

### One runtime per app

Pick Lua OR JavaScript at startup, not both. The entry file extension
decides (`app.lua` → Lua, `app.js` → JS). Both runtimes have the same
stdlib surface - pick on team preference. Most reference apps ship
both variants side-by-side as a parity check.

### Capability sandbox

Apps **cannot** call SQLite, the filesystem, or the network directly.
Every external interaction goes through a C-layer capability function
(`hl_cap_db_*`, `hl_cap_fs_*`, `hl_cap_http_*`, `hl_cap_env_*`,
`hl_cap_crypto_*`, `hl_cap_smtp_*`). The script-visible globals
(`db`, `fs`, `http`, etc.) are thin wrappers that call those C
functions. SQL is **always parameterized**. Filesystem paths are
**always validated**.

### Manifest is the contract

Every app declares its contract via `app.manifest({...})` at the top
of the entry file. Sample:

```lua
app.manifest({
    modules = {
        "hull/db@1",
        "hull/log@1",
        "hull/template@1",
        "hull/web/htmx@1",
        "hull/web/htmx/confirm@1",
        "hull/web/htmx/search@1",
        "hull/web/htmx/sort@1",
        "hull/web/htmx/form@1",
        "hull/web/htmx/table@1",
        "hull/web/htmx/pagination@1",
        "hull/web/htmx/toast@1",
        "hull/web/htmx/inline-edit@1",
        "hull/web/middleware/session@1",
        "hull/web/middleware/csrf@1",
        "hull/web/middleware/csp@1",
        "hull/web/auth-flows@1",
        "hull/web/middleware/totp@1",
        "hull/web/middleware/audit-log@1",
        "hull/web/auth-health@1",
    },
    fs = {
        read  = {},                  -- read-only paths under app root
        write = { "data/" },         -- writable paths (for blob.init etc.)
    },
    hosts  = { "api.example.com" }, -- HTTPS hosts http.fetch can reach
    env    = { "STRIPE_API_KEY" },  -- environment variables env.get can read
    csp    = "htmx",                -- preset CSP policy, or a literal string
})
```

What you put in `modules` declares which stdlib modules the app may
`require()` / `import`. What you put in `fs` / `hosts` / `env` is
enforced **at call time** by the cap layer + kernel sandbox. The two
gates are independent - declaring `http-client` doesn't open the
network unless you also list `hosts`.

### Standard library - the parts you'll likely use

| Layer | Module | Use |
|---|---|---|
| **HTTP** | `hull/web/htmx` | HTMX response helpers (retarget, reswap, trigger, push_url, refresh). |
| | `hull/web/htmx/toast` etc. | The eight widget tier. **See docs/htmx_widgets.md.** |
| | `hull/web/middleware/session` | Server-side sessions backed by SQLite. Sliding + absolute TTL. |
| | `hull/web/middleware/csrf` | Stateless HMAC-token CSRF. |
| | `hull/web/middleware/csp` | Per-request CSP nonces (for inline scripts/styles). Use the `csp = "htmx"` preset instead if your app doesn't need nonces. |
| | `hull/web/middleware/idempotency` | Idempotency-Key replay protection on POST. |
| | `hull/web/middleware/oauth` | OIDC Authorization Code + PKCE. Google, Microsoft, generic. |
| | `hull/web/middleware/totp` | TOTP 2FA. |
| | `hull/web/auth-flows` | Register / verify / login / reset / magic-link / 2FA / email-change. ⭐ |
| | `hull/web/middleware/audit-log` | Sign-in events + device fingerprinting. |
| | `hull/web/auth-health` | Probes for the auth stack. |
| | `hull/web/flash` | One-shot flash messages. |
| | `hull/web/pagination` | Page math (used by `htmx/pagination`). |
| | `hull/web/form` | `application/x-www-form-urlencoded` parser. |
| | `hull/web/cookie` | Parse / serialize cookies. |
| **Data** | `hull/db` | SQLite query, exec, batch, transactions. |
| | `hull/blob` | Content-addressed blob storage. |
| | `hull/attachment` | File attachments backed by blob. |
| | `hull/web/attachment-serve` | Serve attachments with Content-Type + ETag + Range. |
| | `hull/mime` | MIME sniffer (magic-bytes). |
| **Crypto** | `hull/crypto` | SHA, HMAC, PBKDF2, Ed25519, NaCl, random. Asymmetric verify (RS/ES). |
| | `hull/jwt` | Sign + verify (HS256 + asym). |
| **Misc** | `hull/template` | HTML template engine. Cached. |
| | `hull/validate` | Declarative input validation. |
| | `hull/log` | Structured logging. |
| | `hull/time` | Time / date / now. |
| | `hull/i18n` | Locale detection, translations, formatting. |
| | `hull/csv` | RFC 4180 parse + encode. |
| | `hull/qrcode` | QR Code generator. |

Run `hull modules available --json` for the live, complete list with
deps + capability requirements. Don't trust this table to be exhaustive
forever - Hull evolves.

### Tooling - the parts you'll likely use

| Command | Use |
|---|---|
| `hull dev` | Run from source tree. Watches files, restarts on change. |
| `hull build` | Compile app + embedded VFS into a standalone binary. |
| `hull test` | Run `tests/test_*.lua` (or `.js`) with `:memory:` DB. |
| `hull check` | Validate manifest before tests fire. |
| `hull migrate` | Apply pending `migrations/*.sql`. |
| `hull agent <subcmd>` | Machine-readable introspection: routes, schema, modules, capabilities, context, deploy. Use this when you need to know what the app currently does. |
| `hull modules available` | Canonical first-party module list. |
| `hull doctor` | Environment check (CA bundle, platform embed, compilers). |

---

## The "Hull-native" mindset

Cheat sheet for what "doing it the Hull way" means.

### Hypermedia, not SPA

Hull is built for **server-rendered HTML returned as fragments via HTMX**.
Do not introduce:

- React, Vue, Svelte, Solid, Lit, htmx-bundle-on-CDN. Use vendored HTMX
  (already in scaffold output) + Pico classless (already in scaffold
  output) + Hull's widget tier.
- npm / yarn / webpack / vite. Hull apps have no JS build step. The
  widget tier ships its JS via Hull's platform VFS (`/static/hull/...`).
- Client-side state management. State lives in the server's session,
  the URL, or the DB. The browser is a thin renderer.

If the spec implies a SPA (a feature genuinely requires complex
client state - real-time canvas, in-browser data manipulation),
**raise it in Phase 1** as a clarifying question. Don't silently
build one.

### Capabilities over libraries

If you need to do X, **check the capability layer first**. Don't
shell out, don't `require("os")` (it's not loaded), don't reinvent.

| You want to … | Use … |
|---|---|
| Read a file | `fs.read()` after declaring path in `manifest.fs.read` |
| Write a file | `fs.write()` after declaring path in `manifest.fs.write` |
| Make HTTPS calls | `http.fetch()` after declaring host in `manifest.hosts` |
| Run SQL | `db.query()` / `db.exec()` - ALWAYS parameterized |
| Sign / verify | `crypto.*` (don't bring in another crypto lib) |
| Hash passwords | `crypto.verify_password()` (PBKDF2, ready to use) |
| Send email | `hull/smtp` after declaring SMTP host |
| Generate IDs | `crypto.random(n)` - don't roll a "secure random" yourself |

### Auth: don't roll your own

If the spec mentions user accounts, password reset, email
verification, 2FA, or magic links: **the answer is
`hull/web/auth-flows@1`.** It went through 13 rounds of independent
audit and ships with TOTP, OAuth, audit logging, and lockout policies.
Read `docs/security.md` and the auth-flows section of CLAUDE.md.

### Widget tier: the right reach order

For UI work, ask in this order:

1. Is there an existing widget that does this? (See `docs/htmx_widgets.md`.)
2. If not, can I compose existing widgets? (e.g., `htmx/table` composes
   sort + inline-edit.)
3. If not, can I render bare HTMX + Pico without new JS?
4. If not, can I add small structural CSS + minimal JS in app's `static/`?
5. If not, **stop and flag a platform gap** (see PLATFORM_GAPS.md
   protocol below).

### Sandbox is your friend

Declare everything in the manifest. Use `csp = "htmx"` preset
unless you have specific reasons to use the nonce-based middleware.
The kernel sandbox will catch your mistakes; treat its complaints
as the truth, not as something to bypass.

### Reproducibility matters

`make` produces a byte-identical binary across runs. `hull build` does
the same for app binaries. **Do not introduce build steps that break
this** (random seeds in code generation, timestamps in embedded
content, etc.).

---

## Anti-patterns: do not do these

| ❌ | ✅ |
|---|---|
| `require("lfs")` or `import "fs"` directly | Use the `fs.*` capability after declaring paths in manifest. |
| `os.execute()` / `child_process.spawn()` | Not available in the sandbox by design. If you genuinely need to shell out, flag a platform gap. |
| Concatenating user input into SQL | Always parameterize. `db.query("... WHERE id = ?", { id })`. |
| Inline `<script>` without nonce or strict CSP | Use the `csp = "htmx"` preset (no inline scripts) OR `csp` middleware with per-request nonces, OR ship the JS from `static/` and reference by `<script src>`. |
| Vendoring npm packages for client code | All client JS comes from the widget tier (`/static/hull/...`) or a single vendored file in `static/vendor/` with a SHA in the Makefile. |
| `Date.now()` for "secure" tokens | `crypto.random()` for any token that affects security. |
| Writing your own session table | `hull/web/middleware/session@1`. |
| Writing your own password hashing | `crypto.hash_password()` / `crypto.verify_password()` (PBKDF2). |
| Hard-coding admin emails in handler logic | Use RBAC (`hull/web/middleware/rbac@1`) and seed roles in a migration. |
| Adding `node_modules/` or `vendor/` Lua deps | Hull's stdlib is the answer 95% of the time. If it isn't, flag a platform gap. |
| Assuming `gpu.*` or a `postgres://` / `mysql://` / `duckdb://` DSN work everywhere | They're opt-in composable **features** (`hull build --with=gpu\|duckdb\|postgres\|mysql`, native-only). Compose the feature, or declare the module optional (`"hull/gpu@1?"`) and branch on `gpu.available()`. |
| Bypassing the manifest because "it works" | The manifest is the contract. If you have to bypass it for the app to work, that's a platform gap, not a workaround. |
| Skipping `hull test` because "I'll test later" | Each PLAN.md step should end with tests passing. No exceptions. |

---

## Platform gap protocol

You will sometimes hit a wall: Hull is missing a primitive, an API has
the wrong shape for your use case, the docs don't cover your scenario,
or the widget you need doesn't exist. **Do not silently work around
these.** They are signal - the maintainer wants to know.

### How to recognize a platform gap

| Sign | Probably a platform gap | Probably just an app concern |
|---|---|---|
| Multiple apps would benefit from this | ✅ | ❌ |
| It involves the capability layer, sandbox, runtime, or stdlib | ✅ | ❌ |
| You're considering bypassing the manifest or capabilities | ✅ | ❌ |
| You're about to ship a "this is fine" workaround | ✅ | ❌ |
| It's specific to this app's business logic | ❌ | ✅ |
| It's a missing widget that's clearly app-specific | ❌ | ✅ |
| It's an API the caller could trivially wrap | ❌ (mostly) | ✅ |

If in doubt: **flag it.** False positives cost the maintainer 30
seconds to triage; false negatives cost everyone a workaround.

### How to flag

Create or append to `PLATFORM_GAPS.md` in the **app's repo** (not
Hull's). Use this template per entry:

```markdown
## YYYY-MM-DD - <one-line title>

**Context**: What were you trying to build when this came up? Cite the
spec section + the relevant Hull doc you were trying to use.

**The gap**: What Hull primitive was missing, insufficient, or
wrong-shaped? Include error messages, API signatures you wished
existed, and the closest existing thing.

**Why this is a platform concern**: Why coding around this in the app
would be wrong / why other apps would benefit.

**Workaround used (if any)**: What you ended up doing in the app, OR
"none - blocked, asking human".

**Suggested platform addition**: Be concrete. Function signature,
manifest schema, capability name, widget API. The maintainer will edit;
your job is to give them a clear starting point.
```

After writing the entry: **continue with the app**. Use the most
sensible workaround you can defend to a future reader, OR ask the
human if you genuinely can't proceed. Don't block on the gap being
filled.

The human will periodically read `PLATFORM_GAPS.md` and decide what
to upstream into Hull.

---

## Hull patterns: quick reference

The 80% you'll need; the rest is in the docs.

### Manifest at the top of `app.lua`

```lua
app.manifest({
    modules = { "hull/db@1", "hull/log@1", "hull/web/htmx@1", … },
    fs      = { write = { "data/" } },          -- if you write files
    hosts   = { "api.example.com" },             -- if you call out
    env     = { "MY_API_KEY" },                  -- if you read env
    csp     = "htmx",                            -- preset
})
```

### Migrations live in `migrations/`

`migrations/001_init.sql`, `002_<name>.sql`, etc. Run automatically
on `hull dev` / `hull test`, or manually via `hull migrate`.

### Routes register at module load

```lua
app.get("/", function(req, res)
    res:html(template.render("pages/home.html", data))
end)

app.post("/items", function(req, res)
    -- parse body, validate, db.exec, return fragment
end)
```

### Templates live in `templates/`

`{{ var }}` HTML-escaped, `{{{ var }}}` raw (rare - usually you want
the widget helpers' `{{ widget_attrs | raw }}` pattern), `{% if %}` /
`{% for %}` / `{% include %}` / `{% extends %}` / `{% block %}`.

### Widget pre-render pattern

Templates can't call functions in `{{ }}`. Pre-render widget
attribute strings at the top of the handler (or at module load if
static), pass them through template data, splice with
`{{ name | raw }}`.

```lua
local DELETE_CONFIRM_ATTRS = confirm.attrs("Delete?", { danger = true })

-- in handler:
res:html(template.render("items/row.html", {
    item = item,
    delete_attrs = DELETE_CONFIRM_ATTRS,
}))
```

```html
<button hx-delete="/items/{{ item.id }}" {{ delete_attrs | raw }}>×</button>
```

### Testing pattern

`tests/test_<feature>.lua` use the in-process test harness:

```lua
local test = require("hull.test")

test("GET / renders home", function()
    local res = test.request("GET", "/")
    test.assert_equal(res.status, 200)
    test.assert_match(res.body, "<h1>")
end)
```

`hull test` runs against an in-memory DB with migrations applied.

### CSRF + session

Both wired in by the standard middleware stack. Forms include a hidden
`_csrf` token; HTMX requests use the same. Session cookie set on first
visit by a `session_bootstrap` middleware (see
`examples/hypermedia_photos`).

### Auth flows

If you need user accounts: configure `hull/web/auth-flows@1` once in
`app.main()`, wire your `users_*` callbacks (or use `standard_users`
adapter), let the module own the routes.

### Building for production

```sh
hull build .                                    # produces ./app binary
./app -p 8080 -d /var/lib/myapp/data.db        # run it
```

**Build flavors.** Your app binary is *already* minimal without a flavor:
the distributed hull's base composes back only the subsystems the app uses
(see the composition note under Phase 0), so a compute app links zero
HTTP/Keel/TLS/SQLite/WASM on its own. `--flavor` is now a thin **preset**
on that composable base: `full` (the default embedded base, a no-op) or
`pure-compute` (validate: reject any HTTP/TLS module at build time - a
build failure rather than a runtime surprise). `--flavor=auto` infers the
minimal flavor from your declared modules. The size win comes from the
composable base, not the flavor, so there is nothing to install:
`hull flavor install pure-compute` reports "preset flavor, nothing to
install". (Pre-built per-flavor platform libs were removed in Phase 4.3.)
See `docs/build_flavors.md`.

**Compiler-free / toolless builds.** `hull build` is **compiler-free by
default** - it emits `app_registry.o` via the object emitter and links,
needing no C compiler at all (`--compiler=system` opts into a system cc;
`--with=` C++ features and cosmo targets fall back to one automatically).
By default it needs only a **linker**: `--linker=lld|zig|lld-static|<path>`.
`hull tools install zig` provides a self-contained per-platform zig bundle
(a static driver + its libc tree + bundled lld) that runs anywhere and
cross-compiles via `--target=`, so even a box with no system toolchain can
`hull build`. `--linker=lld-static` links fully static against the musl
floors (`hull tools install libc-musl-<arch>`). And a cosmo `hull` plus
`hull tools install cosmocc` gives self-contained `hull build` on stock
Windows.

**Composable features (`--with=`).** Some heavy subsystems are NOT in the
base binary: they're opt-in **features** you compose at build time, the
additive counterpart to the subtractive flavors above. Five ship today,
all **native-only (no cosmo)**: **`gpu`** (wgpu-native GPU compute, the
`gpu.*` API), **`duckdb`** (embedded OLAP, a `duckdb://` DSN), **`postgres`**
(pure-C PostgreSQL wire backend, a `postgres://` DSN), **`mysql`** (pure-C
MySQL / MariaDB wire backend, a `mysql://` DSN), and **`tui`** (terminal UI,
`hull/tui`; auto-inferred from the manifest). Install the signed lib then
compose it:

```bash
hull feature install gpu          # or: duckdb / postgres / mysql / tui
hull build myapp/ --with=gpu       # duckdb is C++: add --compiler=system
```

If your spec needs GPU compute, an OLAP/analytics store, or a
Postgres/MySQL database, treat it as a feature - do NOT assume `gpu.*` or a
`postgres://` / `mysql://` / `duckdb://` DSN work on a plain base build.
Either compose the feature, or declare the module optional (next) so the
app degrades gracefully. See `docs/features_and_flavors.md`.

**Companion tools (`hull tools install`).** A few build helpers are
separate signed programs, not part of the hull binary: **`wamrc`** (the
LLVM WASM AOT compiler, for ~50x faster compute modules), the **`zig`**
toolchain-free linker bundle (`--linker=zig`, cross-compiles via
`--target=`), the **`libc-musl-<arch>`** static-link floors
(`--linker=lld-static`), and the **`cosmocc`** bundle (self-contained
`hull build` on stock Windows). `hull build` resolves them from
`~/.hull/tools` → next to the hull binary → `$PATH`. Because `hull build`
is compiler-free by default (it emits `app_registry.o` directly) and needs
only a linker, most boxes build with no extra install; a truly bare box
can `hull tools install zig` for a self-contained linker. (There is **no**
`tcc` - the former embedded/side-loaded TinyCC backend was fully retired;
it is neither vendored, bundled, nor installable.) See
`docs/tools_install.md`.

**Optional modules - graceful fallback.** A trailing `?` on a manifest
spec (`"hull/gpu@1?"`) makes the module optional: on a build without the
capability, `require`/`import` returns nil/null instead of failing app
load, so you can branch to a CPU path. Pattern:
`local gpu = require("hull.gpu"); if gpu and gpu.available() then … else … end`.

Or wrap in `hull deploy` for systemd + Dockerfile + fly.toml generation.

---

## Final reminders for the agent

1. **You are in Phase 0 right now.** Pick the archetype first
   (`hull init --profile htmx` / `hull init --cli` / etc.), then
   move to Phase 1.
2. **Phase 1 starts with required reading**, then 5–10 clarifying
   questions. Don't skip ahead.
3. **PLAN.md before code.** Even for "obvious" features. The human
   will catch misreads of the spec by reviewing the plan.
4. **PLATFORM_GAPS.md is a positive signal.** Maintainers want to
   know what's missing; you flagging gaps is helpful, not annoying.
5. **Ask `hull` before you ask the human or grep the source.**
   `hull modules available`, `hull agent <subcmd>`, `hull modules
   explain`, `hull agent context` - dozens of introspection commands are
   faster and more accurate than reading code. See [When in doubt,
   ask hull](#when-in-doubt-ask-hull).
6. **Hull is opinionated.** When in doubt about whether to use a Hull
   primitive or roll your own: use Hull's. If Hull's doesn't fit,
   that's a gap - flag it.
7. **Tests pass at every step.** `hull test` is fast (in-memory DB);
   run it after every step in Phase 3's sequencing.

Now go pick your archetype (Phase 0).
