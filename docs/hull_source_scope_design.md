# `hull.source.scope` — lexical binding pass (design)

Status: DESIGN (pre-implementation). Slice 2 of `hull analyze` v2
([hull_analyze_lint_design.md](hull_analyze_lint_design.md) §3) — the light lexical
scope / name-resolution pass the scope-backed lint rules (slice 3: `unused-local`,
`unused-param`, `shadowed-local`, `undefined-global`) need. It is the "semantic /
binding pass" the roadmap deferred "only if a consumer needs it"; the linter is that
consumer. Reusable (future codegen may want it), so it ships as its own source-layer
module, not lint-internal.

## 1. Goal

`scope.resolve(unit) -> scope, err` resolves, for a cleanly-parsed `unit.ast`, **which
declaration each `name` reference binds to** (a local / parameter / loop-var in an
enclosing scope, an **upvalue** across a function boundary, or a **global**), and
records **per-declaration usage** (was it ever read? written?) and **shadowing** (does
it hide a binding of the same name?).

**Two failure modes, distinguished.** A *recovered error node* or a locally-missing
field degrades **locally** to "unresolved" (never a crash) — the model is still usable.
An *internal resolver failure* (a bug that raises) must NOT silently return a partial
model that suppresses findings: the traversal is `pcall`-guarded and, on a raise,
`resolve` returns `(nil, err)` where `err` is a diagnostic-shaped table. The analyzer
then surfaces an **internal diagnostic** for the file and **skips the scope-backed
rules** for it (never a false "clean"). On success, `err == nil`.

## 2. Lua 5.4 scoping rules the pass must honor

These are the subtleties that make a naive walk wrong:

1. **A `local` is visible only AFTER its declaration statement**, within the rest of
   its block. So in `local x = x`, the RHS `x` binds to an *outer* `x` (or a global),
   NOT the one being declared. Process a `local_declaration`'s **values before** adding
   its names to the scope.
2. **`local function f` is visible inside its own body** (for recursion) — add the
   binding **before** descending the function body. `local f = function() ... end` is
   NOT (the name is added after the value, per rule 1).
3. **Parameters** scope to the function body; a method `function a:m()` has an implicit
   first parameter **`self`**.
4. **Loop variables** scope to the loop body: numeric-for's `var`, generic-for's
   `names`. (They are NOT visible in the loop's control expressions — those are
   evaluated in the enclosing scope.)
5. **`repeat <body> until <cond>`**: the `until` condition is evaluated **in the scope
   of the body's locals** (a Lua special case — unlike every other loop). A local
   declared in the repeat body IS visible in the `until` expression.
6. **Blocks introduce scopes**: the chunk, each function body, `do`, `while`/`repeat`
   bodies, each `if`-clause body, and each for body. A binding leaves scope at the end
   of its block.
7. **Function boundary = upvalue**: a reference resolving to a binding in an enclosing
   *function* (not just an enclosing block of the same function) is an **upvalue**. The
   pass records the distinction (local-in-this-function vs upvalue).

## 3. What is a reference vs a declaration

The parser represents local/param/loop **declaration names as plain string records**
(`{ name = "x", range }`), NOT `name` AST nodes — so they are never visited as
references. A `name` **node** (`{ kind = "name", name, range }`) is always a use:
- as an **assignment target** (`target.kind == "name"`) → a **write** to the resolved
  binding (or a global write);
- anywhere else (a value, a call callee, the object of a `field`/`index`, a table
  value, a return value, …) → a **read**.

So `unused-local` = a local/param with **zero reads** (writes don't count as use). An
underscore-prefixed name (`_`, `_x`) and the implicit `self` are exempt (slice 3).

## 4. Output shape (attached to `unit`, returned)

```
scope = {
  bindings = { <decl>, ... },   -- every local / param / loopvar, in source order
  ref_of   = <table: name-node -> resolution>,
}
-- <decl> = {
--   name, kind = "local"|"param"|"localfunc"|"loopvar",
--   range,               -- the declaration's range (for an implicit self, the anchor)
--   implicit = true?,    -- ONLY the method `self` param -- a synthetic binding, not
--                        --   a "real source" decl (its range is an anchor, see §6)
--   func_id,             -- which function scope owns it (for upvalue detection)
--   reads = <int>, writes = <int>,
--   shadows = <decl|nil>,-- the binding it hides (§5), if any
-- }
-- resolution = { decl = <decl>, kind = "local"|"upvalue" } | { kind = "global" }
```
`ref_of` is keyed by the `name` node table identity, so a consumer walking the AST can
ask "what does this name resolve to?". `bindings` drives the `unused-*` rules;
`shadows` drives `shadowed-local`; a `resolution.kind == "global"` read (of a name not
in a known-globals allowlist) drives `undefined-global`.

## 5. Shadowing semantics (a decision, §8)

`shadows` is set when a new declaration hides an enclosing binding **of the same name**.
The **default** flags shadowing of a binding **in the same function** (an enclosing
block: an outer `local`, a param, an enclosing loop var) — the genuinely bug-prone
case (`local x` in a nested block hiding the function's `local x`). It does **NOT** flag
shadowing an **upvalue** from an *outer function* by default: a parameter or local
named the same as something in an enclosing closure is extremely common and idiomatic
(e.g. a callback `function(err) ... end` where an outer `err` exists), so flagging it
would be noise. (An `--enable`-able stricter variant is a later add.)

## 6. Algorithm

A single **recursive, scope-aware traversal** of the AST (NOT `lua.walk`, which
flattens and loses block/statement ordering), `pcall`-guarded (§1). State: a stack of
scopes; each scope has a name→decl map and a `func_id`; a `func_id` counter. **The chunk
(main) is itself a function scope with `func_id = 1`** (the main chunk is a vararg
function in Lua); nested function bodies get `func_id = 2, 3, …`, so a nested function
capturing a chunk-level local correctly classifies it as an **upvalue**
(binding.func_id 1 ≠ the nested func_id).

- **Enter a block** → push a scope. **Leave** → pop (its bindings persist in
  `scope.bindings`; only visibility ends).
- **Resolve a `name` node** → search the scope stack top-down for the name; first hit is
  the binding. If the binding's `func_id` ≠ the current function's `func_id` →
  `kind = "upvalue"`, else `"local"`; increment its `reads` (or `writes` if it's an
  assignment target). No hit → `global`.
- **`local_declaration`** → resolve the **values first** (rule 1), then add each name as
  a `local` decl to the current scope.
- **`local function`** → add the `localfunc` decl to the current scope **first** (rule
  2), then descend the body (a new function scope).
- **simple `function f()`** → the declaration name is an **assignment** to the resolved
  `f` binding: a **local write** if `f` is a local in scope, else a **global write** —
  NOT inherently global. (The body is a new function scope.)
- **dotted/method `function a.b:m()`** → the **base** `a` is a **read** (a `name` node);
  the field keys `b` / `m` are NOT `name` references (they are string keys on the
  `field` chain). Resolve only the base name; the body is a new function scope with
  `self` (if method) + params.
- **`function_expr` / function bodies** → new function scope (`func_id`++); add params
  (+ implicit `self` for methods); descend the body.
- **numeric_for / generic_for** → resolve control expressions in the ENCLOSING scope
  (rule 4), then push the body scope with the loop var(s), descend the body.
- **`repeat`** → push the body scope, descend the body, THEN resolve the `until`
  condition **in that same scope** (rule 5), then pop.
- **`assignment`** → resolve values (reads), then each target: a bare `name` target is a
  **write** to its resolution; a `field`/`index` target reads its object/key.
- **shadowing** → when adding a decl, look up the same name in the current scope **and**
  the enclosing scopes of the **same function** — the current scope INCLUDED, so a
  same-block redeclaration (`local x; local x`) and a repeated name within one
  declaration (`local x, x`) both count as shadowing (each added left-to-right, so the
  later one sees the earlier). If found, set `shadows` (§5).
- **implicit `self`** → a method's `self` param is added as
  `{ name = "self", kind = "param", implicit = true, range = <anchor> }`, where the
  anchor is the method declaration's name range (a stable, real span) — explicitly
  marked synthetic, NOT a zero-width fake "real source" decl.

`goto`/labels are ignored for binding purposes (they do not introduce value bindings;
Lua's goto-scope validation is out of scope for v2 — §7).

## 7. Non-goals

- **No goto/label scope validation** (Lua's "jumps into the scope of a local" is a
  `load()` semantic check, not a binding question; the conformance gate already pins it
  as an expected divergence).
- **No type/flow analysis**; `reads`/`writes` are lexical counts, not reachability.
- **No cross-file / project symbol graph** (per-file, like the rest of v2).
- **Not** wired into any rule in slice 2 — this slice ships the pass + its own tests;
  slice 3 consumes it.

## 8. Testing

`test_scope.lua` (a new `UTEST(lua_source, scope)` leg) over hand-built cases, asserting
`ref_of` resolutions + per-decl `reads`/`writes`/`shadows`:
- `local x = x` → the RHS resolves to an outer `x` / global, not the new local.
- `local function f() return f() end` → `f` inside the body resolves to itself (recursion).
- `local f = function() return f end` → the inner `f` does NOT resolve to `f` (global).
- a nested block `local x; do local x end` → the inner `x` `shadows` the outer.
- **same-block redeclaration** `local x; local x` → the second `shadows` the first.
- **repeated names in one declaration** `local x, x = 1, 2` → the second `shadows` the first.
- **`function f() end`** where `f` is a local → recorded as a WRITE to that local (not a
  global); where `f` is not local → a global write.
- **implicit `self`** → present as a `param` decl with `implicit = true` and a non-empty
  anchor range.
- a param / callback shadowing an **upvalue** → NOT flagged (default).
- numeric/generic-for var scoped to the body; control exprs in the enclosing scope.
- method `function a:m()` → `self` is a param, resolvable in the body.
- `repeat local y = 1 until y` → the `until y` resolves to the body's `y`.
- upvalue: an inner function reading an outer local → `resolution.kind == "upvalue"`.
- an unused local (0 reads) vs a read one; a written-but-never-read local (0 reads, ≥1
  write) → still "unused" for slice 3.
- never-raises on a recovered/error-bearing AST.

## 9. Decisions (ratified)

1. **Shadowing** flags **same-function** bindings only by default (the current scope
   INCLUDED, so same-block redeclaration counts); NOT upvalue shadowing from an outer
   function.
2. **`goto`/labels** validation stays out of scope (a `load()` semantic check, already a
   pinned conformance divergence).
3. **Only reads count as use**; a write with no read is dead (`unused-local` still
   fires).
4. **`self` / underscore exemptions live in the lint rules** (slice 3), not the reusable
   pass — the pass records them as ordinary bindings (with `self` marked `implicit`) and
   the rule filters.

Contract tightenings folded in: `resolve -> (scope, err)` with a `pcall`-guarded
traversal (an internal failure surfaces an internal diagnostic + skips scope-backed
rules, never a silent partial); `function f()` is a write to the resolved `f` binding
(local or global), dotted/method decls read only the base name; same-block +
same-declaration redeclaration count as shadowing; implicit `self` is an explicit
`implicit = true` binding with a real anchor range; the chunk is `func_id = 1` so nested
captures of chunk locals classify as upvalues.
