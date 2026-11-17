# TypeScript - design

Status: **T1 (types + `hull types`) and T2 (the TypeScript frontend, §3.5) implemented;
T3 design-only, its prerequisites in progress (§1).** Builds on
[buildcontext_design.md](buildcontext_design.md) (BuildContext),
[dev_pipeline_generations_design.md](dev_pipeline_generations_design.md) (pipeline
generations), [javascript_source_frontend_design.md](javascript_source_frontend_design.md)
and [project_discovery_design.md](project_discovery_design.md). Where this document and
BuildContext seem to differ, BuildContext wins.

## 0. What this is

Two independent capabilities, delivered in this order:

1. **Types for authoring** (§2). `.d.ts` declarations for Hull's JavaScript surface and a
   `hull types` command that installs them in an app. Editors and an external `tsc
   --noEmit` type-check a plain JavaScript app or a TypeScript one. Hull itself does not
   see TypeScript. Ships first; depends on nothing below.
2. **Running TypeScript source** (§3, §4). `app.ts` and `.ts` modules run in `hull dev`,
   `hull test`, `hull agent` and `hull build` with no external tool. Types are ERASED, never
   checked or compiled: a TypeScript frontend finds the type syntax, and a bundled
   build-pipeline plugin replaces it with whitespace. It runs through the one BuildContext
   pipeline and, outside `hull build`, through pipeline generations - there is no loader
   transform and no TypeScript-specific path.

Until (2) lands, a TypeScript app is compiled by the developer (`tsc`, esbuild) and Hull
runs the emitted `.js`; (1) makes that workflow typed. Documented as the supported
interim, not a workaround.

Non-goals: type checking inside Hull, `.tsx` / JSX, decorators beyond what the JS runtime
runs natively, downlevel transforms, `tsconfig.json` interpretation (paths, baseUrl,
module resolution modes), CommonJS interop.

## 1. Ordering and dependencies

| Step | What | Depends on |
|---|---|---|
| T1 | `.d.ts` + `hull types` (§2) - implemented | nothing |
| T2 | TypeScript frontend, analysis only (§3) - implemented (§3.5) | the shipped JS frontend |
| BuildContext 1, 2, 5 | action core (1, implemented), outputs (2), plugin runner (5) | - |
| G1, G2 | pipeline generations: overlay, runner wiring | BuildContext 2, 5 |
| T3 | type-erasure plugin + `.ts` entry points (§4) | T2, BuildContext 1, 2, 5, G1, G2, and the explicit-route part of BuildContext 6 |

BuildContext checkpoints 3 (artifact inputs) and 4 (the tool seam) are not on T3's path:
erasure takes a source selection, not artifacts, and runs no tool. They follow when Compute
IR needs them.

T2 is analysis, like the JS frontend before it: it makes `.ts` files visible to
ProjectDiscovery (declarations, annotations, `hull agent inspect`) and lowers nothing, so
it does not wait for the pipeline. T3 is the first real source plugin. It is not
BuildContext checkpoint 5's parity demonstration (that one consumes Lua AND JavaScript
selections; erasure has no Lua input), and it needs one explicit pipeline route from
checkpoint 6, not the full BuildArtifact model (§4.2).

## 2. Types for authoring (T1) - implemented

### 2.1 What ships

`types/` in the repository, embedded in the hull binary only (`hl_types_entries`, a
registry linked into `hull` alone, so a built app carries none of it):

- `types/hull/modules/<a>.<b>.d.ts`: one `declare module "hull:<a>:<b>"` per JavaScript
  module of the registry, first-party stdlib and native alike. Internal modules
  (`hull:_template`, `hull:db:_internal`, `hull:ssh:wire`, ...) are not declared; nor are
  registry modules with no JavaScript import (`hull:timers`, `hull:web:sse`,
  `hull:inspect`), and the drift check (§2.3) asserts they stay undeclared;
- `types/hull/app.d.ts`, `globals.d.ts` (`hull`, `console`) and `core.d.ts`: the `app`
  intrinsic (module-conditional members documented), the shared `Hull` namespace -
  request / response, handlers and middleware, the `app.main` context, WebSocket / SSE
  shapes, database connections, buffers, refusal errors;
- `types/hull-test/test.d.ts`: the `test` global of `hull test` files, kept apart so app
  code does not see it;
- the manifest: `app.manifest()` takes a typed `Hull.Manifest` (modules as the
  `"hull/<name>@<major>"` union with an optional `?`, plus every section the extractor
  reads), so a misspelled manifest key is a type error.

Values whose shape the APP defines - a database row, a JSON body or payload, a job input,
a `req.ctx` slot - are `Hull.Data` (`any`, as `JSON.parse` returns), the one exception to
the no-`any` rule; where the API can take a type parameter (`conn.query<User>()`,
`test.get<T>()`) it narrows them. Everything the API defines is typed exactly.

### 2.2 `hull types [app_dir] [-o DIR] [--force] [--check]`

A tool-mode command (`stdlib/cli/lua/hull/types.lua` + a thin C dispatcher, like
`hull init`), reading the declarations through `tool.types_files()`:

- writes them to `<app_dir>/types/` (or `-o DIR`); the files it wrote are recorded with
  their SHA-256 and the hull version in `types/hull/.hull-types`;
- a re-run (after `hull update`) replaces a recorded file that is unchanged and removes
  one this version no longer ships; a file that was edited, or that Hull did not write,
  is a conflict: nothing is written and the command fails unless `--force`;
- the record is a file in the app tree, so its names are repo content: only a name hull
  types could have written (`hull/...` or `hull-test/...`, plain components, ending in
  `.d.ts` or `.md`) is acted on, any other entry is ignored (and reported by `--check`);
  a path below `types/` that runs through a symbolic link (a directory included) is a
  conflict, for writes and removals alike, and no `tsconfig.json` is written through a
  linked `tests/` (audit 46);
- `--check` writes nothing and fails when the installed declarations differ from this
  hull's (an app's CI can run it);
- writes `tsconfig.json` (and `tests/tsconfig.json` for test files, which adds
  `types/hull-test`) only where none exists, an existing one is left alone; the defaults
  are `checkJs`, `strict` with `noImplicitAny` and `strictNullChecks` off - plain
  JavaScript without annotations - which a TypeScript app can tighten;
- refuses a symbolic link as the destination, and escapes what it prints.

Offering it from `hull new` / `hull init` is left for T3, when a scaffold can be
TypeScript.

### 2.3 Keeping them true (`make check-types`, CI job `types`)

A declaration that disagrees with the runtime is worse than none. With the TypeScript
compiler pinned in `tests/types/package-lock.json` (7.0.2):

- **Strict compile:** the declarations and fixtures compile under `strict`.
- **Drift check:** `tests/types/inventory.sh` loads every registry module in the built
  hull and records its live exports, four levels deep (member names and kind: function /
  object / primitive); `tests/types/drift.mjs` turns that into type-level assertions
  (`build/types/drift.ts`) that tsc checks against the declarations: a member missing from
  the types, a declared member the runtime lacks, a wrong kind, or a non-JavaScript module
  that is declared all fail, naming the key. No compiler API is used - tsc is the checker.
- **Fixtures:** `tests/types/fixtures/*.ts` use every module as an app would, with
  `// @ts-expect-error` on calls that must be rejected (an unused one fails).
- **Examples:** every `examples/**/*.js` type-checks as JavaScript (`checkJs`, the
  `hull types` defaults). This found example apps that failed at load (a missing
  `hull:app` import, getters called as methods) and manifest keys the runtime ignores.
- **`make e2e-types`:** installs into an app, the conflict / `--force` / `--check` /
  removal rules, and a type-check of an app with the installed files (a wrong call fails).

The CI job runs on every change, as the invariant lint does: a change to a C binding or a
stdlib export changes the inventory, and the drift check fails until `types/hull` follows.

### 2.4 Lua

The same declarations for LuaLS (`---@meta` files, `hull types --lua`) are a natural
follow-up on the same source: out of scope here (declarations are grouped per module, so
the layout does not preclude it).

## 3. The TypeScript frontend (T2)

### 3.1 Placement

A new frontend in the ProjectDiscovery registry (`hull.project.registry`):
`typescript`, extensions `.ts` and `.mts`, engine `js` (it runs in the bundled QuickJS
tooling session, never the app runtime - JS frontend design §5). It is the JavaScript
parser (`stdlib/cli/js/hull/source/`) with a TypeScript mode, not a second parser:
TypeScript syntax is JavaScript plus type positions, and one parser keeps one byte-range
contract (JS frontend design §4), one set of limits (§5) and one conformance corpus.

`.tsx` and `.cts` stay known-but-unanalyzable (JS frontend design D11 behaviour:
honest per-file diagnostic, generation `complete=false`).

### 3.2 What it produces

Through the existing frontend contract, nothing new for consumers: declarations,
annotations (JSDoc `/** @tag */`, so annotation lowering works on `.ts` exactly as on
`.js`), scope. Plus one frontend semantic operation:

```text
type_erasure(unit) -> { spans: [ {start, stop} ... ], refusals: [ diagnostic ... ] }
```

`spans` are half-open byte ranges of erasable type syntax, sorted, non-overlapping;
erasing them (§4.1) yields JavaScript with the same byte offsets. `refusals` are the
non-erasable constructs (§3.3), each a `ts.unsupported` error diagnostic with its range.
Other frontends report the operation unsupported. It is a semantic operation behind the
analyzer's generation boundary (project discovery D3 / D3.1), so a plugin consuming it never
sees the TypeScript AST: `analyze.type_erasure(disc, path)` on a discovery made with
`retain_frontend = true`, gated by the same lease as `declaration_semantics` and `scope`. It is
keyed by the source's project-relative path rather than a declaration handle: erasure is per
unit, and a unit may declare nothing. `project.operation_unsupported` for a source of another
frontend, `project.handle_invalid` for a path with no analyzed unit.

### 3.3 Accepted syntax: erasable only

Hull accepts the subset whose removal leaves valid JavaScript with the same meaning
(TypeScript's `erasableSyntaxOnly`, Node's type stripping):

- erased: type annotations, return types, type parameters and arguments, `interface`,
  `type` aliases, `declare` declarations (including `declare module "m"` / `declare
  global` / `declare namespace` / `declare enum`), `import type` / `export type` (and
  `import type X = require("m")`) and inline `type` specifiers, `as` / `satisfies` /
  non-null `!` assertions, `abstract` / `override` / `readonly` /
  `public` / `private` / `protected` modifiers that are type-only, optional `?` on
  parameters and properties, definite assignment `!`, `this` parameters, overload
  signatures, `implements` clauses, class field type annotations;
- refused (diagnostic, the file does not run): `enum` and `const enum`, a non-`declare`
  `namespace` / `module` with a body, `declare module N {}` (the old spelling of a
  namespace, which TypeScript deprecates and Node refuses), parameter properties
  (`constructor(private x: T)`), `import x = require()` / `export =`, legacy
  `experimentalDecorators` metadata, angle-bracket assertions `<T>x`, JSX.

A refusal is never "transformed anyway": the developer rewrites it (an object literal
`as const` for an enum), exactly as Node requires. A refused declaration (`enum`, `const
enum`, `namespace`, `module`, or an erasable one outside a statement list) is still read
whole, so its refusal is the only diagnostic it produces and the code after it is parsed as
usual.

### 3.4 Erasure that would change the parse

Replacing a span with whitespace can, in rare shapes, change JavaScript's parse: an
arrow function whose return type spans a line break (`(a):\n  T => a` leaves a line
terminator before `=>`), or erasure that creates or removes an automatic semicolon. The
frontend checks each span's neighbourhood and reports these as `ts.unsupported` with a
fix hint (keep the return type on the arrow's line). The conformance corpus (§5) carries
one case per shape.

Implemented, six rules (the first two in slice 1):

- **Arrow types and `=>`.** A line break between the end of an arrow's typed parameter
  list or return type and its `=>` is refused ("keep '=>' on the line the types end").
  It is checked over the bytes between `)` and `=>`, so a line break inside the return
  type counts.
- **Erased declarations and ASI.** Whole declarations (`interface`, `type`, `declare`,
  `import type` / `export type`) erase only as items of a statement list (program,
  block, function body, `case`); elsewhere they are refused. One is refused when the
  statement before it ended by automatic semicolon insertion and the token after it
  could continue that statement (`(`, `[`, a template, a regular expression, `/`, `/=`,
  `+`, `-`): erased, `x` / `type T = A` / `(y)` would read as `x(y)`. The fix hint is to
  end the earlier statement with `;`. Node's type stripper writes a `;` in such places
  instead; Hull does not rewrite, it refuses, and leaves the safe cases blank. Slice 2
  adds `<` to those tokens (a generic arrow's erased `<T>` leaves its `(`), extends the
  rule to overload signatures, which erase with their statement, and refuses a `;` there
  too: `x` / `interface I {}` / `;` is two statements in TypeScript and one in the erased
  JavaScript.
- **`as` and `satisfies` before a line.** TypeScript ends an `as` / `satisfies` type at a
  line break, and the statement then ends by automatic semicolon insertion; erased, the
  expression before it would take the next line. Refused when that line starts with a token
  that would continue the expression (`(`, `[`, `.`, `?.` or a template): `x as T` /
  `[1].f()` reads `x[1].f()` (`refuse/asi_as_bracket.ts`,
  `asi_as_template.ts`, `asi_satisfies_call.ts`).
- **Async arrow type parameters.** `async <T>` followed by a line break and `(x) => x` is an
  async arrow in TypeScript; erased, `async` / `(x) => x` is a call of `async` and then a
  plain arrow, so a line break after the type parameters of an async arrow is refused
  (`refuse/async_arrow_type_parameters_newline.ts`). Node's output there is not valid
  JavaScript.
- **Class members.** Erased modifiers, whole members (overload signatures, `abstract` and
  `declare` members, index signatures) and a field's own type can sit between a class
  field ended by automatic semicolon insertion and the next member. Refused when what
  follows would then join the field: a `;` (an erased member takes one `;`, so a second
  one, `m(): void;;`, would end the field in JavaScript); after an initializer, a member
  starting `[`, `*`, `in` or `instanceof` (`x = a` / `private [k] = 1` reads `a[k] = 1`);
  after a field named `static`, `get` or `set` with no initializer, any member (`static:
  number` / `x = 1` reads `static x = 1`). Node writes a `;` after an erased modifier, but
  its output for an erased member there joins the lines (`refuse/asi_class_member.ts`).
- **Instantiation expressions add none.** A `<...>` erased after an expression (§3.6)
  leaves exactly what TypeScript goes on to parse after it, a line break included, so no
  shape arises there.

### 3.5 Delivery

T2 lands in three slices.

**Slice 1 (implemented): the parser's TypeScript mode.** Internal; no command reaches it
yet, and no extension selects it.

- `parse(bytes, { lang: "ts" })` in `hull:source:parser`. `lang` defaults to `"js"`; any
  other value (`"tsx"`, `"TS"`, `true`, ...) is refused with one `js.internal` diagnostic
  rather than read as JavaScript. Every TypeScript branch is behind the `ts` flag, and two
  gates hold JS-mode output fixed (§5).
- The type grammar is `hull:source:ts_types`, driven by the parser's tokenizer, limits and
  diagnostic budget; `>>` and its kin are split for nested generics (the lexer's
  `splitGreater()`).
- A TS-mode unit carries `typeSpans`: sorted, non-overlapping, half-open byte ranges.
  Type tokens never move a node's range, so the TS-mode AST equals the JS-mode AST of the
  erased bytes. `hull:source:ts_erase` gives `typeErasure(unit, bytes, path)` -> `{ ok,
  spans, refusals }` (§3.2) and `erase(bytes, spans)`: span bytes become spaces, CR and LF
  are kept, a span holding U+2028 / U+2029 is refused. JSDoc attaches over the erased
  bytes, as for a `.js` file.
- Erased: annotations on variables, parameters (arrow and async-arrow included), returns
  and class fields; optional `?` and definite `!`; `as`, `as const`, `satisfies` and
  non-null `!`; `interface`, `type`, `declare` const / let / var / function, `import
  type` / `export type` and their `export` forms, and inline `type` specifiers.
- Refused, `ts.unsupported` with the construct named: everything else. For good (§3.3):
  `enum` / `const enum`, `namespace` / `module` with a body, parameter properties,
  `import x = require()` / `export =`, angle-bracket assertions. For now (slice 2): type
  parameters and arguments, overload signatures, `this` parameters, `implements`, class
  and member modifiers (`abstract`, `readonly`, `private`, ...), `declare class` /
  `declare module` / `declare global`. Also refused: an initializer in a `declare`
  declaration (TypeScript rejects it) and the §3.4 shapes.

**Slice 2 (implemented):** the rest of the erasable syntax (§3.3).

- Generics: type parameters on functions, classes, methods and arrows (`<T>(x: T) => x`,
  `async <T>(x) => x`), and type arguments after a callee, a `new` callee, a `?.`, a tag
  and a heritage class (`f<T>(x)`, `new Map<K, V>()`, `` tag<T>`s` ``, `extends Base<T>`)
  and as instantiation expressions (`f<T>;`). §3.6 says how a `<` after an expression is
  told from a comparison.
- Classes: `abstract class` (with `export` / `export default`), `implements`, member
  modifiers (`public`, `private`, `protected`, `readonly`, `override`), and members erased
  whole: `abstract` and `declare` members, index signatures, method and constructor
  overload signatures. A member's node starts at its first JavaScript token. A field named
  like a modifier stays a field (`private;`, `readonly` on its own line, `declare: T`).
- Functions: overload signatures erase with their statement (`function f(a: T): R;`,
  `export` / `export default` / `async` forms) in a statement list; elsewhere they are
  refused. A `this` parameter erases with its comma when it is first; later, TypeScript
  rejects it and Hull refuses it.
- `declare class` / `abstract class` / `enum` / `const enum` / `namespace` / `module "m"`
  (with or without a body) / `global`, also after `export`: the header is read and the
  body, ambient declarations only, is skipped as one balanced `{ ... }` group. `declare
  interface` / `declare type`, and `import type X = require("m")` (or `= A.B`).
- The §3.4 rules for generic arrows, overloads, a following `;` and class members.

**Slice 3 (implemented):** TypeScript reaches project discovery.

- Registry: `ts` and `mts` rows, language `typescript`, frontend
  `hull.project.frontend_typescript` (the JavaScript proxy with the mode set, plus
  `type_erasure`), engine `javascript`, so they are analyzable exactly when JavaScript is. `tsx`
  and `cts` are rows that are never analyzable: each such source is reported unsupported with
  its reason and makes the generation incomplete (D11). The frontend summary lists them apart,
  as `unsupported_extensions`, so they decide neither the language's `analyzable` nor its
  capabilities; TypeScript adds a sixth capability, `type_erasure`.
- The mode: `tool.frontend_analyze(lang, token, path, src, mode)`, where `mode` is `"js"`
  (default) or `"ts"` and anything else is an argument error; `hl_js_gen_analyze` builds the
  options JSON from those two literals only and refuses any other value before QuickJS is
  entered. The session keeps a TypeScript unit's SourceUnit and bytes for the erasure.
- `type_erasure`: `typeErasure(unitId)` in the JS adapter (`hull:source:ts_erase` over the
  retained unit, refusal ranges normalized with line and column),
  `tool.frontend_type_erasure`, and `analyze.type_erasure` (§3.2).
- `hull dev` reloads on a `.ts` / `.mts` change, so `hull dev --agent` publishes a new
  generation for it.
- Tests: `test_js_generation` (the real session: spans, refusals, a JavaScript unit, a stale
  token, every rejected mode), `test_project` (the analyzer over a mocked bridge: the mode per
  extension, the lease, the unit keying, the summary) and `tests/e2e_project_discovery.sh`
  (`hull agent inspect` on `.ts` / `.mts` / `.tsx` / `.cts` on both the full and the lua-only
  build, and `hull dev --agent` republishing on a `.ts` edit, on an app that serves so that only
  the watcher can cause the reload).

Not in slice 3 (T3, §4): running a `.ts` file, entry discovery of `app.ts`, and the analyzed
bytes on the handle.

### 3.6 `<` after an expression

After an expression a `<` is either type arguments or a comparison, and only what follows
tells them apart. The parser reads it as type arguments under speculation: the lexer's
`specMark()` / `specRewind()` save and restore its whole state (the template context
stack copied), and the parser saves its own. The reading is kept only when the list parses
with no diagnostic and the token after its `>` can follow one by TypeScript's rule
(`canFollowTypeArgumentsInExpression`): `(` or a template make a call or a tagged
template; `<`, `>`, `+` and `-` make a comparison; otherwise a line break, a binary
operator or a token that cannot start an expression make an instantiation expression, and
anything that can start one a comparison. `.`, `?.` and an assignment after the list,
which TypeScript rejects, are read as a comparison so that JS mode reports them. Otherwise
everything is rewound, and every token is lexed again under its real slash goal (`a <
/re/` stays a regular expression).

So `i < n`, `a < b > c`, `a < b >= c` and `a < b && c > d` stay comparisons, and `a < b >
(c)` is a call, as it is to tsc and Node: the one place where valid JavaScript parses
differently in TS mode. A `<` at an operand position is a generic arrow when a type
parameter list followed by `(` reads and an arrow follows, else an angle-bracket assertion
(refused).

Speculation is bounded per file: the rewound tokens, plus the lexer context copied per
attempt, may total at most four times the tokens kept plus 65536. Past that the construct
is refused ("type lists past this file's lookahead budget"), so input built to make every
`<` a long failed attempt stays linear (`speculation_budget` in `test_js_typescript`).

## 4. Running TypeScript (T3)

### 4.1 The erasure plugin

A bundled source plugin, `stdlib:ts-erase@1`: a small JavaScript Hull application
embedded in the `hull` binary, like every plugin (BuildContext §9.1). It parses nothing.
The TypeScript parse runs once, in the host's JavaScript frontend session - the one
ProjectDiscovery analyzed - and the host does the erasure too; the plugin turns its
answer into the action's result.

- **The host operation, `erased_source(unit)`.** Offered by the `typescript` frontend
  only (another frontend answers `project.operation_unsupported`). It returns
  `{ bytes, refusals }`: the frontend's `typeErasure` over the unit it retained gives
  the spans and refusals, and the frontend's `erase()` blanks the spans in the bytes it
  analyzed - each span byte replaced by a space, CR and LF kept, a span holding U+2028 /
  U+2029 refused - so offsets, lines and columns are unchanged (position map
  `identity`, generations design §5). With any refusal, `bytes` is absent. It extends
  slice 3's `type_erasure` (spans + refusals, §3.5), which stays for analysis.
- **Input:** a source selection of `typescript` units (BuildContext §1.3: chosen by the
  planner; the plugin rescans nothing).
- **Work:** per unit, `erased_source(unit)` over the plugin's channel; any refusal is
  emitted as an error diagnostic (BuildContext §8) and the action aborts; otherwise the
  bytes are the unit's output.
- **Output:** one generated output per unit through `ctx.outputs`
  (`declare_generated_output`), logical name = the unit's source name (`./app.ts`,
  `./lib/a.ts`), payload = the erased JavaScript.
- **Authority:** no workspace grants, no `ctx.inputs` reads, no tools, no configuration
  that changes output. Deterministic and cacheable (BuildContext §10); its key covers
  the units' analyzed bytes, which the source selection records.

```js
app.transform(async (ctx) => {
  for (const unit of ctx.source.units()) {
    const r = await ctx.source.erasedSource(unit);
    if (r.refusals.length) { r.refusals.forEach((d) => ctx.diagnostics.emit(d)); return; }
    ctx.outputs.declareGeneratedOutput({ name: unit.name }, r.bytes);
  }
});
```

The bytes erased are the bytes the frontend analyzed: the host erases them in the same
session, so there is no second read of the workspace and no time-of-check gap between
the spans and the bytes, and the erasure rules exist once, in the frontend's `erase()`.

### 4.2 Activation

The planner adds the erasure route when ProjectDiscovery reports `typescript` units. It is
one explicit, Hull-defined route (`typescript source -> erased JavaScript generated
output`), fixed in the pipeline plan like any route of BuildContext checkpoint 6 - not an
"automatic execution of every compatible plugin" (BuildContext §14) and not a domain
special case in Hull core (§1.2): the planner maps a frontend's units to the route
registered for it. An app plugin cannot replace or shadow it in v1. To be ratified with
the checkpoint-6 route model.

### 4.3 Module names and imports

- The entry may be `app.ts` (or `app.mts`); entry discovery gains the extension where it
  looks for `app.js` today (dev and the build). `app.ts` with `app.js` or `app.lua`
  beside it is refused as ambiguous.
- Imports name the file as written: `import { f } from "./lib/a.ts"`. No extension
  guessing or rewriting in v1 (`allowImportingTsExtensions`, written by `hull types`, lets
  `tsc` accept it). `.ts` and `.js` modules may import each other.
- The loaded module keeps its source name (`./lib/a.ts`), so stack traces and
  `hull:` checks see the developer's file; the bytes come from the overlay (generations
  design §2.1). The JS bytecode cache keys on module name + source as today; the erased
  payload is the source it sees.
- `hull/` and `hull:` imports are unchanged: the stdlib stays JavaScript.

### 4.4 Entry points

All through the pipeline, none through a loader transform:

- `hull dev` / `hull test` / `hull agent` / `hull mcp` / dev `hull app.ts`: the pipeline
  generation (generations design §4) supplies the erased modules; a refusal is a
  diagnostic (dev keeps the last good generation; a one-shot entry point fails to load).
- `hull build`: the pipeline runs before manifest extraction (`app.manifest()` lives in
  the entry), and the manifest-extraction child loads the entry through the same handed
  generation; the erased modules are embedded under their source names and signed as
  generated entries with their provenance (generations design §6).
- A built binary runs the embedded JavaScript; it contains no TypeScript machinery.

## 5. Testing

- **Frontend (implemented for slices 1 and 2):** the JS frontend's conformance approach (JS
  frontend design §8) with a TypeScript corpus, `tests/fixtures/ts_frontend/` (byte-exact,
  `-text` in `.gitattributes`). `test_js_typescript` (every `make test`, and `make
  test-js-frontend`) checks:
  - every valid JavaScript file of the JS conformance corpus parses in TS mode to the same
    AST with no spans and no refusals, and JS uses of TypeScript's contextual words
    (`type`, `as`, `declare`, ...) stay JavaScript;
  - each refusing hook fires once at its token, and `lang` takes only `"js"` / `"ts"`;
  - `accept/NAME.ts`: parses cleanly; **M1** the erasure only turns bytes into spaces and
    never touches CR / LF; **M2** the erased bytes parse as JavaScript to the TS-mode AST;
    **M3** QuickJS compiles them (compile only, imports stubbed); **M4** they equal
    `accept/NAME.js` byte for byte;
  - `refuse/NAME.ts`: its diagnostics equal `refuse/NAME.expect`, one line per refusal
    (`<code> <start> <stop> <message>`).
  To add a case, write the `.ts` and run the suite with `HULL_TS_CORPUS_UPDATE=1`, which
  writes the `.js` / `.expect`; review them before committing.
- **M5, Node cross-check** (`make check-ts-node`, `scripts/ts_node_crosscheck.mjs`; CI job
  `types` on Node 22.22.0): Node's `module.stripTypeScriptTypes()` must accept every
  accepted file and produce its `.js` byte for byte, except that Node blanks a non-ASCII
  character with one whitespace character of the same UTF-8 length (Hull: one space per
  byte) and writes a `;` where it erases a statement (§3.4). Every file Node refuses, Hull
  refuses; the refused files Node strips are listed in the script with the reason, and the
  list must be exact. On a Node without the function it skips, unless
  `HULL_REQUIRE_NODE_STRIP=1`.
- **M6, fuzzing:** `fuzz-js-source` runs TS mode when the first input byte's bit 2 is set
  (docs/js_source_fuzz_design.md §5): never a crash, spans sorted and in range, and a clean
  unit erases to bytes that parse cleanly to the same AST.
- **Plugin:** the BuildContext acceptance cases (abort on refusal, nothing committed;
  cache hit on unchanged bytes; reuse invalidated by any byte change).
- **E2E** (`tests/e2e_typescript.sh`, both a modular and a single-file app): `hull dev`
  serves, an edit with a refused construct keeps the old generation and reports it, a fix
  reloads; `hull test` runs `.ts` test files; `hull build` produces a binary that runs
  with no `.ts` on disk and `hull verify` passes; a runtime error's stack trace names
  `lib/a.ts` with the right line.
- **Types (T1):** §2.3.
- **JS mode does not change (implemented, ahead of T2).** The TypeScript mode lives in the
  JS lexer and parser, so two gates hold JS-mode output fixed while it is built:
  - `test_js_golden` (every `make test`, and `make test-js-frontend`) digests the lex,
    parse + annotation and scope output of a frozen input set - the pinned Test262 subset
    and the hand-written cases in `tests/fixtures/js_frontend_golden/cases/` - and compares
    with the committed `golden.txt`. It also checks that TypeScript syntax stays a
    `js.syntax` error in JS mode, under a `.js` or a `.ts` path, with no `ts.*` code. A
    meant change regenerates the golden (`make js-frontend-golden`) and says why in its PR.
  - `scripts/js_frontend_diff.sh` (`make js-frontend-diff`, CI job `focused-js-frontend`
    on a PR) builds the dump tool against the frontend at the PR's base and at its head,
    dumps the repo JS (the conformance corpus, the frozen inputs and the frontend's own
    modules) with both, and fails on any byte of difference, except in an input whose
    `golden.txt` line the change itself regenerated: a meant change is visible in review.
    It prints both timings.

## 6. Open questions

1. Test files: `tests/test_*.ts` - discovered by `hull test` beside `.js` (recommended:
   yes, same rules, with T3).
2. `hull new --runtime ts`: a scaffold variant once T3 ships (recommended), with
   `hull types` run by default.
3. Whether `hull check` should run an installed `tsc` as a registered tool (BuildContext
   §7) for type checking. Recommended: no in v1 - type checking stays the developer's
   `tsc --noEmit`, and Hull never makes types a runtime or build gate.
