# JS frontend Slice 5 - frontend adapter + declaration_semantics

Design record for Slice 5 of the JavaScript source frontend
(docs/javascript_source_frontend_design.md slice 5, sections 20-21). The adapter turns the
Slice 2-4 pipeline (parser + annotations + scope) into the SAME normalized per-source facts
the Lua frontend produces, so the unchanged `hull.project.model` builds one ProjectDiscovery
across both languages. Mirrors stdlib/cli/lua/hull/project/frontend_lua.lua. This is the
DESIGN; implementation follows on approval. ASCII-only, no em-dashes.

## 1. Goal + scope

Deliver the JS-side adapter that runs INSIDE the QuickJS tooling session and exposes the
frontend contract as serializable operations:

- `analyze(bytes, path, opts) -> facts` - parse + annotate + collect the normalized
  declaration facts (the shape `hull.project.model.build` consumes), plus the diagnostics.
- `declarationSemantics(declId) -> sem | error` - the JS-frontend-specific SOURCE semantics
  of one discovered declaration, over a RETAINED AST (for a future JS lowerer).
- `scope(unitId) -> scopeModel | error` - the advertised scope capability (Slice 4), reached
  THROUGH the adapter, never bypassing the boundary.

In scope: the adapter module, the facts shape, the declaration model (one fact per declared
NAME, multi-declarator identity, group ranges, annotations), `declarationSemantics`, the
scope capability surface, the private-handle + session/generation lifetime contract, and the
transport shape. Out of scope: the C dispatcher bindings + registry flip + `frontend_
javascript.lua` proxy (Slice 6), the mixed-language ProjectDiscovery wiring + dev/inspect
lifecycle (Slice 7), and any lowering (Query/Compute). Never executes application JS.

## 2. Session ownership + the two lifetimes

C owns the session (Slice 1). The unit of ownership is ONE QuickJS session per project-analysis
GENERATION, SHARED across all of that generation's JS files (not one session per file): the
bridge creates the session once, calls `analyze` per JS file within it (each file gets its own
`unit_id`, section 6), and closes it explicitly. Two generation lifetimes (Q6/Q7):

- **Serialized-inspection generation** (`hull agent inspect`, `hull dev --agent`): the bridge
  opens one session for the generation, `analyze`s every JS file, builds the ProjectDiscovery,
  and CLOSES the session right AFTER projection. Nothing is serialized beyond the facts. This is
  the only path the two user surfaces use.
- **In-process semantic generation** (a future lowering step + this slice's semantic tests): the
  bridge RETAINS the one session for the whole generation so `declarationSemantics(declId)` /
  `scope(unitId)` can reach the live JS AST; the generation CLOSES it at generation END, when
  consumers finish.

After the session closes, every `declId` / `unit_id` of that generation is STALE. A serialized
generation may still hold dead bridge-private ids in an already-emitted facts blob, but
resolution of any such id deterministically reports STALE (the C bridge checks session liveness
before calling the adapter); it MUST NEVER advertise a dead id as live. The adapter keeps its
retained ASTs in MODULE-SCOPE state within the session (section 6); `JS_FreeRuntime` at close
drops all of it. The adapter never decides lifetime - C owns open + close.

## 3. The normalized facts shape (transport out)

`analyze` returns a single validated-JSON object (the session enforces "must be JSON",
section 7). It mirrors the per-source facts the Lua path yields via `collect_decls`
(analyze.lua:48-66) + the accessors:

```
{ schema_version: 1,
  status: "analyzed" | "error",        // "error" iff any diagnostic is severity "error"
  unit_id: <int>,                      // BRIDGE-PRIVATE: identifies this file's retained unit for
                                       //   scope(unit_id)/declarationSemantics. The Slice-6 proxy
                                       //   keeps it in its opaque unit object; it NEVER enters the
                                       //   ProjectDiscovery or the public JSON (section 6).
  diagnostics: [ { severity, code, message, range? } ],   // js.syntax / js.unsupported / js.limit.* / js.internal
  declarations: [ Decl, ... ] }        // one per declared NAME (section 4)
```

Decl (the exact shape `model.build` consumes - `model.lua:44-45`, minus language/path which the
analyzer adds):

```
{ kind:        "const" | "let" | "var" | "function" | "class",
  name:        "getUsers",             // the declared name
  range:       Range,                  // the NAME Identifier's range
  group_range: Range,                  // the declaration NODE's range (the group; section 4)
  is_method:   false,                  // always false for JS (section 4)
  annotations: [ { name, args?, value?, raw, range: Range } ],   // Slice-3 tags, text renamed to value
  decl_id:     <int> }                 // BRIDGE-PRIVATE handle (section 6); NEVER copied into a
                                       //   public declaration; the analyzer assigns the model handle
```

**Range = `{ start, stop, line, col }`** (Lua-normalized parity; section 4a). Every range in the
facts - declaration `range`, `group_range`, each annotation `range`, and every diagnostic
`range` - carries the derived `line`/`col` of the START offset alongside the authoritative byte
`start`/`stop`. So mixed Lua/JS inspection is symmetrical.

The facts carry ONLY normalized metadata + the bridge-private integers (`unit_id`, `decl_id`).
NO AST, NO JSValue, NO QuickJS pointer ever crosses. `annotations` are the Slice-3 records with
`text` renamed to `value` (matching `frontend_lua.norm_annotations`); `args`/`value` are omitted
when absent.

### 3a. Range normalization

The parser/annotations/scope layers emit byte-only `{ start, stop }`. The ADAPTER normalizes
every FACTS range to `{ start, stop, line, col }` using the retained linemap (the JS mirror of
`frontend_lua.mkrange` + `unit:line_col`): `line`/`col` are the 1-based line and 1-based BYTE
column of the `start` offset, computed via `hull:source:range` `position(linemap, start)`. Byte
offsets stay authoritative; line/col are derived presentation fields. (The scope model returned
by `scope(unit_id)` keeps the Slice-4 byte-only ranges, matching the Lua scope layer, which does
not normalize its binding/ref ranges; only the facts ranges are normalized, per the Lua adapter.)

## 4. The declaration model (which facts, one per NAME)

**Full-AST traversal (Lua parity).** `frontend_lua.declarations` walks the ENTIRE AST
(`lua.walk`), not only top-level statements, so a declaration nested inside a function, block,
loop, `catch`, or a class method body IS collected. The JS adapter mirrors this: it walks every
node and collects each `VariableDeclaration` / `FunctionDeclaration` / `ClassDeclaration` at any
nesting depth. It does NOT collect the `Export{Named,Default}Declaration` wrapper node itself (it
is not a declaration kind), so the walk reaches the INNER declaration exactly once - no duplicate
through the export wrapper. Method definitions themselves are excluded: a class `MethodDefinition`
/ object method is not a declaration node (its value is a `FunctionExpression`, not a
`FunctionDeclaration`), so it produces no fact - but a `function`/`const`/`class` declared INSIDE
a method body is collected like any other nested declaration.

A declaration is one of, INCLUDING its `export`-unwrapped form (an anonymous
`export default function(){}`/`class{}` has no name, so no fact):

| Source form | kind | one fact per | group_range | annotations from |
|---|---|---|---|---|
| `const`/`let`/`var` declaration | const/let/var | each bound NAME (all declarators + destructuring) | the VariableDeclaration node | the VariableDeclaration's attached tags |
| `function f(){}` (incl. async) | function | the function name | the FunctionDeclaration node | the FunctionDeclaration's tags |
| `class C {}` | class | the class name | the ClassDeclaration node | the ClassDeclaration's tags |

**One fact per declared NAME (D4).** A multi-declarator or destructuring declaration yields
one Decl per bound name, all sharing the declaration NODE's `group_range` (so the model gives
them one `group_id`, and their annotations share `target_group_id`):

- `const a = 1, b = 2;` -> two Decls (a, b), same group_range (the VariableDeclaration).
- `const [ok, err] = f();` -> two Decls (ok, err), same group_range.
- `const {p, q: r} = o;` -> two Decls (p, r), same group_range.

**Multi-declarator identity (the section-20 example).** `const a = foo(), q = bar(), c =
baz();` yields three Decls; `q`'s `declarationSemantics` recovers `q <= bar()` (section 5) via
its declarator index. Each name range is the exact Identifier; the group is the whole
declaration.

**is_method is always false for JS.** A class/object METHOD DEFINITION is not a declaration node
and is excluded (above), so no JS declaration is a method. The field is kept for shape parity and
is always false. (Method-definition discovery is a documented possible extension, not v1.)

**Annotations are shared across a group.** Slice 3 attaches a JSDoc run to the
declaration NODE (the VariableDeclaration / FunctionDeclaration / ClassDeclaration, via the
export line for exported forms). Every per-name Decl in that node carries the same
`annotations` array (exactly as Lua's `local a, b` shares the node's annotations).

## 5. declaration_semantics (JS-frontend-specific, over retained AST)

Recovers the SOURCE semantics of one discovered declaration for a future JS lowerer, reached
ONLY through the retained handle (never on the wire; the neutral ProjectDiscovery never
carries it). Mirrors `frontend_lua.declaration_semantics`. `declarationSemantics(declId)`
returns a small frontend-specific record or a Diagnostic-shaped error.

Records by kind:

- **value** (`const`/`let`/`var` name):
  ```
  { form: "value", kind, declarator_index, binding_path, initializer }
  ```
  - `declarator_index` - which VariableDeclarator (0-based) binds this name.
  - `binding_path` - the STRUCTURAL, AST-relative path from the declarator's id pattern to this
    name; an array of edge steps, EMPTY for a plain `const a = x` (a IS the whole id). The steps
    are pattern-EDGE descriptors that INDEX the pattern arrays (not normalized keys), so they
    faithfully identify computed, numeric, duplicate, or otherwise non-string object keys:
      - `{ array_index: <i> }`    - selects `ArrayPattern.elements[i]`. If that element is a
        RestElement, the next step is `{ rest: true }`; otherwise the element IS the sub-pattern
        (continue).
      - `{ property_index: <j> }` - selects the j-th entry of `ObjectPattern.properties[j]`. If
        that entry is an ordinary Property, revalidation CONTINUES through its `.value`; if it is
        a RestElement, revalidation REMAINS at that node and the following `{ rest: true }`
        traverses its `.argument`. (So `property_index` indexes the property ARRAY, whether the
        entry is a Property or an object-rest.)
      - `{ rest: true }`          - into the current RestElement's `.argument`.
      - `{ assignment: true }`    - into an AssignmentPattern's `.left` (a default `= expr`).
    Examples: `const [a, b] = xs` -> b's path `[ { array_index: 1 } ]`; `const {q: r} = o` -> r's
    path `[ { property_index: 0 } ]` (continues into `.value`); `const {a = 1} = o` -> a's path
    `[ { property_index: 0 }, { assignment: true } ]`; `const [...rest] = xs` -> rest's path
    `[ { array_index: 0 }, { rest: true } ]`; `const {a, ...rest} = o` -> a's path
    `[ { property_index: 0 } ]` and rest's path `[ { property_index: 1 }, { rest: true } ]` (the
    object-rest is `properties[1]`). A future lowerer inspects the RETAINED property-key AST (via
    the node) rather than any lossy normalized key - the object key is NOT flattened into the
    path. This is the section-20 destructuring binding rule (declaration_node + declarator_index
    + binding_pattern_path + initializer_node + kind).
  - `initializer` - the declarator's init expression node (its exact `.type` + byte `.range`;
    no synthesized ranges), or `null` for a name with no initializer (a bare `let x;`). This is
    NOT an error - a lowerer reads `null` as "no source expression".
  A destructured name has ONE shared `initializer` (the declarator's init) plus its own
  `binding_path`, so a lowerer recovers "this name comes from `<binding_path>` of `<initializer>`"
  without the resolver pretending each destructured name has its own expression.
- **function** (`function` decl): `{ form: "function", is_async, is_generator, params, body }`
  where `params`/`body` are the parser's exact subtrees (generators are js.unsupported but the
  shape stays complete).
- **class** (`class` decl): `{ form: "class", super_class, body }` - `super_class` the extends
  expression node or null; `body` the member array.

**Corrupt-state -> a Diagnostic (js.internal), never a wrong answer.** An error is returned for
any impossible/corrupt frontend state - a missing retained node, a node whose `.type` does not
match the declaration kind, a `declarator_index` out of range, a `binding_path` that does not
resolve to the recorded name, a malformed params/body - never for an ordinary "no initializer",
and never for an unsupported LOWERING construct (that belongs to the future lowerer).
Revalidation FOLLOWS the exact `binding_path` pattern EDGES from the declarator's id
(array_index -> `elements[i]`; property_index -> `properties[j]`, continuing into `.value` for a
Property or remaining at a RestElement; rest -> the RestElement's `.argument`; assignment -> the
AssignmentPattern's `.left`) and confirms the TERMINAL node is an `Identifier` whose `name` equals
the recorded name - so a desynced handle cannot produce plausible-but-wrong semantics (mirrors the
Lua name-index guard, generalized to the structural path).

## 6. Two handle layers (kept separate; never serialized)

There are TWO distinct identifier layers; they must not be conflated.

- **Bridge-private ids** `{ session_token, unit_id, decl_id }` - all INTEGERS, never JSValues or
  pointers (Q7). `session_token` names the QuickJS session (C-owned); `unit_id` names a file's
  retained unit within the session; `decl_id` names a retained declaration. The adapter keeps
  session module-scope state: `unit_id -> { ast, comments, linemap }` and `decl_id -> { unit_id,
  node, kind, declarator_index, binding_path, name }`. `analyze` returns `unit_id` + each Decl's
  `decl_id` in the facts (integers only; the retained node/index/path are NEVER on the wire).
  `declarationSemantics(decl_id)` and `scope(unit_id)` look up this state.

  **Session tokens are MONOTONIC / generation-tagged, never raw pointers or reusable slot ids.**
  The C bridge issues each session a strictly monotonically-increasing `session_token` (a
  never-reused counter, not an allocator address or a recycled table index). A resolution carries
  the `{ session_token, unit_id, decl_id }` it was issued; the bridge accepts it only if
  `session_token` matches the CURRENTLY-LIVE session's token. So a stale triple from a closed
  generation cannot accidentally resolve against a LATER session that happens to reuse the same
  heap slot / allocator address (token mismatch -> deterministic stale). `unit_id`/`decl_id` are
  session-local counters, meaningful only under their issuing `session_token`.
- **Analyzer generation handle** - the integer the EXISTING `collect_decls` (analyze.lua:48-66)
  assigns into `disc._handles` (`{ frontend = <js-proxy>, session = <session_token>, decl_id }`).
  This is the MODEL handle a future lowerer resolves via `M.resolve_handle`.

`decl_id` is NOT the model handle and MUST NEVER be copied into a public declaration: the Slice-6
proxy exposes an OPAQUE declaration object (holding the bridge-private `decl_id` + its retained
`unit_id`/`session_token`) to the analyzer, and `collect_decls` assigns the generation handle
into `disc._handles` as it does for Lua. The public declaration carries the model `handle`, never
the bridge-private `decl_id`; the projection drops `_handles` (D6). `unit_id` likewise stays
inside the proxy's opaque unit object and never enters the ProjectDiscovery or the public JSON.

**Ownership + explicit cleanup.** C owns open + close of the one per-generation session
(section 2): inspection mode CLOSES it right after projection; semantic mode CLOSES it at
generation end. After close, the `session_token` is dead; the C bridge checks session liveness
BEFORE calling the adapter and rejects any `decl_id`/`unit_id` of a closed generation with a
deterministic STALE diagnostic - it never dereferences freed QuickJS state and never advertises a
dead id as live. WITHIN a live session, an unknown/never-issued `decl_id`/`unit_id` (a desync)
yields a js.internal-shaped error from the adapter (fail closed). The adapter never frees its own
state mid-session.

## 7. Transport validation (reaffirmed; enforced by the session)

The Slice-1 session already enforces the transport (bytes in, validated JSON out, never-raise,
three-state); the adapter relies on it and adds nothing that can escape it:

- **In:** source crosses as raw bytes (an ArrayBuffer built with the exact VFS/file byte
  length; embedded NUL preserved). `opts` crosses as length-aware JSON, fail-closed on
  malformed/NUL-bearing/trailing-garbage input (js.transport). The adapter lexes over the
  bytes; app source is DATA, never a module, never eval'd.
- **Out:** the adapter's return MUST be JSON (the session fails closed as js.internal if
  JSON.stringify yields a non-string). Facts carry only normalized metadata + integer handles.
- **Resource breaches** in any phase surface as the host-classified js.limit.* (Slice-1
  authoritative markers), never an escaping exception.
- **The adapter never raises.** Every entry (`analyze`, `declarationSemantics`, `scope`) is a
  protected boundary: an internal defect becomes a js.internal-shaped result (a facts object with
  an error diagnostic, or an error record), mirroring the parser/scope hardening. `analyze` on a
  recovered/error-bearing AST still returns `status: "error"` + the diagnostics + whatever
  declarations were recoverable (local degradation), not an internal fault.

## 8. Scope capability through the adapter

`scope(unit_id)` is the advertised scope capability reached through the frontend boundary (the
Lua "scope" capability, D-scope). It looks up the retained unit (by the bridge-private `unit_id`
from `analyze`) and returns the Slice-4 `resolve(unit)` model (`{ ok, bindings, refs }`; byte-only
ranges, section 3a) or, on an unknown `unit_id` / an internal fault, a Diagnostic-shaped error. A
consumer never calls `hull:source:scope` directly across the boundary; it goes through the adapter
so the frontend owns the crossing.

## 9. Module boundary + methods

New module stdlib/cli/js/hull/source/frontend_javascript.js (hull:source:frontend_javascript),
mirroring hull.project.frontend_lua. It imports the Slice 2-4 modules (parser, scope) and
exposes the three operations above as functions the C bridge invokes by method name via
`hl_js_session_analyze(session, "hull:source:frontend_javascript", "<method>", bytes, path,
opts)`. The `capabilities` it advertises: `["declarations", "annotations", "source_ranges",
"scope", "semantics"]` (identical to the Lua adapter). For Slice-5 testing a `frontendAnalyze` /
`frontendSemantics` / `frontendScope` driver in lextest.js exercises the three methods and the
handle lifetime end to end (the real C bridge bindings are Slice 6).

## 10. Testing plan

A new test_js_frontend suite driving the adapter through the driver:

- **facts shape**: a file with const/function/class declarations yields the exact Decl shape
  (kind/name/range/group_range/is_method:false/annotations/decl_id) + status + diagnostics +
  `unit_id`; `status: "error"` iff an error diagnostic is present.
- **nested annotated declarations (full-AST parity)**: an annotated `const`/`function`/`class`
  declared INSIDE a function body, a block, a loop, a `catch`, and a class method body is
  collected (not just top-level); the export wrapper is not double-collected; a method definition
  itself yields no fact.
- **range shape + mixed parity**: every facts range (declaration/group/annotation/diagnostic) is
  `{ start, stop, line, col }` with correct derived line/col; a JS declaration's range shape
  equals the Lua declaration's range shape (mixed-language symmetry).
- **bridge-private ids**: `analyze` returns an integer `unit_id`; the public declaration facts do
  NOT leak the AST or a JSValue; `decl_id`/`unit_id` are integers; a fresh analyze issues fresh ids.
- **one-per-name + group**: `const a = 1, b = 2` -> two Decls sharing group_range; destructuring
  `const [ok, err] = f()` / `const {p, q: r} = o` -> two Decls each, shared group_range; the
  annotation on the declaration is shared across the group's names.
- **kinds + exports**: const/let/var/function/class each with the right kind; exported +
  default-exported named forms produce facts; anonymous default produces none; `text` renamed to
  `value` in annotations.
- **declaration_semantics value**: the multi-declarator identity `const a=foo(), q=bar(),
  c=baz()` -> q's record has declarator_index 1, empty binding_path, initializer the `bar()`
  call node (exact type + range); a bare `let x;` -> initializer null (not an error).
- **declaration_semantics structural paths**: `const [a, b] = xs` -> b's binding_path
  `[{array_index:1}]`; `const {q: r} = o` -> r's path `[{property_index:0}]`; a COMPUTED /
  numeric / duplicate-key object pattern still yields a `property_index` (no lossy key); a DEFAULT
  `const {a = 1} = o` -> `[{property_index:0},{assignment:true}]`; an ARRAY REST
  `const [...r] = xs` -> `[{array_index:0},{rest:true}]`; an OBJECT REST `const {a, ...rest} = o`
  -> a `[{property_index:0}]` and rest `[{property_index:1},{rest:true}]`; a NESTED/default object
  rest revalidates through the exact edges; all names in one declarator share its initializer.
- **declaration_semantics function/class**: params/body / super_class/body recovered exactly.
- **corrupt-state**: an unknown declId, a desynced node/kind, a bad declarator_index -> a
  js.internal-shaped error, never a wrong record; never an error for "no initializer".
- **scope capability**: `scope(unitId)` returns the Slice-4 model for a valid unit; an unknown
  unitId -> an error.
- **handles / lifetime**: decl_id/unit_id are integers; the facts carry no AST/handle-to-JSValue;
  a fresh analyze issues fresh ids; a stale/unknown id fails closed; a STALE-GENERATION resolution
  (a decl_id/unit_id of a closed session) deterministically reports stale, never live (the two
  handle layers stay separate - decl_id is never the model handle).
- **transport / never-raise**: a recovered syntax-error source yields status:"error" + partial
  declarations (no js.internal); a forced internal defect yields a js.internal result; the
  return is always JSON.
- **corpus regression**: `analyze` over all committed JS files yields status in
  {analyzed, error} with no js.internal, and every Decl has a well-formed shape.

All under ASan + UBSan; ASCII-only; no em-dashes.

## 11. Non-goals

No C bridge bindings / registry flip / frontend_javascript.lua proxy (Slice 6). No mixed-language
ProjectDiscovery / dev / inspect wiring (Slice 7). No method-DEFINITION discovery (declaration
nodes only, excluding class/object method definitions; declarations NESTED inside method bodies
ARE collected, section 4). No lowering (Query/Compute) - `declarationSemantics` recovers source
shape, it does not interpret it. No execution of application JS.
