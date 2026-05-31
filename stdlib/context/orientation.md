<!-- minimal -->
## Orientation for AI agents working with Hull

You are working with Hull — a single-binary, capability-secure, local-first
runtime for Lua 5.4 and JavaScript (QuickJS). Apps are one file (`app.lua`
or `app.js`); Hull provides routing, DB, crypto, HTTP, WASM compute, GPU
compute, all sandboxed.

### The fastest path to productive work

1. **`hull agent context --list`** → JSON list of every topic doc bundled
   in this binary. Each doc has `minimal`/`compact`/`full` levels; start
   minimal, bump only if needed.
2. **`hull agent context --task=NAME --level=minimal`** → focused
   snippet for the topic (compute, db, auth, routing, etc.).
3. **`hull agent overview [app_dir]`** → one-shot project summary
   (runtime, routes, modules, tests, build readiness) when dropped into
   an unfamiliar tree.
4. **`hull agent <verb>`** for everything else — `routes`, `modules`,
   `capabilities`, `compute`, `tools`, `manifest`, etc. All JSON. Run
   `hull agent` with no args to see the catalog.

### Picking a starting point by task

| You want to … | Run |
|---|---|
| Bootstrap a web app | `hull agent context --task=quickstart-web --level=minimal` |
| Bootstrap a CLI | `hull agent context --task=quickstart-cli --level=minimal` |
| Bootstrap a TUI | `hull agent context --task=quickstart-tui --level=minimal` |
| Add auth | `hull agent context --task=auth` |
| Use the DB | `hull agent context --task=db` |
| Add WASM compute | `hull agent context --task=compute` |
| Add GPU shaders | `hull agent context --task=gpu` |
| Install side-loaded tools (e.g. wamrc) | `hull agent context --task=tools` |

### Hard rules

- App files MUST be `app.lua` or `app.js` at the app root.
- Every external capability MUST be declared in `app.manifest({modules=...})`
  — see `hull agent context --task=middleware --level=minimal`.
- Build with `hull build .` (auto-handles compute AOT, signature,
  embedding). Don't invoke `clang` directly.

<!-- compact -->
## Agent verb catalog

Every `hull agent <verb>` returns JSON to stdout. The verbs:

**App-shape introspection** (most useful for cold start):
- `overview [app_dir]` — composite summary (runtime, routes, compute, gpu,
  migrations, modules, tests, build_ready)
- `routes [app_dir]` — list of registered routes + middleware
- `modules [app_dir]` — declared first-party modules vs registry
- `capabilities [app_dir]` — source-walk: caps USED vs caps DECLARED
- `manifest [app_dir]` — effective manifest
- `vfs [app_dir]` — embedded files (templates, static, migrations, etc.)
- `compute [app_dir]` — WASM modules + AOT readiness + `wamrc` state
- `gpu [app_dir]` — WGSL shaders + GPU availability
- `tools` — side-loaded tool registry (wamrc, etc.) + install state

**Doc lookup**:
- `context --list` — registry of topic docs
- `context --task=NAME [--level=L]` — fetch one
- `context --interactive` (TUI builds only) — interactive picker

**Lifecycle**:
- `status [app_dir] [-p port]` — is dev server running?
- `errors [app_dir]` — structured errors from last reload
- `test [app_dir]` — run tests via in-process harness
- `migrate [app_dir]` — migration status
- `deploy [app_dir]` — deployment readiness

**Targeted probes**:
- `endpoint METHOD PATH [app_dir]` — preview which handler+middleware would
  fire (no execution)
- `middleware METHOD PATH [app_dir]` — just the middleware stack
- `validate <file>` — parse + sandbox-check without running
- `eval <code> [app_dir]` — one-shot snippet, returns JSON
- `template <name> [data.json] [app_dir]` — render a template with data
- `compute-call <module> <input_file> [app_dir]` — invoke a WASM module
- `request METHOD PATH [opts]` — HTTP request to a running dev server

**DB** (HL_ENABLE_DB=1):
- `db schema [app_dir] [-d path]`, `db query "SQL" [app_dir]`
- `schema-diff [app_dir]`, `sql named <qname> [--params J] [app_dir]`

**Performance/logs**:
- `perf [app_dir]`, `logs [app_dir] [--tail N]`

## Project lifecycle (the prescribed flow)

```bash
hull init myapp --runtime=lua      # or --runtime=js
cd myapp
hull dev                           # hot-reload server
# … edit app.lua, write tests …
hull test                          # in-process HTTP harness
hull build .                       # standalone signed binary
```

**Profile selection at scaffold time.** `hull init` takes a `--profile`
flag for richer starter apps. For server-rendered web apps that need
partial-page updates (forms, lists, optimistic UI), pick the `htmx`
profile. It ships HTMX + Pico classless + per-request CSP nonce +
session + CSRF wired together:

```bash
hull init myapp --profile htmx     # full details: hull agent context --task=htmx
```

For non-server apps (CLI / TUI), `app.main(fn)` replaces route registration
— see `quickstart-cli` / `quickstart-tui`.

## The two planes

Hull splits **orchestration** (Lua/JS sandbox, capability-mediated I/O,
sub-millisecond glue) from **compute** (WASM under WAMR or WGSL on
wgpu-native, no I/O, gas/timeout-metered). Orchestration code calls
compute via `compute.call(name, bytes, opts)` / `gpu.dispatch(...)`.

## Module declaration is the contract

`app.manifest({modules = {"hull/db@1", "hull/crypto@1"}})` is the
trust boundary. The module resolver enforces declared imports at load
time; per-call capability caps (`fs.read`, `env`, `hosts`) are enforced
at call time. Forgetting a `modules` entry fails at module-load with an
exact line + fix-it hint. Run `hull modules available` for the
registry.

<!-- full -->
## Source-of-truth files

The agent context docs are extracted from the codebase, but the
ground truth for behavioral questions lives here. Read these directly
if you need certainty:

| Topic | Authoritative file |
|---|---|
| Build flags, runtime sandbox, capability layer | `CLAUDE.md` (root) |
| Long-form developer guide (20 sections) | `docs/agent_guide.md` |
| Architecture deep dive | `docs/architecture.md` |
| Security model + threat model | `docs/security.md` |
| Module registry semantics | `docs/security.md §5b` |
| WASM compute internals | `docs/wamr_architecture.md` |
| Release signing chain | `docs/release_signing.md` |
| Tools side-loading design | `docs/tools_install.md` |

## Recommended interaction pattern for small models

Hull's `--level=minimal` typically fits in ~400 tokens. For models with
small context windows (Gemma 27B, Qwen 27B class):

1. ALWAYS request `--level=minimal` first. Bump to `compact` only if
   the minimal answer is insufficient. Reach for `full` only when
   actively debugging the topic.
2. Cache `hull agent context --list` and `hull agent overview` results
   for the session — they don't change between calls.
3. Prefer `hull agent endpoint METHOD PATH` over reading source to
   understand a route — the JSON shows exactly which middleware fires
   and in what order.
4. Prefer `hull agent capabilities` over inferring caps from source —
   it cross-checks USED vs DECLARED and surfaces drift.
5. When suggesting code, run `hull agent validate <file>` after
   edits to catch parse + sandbox errors before the user sees them.

## What Hull is NOT

- NOT a Node/Bun/Deno alternative — Hull's runtime is sandboxed by
  design; you cannot `require("fs")` from app code, only the
  capability-mediated `fs.read/write` after manifest declaration.
- NOT a serverless runtime — Hull binaries are long-lived processes
  with sticky in-memory state (WASM pools, GPU buffers, prepared
  statements). Treat them like a daemon, not a function.
- NOT a package manager — `manifest.modules` selects from a fixed
  first-party stdlib registry. There is no `npm install`.
- NOT a build tool you compose yourself — `hull build` is one verb.
  No webpack, no rollup, no bundler config.

## When to refer the user back to source

If the question is about INVARIANTS (what's enforced, what's
sandboxed, what's signed) — quote `CLAUDE.md` or `docs/security.md`
directly. If the question is about WORKFLOW (how to scaffold/build/
test/deploy) — these context docs are sufficient and current.

## See also

- `hull agent context --list` — every topic
- `hull --help` — the full subcommand catalog
- `hull doctor` — what this binary can do (build readiness, embedded
  CA bundle, wamrc presence, etc.)
- https://gethull.dev — the project site
