# `hull analyze` v2 - lint rules (design)

Status: SLICE 1 IMPLEMENTED (engine + structural rules + CLI + schema 2);
slices 2-3 (scope pass + scope rules) pending. Extends the shipped `hull analyze`
(syntax-only,
[hull_analyze_design.md](hull_analyze_design.md)) with a **rule-based linter** over the
AST + comments + a light lexical **scope** pass - still without running or building the
app. This is the "AST/annotation lint rules" follow-up named in §10 of the v1 design.

## 1. Goal

Turn `hull analyze` from a syntax checker into a real linter: flag likely bugs and
dead code that are valid syntax but wrong or wasteful - `unused-local`,
`shadowed-local`, `unused-param`, `empty-block`, duplicate table keys, `TODO`/`FIXME`
markers - each a small **rule** over `lua.walk` + a scope model + `unit.comments`,
behind a **rule registry** with per-rule enable/disable and a `--strict` gate. The
infrastructure (registry, severities, config surface, JSON) is designed so new rules
are ~20 lines each.

Why it matters: these are the errors a syntax check can't see - a `local cfg` that's
never read (a typo'd later reference), a loop var that shadows an outer one, an
`if ... then end` with an empty body. Caught statically, across the whole tree, in one
shot.

## 2. Scope + boundaries

- **Per-file rules** in v2. Cross-file rules (e.g. a `---@deprecated` function called
  from another module) need a project-wide symbol graph - a later version.
- **NOT `undeclared-import` / `unused-require`** - that is DECLARATION analysis
  (require/import vs `manifest.modules`), already shipped as `hull modules analyze`
  (module `hull.analyze`). v2 is orthogonal: syntactic / scope / comment rules. The two
  compose; they do not overlap. (A future `hull analyze` could *surface* modules-analyze
  findings, but it will not re-implement them.)
- **NOT type inference, NOT control-flow/data-flow** beyond simple reachability. Lua
  makes `return`/`break` block-terminating at the syntax level already, so
  "unreachable after return" is a *syntax* error, not a lint.
- **NOT auto-fix** (unchanged from v1).

## 3. The scope pass - `hull.source.scope` (the motivated Step B)

The high-value rules (`unused-local`, `shadowed-local`, `unused-param`,
`undefined-global`) all need **name resolution**: which declaration each `name`
reference binds to. This is the "semantic/binding pass" the roadmap deferred "only if a
consumer needs it" - the linter is that consumer. It ships as a new, reusable
source-layer module `hull.source.scope` (future codegen consumers may want it too).

`scope.resolve(unit)` walks `unit.ast` and produces a **scope model** with Lua 5.4
scoping semantics:
- **Lexical scopes**: the chunk, and each block that introduces bindings - function
  bodies (params + locals), `do`, `while`/`repeat` bodies, `if` clause bodies, numeric-
  and generic-`for` bodies (loop vars scoped to the body).
- **Declaration visibility**: a `local x = ...` is visible only to statements **after**
  it in its block (Lua's "a local is in scope from *after* its declaration"), so
  `local x = x` binds the RHS `x` to an *outer* `x`. `local function f` is visible
  **inside its own body** (recursion) - distinct from `local f = function() end`.
- **Resolution**: every `name` node resolves to (a) a specific local/param declaration
  in an enclosing scope (nearest wins - this is where **shadowing** is observed), or
  (b) an **upvalue** (a local from an enclosing *function*, across a function boundary),
  or (c) a **global** (no binding found). `self` in a `function a:m()` method is an
  implicit first param.
- **Per-declaration usage**: each local/param records whether it is ever **read**
  (a `name` reference that isn't the assignment target of its own declaration). Powers
  `unused-*`. (An assignment to a local - `x = 1` where `x` is local - counts as a
  write, not a read; `unused-local` flags a local written/declared but never read.)

Output shape (attached to the unit, or returned):
```
scope = {
  bindings = { <decl>, ... },     -- every local/param declaration
  -- <decl> = { name, kind = "local"|"param"|"localfunc"|"loopvar",
  --            range, scope_id, reads = <int>, writes = <int>,
  --            shadows = <decl|nil> }   -- the outer decl it hides, if any
  ref_of = <map: name-node -> decl | "global" | "upvalue-decl">,
}
```
The pass NEVER raises (best-effort over a possibly error-bearing AST); an `error` node
or a missing field degrades to "unresolved", never a crash. It is conformance-adjacent:
a dedicated test suite pins resolution on hand-built cases (shadowing, recursion,
`local x = x`, loop-var scope, method `self`, upvalues).

## 4. Rule engine + registry

A **rule** is a small declarative record:
```
{ id = "unused-local",
  severity = "warning",          -- "error" | "warning" | "info"
  default = true,                -- on unless --disable'd
  needs = { "scope" },           -- "scope" pass required? (else AST/comments only)
  describe = "a local variable that is never read",
  check = function(unit, scope, emit)
      -- walk / inspect; emit { code, message, range } per finding
  end }
```
- **Registry**: a sorted table `RULES` in `hull.source.lint` (the new rule module). One
  row per rule; `check` is pure (no I/O, no cross-file state). Adding a rule is one row.
- **Codes**: each finding's `code` is `lua.lint.<id>` (namespaced, distinct from
  `lua.syntax` / `lua.limit.*` / `analyze.*`). Deterministic: findings sorted by
  `(path, range.start, code)` like v1.
- **Engine**: for each file's `unit`, run `scope.resolve(unit)` once (only if any active
  rule `needs` it), then invoke each active rule's `check`, collecting findings. The
  scope pass is computed at most once per file.

## 5. Curated v2 rule set

| id | severity | needs | flags |
|---|---|---|---|
| `unused-local` | warning | scope | a `local` or `local function` (`kind` local/localfunc) never READ. Loop vars (own concern, deferred) and params (→ `unused-param`) are excluded; a leading-`_` name is exempt. |
| `unused-param` | warning | scope | a parameter never read. Exempt ONLY: `_`, any underscore-prefixed name (`_x`), and the implicit method `self`. **No trailing-unused-run suppression by default** - it would hide every unused parameter in a function; broader callback suppression is a later add, evidence/config-gated. |
| `shadowed-local` | warning | scope | a declaration whose `shadows ~= nil` (same-function shadowing; §3/scope). Exempt: `_`/underscore names and the implicit `self`. |
| `undefined-global` | warning (default OFF) | scope | a global READ not in the evidence-based app allowlist (above). Off by default; over reads only. |
| `empty-block` | warning | - | an `if`/`elseif`/`else`/`while`/`for`/`do` body with zero statements |
| `duplicate-table-key` | warning | - | a table constructor with two `field_name`/`field_expr` entries for the same literal key |
| `todo-comment` | info | - | a comment containing `TODO`/`FIXME`/`XXX` (surfaced, never fails a build) |

`undefined-global` ships **off by default** and reads over global **reads** only
(`access == "read"`), enabled with `--enable=undefined-global`.

**The allowlist is EVIDENCE-BASED, derived from Hull's app-runtime sandbox +
registration, NOT guessed.** It contains exactly:
- **Lua base globals surviving the sandbox.** `hl_lua_sandbox` (`runtime/lua/runtime.c`)
  removes `io, os, load, loadfile, dofile, package, debug`; the rest of Lua 5.4's base
  library remains: `assert, collectgarbage, error, getmetatable, ipairs, next, pairs,
  pcall, print, rawequal, rawget, rawlen, rawset, select, setmetatable, tonumber,
  tostring, type, warn, xpcall, _G, _VERSION`, plus the surviving library tables
  `coroutine, math, string, table, utf8`. `_ENV` is also allowlisted: Lua 5.4 supplies
  it as the implicit environment upvalue (valid code may reference it directly), and the
  lightweight scope pass does not synthesize that binding, so without the allowlist entry
  a direct `_ENV` read is a false positive.
- **Hull-injected app/test globals**: `app` + `hull` (`runtime/lua/modules.c`),
  `require` (`mod_fs.c`), `test` (`mod_test.c`).

Deliberately **NOT** in the app allowlist (so they correctly trigger when read as a
global): `req` / `res` are handler **parameters**; `db, fs, http, json, log, crypto,
compute, gpu, …` are **imported locals** (`require("hull.X")`); `io, os, load, loadfile,
dofile, package, debug` are **sandbox-removed**; `tool` / `arg` and promoted short
module names belong to the **tool VM** (`mod_tool.c`), not app code. A later explicit
`--tool-mode` profile can add the tool-VM globals without weakening app analysis. The
list is locked with positive tests (allowed globals do NOT fire) AND negative tests
(`db`, `req`, `os`, `json` DO fire when read globally).

## 6. CLI surface (additions)

```
hull analyze [app_dir] [files...] [--json] [--quiet]
             [--strict] [--rules=a,b] [--disable=c,d] [--enable=e] [--list-rules] [--max-depth=N]
```
- `--list-rules` → print every rule (`id`, severity, default, one-line describe), exit 0
  (human) or a `{ rules: [...] }` JSON with `--json`.
- `--rules=<ids>` → run ONLY these rules (comma-separated) - overrides defaults.
- `--disable=<ids>` / `--enable=<ids>` → adjust the default set (compose: defaults −
  disabled + enabled). An unknown id is a usage error (exit 2).
- `--strict` → treat **warnings** as failures (they contribute to exit 1). Without it,
  warnings/infos are advisory (exit 0 if there are no errors).
- Syntax analysis (v1) always runs first; lint rules run on the `complete` files. An
  `incomplete`/`internal` file is NOT linted (its AST is unreliable) - it already
  drives exit 1 from v1.

## 7. Exit codes + severities (extends v1 honestly)

v1's contract is preserved: **error**-severity diagnostics (`lua.syntax`,
`lua.unsupported`, `lua.internal`, `lua.limit.*`, `analyze.*` targets) drive exit 1.
Lint findings add **warning** and **info** severities:
- `0` - no error-severity diagnostics, and (no warnings OR `--strict` not set).
- `1` - any error-severity diagnostic, OR (`--strict` AND ≥1 warning).
- `2` - usage / operational failure (unknown flag/rule, discovery failure) - unchanged.

`info` (e.g. `todo-comment`) never affects the exit code. This matches the ubiquitous
linter convention (warnings advisory, `-Werror`/`--strict` to gate).

## 8. JSON schema evolution → `schema_version: 2`

Backward-compatible superset of v1 (same top-level keys; new codes + severities +
counts). Bumped to `2` to signal the lint capability:
- `diagnostics[].severity` gains `"warning"` / `"info"`; `code` gains `lua.lint.<id>`.
- `summary` gains `warnings`, `infos` (alongside `errors`, `incomplete`, `internal`);
  `clean` stays "exit 0". A new `summary.by_rule` map (`{ "unused-local": 3, ... }`)
  gives per-rule counts for dashboards.
- `--list-rules --json` emits `{ "schema_version": 2, "rules": [ { id, severity,
  default, description } ] }`.
A v1 consumer still parses a v2 document (it just sees new codes/severities); the bump
is the explicit signal that lint data may be present.

## 9. Architecture + files

- **`stdlib/cli/lua/hull/source/scope.lua`** - `hull.source.scope`, the binding pass
  (§3). Pure Lua over the AST; no new C.
- **`stdlib/cli/lua/hull/source/lint.lua`** - `hull.source.lint`: the `RULES` registry,
  the engine (`lint.run(unit, scope, opts) -> findings`), and the rule `check`
  functions. Pure Lua.
- **`analyze.lua`** - extended: parse args (new flags), after a `complete` parse run
  `scope.resolve` + `lint.run`, merge lint findings into the diagnostics list, apply the
  `--strict` exit logic, add `--list-rules`.
- **Tests**: `test_scope.lua` (resolution cases via the vanilla-`lua_State` harness, a
  new `UTEST(lua_source, scope)` leg) + `test_lint.lua` (each rule: a positive fixture
  that fires and a negative that must not) + `tests/e2e_analyze.sh` additions (a
  `--strict` run flips exit 0→1; `--list-rules`; `unused-local`/`shadowed-local`/
  `todo-comment` end to end; `--rules=`/`--disable=` selection; JSON `schema_version:2`
  + `lua.lint.*` codes + `summary.warnings`). No new C.

## 10. Slice plan

1. **Engine + structural rules** - DONE. `hull.source.lint` registry/engine +
   `empty-block`, `duplicate-table-key`, `todo-comment` (no scope), the CLI flags
   (`--strict`, `--rules`/`--disable`/`--enable`/`--list-rules`), per-diagnostic
   severities, `lua.lint.<id>` codes, JSON `schema_version: 2` (+ `summary.warnings`/
   `infos`/`by_rule`). `test_lint.lua` + `tests/e2e_analyze.sh` (23 cases). Lint runs
   only on a CLEANLY-parsed file (complete + no syntax errors).
2. **Scope pass** - DONE. `hull.source.scope` (`resolve -> scope, err`; pcall-guarded)
   + `test_scope.lua`. The reusable Step B; not yet wired into a rule (slice 3).
   Design: [hull_source_scope_design.md](hull_source_scope_design.md).
3. **Scope rules** - DONE. `unused-local` (local/localfunc, `_`-exempt),
   `unused-param` (`_`/implicit-`self`-exempt), `shadowed-local` (same-function,
   `_`/implicit-exempt), and `undefined-global` (OFF by default, evidence-based app
   allowlist, reads only) on top of `hull.source.scope`. The engine gains `needs_scope`
   (rules declare it; skipped when scope is unavailable); `analyze.lua` computes the
   scope once per clean file and, on a resolver failure, DOWNGRADES the file to state
   `internal` (so JSON `files[].state` + `summary.internal` stay consistent with exit 1),
   surfaces the internal diagnostic, and skips scope-backed rules (structural rules still
   run). `analyze_source` is exported (module return, CLI entry guarded on `tool`) so the
   three-state contract is unit-tested with an injected resolver failure. test_lint (177)
   + test_analyze (9) + e2e (25).

Each slice is a design-first → ratify → implement → green → merge cycle, matching how
the layer itself was built.

## 11. Decisions (ratified)

1. **All three slices** - engine + structural rules, the `hull.source.scope` pass, and
   the scope rules (`unused-local` / `unused-param` / `shadowed-local` /
   `undefined-global`).
2. **Rule set as tabled**, with `undefined-global` **off by default** (needs the global
   allowlist). `unused-param` exempts ONLY `_`, underscore-prefixed names, and the
   implicit method `self` - **no trailing-unused-run suppression by default** (it would
   hide every unused param; broader callback suppression is a later, evidence/config-
   gated add).
3. **`--strict` advisory-by-default** - warnings/infos do not affect the exit code
   unless `--strict` (non-breaking for CI running `hull analyze` today).
4. **JSON `schema_version: 2`** - backward-compatible superset.
