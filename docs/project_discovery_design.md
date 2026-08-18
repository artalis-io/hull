# Project Source Discovery — design record

Status: **RATIFIED (with amendments). Slices 1-2 MERGED (#356, #357); Slice 3
implemented; Slice 4 pending.**
Author: (design-first, per the story's required cadence)
Related: `docs/lua_source_analysis_design.md`, `docs/hull_source_scope_design.md`,
`docs/hull_analyze_design.md`, `docs/hull_analyze_lint_design.md`.

## 0. What this is (and is not)

A **host-owned, frontend-neutral project source-discovery system**: Hull's
canonical representation of statically discovered, **annotated declarations**
across an app's source tree. Source text is the input; a normalized
`ProjectDiscovery` value is the authoritative result. The first production
frontend is Lua (adapting the shipped `hull.source.*` layer). JavaScript gets a
genuine, reserved registry slot with an **honest capability model** — no regex
scanner, no parser, no pretence of parity.

**In scope:** one host analysis entry point; one frontend registry +
extension→frontend map; one Lua frontend adapter; one `ProjectDiscovery` model
with deterministic indexes + IDs + structured diagnostics; standalone agent
inspection; `hull dev` integration on the same analyzer; a narrow, tested
`hull build` seam.

**Explicitly NOT in scope** (non-goals, restated so the reviewer can hold me to
them): Query IR / Compute IR / WASM lowering / annotation transformers / a
generic plugin framework / `build.js` / a JavaScript parser / a universal AST /
source execution for discovery / incremental dependency analysis / persistent
identity across arbitrary edits.

## 1. Architectural invariant (target)

```
Hull host (C dispatcher)
    ↓  hull_tool("hull.project.analyze", …)   ← the ONLY entry
Project analyzer            (hull.project.analyze)
    ↓
canonical source discovery  (hull.source.discover — the extracted hardened walker)
    ↓
frontend registry           (hull.project.registry: ext → frontend + capabilities)
    ↓
language frontend           (hull.project.frontend_lua — wraps hull.source.lua)
    ↓
normalized ProjectDiscovery (hull.project.model)
    ├── hull dev
    ├── hull agent <inspect>
    └── hull build context   (integration seam — pass-through, not consumed yet)
```

`build.lua`, a future `build.js`, and future transformers **consume**
`ProjectDiscovery`. They do not scan or parse application source themselves.

## 2. Investigation findings (assumptions verified against the tree)

Every claim below is cited; these are the load-bearing facts the design rests on.

### 2.1 `hull.source.*` — the shipped analysis layer

- Public contract `hull.source.lua` (`stdlib/cli/lua/hull/source/lua.lua`):
  `M.parse(source, {path?, limits?}) -> (unit, err)`. Syntax problems land in
  `unit.diagnostics` with `err == nil`; `err` is reserved for API misuse /
  internal failure; **never raises, never prints** (lua.lua:1-20, 60-90). Live
  surface: `unit.ast` (chunk), `unit.tokens`, `unit.comments`,
  `unit.diagnostics`, `unit:text/position/line_col(range)`,
  `unit:annotations_for(node)`, `M.walk`, `M.is`.
- AST declaration node shapes (`stdlib/cli/lua/hull/source/parser.lua`):
  - `local_declaration` = `{ kind, names=[{name, attrib?, range}, …], values=[…], range }`
    — **a multi-name `local a, b = …` is ONE node** (parser.lua:535, name
    entries at :533).
  - `function_declaration` = `{ kind, name=<name|field-chain>, is_local,
    is_method?, params, body, range }` (parser.lua:495, 508). `function a.b.c()`
    → nested `field` chain; `function a:m()` → `is_method=true`.
  - `assignment` = `{ kind, targets, values }` (parser.lua:556) — **not a
    declaration.** A global `X = function() end` is an assignment carrying a
    `function_expr` value, and the annotation layer does not attach to it.
- Annotation layer (`stdlib/cli/lua/hull/source/annotations.lua`): attaches
  **only** to `local_declaration` + `function_declaration` (`DECLARATION_KINDS`,
  annotations.lua:41-44). On the node: `node.annotation_list` (ordered array of
  `{name, args?, text?, raw, range}`, annotations.lua:70) and `node.annotations`
  (name → first). Whitelist-free; unknown `@names` are recorded, never errors.
- Scope resolver (`stdlib/cli/lua/hull/source/scope.lua`): `M.resolve(unit) ->
  (scope, err)`, pcall-guarded; `scope.bindings[]` +
  `scope.ref_of[node]`. Ships today (kind `local|localfunc|param`, `reads`,
  `writes`, `shadows`).

### 2.2 The hardened discovery path (`hull analyze`)

`stdlib/cli/lua/hull/source/analyze.lua` already implements the exact bounded,
containment-safe walker the story mandates reusing:

- `EXCLUDE_LIST = {".git", ".hull", "build", "vendor", "node_modules"}` pruned
  **during** traversal via `tool.find_files(root, "*.lua", {exclude_dirs=…})`
  (analyze.lua:27, 151), plus a cheap `excluded()` segment belt (analyze.lua:60).
- Canonical containment on `tool.realpath` output (symlinks resolved):
  `inside_root(canon_root, canon_target)` (analyze.lua:54, 297-316). Root-canon
  failure is an **operational error, not a lexical fallback** (a locked
  invariant from the analyze reviews).
- `tool.path_kind` for dir/file/other classification (analyze.lua:136, 316);
  `op_fail` → exit 2 stderr-only so JSON stdout stays pure (analyze.lua:78).
- Deterministic order (`tool.find_files` returns sorted, regular, non-symlink),
  fail-closed on discovery error (analyze.lua:152).

These helpers (`normalize`, `join`, `inside_root`, `excluded`, the
`find_files`+`realpath`+`path_kind` sequence) are the extraction target.

### 2.3 Tool VM boundary

- `hull analyze` runs in the **Lua tool VM** via `hull_tool("hull.source.analyze")`
  (`src/hull/commands/analyze.c`), and `analyze.lua` already
  `require("hull.source.lua")`. **Therefore `hull.source.*` is resolvable in the
  tool VM** (a sub-agent guessed otherwise — that guess is wrong; the shipped
  `hull analyze` proves resolution). New `hull.project.*` modules resolve the
  same way.
- `tool.*` bindings relevant here (`src/hull/runtime/lua/mod_tool.c`):
  `find_files(dir, pat, {exclude_dirs, include_vendor})`, `path_kind`,
  `realpath`, `read_file`, `file_exists`, `file_mtime`, `stdout`, `sha256_file`;
  orchestration `modules_resolve`, `build_caps` (`tool_orchestration.c`). The
  tool VM never has HTTP/DB/network.
- Tool mode is **Lua-only by design** (CLAUDE.md "Command Dispatch"): there is no
  JS tool VM. This is why the project analyzer is a Lua tool module even though
  it will one day host a JS *frontend*.

### 2.4 `hull dev` + `--agent` sidecars

- `src/hull/commands/dev.c`: forks a child `hull` per run; **poll-based** mtime
  watcher (1s, `scan_mtime` dev.c:101-132) over `.lua/.js/.html/.wgsl/.sql/.json`,
  skipping dotdirs + `node_modules/vendor/build`; reload = SIGTERM child + refork
  (dev.c:561-578). No file-watch library; no existing generation counter (the
  TUI's `reload_count` is in-memory only, `dev_state.h`).
- `--agent` sidecars under `{app_dir}/.hull/`: `dev.json`
  (`{"port","pid","started_at"}`, dev.c:41-52, written per spawn, removed on
  exit) and `last_error.json` (`{"error","timestamp"}`, written by the CHILD's
  `serve.c:1076-1104` on load failure via `sh_json_*`, cleared on success). Dir
  created 0755 (dev.c:33-39). Readers: `hull agent status` (reads dev.json,
  probes the port — hybrid), `hull agent errors` (passes `last_error.json`
  through — pure sidecar) in `src/hull/agent/request.c`.

### 2.5 `hull agent` surface + the `inspect` question

- ~31 subcommands dispatched in `src/hull/commands/agent.c:738-814`. The one
  closest in spirit is **`hull agent overview`** (`src/hull/agent/overview.c`):
  a standalone, filesystem-only composite summary (`runtime`, `routes`,
  `compute_modules`, `gpu_shaders`, `migrations`, `modules_declared`, `tests`,
  `build_ready`).
- **`hull agent inspect` does NOT exist.** A **top-level `hull inspect` DOES
  exist** (`src/hull/commands/inspect.c` → `hull_tool("hull.inspect")`) and
  inspects a **built binary** (manifest + signature). Different noun (binary vs
  project), different dispatch path, so no *dispatch* collision — but a real
  *semantic-overlap* risk (see Decision 8).
- Agent JSON is emitted via `ShJsonWriter`; **none of the existing agent
  commands carry a `schema_version` field** (overview/deploy/routes verified).
  Our new surface will add one (the source/analyze layers already version their
  JSON at `schema_version = 2`).
- Established pattern: standalone-first (overview, deploy), hybrid sidecar+probe
  (status), pure-sidecar (errors). This licenses our Decision 9.

### 2.6 `hull build` context flow

- `src/hull/commands/build.c` → `hull_tool("hull.build", argv…)`: **only argv is
  passed, no structured context** (build.c:12-15).
- `build.lua main()` (`stdlib/cli/lua/hull/build.lua:1698`): `parse_args()` →
  `discover(opts)` (build.lua:1756) → manifest extract + `tool.modules_resolve`
  (build.lua:1781-1783). `discover()` (build.lua:1453-1696) runs **several**
  per-type `tool.find_files` walkers (lua/js/json/html/static/sql/wasm/wgsl) and
  returns a plain multi-field ctx (build.lua:1690). **Source is not parsed at
  build time** for declaration discovery; module resolution is manifest-driven.
- Narrowest seam: between `parse_args()` and `discover()` in `main()` — a
  pre-flight where the host analyzer can be invoked and its result stashed on
  ctx, without touching the embedding walkers.

### 2.7 Existing "source-walk" surfaces (relationship, not replaced)

`src/hull/agent/capabilities.c` and `src/hull/agent/validate.c` are **substring /
heuristic** scanners (self-described: capabilities.c is "a substring scan… a
heuristic"). They are C-side and crude. The new AST-based analyzer is a strict
improvement, but **this story does not replace them** — it stands beside them.
(A later story may re-point `capabilities` at real declarations; out of scope.)

## 3. Surfaced conflicts / ambiguities (require a decision)

1. **`hull agent inspect` vs top-level `hull inspect`** — no dispatch collision,
   but a naming/semantic overlap (project model vs built binary). Decision 8.
2. **Ownership language** — "host-owned" is satisfied by a Lua tool module +
   single C entry (Decision 1); flagged so the reviewer can reject if they want
   a C-native service.
3. **Multi-name Lua declarations + annotation application** — the AST gives one
   node for `local a, b`; the normalized model can be per-name or per-group.
   Decision 4.
4. **Dev failure semantics** — block reload / preserve previous valid / publish
   invalid generation. Decision 7.
5. **Which unannotated declarations are public** — Decision 4 (retention).

No other collisions found (`hull.project.*` is an unused module namespace;
`e2e_project_discovery.sh` is an unused test name; `.hull/discovery.json` is an
unused sidecar name).

## 4. Required design decisions (each explicit; recommendation + alternatives)

### D1 — Host implementation boundary → **trusted Lua tool module + thin C entry (hybrid, Lua-leaning)** [RATIFIED]

The analyzer is a Lua tool module `hull.project.analyze` — the **single canonical
implementation** of project discovery. The C dispatcher `hull agent inspect`,
`hull dev`, and (later) any build consumer all route through it via `hull_tool`.
"Host-owned" is ownership + lifecycle, not access control: `build.lua` and other
trusted tool modules *can* technically `require` it, and that is fine. Host
ownership is enforced by **every official consumer using the canonical module**
(and by review), not by module-access restrictions — there is no attempt to make
it the only *callable* entry, only the one *authoritative* one. No consumer
re-implements discovery or parses application source independently. Rationale:
`hull.source.*` is pure Lua in the tool VM; a C reimplementation would duplicate
the parser; `hull analyze` already proves the pattern. *Alternative:* C-native
service — rejected (parser duplication, no payoff for a static, non-hot path).

### D2 — Extract/reuse the hardened walker → **extract into `hull.source.discover`; refactor `hull analyze` onto it**

Move `analyze.lua`'s `normalize/join/inside_root/excluded/EXCLUDE_LIST` + the
`find_files(exclude_dirs)` + `realpath` containment + `path_kind` sequence into a
new `hull.source.discover` module exposing e.g. `discover(root, {ext}) ->
(files[], err)` (canonical, contained, sorted, fail-closed) and the containment
helpers. `hull.source.analyze` refactors to call it (its **existing 25-case
`e2e_analyze.sh` is the no-regression proof**). `hull.project.analyze` uses the
same module. **No second walker.** Placing it under `hull.source.*` keeps the
dependency direction clean (`project` depends on `source`, never the reverse).

### D3 — Frontend registry + capability vocabulary → **plain table registry; 4-capability vocabulary**

- Registry `hull.project.registry`: a sorted table mapping extension →
  `{language, frontend_module, capabilities, analyzable}`. One canonical map.
  Adding a frontend = one row; **no edits to dev/agent/build/consumers** (they
  ask the registry).
- Frontend **contract** is a plain table of functions (Hull's table/registry
  convention, not an OO interface):
  `{ language, extensions, capabilities, parse(source, path) -> (result, diags),
  declarations(result) -> decl[], decl_name(d), decl_kind(d), decl_range(d),
  decl_annotations(d), decl_handle(d) }`. `decl_handle` is a **generation-local
  opaque** value (an integer index into that generation's frontend table) — not
  a pointer, not serialized, not stable across generations.
- Capability **vocabulary** (smallest justified by real consumers):
  `declarations`, `annotations`, `source_ranges`, `scope`. **Not**
  `bindings`/`semantic_analysis`/`lowering` — no consumer yet; advertising them
  would violate "do not advertise capabilities that are not implemented." Lua
  reports all four (scope ships). *Consumers talk only to this contract, never
  to Lua AST field layouts.*

### D4 — Multi-name Lua declaration + annotation semantics → **per-name facts with a shared `group_id`; annotations carry `target_group_id`; annotated-only public retention** [RATIFIED: 4a]

The AST gives one `local_declaration` node for `local a, b = …` with
`names=[{name,range},…]` and one `annotation_list`. **Chosen (4a): one
declaration fact per NAME.** `local a, b` → two facts. Each fact's `range` is
that name's own range; both carry:

- a shared **`group_id`** = the identity of the declaration NODE
  (`<language> ":" <rel_path> ":" <node_kind> ":" <node.range.start> "-"
  <node.range.stop>`), identical for every name of the same statement; and
- the **same** `annotations` list, where **each annotation carries
  `target_group_id = group_id`** — making explicit that the annotation
  originated on the declaration *group*, not independently on each name.

So a future lowerer can see, per annotation, that it came from a multi-name
group and **deliberately** dedupe or reject annotated multi-name groups (rather
than silently applying one `@type` to two unrelated names). A single-name
declaration still has a `group_id` (a group of one), and its annotations'
`target_group_id` equals that group — uniform, no special case. Rejected (4b):
one per-node group fact — non-uniform (`name?` → `names[]`), and by-name lookup +
the ID scheme would need special-casing.

Normalization of the other sub-questions (all confirmed):
- **Dotted/method names:** walk the `function_declaration` name chain →
  normalized string `a.b.c` (dotted) / `a:m` (method). Store `name="a.b.c"` +
  `kind = "function"` with `is_method` folded into a `receiver`/flag field, so a
  consumer never parses field layouts.
- **Nameless declarations:** none of the in-scope kinds are nameless (an
  anonymous `function()` is the *value* of a named `local`/assignment, not a
  declaration). Guard anyway: a decl whose name can't resolve to a stable
  identifier gets `status="unnamed"` and is kept out of the public annotated set.
- **Retention (public vs internal) [RATIFIED]:** the public `declarations[]` holds
  **annotated declarations only** — the model's stated purpose is "statically
  discovered, *annotated* declarations," and the future consumers (Query/Compute
  IR) key off annotations. The generation **retains total counts** in the summary
  (`declarations_total`, `declarations_annotated` per source and overall) AND
  **retains the full per-source declaration data internally** (an internal
  `_by_source` index carrying every discovered declaration, annotated or not) so
  a later consumer can reach unannotated declarations without re-analysis. Only
  the *public serialized* `declarations[]` is filtered to annotated facts;
  nothing is discarded.

Kinds emitted (Lua frontend): `local`, `local_function`, `function` (with
`is_method`). Recommend NOT emitting bare `assignment`-as-global-function in
this story (it is not an annotation-attach target; adding it means widening the
annotation model — out of scope).

### D5 — Deterministic declaration ID → **`language ":" rel_path ":" kind ":" name ":" start "-" stop`** [RATIFIED]

Per-name `id` (uniquely identifies a declaration fact); `group_id` (D4) is the
node-level variant (`… ":" node_kind ":" node.start "-" node.stop`). `rel_path`
is the canonical **project-relative** path (from the contained realpath, minus
the root prefix). Deterministic within a generation, textual, no
pointers/table-identities/addresses. For a multi-name declaration, the per-name
`start-stop` disambiguates each `id` while the shared `group_id` links them.
**Not** persistent across arbitrary edits (a non-goal) — identity is a
within-generation key, documented as such.

### D6 — In-memory vs serialized → **in-memory Lua table is authoritative; JSON is the wire/sidecar projection** [RATIFIED]

The `ProjectDiscovery` Lua table is the authoritative in-process value. A
`schema_version`'d JSON projection is produced for (a) agent inspection stdout
and (b) the dev sidecar. There is **no persistent generation store / cache** in
this story (no incremental). Opaque frontend handles are **never** serialized.

**The `ProjectDiscovery` model (serialized shape):**

```
{ schema_version, generation, project_root,
  valid,        // the analysis ran with no analyzed source rejected + no internal/operational failure
  complete,     // every APPLICATION source had an analyzable frontend (see below)
  sources[]     = { path, language, role, status },   // role ∈ app|asset; status ∈ analyzed|unsupported|error
  frontends[]   = { language, extensions, capabilities[], analyzable },
  declarations[]= { id, group_id, language, path, kind, name, range,
                    annotations[] = { name, args?, value?, range, target_group_id, frontend, raw? },
                    status },      // PUBLIC: annotated facts only (D4)
  diagnostics[] = { severity, code, message, path, range?, language? },
  summary       = { sources_total, sources_analyzed, sources_unsupported,
                    declarations_total, declarations_annotated, by_language{…} },
  indexes }     // deterministic access (D-indexes): by_annotation, by_source, by_language, by_id, annotated
```

`valid` and `complete` are **independent** axes and a consumer that wants a
"clean, trustworthy generation" checks **`valid && complete`**:
- `valid = false` when any analyzed source produced a parse error (malformed Lua)
  or the analyzer hit an internal/operational failure. (A syntactically-clean
  all-Lua project is `valid = true`.)
- `complete = false` when any **application source** has no analyzable frontend
  (e.g. application JavaScript today). An unsupported *application* source can
  never yield a clean generation, but it does not by itself make the analysis
  `invalid` — it makes it *incomplete*.

**"Application source" is defined narrowly** so browser assets do not poison a
Lua project: it is a discovered file whose extension maps to a **known language**
in the registry (`.lua` today; `.js/.mjs/.cjs` reserved) AND that is **not** under
the `static/` asset directory (nor under the hardened generated/dependency
excludes `.git/.hull/build/vendor/node_modules`). Concretely, the project
analyzer's scan prunes `static/` in addition to the standard `EXCLUDE_LIST`
(D2's shared walker takes an `extra_exclude` list; `hull analyze` passes none, so
its behavior is unchanged). A `static/app.js` browser asset is therefore never
discovered as application source and never affects `complete`; a root-level
`app.js` / `routes/x.js` IS application source, is recorded
`role="app", status="unsupported"`, and sets `complete = false`.

### D7 — Dev publication + failure behavior → **publish EVERY new generation (incl. `valid=false`); never preserve stale as current; never gate reload; publish atomically with a session identity** [RATIFIED, amended]

`hull dev` runs the canonical analyzer on startup and on each reload
(deterministic full reanalysis — no incremental). In **`--agent` mode** it
publishes `{app_dir}/.hull/discovery.json` with a monotonic `generation` counter
(consistent scope with `dev.json`/`last_error.json`, which are also
`--agent`-only). **Every** analysis publishes a new generation — including a
malformed/partial one, which publishes `valid=false` (and/or `complete=false`) +
structured diagnostics. It does **not** gate the server reload (discovery is
advisory metadata, not a serving gate) and does **not** silently keep the prior
generation as current (that would let a failed analysis masquerade as clean —
forbidden by the invariant).

**Atomicity + session identity (amendment).** Publication is atomic: write
`{app_dir}/.hull/discovery.json.tmp` then `rename(2)` over the target (a reader
never sees a half-written file). Both `dev.json` and `discovery.json` carry a
**dev-session identity** = the `hull dev` **supervisor (parent) PID** (`session_pid`),
which is stable across reloads — unlike the served child PID, which changes every
reload. `dev.json` currently records only the child/served PID; this adds
`session_pid` (the supervisor) to it and mirrors `session_pid` + `generation`
into `discovery.json`. This closes the spawn/publication race: a reader can bind
a published generation to a specific live dev session and reject a stale or
cross-reload sidecar (see D9). `dev.json` publication becomes atomic (tmp+rename)
for the same reason.

### D8 — Agent command + collision → **RAISE; recommend `hull agent inspect`, with `hull agent discovery` as the collision-free fallback**

`hull agent inspect` is free at the dispatch level. It reads naturally ("inspect
the analyzed project model") and matches the story's own prose. The only risk is
semantic overlap with top-level `hull inspect` (which inspects a *built binary*).
Recommendation: **`hull agent inspect`**, documented as project-model inspection,
distinct from the binary-inspecting `hull inspect`. If the reviewer finds that
overlap confusing, **`hull agent discovery`** (or `hull agent project`) is
collision-free and unambiguous. Schema: `schema_version`, `generation`, `valid`,
`project_root`, `sources[]`, `frontends[]`, `declarations[]` (annotated),
`diagnostics[]`, `indexes` (summary counts). I will not implement until this
name is picked.

### D9 — Standalone vs running-dev equivalence → **dev running: read the published generation; else: one standalone analysis; identical schema** [RATIFIED]

`hull agent inspect`: accept the **latest published generation** only when
`{app_dir}/.hull/discovery.json` exists AND its `session_pid` matches
`dev.json`'s `session_pid` AND that supervisor PID is **live** (the same
liveness idea `hull agent status` uses). Any mismatch or dead session → fall back
to **one standalone analysis**. This rejects a stale or cross-reload sidecar
left by a prior/killed dev session. The sidecar JSON is literally the serialized
standalone output, so equivalence is structural by construction (same analyzer,
same projection). Standalone results carry `generation = 0` + a `source:
"standalone"` marker to signal they were computed on demand rather than published
by a dev session.

### D10 — Build-context seam → **abstraction-only + a documented seam; NO per-build parse** [RATIFIED]

`hull.project.analyze(app_dir) -> ProjectDiscovery` is the host abstraction. For
this story build integration is the **abstraction plus a precisely documented
seam** — build.lua is **not** wired to call the analyzer, and **no build performs
an unused full parse** merely to stash `ctx.discovery`. Making `build.lua`
initiate host analysis before any lowering consumer exists would be premature
work on every build for no consumer. The documented seam: the **first actual
lowering consumer** (future Query/Compute codegen) is what will justify threading
discovery into the build context — at that point build.lua calls
`hull.project.analyze(app_dir)` in `main()` (after `parse_args()`, before
`discover()`) and passes the result to the codegen step; the file-embedding
walkers are untouched. This story ships + tests the host abstraction standalone
and records this exact insertion point, without rewriting the build pipeline or
adding a dormant per-build cost. (Slice 4 therefore documents + tests the
abstraction and the seam; it does not modify `build.lua`.)

### D11 — JavaScript honest behavior → **known language, `analyzable=false`, no parse, honest per-file diagnostic, and it makes the generation `complete=false`** [RATIFIED, amended]

The registry knows `javascript` (`.js/.mjs/.cjs`) with `capabilities={}` and
`analyzable=false`. **`.js/.mjs/.cjs` are NOT registered as analyzable**, so the
scan does not select a frontend for them. An application `.js` file (per the
"application source" definition in D6 — i.e. NOT a `static/` browser asset) is
recorded in `sources[]` with `language="javascript"`, `role="app"`,
`status="unsupported"`, no declarations, and a discovery diagnostic
(`severity="warning"`, `code="project.frontend.unsupported"`), and **sets
`complete = false`** on the generation — an unsupported application source can
never yield a clean, complete generation. It is **never** parsed as Lua and
**never** reported as analyzed. A `static/app.js` browser asset is pruned from
the application-source scan (D6) and does not affect `complete`. A future JS
frontend flips one registry row (`analyzable=true`) + ships an adapter; nothing
else changes, and such files then count as analyzed.

### D12 — Slice plan + gates → below (§5).

## 5. Slice plan

Small, reviewable, each green before the next. All commits `-s`
(`Signed-off-by: Mark Farkas <mark@artalis.io>`), no Co-Authored-By, no
attribution, no em-dashes.

- **Slice 1 — core (host analyzer + Lua frontend + model).**
  - Extract `hull.source.discover`; refactor `hull.source.analyze` onto it (D2).
  - `hull.project.registry` (D3) + `hull.project.frontend_lua` (D3/D4) +
    `hull.project.model` (IDs D5, indexes, diagnostics) + `hull.project.analyze`
    (D1) + a C entry (name pending D8).
  - Vanilla-`lua_State` unit tests (the `hull.source` harness style):
    declarations/annotations/ranges extracted without executing app source;
    multi-name semantics (D4); dotted/method normalization; unknown-annotation
    survival; malformed Lua → `valid=false` + diagnostics; capability reporting
    accuracy; ID determinism; **no consumer touches Lua AST fields** (adapter is
    the only AST-aware module).
  - Gate: `make test`, the source UTEST suite, `luacheck`, and **`e2e_analyze.sh`
    green** (proves the walker extraction is behavior-preserving).

- **Slice 2 — standalone agent inspection (D8/D9/D11). DONE.**
  - `hull agent inspect [app_dir]` (C dispatch in `agent.c` → `hull_tool(
    "hull.project.inspect")` → `hull.project.analyze`). The wire schema lives in a
    SINGLE side-effect-free module `hull.project.projection` (`M.project(disc) ->
    plain table`) that DROPS every generation-internal value (per-decl `handle`,
    `_by_source`, `_handles`, the `by_id` decl map); both standalone inspection and
    Slice 3's `discovery.json` publication call it, so there is one wire-schema
    definition. `hull.project.inspect` follows the tool-module convention (`return
    main`; the dispatcher invokes it only as the entry command -- requiring it
    never runs the CLI). At most one positional root (`inspect a b` → exit 2). Exit
    0 when a discovery was produced (validity is data: `valid`/`complete`); exit 2
    on a usage error. Standalone only (the dev-running path is Slice 3).
  - Gate: `tests/e2e_project_discovery.sh` (25 assertions, wired into CI) over the
    REAL tool VM — deterministic output; annotations discovered without execution;
    unknown annotations survive; malformed → `valid=false` + diagnostics; `.js`
    present but not falsely analyzed (→ `complete=false`) while `static/*.js` is
    pruned; capability reporting; and a leak check that no generation-internal
    state reaches the wire.

- **Slice 3 — `hull dev` integration (D7/D9). DONE.**
  - `hull dev --agent` publishes `.hull/discovery.json` per (re)spawn with a
    monotonic `generation`, by spawning `hull agent inspect <app_dir> --out=…
    --generation=N --session-pid=<supervisor>` (fresh analysis, `source="dev"`,
    projected via the shared `hull.project.projection`, written atomically
    tmp+rename). `dev.json` gains `session_pid` (the supervisor PID, stable across
    reloads) and is itself written atomically; both sidecars are removed on dev
    exit. `hull agent inspect` (C `cmd_inspect`) streams the published generation
    only when `discovery.json`/`dev.json` `session_pid` match AND `kill(pid,0)`
    confirms the supervisor is live (rejects a stale/crashed-session sidecar);
    otherwise it delegates to the tool VM for a standalone analysis. Publish is
    the fresh-analysis path (never reads a published generation → no recursion).
    Scope: the non-TUI `--agent` loop (the agent-facing path); publishing from the
    `hull dev --tui` reload path is a documented follow-up.
  - Gate (e2e, in `e2e_project_discovery.sh`): dev publishes a generation; the
    session_pid matches across both sidecars; a source change → reload → a new
    generation (1→2); sidecars removed on exit; after exit → standalone; and a
    dead-`session_pid` sidecar is ignored → fresh standalone analysis.

- **Slice 4 — build seam (D10), abstraction-only.**
  - Ship + test `hull.project.analyze` as the host abstraction a build consumer
    would call; document the exact `build.lua main()` insertion point for the
    first lowering consumer. **`build.lua` is not modified** and no per-build
    parse is added. A test asserts the abstraction returns the same
    `ProjectDiscovery` a build consumer would receive; the doc records the seam.

**Acceptance (mapped to the story's list):** deterministic output (S1/S2);
annotations discovered without execution (S1/S2); unknown annotations survive
(S1/S2); malformed → structured diagnostics + `valid=false` (S1/S2); accurate
capability reporting (S1); exclusions/containment reuse (S2); standalone agent
inspection (S2); `hull dev --agent` publishes + exposes an equivalent generation
(S3); modify+reload → new generation (S3); no consumer imports Lua parser
internals (S1, enforced by the contract + a test); no `.js` falsely analyzed +
application `.js` sets `complete=false` while `static/*.js` does not (S2);
build ownership as a tested abstraction + documented seam (S4).

## 6. Ratification (sign-off recorded)

All decisions ratified with amendments (folded into D1-D12 above):
1. **D8** — `hull agent inspect` (project model), documented as distinct from
   top-level `hull inspect` (built binary).
2. **D4** — per-name facts with a shared `group_id`; each annotation carries
   `target_group_id` (originated on the declaration group, not per name);
   public `declarations[]` annotated-only; totals + full per-source data retained
   internally.
3. **D7** — publish every new generation incl. `valid=false`/`complete=false`;
   never preserve stale as current; never gate reload; publish atomically
   (tmp+rename) with a `session_pid` (supervisor) identity in both sidecars.
4. **D6/D11** — an unsupported **application** source makes the generation
   `complete=false` (independent of `valid`); "application source" excludes
   `static/` browser assets + the generated/dependency dirs, so browser JS
   cannot poison a Lua project.
5. **D10** — abstraction-only + a documented build seam; `build.lua` unmodified;
   no unused per-build parse. The first lowering consumer justifies threading
   discovery into build context.
6. **D1/D2** — trusted Lua tool module as the single **canonical** implementation
   (ownership by consumer convention, not access control); extract the hardened
   walker into `hull.source.discover` and refactor `hull analyze` onto it.

Implementation proceeds per §5, Slice 1 first.

### 6.1 Slice 1 review fixes (folded in)

Four core-contract fixes from the Slice 1 review, all in the shipped code + tests:

- **Root validation/canonicalization.** `hull.project.analyze` requires
  `tool.realpath(root)` + `path_kind == "dir"` and uses the CANONICAL root for
  containment + relative paths (covers symlinked roots and `/`). A missing /
  non-directory root is a `project.discovery_failed` generation
  (`valid=false, complete=false`), never a valid, complete, empty project (which
  the ENOENT-benign `find_files` would otherwise produce).
- **Protected public boundary.** `M.analyze` wraps the unprotected analysis in
  `pcall`; any frontend/adapter/model defect becomes an INVALID discovery with a
  structured `project.internal` diagnostic — honoring "never raises."
- **Generation-unique, resolvable handles.** The per-file adapter index is gone.
  The analyzer (the generation owner) assigns a **generation-unique** integer per
  declaration and retains `{frontend, unit, declaration}` in an internal
  `disc._handles` table; `M.resolve_handle(disc, h)` is the controlled lookup a
  future lowerer uses to reach frontend-specific semantics through the boundary.
  Handles are generation-internal and excluded from the serialized projection.
- **Scope is callable through the boundary.** The advertised `scope` capability is
  now a real contract method `frontend.scope(unit) -> (scope, err)` (a protected
  wrapper around `hull.source.scope.resolve`), so a consumer never bypasses the
  adapter. (Had it not been made callable, the capability would have been removed.)
