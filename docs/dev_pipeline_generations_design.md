# Pipeline generations outside `hull build` - design

Status: **G1 and G2 IMPLEMENTED** (the handed generation, the overlay, the loaders, INV-14,
§9.1; the pipeline run before an app from source loads and `hull dev`'s hand-off, §9.2). G3
is design only. This document extends
[buildcontext_design.md](buildcontext_design.md) (BuildContext). It defines how the
outputs of the ONE build pipeline reach an app that is not built: `hull dev`,
`hull test`, `hull agent`, `hull mcp`, and a development `hull app.lua` / `app.js`.
It changes nothing in BuildContext's action model, authority, cache or commit
protocol; where the two seem to differ, BuildContext wins and this document is wrong.

Consumers that need this: annotation lowering (the compute islands in
[roadmaps.md](roadmaps.md) "Typed Lua Compute Islands", later Query IR) and
TypeScript type erasure ([typescript_design.md](typescript_design.md)).

## 0. The problem

Today every development entry point runs the app from its source files: the loaders
read `app.lua`, modules, templates, migrations, `compute/*.wasm` and `shaders/*.wgsl`
from disk (`hl_vfs_embedded` false), and a built binary reads only its embedded VFS.
That works because every file the runtime consumes in development is already in a
runtime format (WGSL, WASM, Lua, JS); the only build-only transform, AOT, is an
optimisation the interpreter does without.

A pipeline output is not optional in that way:

- a decorated `@wasm` Lua function is replaced by a generated stub, and the roadmap
  rules out a runtime fallback ("unsupported compile feature: build/validate error,
  not runtime fallback");
- a `.ts` module cannot be loaded at all until its types are erased.

So every entry point that runs an app from source has to run the pipeline first and
load its outputs. Two existing texts point at this and leave it open:

- BuildContext §2.3: "For `hull dev`, a failed pipeline generation does not replace
  the last valid published generation", and §11.12. It does not say how a dev
  generation reaches the app, or what `hull test` / `hull agent` do.
- The compute-islands roadmap: `hull dev` compiles into `.hull/compute-cache/` and
  exposes it "through the existing filesystem/VFS fallback path". That predates two
  decisions: BuildContext (one host pipeline, plugins, committed action results),
  and audit 37 (`<app>/.hull` is reserved: the loaders refuse to step into it). It is
  superseded here.

## 1. Governing decisions

### 1.1 One pipeline; entry points differ only in materialization

Every entry point that runs an app from source runs the SAME host pipeline as
`hull build`: ProjectDiscovery, pipeline planning, dependency ordering, action
transactions, artifact validation and cache (BuildContext §1.1). They differ only in
the last step, materialization:

| Entry point | Pipeline runs | Materializes to |
|---|---|---|
| `hull build` | once | the embedded VFS + `package.sig` (BuildContext's materialize/link/sign) |
| `hull dev` | per (re)spawn, in the supervisor | a published **generation** handed to the child |
| `hull test`, `hull agent`, `hull mcp`, dev `hull app.*` | once, before the app context loads | a generation handed to the app process |
| built binary | **never** | - (it has its embedded VFS) |

There is no dev-only lowering path, no loader-side transform, no second cache and no
per-language variant. A plugin cannot tell which entry point planned it: the target
profile it receives is the same for `hull dev` and `hull build` of the same host
(BuildContext §10 target profile), so the dev outputs are the bytes `hull build`
would embed. (A future target-specific stage, such as AOT, runs only where the plan
asks for it; dev plans omit AOT exactly as dev runs the interpreter today.)

### 1.2 A generation is a manifest over committed action results

A **pipeline generation** is the materialization record of one pipeline run:

```text
PipelineGeneration
  generation_id
  pipeline_plan_hash
  entries[]:
    vfs_name        the logical name the loaders ask for ("./app.ts",
                    "compute/__generated/app/distance_4f8c2d1a.wasm", ...)
    content_hash    SHA-256 of the payload
    payload         reference to a committed action result payload
    origin          source path + producing action identity (provenance)
    position_map    none | identity | table (see §5)
  diagnostics_summary
  inputs[]          the union of the actions' dependency manifests (§4.2)
```

It is built by the host from COMMITTED action results only (BuildContext §2.1); a
plugin never writes one. Publishing it is one atomic manifest installation
(BuildContext §2.2: never several unrelated renames). It contains no Lua tables,
QuickJS values or frontend handles (BuildContext §5).

### 1.3 The plugin runtime never runs in the app process

The pipeline runs in a tool process: the `hull dev` supervisor (which already runs
`hull.project.publish` per spawn), or, for a one-shot entry point, a child process of
the same binary spawned before the app context loads - the pattern the JS
manifest-extraction child already uses. So:

- the app process never loads BuildContext bindings, frontends or tools, and never
  runs a plugin: each plugin is a Hull application in a process of its own that the
  tool process spawns (BuildContext §9.1), so §11.1 holds structurally, not only by API
  absence;
- the tool sandbox and the app sandbox are never applied in one process (both are
  irreversible per process);
- a crash or limit trip in a plugin cannot take the app process with it.

### 1.4 The generation is handed to the app, never found on disk

A generation is something Hull GIVES the app process, not something the app process
LOOKS FOR. The app tree may be a cloned repository, and anything found there is repo
content (audits 36 / 37). The app process receives the generation manifest from its
parent over an inherited descriptor (the way `HULL_DEV_SESSION` hands the session
key: taken and closed before any app code runs), and verifies every payload it reads
against the manifest's `content_hash`. It needs no key: the manifest's authority is
"my parent gave it to me", and the payloads are content-addressed.

Consequences:

- a `.hull/compute-cache/`, a planted `generation.json`, or any file in the tree is
  never read as a generation;
- the payloads may live in a shared per-user store (§3) without a sealing key in the
  app process: a forged payload fails its hash, and an app cannot change which hash
  its parent handed it;
- no new `.hull/` reader, so the audit 37 reservation is untouched.

As built (G1), the descriptor carries the payloads too, after the manifest: the parent, which
holds the tool key, reads the committed results from the store and writes their bytes; the app
process opens no store path at all (§3, §9.1).

### 1.5 Production loads no generation

A built binary (`hl_vfs_embedded`) has no generation and refuses one: the descriptor
is not consulted, and the overlay (§2) is never built. `--verify-sig` refuses a
generation too (§6). This keeps the compute-islands invariant: runtime app execution
never compiles code, and a built app loads only sealed, signed VFS entries.

## 2. The generation overlay

### 2.1 Shape

The app process builds one read-only **overlay** from the handed manifest: a sorted
`HlVfs`-shaped table (`vfs_name` -> payload reference + hash), O(log n) like
`hl_vfs_find`. Every loader that reads app content from disk in development consults
the overlay FIRST and the disk second:

- Lua `require` / JS `import` of app modules (and the entry itself);
- compute module loading (`compute/<name>.wasm`);
- templates, static files, migrations, shaders: no stage produces these today, so
  the overlay has no entries for them, but the lookup order is the same for when one
  does.

An overlay entry SHADOWS the disk file of the same name. For a transform of a source
file (`./app.ts` -> its erased JS) the overlay entry keeps the SOURCE name, so chunk
names, module names and stack traces name the file the developer edits. Every
existing rule on those names still applies: `hl_lua_app_chunkname` /
`hl_js_app_module_name` never produce a `hull.` / `hull:` name, and a relative
specifier never normalizes to one.

### 2.2 Reading a payload

Every payload is hashed against its manifest entry when the generation is taken, before
any app code runs, and a mismatch stops the app (it never starts on part of a
generation). The checked bytes are then kept in sealed, read-only memory, so a loader
that hits the overlay loads them as they were checked. It never falls back to the disk
file it shadows: a corrupt payload is an error, not a reason to run the untransformed
source.

### 2.3 What app code sees

Nothing new. App code has no API that names, lists or reads the overlay; `fs.*` still
reads the real tree through the fs policy (an app that `fs.read`s `app.ts` gets its
source, as now). The overlay only changes what the LOADERS load.

### 2.4 Invariant and lint

"Every development disk loader consults the generation overlay before the disk, and
never reads the shadowed file on a miss" is a new contributor invariant. Per
CLAUDE.md, the PR that adds the overlay adds the lint rule for it to
`scripts/check_invariants.py` (beside INV-5, "disk loaders check
`hl_vfs_embedded`"), with good / bad fixtures.

## 3. Where payloads live

Payloads are committed action results, so they live where BuildContext's action
results live: Hull's per-user artifact store, outside the app tree (its exact layout
is BuildContext checkpoint 2 / 6 business, not this document's). The generation
manifest references them by store identity + content hash.

- **Never under the app tree.** Not `.hull/`, not `build/`. (A cloned tree cannot
  ship a generation; the app's fs grants cannot reach one.)
- **Sealing.** Action results are build outputs, sealed like compute-AOT under the
  tool key (`$HOME/.hull/tool-cache.key`), which app processes never load. The app
  process does not need the key (§1.4): it checks payload hashes against the handed
  manifest.
- **Kernel sandbox.** The app process reads the whole generation, payloads included,
  from the handed descriptor BEFORE its phase-2 sandbox (the generation is small and
  known at startup), so the sandbox grants no store path. This mirrors the CA bundle,
  read into memory before the sandbox applies. A dev reload is a new process with a
  new generation.
- **Cache reuse.** A reload re-plans; actions whose two-level keys validate
  (BuildContext §10, §4.4) reuse their committed results, so editing one module
  re-runs only the stages that observed it.

## 4. Entry points

### 4.1 `hull dev`

On startup and each reload, BEFORE it spawns the child, the supervisor:

1. runs ProjectDiscovery (it already does, for `.hull/discovery.json`);
2. plans and runs the pipeline for the dev target profile;
3. on success, publishes the new generation (atomically) and hands it to the child it
   spawns;
4. on failure, does NOT replace the last valid generation (BuildContext §2.3, §11.12):
   it keeps the running child (or, if there is none yet, starts none), and reports
   the diagnostics as a load failure does today (`.hull/last_error.json`, MAC'd with
   the session key; `hull agent errors`; the `--tui` log).

This differs on purpose from ProjectDiscovery D7, which publishes EVERY discovery
generation including `valid=false` and never preserves a stale one: a discovery
generation is a report (an agent must see the current, broken state), while a
pipeline generation is executable output (the app must never run half a pipeline).
`hull agent inspect` keeps serving the current discovery; the app keeps running the
last good outputs; `hull agent errors` says why they are stale.

### 4.2 Watching

The dev watcher watches the union of the last generation's input observations
(`inputs[]`, from every action's dependency manifest, BuildContext §4.2) in addition
to the files it watches today, so a change to anything a plugin observed - including
a directory listing - triggers a reload.

### 4.3 One-shot entry points

`hull test`, `hull agent`, `hull mcp` and a development `hull app.*` run the pipeline
once, in a child tool process, before the app context loads (where migrations run
today relative to `hull test`). There is no "last valid generation" for a one-shot
run: a failed pipeline is a load error (exit non-zero, diagnostics through the usual
channel), never a fallback to the shadowed sources. Cached action results make a
repeat run cheap.

`hull mcp` keeps a warm context; it re-plans when its context is rebuilt, never
mid-request.

### 4.4 An app with no pipeline work

When the plan has no actions (no annotations any active plugin consumes, no `.ts`
source), no child is spawned, no generation is handed, and the entry point runs
exactly as today. The pipeline costs nothing for an app that does not use it, the
same rule ProjectDiscovery D10 applies to `hull build`.

## 5. Positions and diagnostics

Runtime errors in generated code must point at the developer's source.

- `identity`: the payload's byte offsets equal the source's (TypeScript erasure,
  which replaces type syntax with whitespace). Nothing to map.
- `table`: a generated stub or rewritten region carries a position table (generated
  range -> source range) in the generation manifest; the runtimes' error paths
  translate a generated line through it before formatting. Its schema lands with the
  first stage that needs it (compute-island stubs), not before.
- `none`: a payload with no source position (a WASM module); errors name the
  generation entry and its `origin`.

Pipeline diagnostics (BuildContext §8) already carry source ranges from
ProjectDiscovery handles and flow unchanged through `hull build`, `hull dev` and agent
inspection; this document adds no second diagnostic channel.

## 6. Signatures

- `hull build` signs what it embeds, generated entries included, exactly as BuildContext
  materialization and the existing signature path define; the signed record carries
  each generated entry's provenance (source hash, plugin identity) from the
  generation manifest.
- `--verify-sig` in development (filesystem mode) refuses to start when the plan has
  pipeline work: the signature and the disk gate hash SOURCE, and a generation is not
  covered by them. Run the built binary to verify a signed app.
- `hull verify` against a source tree whose signature has generated entries does not
  compare a generated entry with the source file of the same name.
- As built: `package.sig` carries a signed `generated` map, file name to `{ key, plugin,
  source }` (the action key and plugin that made it, and the SHA-256 of the source file of
  that name when there is one). `files` holds each generated entry's hash of the bytes
  embedded, so a built binary's `--verify-sig` checks them as any embedded file, and
  `hull verify` checks them through `binary_hash` (they need the binary, as AOT entries
  do). A check of the source tree - `hull verify`, `hl_sig_verify_files_fs` - compares the
  source file with `source` instead, and skips an entry with none: an edited source fails
  even before the binary is rebuilt. Re-deriving the entries through the pipeline is not
  done; the source hash and the action key say what they were made from.

## 7. Security invariants (in addition to BuildContext §11)

1. The app process never runs plugin code, frontends or build tools (§1.3).
2. A generation reaches the app process only from its parent, over a descriptor taken
   before any app code runs; nothing on disk is read as a generation (§1.4).
3. Every overlay payload is verified against the handed hash before it is loaded; a
   mismatch or a missing payload is a load error, never a fallback to the shadowed
   source (§2.2).
4. App code has no API that reaches the overlay or the store (§2.3).
5. A built binary and `--verify-sig` load no generation (§1.5, §6).
6. A failed pipeline never replaces the last valid `hull dev` generation, and a
   one-shot entry point never runs without the generation its plan requires (§4).
7. Overlay names obey the existing app-name rules (no `hull.` / `hull:` names, no
   relative specifier normalizing to one) (§2.1).

## 8. Acceptance matrix

- `hull dev`: an edit that fails a stage keeps the running app on the last good
  generation and reports the diagnostic; the next good edit replaces it.
- `hull dev`: an edit to a file only a plugin observed (not loaded by the runtime)
  triggers a reload (§4.2); a directory entry added under an observed listing does
  too.
- `hull test` / `hull agent` / dev `hull app.*`: a failing stage is a load error with
  the diagnostic; the untransformed source is never run.
- A payload altered in the store after publication fails its hash at load.
- A generation-shaped file planted anywhere in the app tree (including `.hull/`) is
  never read.
- App code cannot read a payload or the manifest through any API (`fs.*`, `blob.*`,
  `db.open` file DSNs).
- An app with no pipeline work spawns no pipeline child and behaves byte-for-byte as
  today.
- The same source produces byte-identical payloads under `hull dev` and `hull build`
  for the same host target profile.
- A built binary started with a generation descriptor in its environment ignores it;
  `--verify-sig` in development with pipeline work refuses to start.
- Stack traces from generated code name the source file and line (identity and table
  maps).

## 9. Implementation checkpoints

These interleave with BuildContext's (BuildContext §13); each stops for review.

- **G1. Overlay + handed manifest** (implemented, §9.1; after BuildContext checkpoint 2, `ctx.outputs`:
  committed results exist). The overlay, the descriptor hand-off, hash-verified
  payload reads, the loader lookup order and its lint rule, refusal in built binaries
  and under `--verify-sig`. Fed by test fixtures (a hand-built generation over
  committed fixture results); no plugin runs.
- **G2. Runner wiring** (implemented, §9.2; after BuildContext checkpoint 5, plugin runner). The
  supervisor and the one-shot child plan and run the pipeline, publish, hand off; the
  failure rules of §4; the watcher union of §4.2. Proven with checkpoint 5's
  demonstration plugin.
- **G3. Positions** (with the first stage that needs a `table` map, the compute-island
  stubs). Identity maps need nothing beyond G1.

### 9.1 G1 as built

- **Hand-off.** `HULL_GENERATION_FD=<n>` (n >= 3) names a descriptor (a file, pipe or
  socket) the app process reads whole and closes, removing the variable, before any app
  code runs: the runners take it beside the build plugin channel (`serve.c`,
  `serve_cli.c`), before any child (the GPU compile helper) is spawned; the app context
  takes it for `hull test`, `hull agent` and `hull mcp` (`shared/generation.c`,
  `hl_generation_take_env` / `hl_generation_apply`).
- **Bundle.** `hull-generation 1 <manifest length>\n`, the manifest JSON
  (`format` `"hull-pipeline-generation"`, `version` 1, `entries[]` of `vfs_name`,
  `content_hash` (64 lowercase hex SHA-256), `size`, `origin`, `position_map` `none` |
  `identity`), then each entry's bytes in entry order and nothing after. At most 256 MiB
  in all, a 4 MiB manifest and 16384 entries. `origin`, `generation_id`,
  `pipeline_plan_hash`, `diagnostics_summary` and `inputs` are carried, not read: G2
  (watching) and G3 (positions) read them. `position_map: "table"` is refused until G3.
- **Names.** App-relative, `/`-separated, no leading `./`, no empty / `.` / `..`
  component, valid UTF-8 with no control byte, backslash or `:`, not under the reserved
  `.hull` in any case, not starting `hull.` / `hull:` (§7.7).
- **Overlay.** One sorted table in a sealed arena (`hl_vfs_overlay_install` /
  `hl_vfs_overlay_find`, `vfs.c`); each payload is followed by a NUL its length does not
  count. Installed once per process, never in a built binary (the descriptor is not
  consulted) and never under `--verify-sig` (refused: the app does not start).
- **Loaders.** The entry (Lua and JS), Lua `require` and JS `import` of app modules,
  templates, static files, migrations (a file the listing finds), shaders (`gpu.load`)
  and compute modules (`compute/<name>.wasm`, bytecode only) ask the overlay first.
  A name only the generation holds loads too (a module, a template, a static file, a
  compute module); migrations come from the `migrations/` listing, so a migration only a
  generation holds is a G2 question.
- **Lint.** INV-14 (`scripts/check_invariants.py`): every INV-5 disk loader, or a
  same-file caller, calls `hl_vfs_overlay_find`.
- **Tests.** `tests/hull/test_generation.c` (the bundle checks, names, the hand-off, the
  refusals) and `tests/e2e_generation.sh` (`make e2e-generation`: Lua and JS apps, `hull
  test`, the refusals, a planted generation-shaped file ignored; also on the
  pure-compute `app.main` runner in CI).

### 9.2 G2 as built

- **The pipeline child.** `hull plugins generation <app_dir>` (an internal verb of the `hull
  plugins` tool module) runs the app's pipeline (`hull.buildctx.pipeline`) and writes its
  generation to stdout - nothing else goes there; diagnostics go to stderr - and exits 1 when
  a plugin failed or its outputs cannot be one generation. `hull.buildctx.generation` builds
  it: every `generated` output of a committed or cached result becomes an entry named for the
  app file it shadows (its descriptor's `name`, `./` dropped; a tree output's files are
  `<name>/<path>`), with `origin` = the plugin and the action key and the descriptor's
  `position_map` (`none` by default); artifacts are not entries. Two outputs of one name, or a
  name an app cannot be handed (the app process's rule), fail the run. `inputs` is every path a
  result's dependency manifest observed; `pipeline_plan_hash` (also the `generation_id`) hashes
  the actions' identities and keys.
- **One-shot entry points.** With no generation handed over, an app from source whose
  directory holds a regular `hull.build.json`, or a TypeScript entry (`app.ts` / `app.mts`,
  whose modules the erasure route makes; typescript_design.md §4.4), runs that child before it loads
  (`hl_generation_take`, `shared/generation.c`): `hull app.*` (serve.c and serve_cli.c, right
  after the entry resolves, before any sandbox) and `hull test`, `hull agent`, `hull mcp` (the
  app context). A failed pipeline is a load error. Never in a built binary, never in a build
  plugin's own process (one with a plugin channel). The child is this binary
  (`hl_chan_self_exe`), so build plugins need a native hull, as `hull plugins run` already
  does. Results are reused from the build result store, so a repeat run spawns no plugin.
- **`hull dev`.** Before every (re)spawn the supervisor runs the child
  (`hl_generation_produce`) while the old app keeps running. A good generation goes to the new
  child on an unlinked temporary file named by `HULL_GENERATION_FD`; then the old child is
  stopped and the new one started. A failed one replaces nothing: a running child keeps its
  last good generation (`[hull:dev] the app keeps running its last good build`), with none
  running nothing starts until the next change; under `--agent` the failure is written to
  `.hull/last_error.json` (MAC'd, `hl_dev_last_error_write`, shared with serve.c). The
  watcher adds the generation's `inputs` (created, removed or changed) to the app's own
  files. `hull dev --tui` does not hand a generation yet: its child runs the pipeline itself,
  as a one-shot entry point does, without the keep-the-last-good rule.
- **`--verify-sig`** refuses a generation with entries; pipeline work that generates no app
  file is nothing to cover.
- **Proof.** The inventory demonstration plugin generates `inventory.json` when configured
  with `module` (`tests/fixtures/plugin_demo/app_gen_*` load it; it is in no tree).
  `tests/e2e_generation.sh` runs it one-shot (Lua and JS, `hull test`, a failed pipeline) and
  under `hull dev` (first spawn, a reload, a failed pipeline keeping the app, recovery);
  `stdlib/cli/lua/hull/buildctx/tests/test_generation.lua` covers the builder.

## 10. Non-scope

- no transform in a loader, no dev-only lowering, no app-process plugin runtime;
- no `.hull/compute-cache/` or any generation inside the app tree;
- no hot module replacement inside a running app process (a reload is a new process,
  as today);
- no pipeline in a built binary;
- the artifact store's layout, the planner and BuildArtifact schemas stay BuildContext
  checkpoint 6's.
