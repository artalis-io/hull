# JavaScript source-analysis frontend - design record

Status: **RATIFIED (with amendments to D6/D7; D2/D3/D5 wording locked). Slice 1 in
progress.** Sign-off recorded in §11.
Related: `docs/project_discovery_design.md` (the host-owned project-discovery layer this
extends), `docs/lua_source_analysis_design.md` (the Lua source layer this mirrors in JS),
`docs/hull_analyze_design.md`.

## 0. Scope

Add a **genuine JavaScript source-analysis frontend** - a real hand-written JS
lexer/parser/annotations/scope/adapter, **implemented in JavaScript** under
`stdlib/cli/js/hull/`, executed by a **new restricted QuickJS tooling runtime**, and
integrated into the existing (Lua-hosted) `ProjectDiscovery` architecture through a small
**C frontend-dispatch bridge**. Lua and JS become genuine peer frontends for static
discovery + controlled frontend-semantic recovery.

**Non-goals (restated so I can be held to them):** Query IR, Compute IR, SQL/WASM
lowering, generic build plugins, `build.js`, a universal AST, executing application JS,
persistent handles, arbitrary-edit-stable identity, full optimizing semantic analysis.
Success = static discovery + controlled semantic recovery, **not** domain lowering.

## 1. Investigation findings (the 13 required answers, with evidence)

### Q1 - Where the single authoritative frontend registry lives after C owns dispatch

Today the authoritative map is the Lua table `FRONTENDS` in
`stdlib/cli/lua/hull/project/registry.lua:20-26` (`ext → {language, frontend_module,
analyzable}`), with capabilities read from the loaded frontend module
(`registry.frontends()`), and `known_exts()` already listing `lua/js/mjs/cjs`. **Decision
surfaced (§11-D1).** Recommendation: **keep the authoritative registry in Lua**, extended
with a per-language `engine` field (`lua` | `quickjs`) and flipping `js/mjs/cjs` to
`analyzable=true, engine="quickjs"`. The C bridge is *told* the language per file by the
Lua orchestrator and maps `language → engine-creation` inside the dispatch binding (the
dispatch *implementation*, not a duplicate registry). No second extension/capability map
appears in C, `hull dev`, `hull agent`, or `build.lua`. (Alternative, if the reviewer
wants C to be the literal authority: migrate the table to a C `static const
HlFrontendSpec[]` surfaced to Lua via a `tool.frontend_registry()` binding - more
invasive; capabilities stay frontend-owned either way.)

### Q2 - Does the Lua orchestrator stay in Lua, or move to C?

**Stays in Lua, unchanged in shape.** `hull.project.analyze` (the orchestrator),
`hull.project.model` (the one `ProjectDiscovery`), `hull.project.projection`, the handle
table, IDs, indexes, and diagnostics remain the single Lua implementation. Evidence: the
analyzer already loops files → `registry.load(row)` → `fe.parse` → `collect_decls`
(`analyze.lua:96-146`, `48-66`), building normalized facts the model consumes. **No part
of the model moves to C.** What C gains is ownership of a new *frontend runtime* (the
QuickJS session) reached through a dispatch binding - orchestration and the model stay
Lua. This satisfies §2 ("do not create a second C implementation of ProjectDiscovery").

### Q3 - How JS frontend results enter the existing builder without a second model

Via the **same normalized per-source facts** the Lua frontend already produces. The
analyzer gains a single uniform seam `analyze_one(language, path, src, opts) -> facts`:
- `language == "lua"` → the in-process Lua frontend (as today: `fe.parse` +
  `fe.declarations` + `decl_*` accessors), producing `{status, diagnostics, declarations
  = [{kind,name,range,group_range,is_method,annotations, _handle_ref}]}`.
- `language == "javascript"` → **one** C-bridge call `tool.frontend_analyze("javascript",
  path, src, opts)` that runs the bundled JS adapter in a QuickJS session and returns the
  **identical facts shape** (validated JSON → decoded to a Lua table).

Both feed the unchanged `model.build` (`docs/project_discovery_design.md` D4/D5/D6). The
facts carry only normalized metadata - never AST. The JS path is one crossing per file,
not per-accessor.

### Q4 - How source bytes cross C → QuickJS without truncation/encoding ambiguity/execution

Source is passed **as raw bytes, length-aware**: C hands the QuickJS tooling context an
`ArrayBuffer` (or `Uint8Array` view) built with the exact byte length from the VFS/file
read (never a NUL-terminated `const char *`; embedded NUL is preserved). The bundled JS
frontend lexes **over the byte array** (§4), so the app source is *data*, never a module,
never `eval`'d - it is compiled by nothing but the hand-written parser. The only place the
*vendored QuickJS parser* touches app source is the **compile-only conformance oracle**
(`JS_EVAL_FLAG_COMPILE_ONLY`, no execution - proven in `bytecode_cache.c:91-96`), which is
a test-time gate, not the production path.

### Q5 / Q4-encoding - Byte ranges despite JavaScript's UTF-16 strings

**The lexer operates on bytes, not JS strings**, so Hull's 1-based, half-open, byte-offset
ranges (`docs/project_discovery_design.md`; `range.lua`) are produced **by construction** -
there is no UTF-16 code-unit indexing to map back. The JS lexer reads the `Uint8Array`,
decodes UTF-8 code points only where classification needs it (identifier-start,
whitespace, line terminators incl. LF/CR/CRLF and U+2028/U+2029), and tracks positions in
**bytes**. This mirrors the Lua lexer (which scans the byte string). Malformed UTF-8 is
classified deterministically: a byte sequence that is not valid UTF-8 inside a token
context yields a `js.syntax` (or `js.unsupported` for an unhandled-but-valid case)
diagnostic with an exact byte range; it never silently maps to a wrong offset. (This
sidesteps §5's "code-unit mapping" allowance entirely; the alternative - lex JS strings +
maintain a tested code-unit→byte map - is rejected as more complex and error-prone.)

### Q6 - Who owns a QuickJS analysis session, how long it lives, when it is destroyed

**C owns it** (a new `src/hull/frontend/js_session.*`). Two lifetimes (§7 / §22):
- **Serialized-inspection generation** (`hull agent inspect`, `hull dev --agent`
  publishing): the bridge creates a session, analyzes the file, extracts normalized facts,
  and **destroys the session immediately** - before facts are returned. No handle is
  resolvable afterward; nothing is serialized. This is the only path the two user surfaces
  use.
- **In-process semantic/lowering generation** (future build/lowering + this story's
  semantic tests): the bridge creates a session and **retains** it for the generation so
  `declaration_semantics` can reach the live JS AST; the generation destroys it when
  consumers finish; every stale handle is then rejected. QuickJS teardown is
  `JS_FreeContext` + `JS_FreeRuntime` (`runtime.c:1038-1043`).

### Q7 - Generation-local handles without leaking JSValue/pointers/parser objects

Public/handle tokens are **integers**, never `JSValue`s or pointers. The Lua handle table
(`disc._handles`) already stores `{frontend, unit, declaration}` for Lua
(`docs/project_discovery_design.md` D3.1). For JS it stores `{frontend = <js-proxy>,
session = <opaque int session token>, decl_id = <int>}` - all integers/tables, no QuickJS
values. `resolve_handle` returns that; the JS proxy's `declaration_semantics(resolved)`
calls `tool.frontend_declaration_semantics(session, decl_id)` → C validates the session is
live (generation match) → runs the JS adapter → returns a **frontend-specific serialized**
semantic record. Post-teardown the session token is dead → the binding returns a stale
diagnostic; nothing dereferences freed QuickJS state. Handles are **never serialized** (the
projection drops `_handles`).

### Q8 - Which JS extensions Hull supports

`.js`, `.mjs`, `.cjs` - activated because the corpus uses them (ES-module `.js` throughout
`stdlib/js/hull/**`; one `.mjs` e2e). `known_exts()` already lists all three; today they
are `analyzable=false`. No extension is activated for symmetry alone.

### Q9 - Which ECMAScript constructs the committed Hull JS corpus uses

Full matrix in §3, grounded in the 151 application-source files (`stdlib/js/hull/**` +
`examples/**` + `tests/fixtures/**`). Headline: heavy `const/let/var`, function decls +
expressions + **arrow** + **async/await** (+ `for await…of`), **classes** (constructor +
method shorthand; no `#`-fields, no statics), **array destructuring** (`[ok, err]`) +
light object destructuring, **template literals** + `${}`, **regex literals**, `??`/`?.`
(mostly in generated output; some source), `import` (named/default/namespace) + `export`
(named/default) + dynamic `import()` + a top-level `await import`, `for/for…of/while`,
`switch`, `try/catch/finally`+`throw`, `new/typeof/instanceof`. **Absent:** generators/
`yield`, private fields, statics, `for…in`, `do…while`, labeled statements, tagged
templates, optional catch binding, rest/spread-in-formal-syntax (code uses `.slice()`/
`Object.assign`). JSDoc `/** … */` with `@`-tags is pervasive.

### Q10 - Compile-only conformance against vendored QuickJS

Oracle = the vendored QuickJS `2024-01-13` (`mk/vendor/quickjs.mk:18`), compile-only:
`JS_Eval(ctx, src, len, path, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY)`
(`quickjs.h:296-307,784`; production precedent `bytecode_cache.c:91-96`). Returns a
bytecode/module object on accept, an exception (`JS_IsException`) on reject, **without
running** top-level code. A C harness links the same vendored QuickJS + runs my JS parser
(inside a tooling session) and compares accept/reject over the corpus + curated
negatives, with a documented closed set of static-semantic divergences and three states
(accept/reject/**indeterminate**), mirroring the Lua conformance harness
(`tests/hull/source/test_lua_source.c`). Details §9.

### Q11 - Resource limits protecting the tooling runtime

Reuse QuickJS's mechanisms with tooling-specific values (§5): `JS_SetMemoryLimit`,
`JS_SetMaxStackSize` (`runtime.c:646-648`), `JS_SetInterruptHandler` instruction budget
(`runtime.c:54-64,678`), plus frontend-level caps (max bytes/tokens/depth/diagnostics,
bounded transport). Exhaustion → structured `js.limit.*` / `js.internal` (indeterminate),
never an escaping exception or crash.

### Q12 - Mixed Lua/JS analysis in dev / dev --agent / agent inspect

**Zero new discovery logic in any of them.** All three already route through the one
canonical analyzer (`agent.c:cmd_inspect` → `hull_tool("hull.project.inspect")`;
`dev.c:agent_publish_discovery` spawns `hull agent inspect --generation --session-pid`).
Flipping `js/mjs/cjs` to `analyzable=true` + wiring `analyze_one` makes the existing single
pass analyze both languages into one `ProjectDiscovery`. Sidecar schema + projector
unchanged (Q27 sample already valid - `sources[].language` exists). `static/*.js` stays
pruned (D6 "application source" definition unchanged).

### Q13 - The precise future build seam (no lowering, no second pass)

Unchanged from `docs/project_discovery_design.md` D10: `hull.project.analyze(app_dir)` is
the abstraction a future lowering consumer calls at the documented `build.lua main()`
point (after `parse_args()`, before `discover()`). This story does **not** modify
`build.lua`, add `build.js`, or add a second build-time parse. The seam now yields mixed
Lua+JS discovery for free.

## 2. Architecture

```
                         Hull CLI / (future) build host  [C]
                                     │  hull agent inspect / hull dev
                                     ▼
                     hull.project.analyze  [Lua tool VM]           ← orchestration (unchanged)
                                     │  analyze_one(language, path, src, opts)
                        ┌────────────┴─────────────┐
              language=lua│                        │language=javascript
                          ▼                        ▼  tool.frontend_analyze (C bridge)
              hull.project.frontend_lua   ┌──────────────────────────────┐
              (in the Lua tool VM)        │  C frontend dispatcher        │  ← C OWNS: runtime
                          │               │  src/hull/frontend/js_session │     selection, session
                          │               │  → restricted QuickJS tooling │     lifetime, transport
                          │               │     VM runs the bundled JS    │     validation, opaque
                          │               │     adapter (compile-only-ish)│     tokens
                          │               └──────────────┬────────────────┘
                          │                              │ validated normalized facts (JSON→table)
                          └──────────────┬───────────────┘
                                         ▼
                          normalized per-source facts (one shape)
                                         ▼
                          hull.project.model → ProjectDiscovery   ← the ONE builder (unchanged)
                                         ▼
                          hull.project.projection → public JSON
                                         ▼
                          hull agent inspect / hull dev --agent sidecar
```

**Layer naming (ratified - resolves the "C dispatcher above the VMs" diagram conflict):**

```
Lua project orchestrator          ← owns orchestration + the one ProjectDiscovery model
    ↓ canonical registry SELECTS frontend + engine   (registry stays in Lua)
C tooling-engine dispatcher       ← C-OWNED RUNTIME DISPATCH (not extension selection)
    ↓ owns the runtime crossing: QuickJS create / invoke / limits / transport-validate / teardown
QuickJS frontend session          ← the JS lexer/parser/annotations/scope/adapter run here
    ↓ normalized transport (bytes in, validated JSON facts out)
existing Lua ProjectDiscovery builder
```

The substantive invariant: **no Lua code loads `parser.js`, touches QuickJS, or interprets
JavaScript ASTs.** Lua *performs frontend selection from the canonical registry* and
*requests* an analysis; **C owns tooling-engine dispatch - QuickJS creation, invocation,
limits, transport validation, and teardown** - Lua never obtains or operates a QuickJS
runtime. This is **C-owned runtime dispatch**, NOT C owning extension selection. C
maintains **no** duplicate extension map. The JS frontend never sees Lua.

### File layout (new)

```
stdlib/cli/js/hull/source/        (NEW - trusted tooling JS, embedded, tooling-VM-only)
    javascript.js                 public contract: parse(bytes, path, opts) -> unit
    lexer.js                      byte-oriented lexer + exact byte ranges
    parser.js                     recursive-descent parser -> JS-specific AST
    range.js                      byte range/linemap helpers (JS mirror of range.lua)
    diagnostic.js                 js.syntax / js.unsupported / js.limit.* / js.internal
    annotations.js                JSDoc /** @tag */ scan + structural attachment
    scope.js                      JS scope/binding resolver
stdlib/cli/js/hull/project/
    frontend_javascript.js        adapter: declarations/decl_*/annotations/scope/
                                  declaration_semantics + the analyze-one entry that emits
                                  normalized facts JSON
src/hull/frontend/                (NEW C)
    js_session.{c,h}              restricted QuickJS tooling runtime + module loader for the
                                  cli/js VFS only; create/analyze/semantics/destroy; limits
    dispatch.{c,h}                tool.frontend_analyze / tool.frontend_declaration_semantics
                                  bindings (registered via tool_orchestration.c)
stdlib/cli/lua/hull/project/
    frontend_javascript.lua       THIN Lua proxy implementing the frontend contract by
                                  calling the C bridge (so the analyzer stays uniform)
    registry.lua                  +engine field; js/mjs/cjs analyzable=true
    analyze.lua                   + analyze_one seam (minimal branch: lua in-proc / js bridge)
```

Makefile: add a `STDLIB_JS_CLI_FILES := find stdlib/cli/js -name '*.js' -not -path
'*/tests/*'` glob + a **separate** embedded registry `hl_stdlib_js_cli_entries[]` (mirrors
`STDLIB_JS_FILES` at `Makefile:1433,1601-1614`), loaded **only** into the tooling VFS -
NOT the application JS runtime (these are trusted tooling modules, never app-importable).

## 3. JavaScript syntax-support matrix (Slice-2 target = the real corpus)

`supported` = Slice 2 parses it into a correct AST. `js.unsupported` = valid ECMAScript
the parser recognizes but declines (structured diagnostic, never a wrong AST). `not-in-
corpus` = absent from Hull's application source; deferred. `js.syntax` = malformed.

| Construct | Status | Notes |
|---|---|---|
| `const`/`let`/`var`, blocks, `return`, `if/else` | **supported** | pervasive |
| function decl / function expr / arrow / async / `await` / `for await…of` | **supported** | async is heavy (`email.js`, `retry.js`, `attachment.js`) |
| calls / member / computed / `new` / `typeof` / `instanceof` | **supported** | |
| object / array literals, shorthand, method shorthand, computed keys | **supported** | |
| unary / binary / logical / conditional / assignment expr | **supported** | |
| template literals + `${}` interpolation | **supported** | `i18n.js` etc. |
| regex literals + regex/division disambiguation | **supported** | `validate.js:25`, `config.js:119` |
| `for` / `for…of` / `while`, `switch`, `try/catch/finally`, `throw`, catch-binding | **supported** | |
| `import` (named/default/namespace), `export` (named/default), dynamic `import()` | **supported** | `hull:`-scheme specifiers are ordinary string literals |
| **array destructuring** decls (`const [a,b] = …`) | **supported** | `[ok, err]` idiom is common; needs correct binding rep |
| **object destructuring** decls | **supported** (light) | present but sparse |
| destructuring **parameters**, default params | **supported** | |
| automatic semicolon insertion | **supported** | required to parse the corpus at all |
| optional chaining `?.`, nullish coalescing `??` | **supported** | `verify.js:79`, `template.js:79` |
| top-level `await` (module) | **supported** | one fixture uses `await import` |
| generators / `yield`, async generators | **not-in-corpus → js.unsupported** | none found |
| private class fields `#x`, static members | **not-in-corpus → js.unsupported** | none found |
| `for…in`, **`for await…of`** | **supported** [Slice-2 conformance] | corpus grew to use both (`template.js`, `irc_chat`, `multipart_upload`, several `web/middleware/*`); the corpus is the target, so they are parsed to `ForInStatement` / `ForOfStatement{await:true}` |
| `do…while`, labeled statements | **not-in-corpus → js.unsupported** | none found |
| tagged templates | **supported** [Slice-2] | trivially fell out of the member/call tail |
| optional catch binding `catch {}` | **supported** [Slice-2] | |
| rest/spread `...` (array/obj literals, params, call args) | **supported** [ratified D4] | absent as formal syntax in source, but idiomatic + spans several grammar contexts; supported structurally in Slice 2 rather than temporary exclusions |
| `export` re-export / `export *` | **not-in-corpus → js.unsupported** | none found |
| BigInt / Symbol / Proxy / Reflect literals-or-syntax | **n/a** | not syntax the analyzer must recognize specially |

**Destructuring binding rule (§20):** array/object destructuring **declarations** are
parsed structurally; each **bound name** becomes a discovery declaration fact sharing the
statement's `group_id`, and its private semantic identity retains
`{declaration_node, declarator_index, binding_pattern_path, initializer_node,
kind}` so a future JS lowerer can recover the binding. If a destructuring shape proves
lowering-ambiguous in Slice 5, that specific *declarator* emits `js.unsupported` for
semantics while remaining a discoverable name - never a silently-wrong flattening.

## 4. Byte-range contract (ratified: byte-oriented lexing over ArrayBuffer/Uint8Array)

The lexer scans the raw byte view with a **byte cursor** and produces exact half-open
`[start, stop)` **byte** offsets by construction (§Q5). It **decodes UTF-8** at exactly the
points classification requires - **identifiers** (a code point's ID_Start / ID_Continue
class), **string** and **template** contents (incl. escape sequences: `\xHH`, `\uHHHH`,
`\u{…}`, line continuations), and **comments** - while the position it *records* stays a
byte offset (decoding advances the cursor by the UTF-8 byte length, 1-4). A code point is
never exposed as a code-unit index. `diagnostic` ranges are byte offsets too. `range.js`
builds a byte→line/col linemap exactly as `range.lua` does.

**Locked range/encoding tests (Slice 2):** ASCII; multibyte UTF-8 *before* a node and
*inside* identifiers/strings/templates; **astral characters** (4-byte UTF-8 / surrogate-
pair source - the byte range must span all 4 bytes, and the lexer must not split a code
point); **string/template escapes** (`\n`, `\t`, `\xHH`, `\uHHHH`, `\u{1F600}`, `\` line
continuation) with exact ranges; **CR / LF / CRLF** (+ U+2028/U+2029) line terminators and
their effect on the linemap and on ASI; line and block **comments** (incl. multibyte
inside); **EOF** positions; **embedded NUL** (length-aware transport - must not truncate
the scan or a following token); and **malformed UTF-8** - an invalid byte sequence in a
token context yields a structured `js.syntax` (or `js.unsupported`) diagnostic with an
exact byte range, never a silently-wrong offset or a crash.

## 5. Restricted QuickJS tooling runtime + limits + security

A **new** context, strictly less privileged than the app runtime (`runtime.c:642-707`):
- `JS_NewRuntime` + `JS_NewContextRaw` (minimal intrinsics), then add **only** what the
  parser needs: base objects, JSON (result transport), RegExp + compiler (the parser JS
  may use regex internally), Map/Set, TypedArrays (byte buffers). **Do NOT add** `eval`,
  the `Function` constructor (both already removed by `hl_js_sandbox` - the tooling VM
  never adds them back), Date, Promise, Proxy. No `std`/`os` (never added in Hull anyway).
- **Module loader:** a tooling-only loader that resolves **exclusively** the embedded
  `hull:cli:source:*` / `hull:cli:project:*` tooling modules from the cli-js VFS (mirrors
  `hl_js_module_loader` VFS lookup, `runtime.c:228-350`). It **cannot** load application
  modules or filesystem paths. App source arrives as an `ArrayBuffer` argument, never a
  module.
- **No host authority bindings:** the session exposes to JS only (a) the source bytes +
  path + options, (b) a return channel. No `db`/`fs`/`http`/`env`/`crypto`/`compute`/
  network/spawn.
- **Limits [ratified D6, calibration-gated].** The 32 MiB-source / 128 MiB-heap pairing
  from the draft was internally inconsistent (a 32 MiB source easily needs >128 MiB once
  bytes + tokens + AST + strings + transport coexist). **Defaults are derived from measured
  corpus peaks + adversarial tests, not guessed.** Starting points, to be calibrated in
  Slice 1/2 and recorded:
  - `max_bytes`: **4 MiB per source** (matches Hull's existing analysis-oriented limits;
    raise only on real corpus evidence);
  - QuickJS **memory**: 128 MiB;
  - QuickJS **stack**: 1 MiB (subject to deep-nesting tests - a pathological nesting fixture
    must hit `js.limit.depth` before the stack limit, cleanly);
  - **instruction budget**: **start below 200 M** and calibrate against the largest corpus
    file × a safety multiplier (record the measured instruction high-water mark);
  - **separate** `max_tokens`, `max_depth` (AST/parser nesting), `max_diagnostics`, and
    **result-size** caps.
  Slice 1/2 record the **measured high-water marks** (peak QuickJS heap, instructions,
  tokens, depth) for the largest corpus file, and each limit is set to that × a documented
  safety multiplier. **Every** limit breach → a structured **indeterminate** `js.limit.<which>`
  diagnostic (never a raw exception or crash), with the session torn down cleanly.
- **Never-raise boundary:** the C bridge runs the JS adapter under a guard; a JS exception
  or interrupt becomes a structured `js.internal` / `js.limit.*`, never an escaping QuickJS
  exception or process abort (mirrors the Lua frontend's `pcall` boundary + the Lua
  three-state accept/reject/indeterminate contract).

**Security verification (§31), tested not claimed:** a tooling-boundary test asserts the
session has no `db`/`fs`/`http`/`env`/`eval`/`Function`/module-loading-of-app-source; that
application JS is parsed, never executed (a fixture whose top-level would throw/side-effect
if run parses clean); that malformed/truncated transport fails closed; that limits fire;
that teardown is clean under ASan/UBSan/LSan.

## 6. C ↔ QuickJS transport (§23) [ratified D5]

- **C → JS (source):** length-aware **raw bytes** as an `ArrayBuffer`/`Uint8Array`
  (NUL-safe) + `path` string + `options` object. No JSON-stringifying of source.
- **JS → C (facts):** the adapter returns **normalized facts as a JSON string** - a
  `schema_version`'d, size-bounded document of structured metadata only (status,
  diagnostics, declarations with kind/name/byte-range/group/annotations; **no AST**). C
  validates it **fail-closed** before the Lua analyzer sees it:
  - **complete-document** validation (`sh_json_parse` over the exact byte length - a
    truncated/embedded-NUL/trailing-garbage result is rejected, mirroring the discovery.json
    envelope check in `agent.c`);
  - **required-field** presence + **unknown-required-field / duplicate-key** policy (a
    missing required field or an unexpected structure fails closed, not a silent default);
  - **numeric range** validation (byte offsets are positive, `start < stop`, within the
    source length; counts within their caps);
  - **deterministic ordering** (declarations in source order; diagnostics in a stable
    order) so inspection output is byte-identical across runs.
  JSON is the transport for **this boundary only** - deterministic, bounded,
  language-neutral, already the discovery wire format. It is **not** a mandate that future
  compiler phases use JSON. Frontend-owned AST + semantic state stay inside the session,
  never in the facts.
- **Semantics (in-process generation) - the in-session model [ratified D7]:**

  ```
  ProjectDiscovery handle
      ↓
  C validates generation + session + declaration token   (stale → structured diagnostic)
      ↓
  the operation executes INSIDE the owning QuickJS tooling session
      ↓
  the JS frontend/lowerer accesses the LIVE JS AST (never shipped through C)
      ↓
  returns a BOUNDED frontend-specific summary today,
  or frontend-neutral domain IR in a future story
  ```

  For **this** story, `tool.frontend_declaration_semantics(session, decl_id)` returns a
  **bounded summary that proves correct identity** - declaration form, declarator index,
  initializer **kind + byte range**, function flags (async/generator/method), parameter
  **ranges**, body **range** - enough to recover "`q` ⇐ `bar()`". It is explicitly
  documented as **NOT the future lowering payload**: the future JS Query/Compute lowerer
  **runs in the owning QuickJS session** and returns **Query IR** (or similar), rather than
  shipping the JS expression tree through C into Lua (which would gradually recreate AST
  transport). This keeps the boundary from ever becoming an AST-over-JSON channel.

## 7. Handles + two lifetimes (§22) - see Q6/Q7. Public model contains **no** ASTs and
**no** handles; sidecars carry no session/JSValue/handle. Stale-handle rejection is tested
for both languages (post-teardown resolve → structured stale diagnostic, no UAF).

## 8. Conformance (§16) + fuzz

Compile-only QuickJS oracle over: (a) the committed repo JS corpus (positive), (b) curated
syntax negatives, (c) a closed, documented list of static-semantic rejections the
structural parser intentionally does not enforce (e.g. duplicate-lexical-binding early
errors), (d) deterministic bounded mutation. Directional invariant: *my parser rejects ⇒
QuickJS rejects*; *my parser accepts (supported subset) ⇒ QuickJS accepts* except the
closed divergence list. `js.internal`/`js.limit.*`/transport failure = **indeterminate** →
fails the gate. Reported for the **declared subset only** (no full-ECMAScript claim). Fuzz:
a `fuzz_js_source` never-crash + range-invariant harness (bounded QuickJS session), plus a
transport-validation fuzz on the C bridge; if not fully in this story, a concrete
continuous-fuzz follow-up is documented (mirrors `fuzz/fuzz_lua_source.c`).

## 9. Delivery slices (§33)

1. **QuickJS tooling harness + transport.** `src/hull/frontend/js_session.*`: restricted
   context, cli-js VFS module loader, limits, ArrayBuffer-in / validated-JSON-out, never-
   raise + three-state, clean teardown. A trivial bundled `probe.js` proves the crossing +
   security boundary + limits under ASan/UBSan/LSan. No parser yet.
2. **JS lexer + byte ranges + parser + diagnostics + compile-only conformance** over the
   supported subset (§3), + the range matrix (§4). Unsupported-valid → `js.unsupported`;
   malformed → `js.syntax`.
3. **JSDoc/Hull annotation scan + structural attachment** (§17-18): generic tags, unknown-
   tag survival, group `target_group_id`, export-wrapper attachment, no string/expr false
   matches.
4. **JS scope/binding resolver** (§19): module/function/block scope; var/let/const/param/
   import/catch/loop/function bindings; read/write/shadow; documented omitted early errors.
5. **JS frontend adapter + `declaration_semantics`** (§20-21): the contract methods, the
   frontend-private declaration identity, the frontend-specific semantic record, corrupt-
   state → `js.internal`; multi-declarator identity (`const a=foo(), q=bar(), c=baz()` →
   `q ⇐ bar()`).
6. **C frontend dispatcher + single-registry integration** (§10-11): `tool.frontend_
   analyze` / `tool.frontend_declaration_semantics`, registry `engine` field, `analyze_one`
   seam, the Lua `frontend_javascript.lua` proxy. Lua stays uniform; C owns the crossing.
7. **Mixed-language ProjectDiscovery + dev + inspect + lifecycle + security validation**
   (§25-28, §31): `app.lua`(@query) + `worker.js`(@compute) → one discovery; `hull dev`,
   `hull dev --agent`, `hull agent inspect` all observe both; live-vs-standalone parity;
   generation teardown + stale rejection; the full security-boundary test; **all validated
   through `hull dev` / `hull dev --agent` / `hull agent inspect`** per §3 (Hull evaluates
   its own output), not just unit tests.

Each slice: reconcile → implement → tests → review → green CI → merge, before the next.

## 10. Testing (§30) - summarized

Lexer/ranges; parser (supported matrix + recovery + `js.unsupported`/`js.syntax` + limits);
annotations (multi/unknown/args/JSDoc/malformed/strings/expr-position/trailing/blank-line/
export/nested/group); scope/bindings (module/fn/block/var/let/const/closures/params/
imports/loops/catch/globals/read-write/shadow/property-vs-reference); multi-declarator
identity (`q ⇐ bar()`) + destructuring per its declared status; conformance (corpus +
negatives + divergence list + mutation + indeterminate-fail-closed); tooling boundary
(execute-never, no capabilities, malformed-transport-fail-closed, exceptions→diagnostics,
limits, teardown, stale-handle reject, same-generation semantics resolve); mixed-language
one-discovery; **CLI/dev**: `hull dev` / `hull dev --agent` / `hull agent inspect` all see
the mixed model (live + standalone); sanitizers + fuzz.

## 11. Ratification (sign-off recorded)

- **D1 - Registry ownership [APPROVED].** The authoritative registry **stays in Lua**
  (`hull.project.registry`) with an explicit `engine` field (`lua` | `quickjs`);
  `js/mjs/cjs → analyzable=true, engine="quickjs"`. It remains the **one**
  extension/language/capability registry. **C maintains no duplicate extension map** - it
  is told the language per file and maps `language → engine-creation` inside the dispatch
  implementation.
- **D2 - Dispatcher shape [APPROVED, wording locked].** **Lua performs frontend selection
  from the canonical registry; C owns tooling-engine dispatch - QuickJS creation,
  invocation, limits, transport validation, and teardown.** Lua *requests* an analysis but
  **never obtains or operates a QuickJS runtime.** This is **C-owned runtime dispatch**, not
  C owning extension selection. Realized as the `tool.frontend_analyze` binding; the model
  is **not** re-hosted in C.
- **D3 - Byte-oriented lexing [APPROVED].** Lex over the `ArrayBuffer`/`Uint8Array` with a
  byte cursor; UTF-8 decode for identifiers/strings/templates/comments/diagnostics while
  recording byte offsets (§4). Locked tests: astral chars, malformed UTF-8, escapes,
  CR/LF/CRLF, embedded NUL.
- **D4 - Rest/spread [APPROVED].** Implemented in Slice 2 (structurally, across array/obj
  literals, params, call args) - no temporary exclusions.
- **D5 - Transport [APPROVED, requirements locked].** Length-aware raw bytes in;
  `schema_version`'d + size-bounded validated JSON facts out, with complete-document
  validation, required/duplicate/unknown-field policy, numeric-range validation, and
  deterministic ordering (§6). JSON is this boundary's transport only - not a mandate for
  later compiler phases.
- **D6 - Limits [ADJUSTED before locking].** The draft pairing was inconsistent; defaults
  are **derived from measured corpus peaks + adversarial tests** (§5): `max_bytes` 4 MiB;
  QuickJS memory 128 MiB; stack 1 MiB (deep-nesting-tested); instruction budget started
  **below 200 M** and calibrated against the largest corpus file × a safety multiplier;
  separate `max_tokens` / `max_depth` / `max_diagnostics` / result-size caps. Measured
  high-water marks recorded; every breach → indeterminate `js.limit.*`.
- **D7 - Semantic recovery [RE-SPECIFIED: in-session model].** Not a serialized-AST channel.
  The handle → C validates generation+session+declaration token → the operation **executes
  inside the owning QuickJS session** → the JS frontend/lowerer accesses the **live** JS AST
  → returns a **bounded frontend-specific summary today** (form, declarator index,
  initializer kind/range, function flags, parameter ranges, body range - proving identity,
  e.g. `q ⇐ bar()`) **or frontend-neutral domain IR in a future story** (§6). The summary is
  documented as **not** the future lowering payload; the future JS Query lowerer runs
  in-session and returns Query IR, never shipping the JS AST through C. The session stays
  alive for an in-process generation and is destroyed after its consumers finish; serialized
  inspection generations discard sessions and expose no handles.
- **D8 - Seven separate slices [APPROVED].** Do NOT merge runtime and parser. **Slice 1**
  independently proves: the restricted runtime, byte transport, tooling-only module loading,
  limits, exception→structured-diagnostic conversion, session lifecycle, and a trivial
  bundled-tool invocation - so **Slice 2 debugs parser correctness without also debugging
  the VM boundary.**

Amendments folded in. Implementation proceeds per §9, **Slice 1 only**, before Slice 2.
