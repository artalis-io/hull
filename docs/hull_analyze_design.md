# `hull analyze`

Status: IMPLEMENTED (`src/hull/commands/analyze.{c,h}`,
`stdlib/cli/lua/hull/source/analyze.lua`, `tests/e2e_analyze.sh`; the
`tool.path_kind` + `tool.stdout` bindings in `src/hull/runtime/lua/mod_tool.c`). The
FIRST production consumer of the
`hull.source.lua` analysis layer (see
[lua_source_analysis_design.md](lua_source_analysis_design.md) and
[lua_source_conformance_design.md](lua_source_conformance_design.md)). Step **C** of
the post-build-plan roadmap.

## 1. Goal

A static analyzer for Hull apps that **parses the app's Lua source and reports
diagnostics without running the app or building it**. v1 is a **syntax checker**: it
surfaces the layer's diagnostics (`lua.syntax` and friends) with exact `path:line:col`
positions, a human format and a `--json` mode, and a meaningful exit code. It is the
smallest consumer that proves `hull.source.lua` end to end in front of a user, and it
gives an immediate, useful linter foundation that later grows AST/annotation lint
rules.

Why it matters: today a syntax error in a non-entry file (`routes/users.lua`) only
surfaces when that module is first `require`d at run time. `hull analyze` catches it
across the whole tree, statically, in one shot.

## 2. Non-goals (v1)

- **Not JS.** `hull.source.js` does not exist yet; `.js` files are reported as
  "not analyzed (no JS analyzer yet)" and do not count as errors. When
  `hull.source.js` lands, the same CLI surface picks it up (discovery already sees
  `.js`).
- **Not semantic analysis** (scopes / binding / types). Pure syntax + ranges.
- **Not manifest / module validation** — that is `hull check` / `hull modules`
  (§7). `analyze` is orthogonal: source diagnostics, no app load.
- **Not auto-fix.** Diagnostics are reported, never rewritten.

## 3. Command surface

```
hull analyze [app_dir] [files...] [--json] [--quiet]
```

**Positional resolution (locked, unambiguous).** The FIRST positional is `app_dir`
ONLY when it resolves to a **directory** (`tool.path_kind(p) == "dir"`); then every
remaining positional is an explicit file **relative to that app root**. Otherwise the
app root is `.` and ALL positionals are explicit files relative to `.`. So:
- **no positionals** → root `.`, **walk** mode.
- **one positional, a directory** → root = it, **walk** mode.
- **a directory then more positionals** → root = the dir, **explicit-files** mode
  (the trailing positionals, resolved under the root).
- **first positional not a directory** → root `.`, **explicit-files** mode (all
  positionals).

**Flags.**
- `--json` → machine output (one JSON object on stdout, §6).
- `--quiet` → human-mode only: suppress the per-file "ok" chatter, print diagnostics +
  summary. In `--json` mode `--quiet` is a **no-op** (JSON overrides it; stdout stays
  pure JSON) — documented, not an error.

**Exit codes.**
- `0` — analysis ran to completion on every input and found **zero** diagnostics.
- `1` — analysis ran but is **not clean**: any source diagnostic (`lua.syntax` /
  `lua.unsupported`), any **incomplete** (`lua.limit.*`) or **internal**
  (`lua.internal` or a `parse()` `(nil, err)`) result, or any explicit-target error
  (missing / unreadable / non-regular / non-Lua / outside-root).
- `2` — the invocation could not proceed: unknown flag, or a discovery failure (the
  app root is missing / not a directory / unreadable). **Fail closed** — never a
  silently reduced scan.

## Discovery (walk mode) — deterministic + bounded

`tool.find_files(root, "*.lua")` already returns **sorted**, **regular-file-only**,
**no-symlink-traversal** results (it `lstat`s each entry and never descends a
symlinked dir) and skips dotdirs (`.git`, `.hull`), `node_modules`, and `vendor`.
`analyze` adds a Lua post-filter dropping any path with a segment in the explicit
exclusion set `{.git, .hull, build, vendor, node_modules}` — the `build` segment
covers both top-level `build/` and `site/build/`. The result is a sorted, de-duplicated
list of regular `.lua` files under the root. If the root itself is not a readable
directory, that is a discovery failure (exit 2), not an empty scan.

## 4. What it does (pipeline)

1. **Resolve inputs** (positional rule, §3). Walk mode → filtered `tool.find_files`.
   Explicit-files mode → validate each target (below). Sort + de-duplicate the final
   path list for deterministic output.
2. **Per file**: `src = tool.read_file(path)`; `unit, err = lua.parse(src, { path = rel })`.
   Generous limits so `lua.limit.*` only trips on genuinely pathological input.
3. **Positions**: for each diagnostic with a range, resolve `(line, col)` via
   `unit:line_col(diag.range)` (1-based line, 1-based byte column); a range-less
   diagnostic reports `line/col = null`.
4. **Aggregate + report** (human or JSON), set the exit code.

**Explicit-target validation (no silent skips).** For each explicit file, `analyze`
resolves it under the app root and emits a per-target error diagnostic (not a skip)
for any of: **outside the root** (`analyze.outside_root`), **missing**
(`analyze.not_found`), **not a regular file** (`analyze.not_regular`, e.g. a directory
or device via `tool.path_kind`), **unreadable** (`analyze.unreadable`), or **not
`.lua`** (`analyze.not_lua`). Each marks that target's analysis state `internal` and
drives exit 1. Duplicated paths (after normalization) are analyzed once.

`lua.parse` never raises and returns diagnostics as data, so `analyze` is pure glue +
formatting over the already-conformance-tested layer.

## 5. Diagnostic handling — the full `parse()` boundary + per-file state

`analyze` handles **every** outcome of the `parse()` contract, and each non-clean one
drives exit 1. Each analyzed file carries an **analysis state** ∈
`complete | incomplete | internal`:

| `parse()` outcome | file state | shown as | drives exit 1 |
|---|---|---|---|
| unit, only `lua.syntax` / `lua.unsupported` (or none) | `complete` | `error` per diag (clean if none) | iff ≥1 diag |
| unit with any `lua.limit.*` | `incomplete` | `error (incomplete)` — analysis truncated | yes |
| unit with any `lua.internal` | `internal` | `error (internal)` | yes |
| `(nil, err)` (API misuse / internal failure) | `internal` | `error (internal)` — `err.message` | yes |
| explicit-target error (§4) | `internal` | `error` with an `analyze.*` code | yes |

So a `complete` file may still carry syntax diagnostics (fully analyzed, found
errors); `incomplete`/`internal` mean the file could not be fully/authoritatively
analyzed and is never reported as clean — the same guardrail the conformance gate
enforces. `analyze` runs with generous limits so a legitimate file never trips
`incomplete`; if one does, the message says so (and points at future `--max-*` knobs,
§10) rather than pretending the file is fine. A `clean` run requires every file
`complete` with zero diagnostics and zero target errors.

## 6. Output

**Human** (default) — gcc/clang style, editor-clickable, sorted by (path, line, col):
```
routes/users.lua:12:5: error: '=' expected [lua.syntax]
app.lua:3:1: error: unexpected symbol near '@' [lua.syntax]

hull analyze: 2 errors in 2 files (14 files scanned)
```
Clean:
```
hull analyze: no issues (14 files scanned)
```

**JSON stdout purity.** In `--json` mode **stdout contains exactly one JSON object and
nothing else**. All operational / usage failures (exit 2) print a plain message to
**stderr** and emit no stdout. Human-mode chatter never appears in JSON mode.

**Versioned JSON schema (LOCKED, `schema_version: 1`).** Deterministic: `files` sorted
by path, `diagnostics` sorted by `(path, range.start, code)`.
```json
{
  "schema_version": 1,
  "root": "app",
  "files_scanned": 14,
  "files": [
    { "path": "app.lua",          "state": "complete" },
    { "path": "routes/users.lua", "state": "complete" }
  ],
  "diagnostics": [
    { "path": "routes/users.lua",
      "code": "lua.syntax",
      "severity": "error",
      "message": "'=' expected",
      "range": { "start": 120, "stop": 121 },
      "line": 12,
      "col": 5,
      "state": "complete" }
  ],
  "summary": {
    "errors": 2,
    "files_with_issues": 1,
    "incomplete": 0,
    "internal": 0,
    "clean": false
  }
}
```
- **`root`** — the resolved app root (`.` normalized).
- **`files`** — every analyzed file with its `state` (`complete|incomplete|internal`),
  sorted by path.
- **`diagnostics`** — deterministic, each with `path`, `code` (a `lua.*` or
  `analyze.*` code), `severity`, `message`, `range` (`{start, stop}` half-open, or
  `null` when the diagnostic has no range), `line` / `col` (1-based, or `null` when no
  range), and the owning file's `state`.
- **`summary.clean`** — `true` iff exit 0.

Built with `hull.json` (the tool VM already uses it). The `schema_version` is the
forward-compat contract for agent/editor consumers.

## 7. Relationship to `hull check` / `hull agent`

- **`hull check`** (C) composes `hull modules` (manifest/module validation) + `hull
  test`. It loads/validates the app; it does not statically parse every source file.
  `analyze` is complementary — pure static syntax over the whole tree, no load, no
  tests. They can compose later (`check` could run `analyze` first), but v1 keeps them
  independent.
- **`hull modules analyze`** (the existing `hull.analyze` module) statically compares
  an app's `require`/`import` sites against its `manifest.modules` (undeclared / unused
  modules). That is DECLARATION analysis; `hull analyze` is SOURCE SYNTAX analysis.
  Different questions, different modules — the names are deliberately kept distinct
  (this command's module is `hull.source.analyze`).
- **`hull agent`** exposes JSON introspection for AI agents. `hull analyze --json`
  fits that world directly; a thin `hull agent analyze` alias is a possible later
  convenience, but v1 ships the `--json` flag on the top-level command.

## 8. Architecture

A tool-mode command (the analyzer is Lua consuming a Lua module — the natural fit,
same as `hull build` / `hull init` / `hull deploy`):

- **C**: `src/hull/commands/analyze.{c,h}` — `hl_cmd_analyze` is ~15 lines:
  `return hull_tool("hull.source.analyze", argc, argv, env->hull_exe);`. One row in the
  `dispatch.c` table; grouped under "Diagnostics" in `help.c` (next to `check`). **No
  build-flag gate** — analyze needs no HTTP/DB/WASM, so it is present in every flavor
  (including pure-compute); it runs in the tool VM, which is always built.
- **Module name**: `hull.source.analyze` (the source-analysis CLI, living with the
  layer), NOT `hull.analyze` — that name already backs `hull modules analyze`
  (import-vs-manifest declaration analysis, a distinct check; see §7). No collision.
- **Lua**: `stdlib/cli/lua/hull/source/analyze.lua` — reads the standard `arg` global,
  `require("hull.source.lua")`, and uses `tool.find_files` / `tool.read_file` /
  `tool.path_kind` / `tool.stderr` / `tool.exit` / `hull.json`. The source layer is
  already embedded in the platform VFS (under `stdlib/cli/lua/hull/source/`), so the
  tool VM `require`s it with no new wiring. (Verify at implementation:
  `require("hull.source.lua")` resolves in the tool VM.)
- **Two new tool bindings** (both small, in `src/hull/runtime/lua/mod_tool.c`):
  - `tool.path_kind(path)` → `"dir" | "file" | "other" | nil` (via `stat`, missing →
    nil). Needed for the positional rule (§3) and explicit-target regular-file checks
    (§4); `file_exists` is `access(F_OK)` and cannot distinguish a directory.
    Discovery still uses `find_files` (which `lstat`s and excludes symlinks);
    `path_kind` `stat`s (follows) because an explicitly-named root/target may
    legitimately be a symlink.
  - `tool.stdout(str)` → write verbatim to **stdout** (and flush). Hull routes `print`
    to **stderr** in every Lua VM (`hl_lua_print`), so a command whose primary output
    is DATA needs an explicit stdout channel to satisfy JSON purity (§6). `analyze`'s
    real output (human diagnostics + JSON) goes through `tool.stdout`; `print` /
    `tool.stderr` carry only operational messages.

Net new C surface: the ~15-line dispatcher + one table row + one help line + the two
small `tool.*` bindings. Everything else is Lua over the shipped layer.

## 9. Testing

The parser is already conformance-tested; `analyze` is discovery + validation +
formatting + exit codes, best covered end to end via **`tests/e2e_analyze.sh`**
(+ `make e2e-analyze`). Fixtures are built in a **TMPDIR** at runtime (self-contained,
and deliberately NOT committed — an intentionally-broken `.lua` under `tests/fixtures/`
would otherwise enter the conformance corpus). Required cases:

- **Clean app** → exit 0, "no issues".
- **Syntax error in a NON-entry module** (`routes/*.lua`) → exit 1; the error's
  path/line/col/code present in BOTH human and `--json` output (proves whole-tree
  discovery, not just the entry).
- **Deterministic ordering** → the diagnostics/files order is identical across two
  runs and matches the documented sort.
- **Explicit files**: a valid file (analyzed), a **missing** file, an **unreadable**
  file, a **non-Lua** file, and a path **outside the root** → each yields its
  `analyze.*` diagnostic (never a silent skip) and exit 1.
- **Exclusions**: a `.lua` planted under `build/` (and `site/build/`, `vendor/`,
  `.git/`) is NOT scanned; one outside the excluded dirs IS.
- **JSON purity + schema**: `--json` stdout parses as one object, matches
  `schema_version: 1` with all required keys, and carries NO non-JSON bytes; a usage
  error (exit 2) writes to stderr with EMPTY stdout.
- **Quiet**: `--quiet` suppresses human "ok" chatter but still prints diagnostics; with
  `--json` it is a no-op (stdout stays pure JSON).
- **All three exit codes** exercised (0 clean, 1 diagnostics/targets, 2 usage/discovery
  failure such as a non-existent app root).

The pure aggregation/format/sort helpers may additionally be factored into functions
unit-tested via the vanilla-`lua_State` harness style if the e2e proves too coarse.

## 10. Extension path (documented, NOT v1)

- **AST / annotation lint rules**: undeclared-import detection (static, no run),
  unused `require`s, shadowed locals, `---@deprecated` usage, TODO/FIXME extraction —
  each a small rule over `lua.walk` + `unit.annotations`, behind a rule registry and
  `--rules` / `--disable` flags.
- **JS** via `hull.source.js` (the CLI surface is language-agnostic already).
- **`--max-bytes` / `--max-depth`** knobs to raise limits for the rare huge file.
- **`hull agent analyze`** alias; **`hull check` composition**.

## 11. Decisions (ratified)

1. **v1 scope** — syntax-only (lint rules are the §10 follow-up).
2. **Surface** — directory discovery + explicit files + `--json`, with the locked
   positional-resolution rule (§3).
3. **`lua.limit.*` / `lua.internal` / `(nil, err)`** — all make analysis incomplete
   and exit 1, with honest per-file `incomplete` / `internal` state (§5).
4. **Placement** — top-level `hull analyze` (agents use `--json`).

Contract tightenings folded in: unambiguous positional resolution; deterministic +
bounded discovery (regular `.lua` only, sorted, no symlink traversal, exclusions
`{.git, .hull, build, vendor, node_modules}` covering `site/build`); explicit files
kept inside the root, de-duplicated, with `analyze.*` diagnostics for
missing/unreadable/non-regular/non-Lua/outside-root (no silent skips); the full
`parse()` boundary handled; a versioned JSON schema (`schema_version: 1`) with pure
JSON stdout and stderr-only operational failures; `--quiet` as human-mode suppression,
a no-op under `--json`; discovery failures fail closed (exit 2). One new binding,
`tool.path_kind`.
