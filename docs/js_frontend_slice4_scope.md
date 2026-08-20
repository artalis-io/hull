# JS frontend Slice 4 - scope / binding resolver

Design record for Slice 4 of the JavaScript source frontend
(docs/javascript_source_frontend_design.md slice 4, section 19). Mirrors the Lua
counterpart stdlib/cli/lua/hull/source/scope.lua (docs/hull_source_scope_design.md) in
shape and contract, with the JS-specific scoping semantics locked below. This is the
DESIGN; implementation follows on approval. ASCII-only, no em-dashes.

## 1. Goal + scope

A structural lexical-binding pass over the parsed AST (never executed). It resolves every
identifier REFERENCE to its binding (a local / closure / global) and records, per binding,
its kind + scope + reads + writes + shadowing. It is the reusable substrate the Slice-5
adapter surfaces as the frontend's `scope` capability.

In scope: `scope.resolve(unit)` -> a model of bindings + references. Out of scope: tag
interpretation (Slice 3, done), the frontend adapter / ProjectDiscovery (Slice 5),
executing application JS, and the JS EARLY ERRORS a structural pass cannot/should not
enforce (section 9). Mirrors Lua: a recovered/error-bearing AST degrades LOCALLY; only an
internal resolver fault surfaces a diagnostic.

## 2. The model (output shape)

`resolve` returns a flat, JSON-serializable model (so the C harness can assert on it, and
the Slice-5 adapter can index it):

```
{ ok: true,
  bindings: [ Binding, ... ],     // every declared name, in source order of declaration
  refs:     [ Ref, ... ] }        // every resolved identifier reference, in source order
```

Binding:
```
{ name:   "x",
  kind:   "var" | "let" | "const" | "function" | "class" | "param" | "import" | "catch",
  scope:  "module" | "function" | "block",   // the KIND of scope that owns the binding
  funcId: <int>,                 // the function scope the binding lives in (module = 0)
  scopeId:<int>,                 // the specific scope instance owning it
  range:  { start, stop },       // the binding NAME's byte range (the Identifier)
  reads:  <int>,                 // resolved read references
  writes: <int>,                 // resolved write references (assignments; NOT the initializer)
  shadows:{ start, stop } | null,// the nearest same-name binding in an enclosing scope, or null
  hoisted:<bool> }               // true for var + function declarations (hoisted to the scope top)
```

Ref:
```
{ name:  "x",
  range: { start, stop },        // the reference Identifier's byte range
  kind:  "local" | "closure" | "global",   // same function / crossing a function / unbound
  access:"read" | "write",
  declRange: { start, stop } | null }       // the resolved binding's name range, or null for global
```

On an internal fault: `{ ok: false, error: { code: "js.internal", message, range } }` (the
adapter surfaces this on the shared diagnostic contract, mirroring Lua's `(nil, err)`).

## 3. Scopes

Scope-creating forms are EXACTLY these; nothing else creates a scope. In particular an
`if` / `while` / `do-while` body that is a single statement (not a `BlockStatement`) does
NOT get a scope - a scope appears there only when the body is written with braces (a real
`BlockStatement`, per the next bullet).

- **module** - the Program (parser is MODULE mode, hence strict). ONE scope owning all
  `import` bindings + top-level `var`/`function` (hoisted) + top-level `let`/`const`/`class`.
  funcId 0. (The module is not split into a var-scope and a body block.)
- **function** - the environment of a `FunctionDeclaration`, `FunctionExpression`,
  `ArrowFunctionExpression`, or a class `MethodDefinition` value. Owns its PARAMS + the
  `var`/`function` declarations hoisted from the whole body (section 4). Each gets a fresh
  funcId. Its body `BlockStatement` is a nested BLOCK scope (below) that owns the body's
  top-level `let`/`const`/`class`.
- **block** - EVERY `BlockStatement` (a braced `{}`, including a function/loop/if/try body
  written with braces). Owns the `let`/`const`/`class` and block-level `function`
  declarations that are DIRECT children of the block. Shares the funcId of the enclosing
  function; gets a fresh scopeId.
- **loop-head** - the lexical environment of a `for` / `for-of` / `for-in` whose head
  declares `let`/`const` (`for (let i ...)`). It owns those head bindings and encloses the
  loop body (the body block, if braced, nests inside it). The head bindings are visible
  throughout the ENTIRE head, INCLUDING their own initializer / iterated expression: the
  loop-head scope is pushed and its bindings predeclared BEFORE the initializer/RHS is
  resolved, so `for (let x of x)` resolves the RHS `x` to the loop binding, and
  `for (let x = x; ...)` resolves the initializer `x` to the loop binding. (The resulting
  TDZ violation is the deliberately-omitted early error, section 9 - the ranges are retained
  for a future pass.) A `for (var ...)` head declares no loop-head binding (the `var` hoists
  to the function; section 5).
- **switch** - ONE block scope for the whole `switch` body, shared by all cases (a `let` in
  one case is visible, and shadow-checkable, in later cases - the standard JS footgun),
  matching the single brace-delimited body.
- **catch** - a `catch (param)` clause creates a catch binding environment owning the param
  (destructuring binds each name), which encloses the catch body `BlockStatement` (a nested
  block scope). A `catch {}` (no param) creates NO catch environment - only its body block.

**Params.** All of a function's parameter names are predeclared in the function scope BEFORE
any default value is resolved (section 4). A body top-level `var x` when a param `x` exists
is the SAME binding, coalesced (section 5), not a new one.

## 4. Hoisting (two-pass per scope)

JS requires hoisting, so - unlike Lua's single pass - each scope is resolved in TWO passes:
a DECLARE pass that pre-binds names, then a RESOLVE pass that resolves references against
the fully-populated chain. The declare pass differs by scope kind, and the function-wide
`var` collection is an explicit recursive walk (not merely "look at direct children").

**Declare pass, function or module scope:**
1. Predeclare ALL parameter names (function only) as `param` bindings, in source order,
   BEFORE resolving any default value. So in `function f(a = b, b = 1) {}`, `b` in `a`'s
   default resolves to the param `b` (a `local`), NOT an outer/global - the parameters are
   lexically bound before defaults evaluate; the fact that `b` is textually later is a TDZ
   ordering violation the resolver deliberately does not flag (section 9), and each param's
   range is retained so a later TDZ pass can detect it.
2. **Function-wide `var`/`function` collection (algorithmic).** Recursively walk EVERY
   descendant statement and block of this function's body, and collect:
   - every `var` binding name (including inside nested `BlockStatement`s, `if`/`for`/`while`/
     `do`/`switch`/`try`/`catch` bodies, and destructuring `var` patterns), and
   - every `function` declaration that is a DIRECT child of the function/module body (a
     top-level function; a function declared inside a nested block is NOT collected here -
     it is block-scoped, step below).
   The walk STOPS at every nested function boundary (a `FunctionDeclaration` /
   `FunctionExpression` / `ArrowFunctionExpression` / method value): their inner `var`s
   belong to THEIR function scope, not this one. All names collected here are `hoisted: true`
   and bound in this function/module scope.

**Declare pass, block scope (every `BlockStatement`, switch body, loop-head, catch env):**
bind only the `let`/`const`/`class` and block-level `function` declarations that are DIRECT
children of the block (NOT descendants - nested blocks own their own lexical names). These
are `hoisted: false`. A loop-head binds its `let`/`const` head names; a catch env binds its
param name(s).

**Resolve pass:** walk the scope's statements/expressions in source order and resolve each
reference (section 6/7) against the now fully-populated scope chain.

Consequence (locked): a forward reference to a hoisted `var`/`function` resolves to its
binding (`f(); function f(){}` -> the call resolves to `f`). A block-level `function`
declaration in strict/module code is BLOCK-scoped (bound in its block, not hoisted to the
function). Only `var` crosses block boundaries up to the function/module scope.

## 5. Binding kinds + where each is bound

| Source form | kind | scope bound in | notes |
|---|---|---|---|
| `var x` (any nesting, non-fn) | `var` | nearest function/module | hoisted; destructuring binds each name |
| `let x` / `const x` | `let`/`const` | current block | lexical; destructuring binds each name |
| `function f(){}` (direct child of fn/module body) | `function` | that function/module | hoisted; also visible inside its own body (recursion) |
| `function f(){}` (direct child of a block) | `function` | that block | block-scoped (strict) |
| `class C {}` declaration | `class` | current block | lexical; visible inside its own body |
| a function/arrow/method PARAM | `param` | that function | destructuring params bind each name |
| `import ...` (any form) | `import` | module | hoisted; immutable (section 9) |
| `catch (e)` param | `catch` | the catch block | destructuring binds each name; `catch {}` binds nothing |
| `for (let/const x ...)` head | `let`/`const` | the loop block | one binding (per-iteration semantics not modeled) |
| `for (var x ...)` head | `var` | nearest function/module | hoisted |

**Named function / class EXPRESSIONS.** `const g = function f(){}` binds `g` in the
enclosing scope (the VariableDeclarator) and `f` ONLY inside the function expression's own
scope (a self-reference name, kind `function`). `const X = class C {}` binds `X` outside
and `C` only inside the class body. An arrow has no name.

**Class name visibility.** A `class C {}` DECLARATION binds `C` in the enclosing block
(lexical) AND makes `C` visible inside the class body (extends clause is evaluated in the
enclosing scope; method bodies + field initializers + static blocks see `C`). The
`superClass` expression (`extends B`) is a REFERENCE (read) in the enclosing scope.

### 5.1 Redeclaration identity + lookup (deterministic)

A scope maps a name to exactly ONE lookup binding, chosen deterministically, even when the
source declares the name more than once. `bindings[]` preserves every declaration record for
source fidelity, but the scope's name-to-binding table (what references resolve to) has one
entry per name.

- **Legal coalescence (no shadow).** In one VARIABLE scope, repeated `var`, or a `var`
  colliding with a same-scope `param` or top-level `function` of the same name, is legal JS
  and denotes ONE binding. These coalesce into a SINGLE binding record: the one whose
  declaration range starts EARLIEST wins its `name`/`kind`/`range` (a `param` is textually
  first, so `function f(x){ var x; }` reports the `param x`; `function f(){} var f;` reports
  the `function f`); the later same-name declarations do NOT create additional records and do
  NOT set `shadows` (they are the same binding, not a shadow). Reads/writes accumulate on the
  one record.
- **Duplicate lexical declaration (omitted early error).** `let x; let x;`, or a `let`/`const`/
  `class` clashing with another lexical or with a `function`/`param` in the same scope, is a
  SyntaxError JS never runs. The resolver does NOT reject (section 9). Each duplicate is kept
  as its OWN record in `bindings[]` (source fidelity), but the scope's lookup target for that
  name is the FIRST declaration (deterministic: the earliest by range); later same-name
  lexical declarations do NOT become the lookup target and are NOT chased as shadows of each
  other. References in the scope therefore resolve to the first declaration's record.
- **Cross-scope shadowing is unaffected.** Coalescence and duplicate handling are WITHIN one
  scope; a same-name binding in an ENCLOSING scope is still recorded as `shadows` (section 8).

`bindings[]` is SORTED by declaration range (`range.start`, then `range.stop`) before return.
The predeclare pass binds params + hoisted vars before textual order, so the raw insertion
order is not source order; sorting makes `bindings[]` observably deterministic and
source-ordered regardless of the traversal.

## 6. References: reads, writes, and property-vs-reference

An `Identifier` is a REFERENCE only in value position. The locked rules:

`Ref.access` is exactly `"read"` or `"write"` (no third value). A compound access that is
BOTH emits TWO separate `Ref` records at the SAME range, in deterministic READ-THEN-WRITE
order (the read first). The binding's `reads` and `writes` counters each increment by one, so
`x += 1` and `x++` contribute reads:1 + writes:1.

- A bare `Identifier` used as a value -> ONE **read** ref.
- The LEFT of an `AssignmentExpression`:
  - operator `=` on a plain `Identifier` -> ONE **write** ref (no read);
  - a compound operator (`+=`, `-=`, `**=`, `&&=`, `||=`, `??=`, ...) on an `Identifier` ->
    TWO refs at that range: a **read** then a **write**;
  - a MemberExpression target (`a.b = `, `a[k] = `) -> the object (`a`) is a **read**, the
    computed key is a read, the property name is NOT a reference; no binding write;
  - a destructuring target pattern -> each assigned `Identifier` is a **write**; computed
    keys / default values inside it are reads.
- `UpdateExpression` (`x++`, `--x`) on an `Identifier` -> TWO refs at that range: a **read**
  then a **write**.
- `MemberExpression` `a.b`: `a` (object) is a reference; `b` (non-computed property) is NOT
  a reference (it is a property key). `a[k]`: both `a` and `k` are references.
- Object literal `{ key: value }`: `key` is NOT a reference (a property name); `value` is.
  Shorthand `{ x }`: `x` is BOTH the key and a **read** reference. Computed `{ [e]: v }`:
  `e` and `v` are references.
- Object/array BINDING patterns (in a declaration/param/catch): the bound names are
  BINDINGS, not references; their default-value expressions and computed keys are reads.
- `import` specifiers: the imported names are BINDINGS, not references. `export { a }` (no
  `from`): the local name `a` is a **read** reference (it must resolve to a binding).
  `export { a } from "m"` (a re-export, `source` set): `a` is NOT a local/global reference -
  it names an export of the other module, so it is ignored. `export default <expr>`: the
  expression is read normally.

**Export-wrapped declarations.** A module declaration commonly appears inside an
`ExportNamedDeclaration` / `ExportDefaultDeclaration` wrapper (`export function f(){}`,
`export const x = 1`, `export default function g(){}`, `export default class C {}`). The
PREDECLARE pass unwraps the wrapper and gives the inner declaration its normal module
binding (so a self-reference like `export function f(){ return f; }` resolves, not global);
a NAMED default function/class binds its name, an ANONYMOUS default (`export default
function(){}` / `class{}`) has no name and no binding. The RESOLVE pass traverses the inner
declaration EXACTLY ONCE (one path through `resolveStmt`), so a default function's body /
refs / nested bindings are counted a single time.
- Labels (`break outer`, `continue loop`, `outer:`) are a SEPARATE namespace - not variable
  references; they are ignored by the resolver.
- A `function f(){}` DECLARATION name is a binding, not a write reference (contrast Lua's
  global `function f()` which is an assignment; JS declarations bind, they do not assign).

The declaration INITIALIZER is not a write: `const x = expr()` establishes the binding
(`writes: 0`) and reads `expr`; a later `x = y` is the first write. This lets a consumer
flag a `let` never written (could be `const`) or a `const` written (an error, section 9).

## 7. Resolution: local / closure / global

For a reference, walk the scope chain from innermost out; the first same-name binding wins:

- binding found in a scope with the SAME funcId as the reference -> **local**;
- binding found in an ENCLOSING function (different funcId) -> **closure** (the Lua layer
  calls this "upvalue"; "closure" is the JS-idiomatic term);
- no binding anywhere -> **global** (an ambient global like `console`, `Math`, or an
  undeclared name; `declRange: null`).

`import` bindings live in module scope, so a reference to an import from inside a function
is a `closure` reference to a binding of kind `import` - the ref kind and the binding kind
are independent axes.

## 8. Shadowing + declaration ordering

`shadows` on a binding is the nearest same-name binding visible in an ENCLOSING scope at the
point of declaration (any enclosing block or function, not just the same function - JS
lexical scoping shadows across function boundaries too). Legal same-scope COALESCENCE
(section 5.1) does NOT set `shadows` (it is one binding, not a shadow); only a binding in an
enclosing scope is a shadow target.

Declaration ordering: because of the two-pass hoisting, a binding's `range` is its textual
declaration position, and references carry their own range - so a consumer can compare them
(e.g. a `let`/`const`/`class` read whose ref.range precedes its declRange is a TDZ access).
The resolver itself resolves by VISIBILITY (hoisted), not by textual order, and does not
enforce TDZ (section 9).

## 9. Documented omitted early errors (locked)

A structural resolver that never executes deliberately does NOT enforce these JS early /
runtime errors (consistent with the Slice-2 conformance divergence list). Each is recorded
enough that a future consumer could add it, but the resolver never raises or rejects on it:

- **Temporal Dead Zone (TDZ).** A `let`/`const`/`class` reference textually before its
  declaration resolves to the binding (hoisted visibility); the TDZ runtime error is not
  flagged. (`ref.range` vs `declRange` lets a consumer detect it.)
- **Duplicate lexical binding.** `let x; let x;` (or a `let`/`function` clash) in one scope
  is a SyntaxError JS never runs; the resolver keeps each as its own `bindings[]` record but
  resolves references to the FIRST (section 5.1), and does not reject. `var` redeclaration -
  and `var` coalescing with a same-scope `param`/`function` - is legal and denotes ONE
  binding (section 5.1), no duplicate record.
- **Assignment to `const` / `import`.** Recorded as `writes > 0` on a `const`/`import`
  binding; not flagged as an error here.
- **Use of an undeclared name in strict mode** (an assignment to an unbound name) -> a
  `global` write; not flagged.
- **`delete` of a variable**, `with`-introduced dynamic scope (with is js.unsupported
  anyway), `eval`-introduced bindings (eval is unsupported in the tooling runtime) - not
  modeled.

## 10. Failure contract + traversal robustness

Mirrors the Lua boundary and the Slice-3 hardening:

- **Never raises.** `resolve` runs inside a guard. A recovered/error-bearing AST (an `Error`
  node from parser recovery, an incomplete/missing child) degrades LOCALLY: the resolver
  skips the malformed node and continues, producing a partial-but-valid model - NOT an
  internal fault.
- **Internal fault -> structured error.** An unexpected internal defect (an exception in the
  resolver, a structural invariant a well-formed node must satisfy that is violated) returns
  `{ ok: false, error: { code: "js.internal", message, range } }`. `ok === true` guarantees
  the traversal completed without an internal fault (the tested invariant, mirroring Lua's
  "err ~= nil only on an internal failure").
- **Bounded.** No new limits; the resolver is a linear walk over the already-bounded AST
  (the session's byte/token/depth caps already applied at parse). Deep nesting is bounded by
  the parser's maxDepth; the resolver's own recursion rides the same host stack guard, so a
  pathological AST surfaces js.limit.stack via the session, not a crash.

## 11. Module boundary + integration

New module stdlib/cli/js/hull/source/scope.js (hull:source:scope), mirroring
hull.source.scope. Exports `resolve(unit)` where `unit` is the parse result
(`{ ast, ... }`); it operates on the AST and its byte ranges ALONE (no source bytes - names
and ranges are already on the AST Identifier nodes). Returns the section-2 model.

UNLIKE annotations (which run inside parseInternal), scope is a SEPARATE pass - the Slice-5
adapter calls `scope.resolve` on demand, exactly as the Lua analyzer calls
`hull.source.scope.resolve` (parse does not run it). For Slice-4 testing, a `resolveScope`
driver in lextest.js parses a source and returns the scope model as JSON.

## 12. Testing plan

A new test_js_scope suite driving the `resolveScope` driver, covering the locked semantics:

- **scopes**: module/function/block nesting; a block `let` invisible outside; a `var`
  escaping a braced block to the function; switch-body sharing; **NO synthetic scope for a
  single-statement `if`/`while`/`do` body** (a `let` is only possible with braces, so this
  is exercised via `var` visibility and braced-vs-unbraced bodies); a braced body DOES get a
  block scope.
- **hoisting**: forward ref to a hoisted `function`/`var` resolves; a block-level function
  is block-scoped; a `var` in a DEEPLY nested block/if/for/try hoists to the function
  (function-wide recursive collection), while a `var` inside a NESTED FUNCTION does not.
- **params**: all params predeclared before defaults - `function f(a = b, b = 1){}` resolves
  `b` to the param binding (`local`), not global; each param range retained.
- **kinds**: var/let/const/function/class/param/import/catch each recorded with the right
  kind + scope; destructuring params/decls/catch bind each name.
- **redeclaration identity**: `function f(x){ var x; }` -> one binding, kind `param`, no
  shadow; `function f(){} var f;` -> one binding, kind `function`; a duplicate `let x; let x;`
  -> two `bindings[]` records but references resolve to the first; `bindings[]` is sorted by
  declaration range.
- **compound access**: `x += 1` and `x++` each emit a read ref then a write ref at the same
  range; reads and writes counters both increment.
- **class**: declaration binds in the enclosing block + visible in the body; a class
  EXPRESSION name is body-only; `extends B` is a read.
- **imports**: default/named/aliased/namespace bind in module scope; a ref from a function
  is a `closure` to an `import` binding; `export { a }` reads the binding.
- **exports**: export const/let/var bind at module scope; a named exported/default function
  or class binds (self-references resolve); an anonymous default binds nothing; `export { x }`
  reads local x while `export { x } from "m"` (re-export) creates no reference; a default
  function body is traversed exactly once.
- **catch/loop**: catch param scoped to the catch block; `for (let x of ...)` scoped to the
  loop; `for (var i ...)` hoisted; loop-head self-visibility - `for (let x of x)` and
  `for (let x = x; ...)` resolve the RHS/initializer `x` to the loop binding (TDZ omitted).
- **closures**: a ref crossing a function boundary is `closure`; same function is `local`;
  unbound is `global`.
- **reads vs writes**: `=` write, `+=`/`++` read+write, member/computed targets, shorthand
  object read, initializer-not-a-write, property-name-not-a-reference.
- **shadowing**: inner block shadows outer; param shadows module; `shadows` range points at
  the outer binding.
- **ordering / omitted errors**: a TDZ-region read still resolves (documented); a `const`
  reassignment shows `writes > 0` without an error; a duplicate `let` records a shadow, not
  a rejection.
- **failure contract**: a recovered syntax-error AST yields `ok: true` with a partial model
  and no js.internal; a forced internal defect (test-only injection) yields
  `ok: false` + one js.internal.
- **regression**: resolving every file in the Slice-2 corpus yields `ok: true` (no
  js.internal) and never raises.

All under ASan + UBSan; ASCII-only; no em-dashes.

## 13. Non-goals

No tag/type interpretation. No TDZ / duplicate-binding / const-reassignment enforcement
(section 9). No ProjectDiscovery or `declaration_semantics` (Slice 5). No control-flow /
reachability / type inference. No execution of application JS.
