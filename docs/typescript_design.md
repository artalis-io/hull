# TypeScript - design

Status: **T1 (types + `hull types`) implemented; T2 and T3 design-only.** Builds on
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
| T2 | TypeScript frontend, analysis only (§3) | the shipped JS frontend |
| BuildContext 1-5 | action core, outputs, artifact seam, tool seam, plugin runtime | - |
| G1, G2 | pipeline generations: overlay, runner wiring | BuildContext 2, 5 |
| T3 | type-erasure plugin + `.ts` entry points (§4) | T2, BuildContext 5, G1, G2, and the explicit-route part of BuildContext 6 |

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
Other frontends report the operation unsupported. It is a semantic operation reached
through the opaque handle (project discovery D3 / D3.1), so a plugin consuming it never
sees the TypeScript AST.

### 3.3 Accepted syntax: erasable only

Hull accepts the subset whose removal leaves valid JavaScript with the same meaning
(TypeScript's `erasableSyntaxOnly`, Node's type stripping):

- erased: type annotations, return types, type parameters and arguments, `interface`,
  `type` aliases, `declare` declarations (including `declare module` / `declare global`),
  `import type` / `export type` and inline `type` specifiers, `as` / `satisfies` /
  non-null `!` assertions, `abstract` / `override` / `readonly` /
  `public` / `private` / `protected` modifiers that are type-only, optional `?` on
  parameters and properties, definite assignment `!`, `this` parameters, overload
  signatures, `implements` clauses, class field type annotations;
- refused (diagnostic, the file does not run): `enum` and `const enum`, a non-`declare`
  `namespace` / `module` with a body, parameter properties
  (`constructor(private x: T)`), `import x = require()` / `export =`, legacy
  `experimentalDecorators` metadata, angle-bracket assertions `<T>x`, JSX.

A refusal is never "transformed anyway": the developer rewrites it (an object literal
`as const` for an enum), exactly as Node requires.

### 3.4 Erasure that would change the parse

Replacing a span with whitespace can, in rare shapes, change JavaScript's parse: an
arrow function whose return type spans a line break (`(a):\n  T => a` leaves a line
terminator before `=>`), or erasure that creates or removes an automatic semicolon. The
frontend checks each span's neighbourhood and reports these as `ts.unsupported` with a
fix hint (keep the return type on the arrow's line). The conformance corpus (§5) carries
one case per shape.

## 4. Running TypeScript (T3)

### 4.1 The erasure plugin

A bundled source plugin, `stdlib:ts-erase@1`, Lua-authored (BuildContext v1 has no
JavaScript plugin runtime, and needs none here: it parses nothing).

- **Input:** a source selection of `typescript` units (BuildContext §1.3: chosen by the
  planner; the plugin rescans nothing).
- **Work:** per unit, `type_erasure(unit)` through the semantic handle; any refusal is
  emitted as an error diagnostic (BuildContext §8) and the action aborts; otherwise the
  unit's analyzed source bytes with every span replaced byte for byte by spaces, newline
  bytes kept (so offsets, lines and columns are unchanged - position map `identity`,
  generations design §5).
- **Output:** one generated output per unit through `ctx.outputs`
  (`declare_generated_output`), logical name = the unit's source name (`./app.ts`,
  `./lib/a.ts`), payload = the erased JavaScript.
- **Authority:** no workspace grants beyond the source selection, no tools, no
  configuration that changes output. Deterministic and cacheable (BuildContext §10).

The source bytes it erases are the bytes the frontend analyzed - reached through the
source selection, recorded as the action's observation - never a second read of the
workspace (no time-of-check gap between the spans and the bytes). Exposing the analyzed
bytes through the semantic handle is an addition to the frontend-neutral operations
BuildContext §9 lists; it is to be ratified with checkpoint 5.

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

- **Frontend:** the JS frontend's conformance approach (JS frontend design §8) with a
  TypeScript corpus: every accepted construct erases to JavaScript QuickJS compiles
  (compile-only, never run), with offsets preserved; every refused construct and every
  §3.4 shape yields its diagnostic and range; a fuzz target over the TS mode (bytes in,
  spans + diagnostics out, never a crash, spans always sorted and in range).
- **Plugin:** the BuildContext acceptance cases (abort on refusal, nothing committed;
  cache hit on unchanged bytes; reuse invalidated by any byte change).
- **E2E** (`tests/e2e_typescript.sh`, both a modular and a single-file app): `hull dev`
  serves, an edit with a refused construct keeps the old generation and reports it, a fix
  reloads; `hull test` runs `.ts` test files; `hull build` produces a binary that runs
  with no `.ts` on disk and `hull verify` passes; a runtime error's stack trace names
  `lib/a.ts` with the right line.
- **Types (T1):** §2.3.

## 6. Open questions

1. Test files: `tests/test_*.ts` - discovered by `hull test` beside `.js` (recommended:
   yes, same rules, with T3).
2. `hull new --runtime ts`: a scaffold variant once T3 ships (recommended), with
   `hull types` run by default.
3. Whether `hull check` should run an installed `tsc` as a registered tool (BuildContext
   §7) for type checking. Recommended: no in v1 - type checking stays the developer's
   `tsc --noEmit`, and Hull never makes types a runtime or build gate.
