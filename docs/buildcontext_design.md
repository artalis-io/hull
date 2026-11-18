# BuildContext action and authority design

Status: **checkpoints 1 and 2 implemented (§13.1, §13.2), checkpoint 5's plugin side (§13.3), host side (§13.4), demonstration (§13.5) and plugin tests (§13.6); the rest design-only.** This document refines checkpoint 4
of [hull_fs_design.md](hull_fs_design.md) and supersedes the still-unimplemented
BuildContext portions of
[hull_fs_buildcontext_audit.md](hull_fs_buildcontext_audit.md). The shipped
application `hull.fs` resolver, authorization policy, and `stat`/`list` behavior
remain governed by `hull_fs_design.md` and are not changed here.

This design establishes the execution boundary needed by the later Build Plugin,
BuildArtifact, and Compute IR work. It covers:

- one action transaction for every plugin invocation;
- declared and recorded workspace inputs;
- immutable artifact inputs for artifact-to-artifact stages;
- private staged outputs and atomic result commit;
- constrained, identity-pinned build tools;
- one Hull-owned pipeline and plugin contract, not Lua and JavaScript copies;
- the same plugin registration and execution path for bundled and application
  plugins.

It does not implement a plugin registry, BuildArtifact schemas, Compute IR, a
backend, or runtime binding replacement. Those are later checkpoints built on
this boundary.

## 1. Governing decisions

### 1.1 One pipeline, one plugin ABI

Hull owns exactly one build pipeline:

```text
project analysis
    -> pipeline planning
    -> dependency ordering
    -> action transactions
    -> artifact validation and cache
    -> materialization / collect / link / sign
```

The scheduler, action state machine, dependency graph, cache keys, diagnostic
transport, BuildContext authority, and commit protocol are host-owned. They are
not implemented in `build.lua`, a future `build.js`, or individual plugins.

`build.lua` remains the current command driver and build-language entry point. It
may request a host plan, register/configure plugins, and consume action results.
It must not become a second scheduler or independently implement artifact
transactions. There is no `build.js` in this design.

A build plugin is a normal Hull application - an `app.lua` or `app.js` with a
manifest - that registers a transform and runs as its own process under the shipped
runtime, sandbox and limits, talking to the host over one channel (§9.1). So plugins
are written in Lua or JavaScript with no runtime of their own. Source language and
plugin implementation language are orthogonal:

```text
Lua source -> Lua semantic adapter ---------+
                                              +-> one plugin contract
JS source  -> JavaScript semantic adapter --+
```

Plugins consume frontend-neutral semantic handles supplied by ProjectDiscovery.
A plugin in either language can therefore transform Lua or JavaScript declarations.
It does not parse either language itself and does not receive raw Lua AST tables,
QuickJS values, or parser pointers: the semantic operations run in the host's
frontend session and answer over the channel.

A JavaScript plugin is only another Hull application on the same channel. It does
not introduce a JavaScript pipeline, cache, action model, or BuildContext
implementation.

### 1.2 Bundled and application plugins are the same kind of plugin

A bundled plugin and an application-local plugin use the same:

- descriptor schema;
- registration and conflict rules;
- action lifecycle;
- BuildContext operations;
- capability checks;
- artifact validation;
- cache-key construction;
- diagnostics;
- failure and rollback behavior.

There is no domain special case such as `if annotation == "compute"` in Hull
core. Provenance and identity differ, not execution semantics:

```text
stdlib:compute-source@1
app:build/plugins/my_codegen#<content-hash>
```

A bundled plugin is a Hull application embedded in the `hull` binary, like the
stdlib; an application plugin is one in the app tree (its own directory, with its own
`app.lua` / `app.js` and manifest). Both run the same way (§9.1).

Bundled status does not grant hidden filesystem, process, environment, or network
authority. A bundled plugin can have a predefined descriptor and default
configuration, but the resulting action receives only the authority compiled
into its BuildContext. Application plugins are activated explicitly, in
`hull.build.json` (§9.2); Hull does not recursively execute every file under a
magic plugin directory.

Plugin identity includes the exact implementation hash. Replacing local plugin
bytes changes the action identity and invalidates prior results.

### 1.3 Plugins transform supplied inputs; they do not discover globally

Hull owns ProjectDiscovery and artifact selection. A plugin receives only the
source selection or artifact set chosen by the planner. It cannot:

- rescan the project for annotations;
- query a global artifact store for undeclared inputs;
- walk arbitrary workspace paths;
- dynamically activate another plugin;
- mutate the pipeline plan while running.

This preserves the existing ProjectDiscovery seam: analyze once, select normalized
declarations by annotation, and resolve opaque frontend handles through the
registered semantic adapter.

## 2. BuildAction transaction

Every plugin invocation is one `BuildAction`. An action is the unit of authority,
dependency recording, diagnostics, caching, cancellation, and commit.

Conceptual identity:

```text
BuildAction
  action_id
  pipeline_generation
  plugin_identity
  stage_identity
  normalized_configuration
  source_selection | artifact_inputs
  target_profile | null
  BuildContext
```

### 2.1 State machine

```text
CREATED
  -> RUNNING
  -> PREPARING
  -> COMMITTED

CREATED / RUNNING / PREPARING
  -> ABORTED
```

Transitions are monotonic. `commit` and `abort` are host operations, not plugin
methods. Plugin return only requests success or reports failure.

- **CREATED:** authority entries, input handles, output staging root, limits, and
  cancellation token are compiled. No plugin code has run.
- **RUNNING:** the plugin may use its BuildContext. Reads and observations are
  recorded. Writes are confined to the private staging root.
- **PREPARING:** plugin code has returned success. Hull rejects open output
  handles, malformed result descriptors, undeclared effects, schema mismatches,
  symlinks/special output nodes, limit violations, or unresolved diagnostics.
  Hull hashes the complete result and constructs its dependency manifest.
- **COMMITTED:** Hull atomically installs the action-result record into the
  artifact store. The immutable result is now visible to downstream actions.
- **ABORTED:** staged output and incomplete metadata are discarded. No result is
  visible to another action.

An action object cannot be reused. Handles carry the action generation and are
rejected after commit or abort.

### 2.2 Atomicity boundary

The atomic unit is an immutable **action result**, not each generated destination
file independently. An action stages one private result tree containing:

```text
payloads/
result manifest
dependency manifest
diagnostics summary
provenance
```

Hull hashes and fsyncs the staged content, then commits it with one same-filesystem
rename or an equivalent content-addressed manifest installation. A plugin cannot
publish directly into the workspace, VFS, final binary, or another action's
staging root.

Later materialization consumes committed action results. Multi-file generated
outputs are published as a generation tree or immutable blobs plus an atomically
installed manifest. Hull must not claim all-or-nothing publication while issuing
several unrelated destination renames.

### 2.3 Failure, cancellation, and dev generations

Any plugin error, timeout, cancellation, invalid result, tool failure, or commit
failure aborts the action. Downstream actions do not run. For `hull dev`, a failed
pipeline generation does not replace the last valid published generation.

Cleanup is idempotent and closes every held fd, mapped artifact, tool process,
temporary file, and allocator-owned object. Cancellation is checked at every
host call and before PREPARING and COMMITTED.

## 3. BuildContext surface

BuildContext is a Hull-owned opaque action handle. A plugin sees capability-shaped
views over its channel (§9.1), not the host's representation:

```text
ctx.inputs       declared workspace observations
ctx.artifacts    supplied immutable artifact inputs
ctx.outputs      private action-result staging
ctx.tools        declared constrained tools
ctx.diagnostics structured diagnostics
ctx.meta         stable plugin/action/target configuration
```

There is no general application `hull.fs`, unrestricted `tool.*`, process API,
network API, environment table, clock, random source, or global artifact lookup.

All methods fail after action completion and return stable error tokens plus
structured diagnostics. Plugins do not print diagnostics as unstructured console
text.

## 4. `ctx.inputs`: declared workspace inputs

### 4.1 Authority compilation

Input grants are compiled before CREATED using the shipped descriptor-relative
filesystem policy. Each grant is anchored at a held directory fd. Operations are
limited to `read`, `stat`, and deterministic `list`; plugins receive no workspace
write authority.

Input paths are relative names. Absolute paths and lexical escapes are rejected.
For subtree grants, symlinks follow only under the grant's virtual root and clamp
there. Exact and pattern grants retain the shipped no-symlink-widening behavior.
An external tree must be an independently declared input root; a symlink cannot
manufacture authority to it.

### 4.2 Exact observations

Every successful operation records what the plugin actually observed:

- `read`: path, exact bytes, content hash, node type;
- `stat`: path and the explicitly selected metadata fields;
- `list`: path and the complete deterministic ordered entry projection;
- failed existence-sensitive observation: path and stable absence/type result.

Directory listing is a dependency. If a stage discovers sources by listing a
directory, adding or removing an entry invalidates the result even when no old
file content changed.

Default dependency policy is content and type based:

- file bytes participate;
- directory entry names and types participate;
- size is derived from exact content and may be recorded;
- executable/mode bits participate only when the stage declares them meaningful;
- mtime is excluded by default and never substitutes for content hashing.

### 4.3 Repeatable action view

The first observation of a path within an action becomes that action's canonical
observation. Repeated reads/lists return the same captured bytes/projection rather
than silently exposing a second workspace state. Hull may retain small values in
memory and spool large values into a private immutable input snapshot/CAS object.

If Hull cannot capture an observation within the configured byte/entry limits, the
operation fails; it does not fall back to an unrecorded live read. This gives
plugin code a repeatable view without claiming a kernel-wide snapshot of a
concurrently changing workspace.

### 4.4 Cache validation

The committed result stores its complete input-observation manifest. Cache reuse
re-evaluates those declared observations under the same policy and requires their
hashes/results to match. A previous action result is not reusable merely because
the plugin path and top-level source file are unchanged.

## 5. `ctx.artifacts`: immutable artifact inputs

Artifact-to-artifact stages are a first-class requirement. The planner supplies a
fixed ordered set of artifact handles when creating the action. A plugin cannot
open arbitrary artifact IDs or enumerate the global cache.

Each handle exposes only:

```text
identity()
metadata()       kind, schema, producer, target, logical identity
read_blob()
list_tree()
read_tree(path)
mmap_blob()      where the host can provide a bounded read-only mapping
```

Before RUNNING, Hull verifies:

- the artifact ID and content hash;
- expected kind and schema version;
- target/profile compatibility;
- producer/result-manifest integrity;
- that every handle belongs to the planned input set.

Artifact payloads are immutable canonical blobs or trees. They never contain Lua
tables, Lua closures, QuickJS values, parser pointers, frontend session pointers,
or generation-local source handles. A stage that lowers source must serialize its
semantic result into a stable artifact schema before an artifact stage can consume
it.

Reading artifact metadata or content automatically records the artifact hash as a
dependency. The action key includes the ordered input artifact identities; a
plugin cannot hide an artifact dependency by copying bytes through a temporary
workspace file.

Artifact trees reject absolute paths, `..`, device/FIFO/socket nodes, and path
collisions. Symlinks are not emitted or followed in artifact trees in v1. If a
future artifact schema needs links, the link itself must be typed data and each
materializer must opt into a reviewed policy.

## 6. `ctx.outputs`: private result construction

The output view creates result payloads inside the action staging root:

```text
create_blob(logical_name)
create_tree(logical_name)
tree.write(path, bytes, mode_policy)
declare_artifact(descriptor, payload_handle)
declare_generated_output(descriptor, payload_handle)
declare_binding_intent(descriptor)
```

The exact binding/artifact descriptor schemas land with BuildArtifact, not this
checkpoint. BuildContext enforces the generic constraints now:

- logical names and paths are canonical and collision-free;
- writes are descriptor-relative under the private staging fd;
- parent creation is confined to staging;
- shorter replacement truncates;
- output byte/file/depth limits are enforced;
- outputs cannot be symlinks or special files;
- every open writer must close successfully before PREPARING;
- declared payload hashes are computed by Hull from staged bytes, never trusted
  from the plugin;
- no output is externally visible before COMMITTED.

The plugin has no `publish`, `rename_to_workspace`, or `install` primitive.

## 7. `ctx.tools`: constrained tool invocation

Build backends need compilers, linkers, and tools such as `wamrc`, but plugins do
not receive a shell or arbitrary spawn function.

### 7.1 Tool registry and grant

A tool is a Hull-registered descriptor:

```text
tool identity and contract version
resolved executable identity/hash
supported host/target profiles
argument schema
accepted input artifact kinds
produced output shape
environment allowlist
resource limits
network policy
```

The plugin descriptor declares required tool IDs. Pipeline planning resolves them
before action creation. `ctx.tools` exposes only those resolved tools. Bundled and
application plugins use the same resolution and invocation path.

### 7.2 Invocation contract

Conceptually:

```text
ctx.tools.invoke(tool_id, {
  args = typed_options,
  inputs = supplied artifact/blob handles,
  output = staging subtree,
  target = planned target profile,
})
```

Hull, not the plugin:

- maps typed options to argv;
- rejects shell strings, response-file injection, undeclared flags, and arbitrary
  executable paths;
- sets cwd to a private staging/tool directory;
- materializes only declared inputs;
- supplies only allowlisted deterministic environment variables;
- closes inherited fds;
- applies timeout, memory, process, and output-byte limits;
- disables network unless a separately reviewed tool contract requires it;
- captures stdout/stderr as bounded structured diagnostics/log artifacts;
- records exit status, tool identity, normalized request, and output hashes;
- validates produced files before returning handles.

The exact tool identity, contract version, normalized request, target, and relevant
environment values participate in the action key. `clang` from PATH and a pinned
compiler bundle are not the same tool identity.

Existing Hull compiler/linker abstractions and the tools registry should be
adapted into this service rather than bypassed with plugin-owned process logic.

## 8. Diagnostics and provenance

Plugins report through:

```text
ctx.diagnostics.emit {
  severity
  code
  message
  source range | artifact location
  related locations[]
}
```

Hull adds plugin identity, action identity, and pipeline generation. Source ranges
come from opaque ProjectDiscovery handles; artifact stages use artifact ID plus a
schema-defined logical location. Diagnostics flow unchanged through `hull build`,
`hull dev`, and agent inspection.

An error diagnostic prevents PREPARING. Warnings are recorded in the committed
result. Plugins cannot forge another plugin's identity or a source range outside
their supplied inputs.

## 9. Plugin descriptor and runtime

The later registry should require at least:

```text
identity and semantic version
implementation hash/provenance
input class: source | artifact
input kinds/schema constraints
scope/cardinality
output kinds/schema constraints
configuration schema
declared workspace grants
declared tool grants
resource limits
determinism/cache policy
transform entry point
```

Source stages receive a host-created source selection. Artifact stages receive a
host-created ordered artifact set. Both execute as BuildActions with the same
BuildContext and transaction protocol.

A plugin runs as its own process: a Hull application, never inside the tool
process that hosts the action and never inside the application being built. It
does not get the broad `tool.*` table used by legacy `build.lua` - that is tool VM
only. This is the migration seam: existing build behavior remains while new plugins
use the narrow action boundary. New plugin functionality must not expand legacy
`tool.*` as a shortcut.

### 9.1 Plugins are Hull applications on one channel

The host side of an action - the action itself, its inputs, outputs, diagnostics, and
the semantic operations on the supplied source handles - lives in Hull's tool layer
(§13.1). A plugin is a Hull application in a separate process that reaches it only
through one channel.

- **The plugin application.** An `app.lua` or `app.js` whose manifest declares
  `hull/build-plugin@1` and that registers its transform with `app.transform(fn)` (a
  module-conditional decoration, as `hull/timers` adds `app.every`). It is loaded,
  sandboxed, budgeted and given a run deadline by the shipped runtime, like any app;
  the transform runs once per action, as an entry of its own. `hull test` tests it
  against a stubbed context.
- **A narrow manifest preset.** A build plugin's manifest is validated like a flavor
  preset (`--flavor=pure-compute` validates an app's modules): it may declare no
  `hosts`, `env`, `secrets`, `databases`, `fs` grants, `ssh`, `kv`, `tui` or state
  directory, and none of the modules that reach them (`hull/http-client`, `hull/db`,
  `hull/fs`, ...). Time and randomness modules are refused too: a plugin's output is a
  function of its inputs and configuration (§10). Pure modules (`hull/json`,
  `hull/encoding`, `hull/template`, `hull/crypto` digests, ...) are allowed. Every
  workspace byte it sees comes through `ctx.inputs`, so every dependency is recorded.
- **The channel.** The host spawns the plugin process with a descriptor it inherits
  (taken and closed before any app code runs, as `HULL_DEV_SESSION` and the GPU compile
  helper's socket are), and `hull/build-plugin` exposes `ctx.inputs`, `ctx.outputs`,
  `ctx.diagnostics`, `ctx.meta` and `ctx.source` as calls over it. `ctx.source` is the
  action's source selection (`units()`) and the semantic operations its frontends
  offer on those units, answered by the host's frontend session (for TypeScript,
  `erasedSource(unit)`: typescript_design.md §4.1). Each call
  is one framed request and one reply: a named host operation and plain values. The
  operations, their argument checks, their error tokens and their authority are
  defined once, on the host side; the framing and the value codec are written once in
  C; the Lua and JavaScript modules are thin wrappers over one native call, so there is
  no twin binding to drift.
- **Value model.** nil / null, booleans, integers (int64), byte strings, arrays, and
  maps with string keys - nothing else crosses: no functions, userdata, metatables,
  prototypes, getters, cycles, or handles. Depth and size are bounded. Each runtime
  copies a value into this model (Lua: raw accessors, no metamethods; QuickJS: own data
  properties only, as `hl_manifest_json_js` reads a manifest) before it is sent, and
  replies arrive as fresh values, so neither side can hold the other's objects. Workspace
  and payload bytes cross unchanged: a Lua string; in JavaScript an `ArrayBuffer` (text
  the plugin decodes itself), never a string that would re-encode the bytes.
- **Process lifetime.** One plugin process per plugin per pipeline run, serving that
  run's actions one at a time. The host enforces the action boundary: every reference
  it hands out names its action and fails once the action commits or aborts (§2.1), and
  a plugin's run that ends without a result request aborts the action. A process that
  crashes, trips its budget or deadline, or breaks the protocol aborts its current
  action and is not reused; the next action gets a fresh process.
- **Isolation.** The process boundary is the boundary: a plugin shares no memory with
  the host, holds no host object, and runs under the app sandbox with no filesystem,
  network or environment grant of its own.
- **Descriptor.** The plugin descriptor (§9) names the application (its entry, `lua` or
  `js` from the extension) and its configuration schema; registration, authority and
  cache keys are the same for both languages. The language is part of the
  implementation identity (§10), nothing more.
- **Parity.** The same transform written once in each language, given the same
  configuration and inputs, produces a byte-identical result and equivalent
  diagnostics (§12). A JavaScript plugin parses no project source either: it uses the
  semantic operations like a Lua one.

### 9.2 Registration

Three things decide which plugins a pipeline run has.

- **What a plugin is: its own manifest.** A plugin application declares its
  descriptor (§9) in its manifest, beside `hull/build-plugin@1`:

  ```js
  app.manifest({
    modules: ["hull/build-plugin@1"],
    buildPlugin: {                 // Lua: build_plugin
      id: "sql-codegen", version: 1,
      input: "source",             // or "artifact"
      languages: ["lua", "javascript"],
      annotations: ["query"],      // the units it consumes
      output: "generated",
      config: { /* schema */ },
    },
  });
  app.transform(async (ctx) => { /* ... */ });
  ```

  Hull reads it with the existing manifest-extraction child; a plugin's entry is
  `app.lua` or `app.js`, never TypeScript, so it can always be extracted. The manifest
  preset (§9.1) is checked here: a plugin that declares a refused section or module
  is not registered.

- **Which application plugins an app uses: `hull.build.json`.** A code-free file at
  the app root, read by Hull without running any app code - so it works for an app
  whose entry is `app.ts`, which cannot run until it is erased:

  ```json
  { "plugins": [ { "path": "plugins/sql_codegen", "config": { "dialect": "pg" } } ] }
  ```

  Strict schema: unknown keys are refused. `path` is relative, inside the app tree,
  through no symbolic link and not into `.hull`, and names a directory holding exactly
  one of `app.lua` / `app.js`. `config` is checked against the plugin's schema. Nothing
  else activates an application plugin: Hull never scans the tree for plugins. The
  file's bytes are part of every action's key (§10) and it is signed with the app. A
  plugin directory named here is not part of the application being built: build
  embedding, discovery and `hull test` of the app skip it.

- **Bundled plugins: compiled into Hull.** A fixed list (`stdlib:ts-erase@1`, later the
  compute plugins), each on a Hull-defined route that is active whenever its input
  exists - TypeScript units for `ts-erase`. An app neither lists nor replaces them in
  v1.

Identity: `stdlib:<id>@<major>` for a bundled plugin, `app:<path>#<sha256>` for an
application plugin, the hash over every file of its directory (as signing hashes
them), so editing a plugin invalidates its results. The planner's registry for a run
is the bundled list plus the activated plugins; it routes each input kind to the
plugin that claims it, and two plugins claiming the same input kind is an error.

## 10. Cache-key envelope

The action key includes:

```text
pipeline/stage identity and schema version
plugin identity, version, and implementation hash
normalized plugin configuration
source semantic snapshot/selection hash, for a source stage
ordered input artifact hashes, for an artifact stage
declared workspace observation manifest
target profile
tool identities and normalized invocations
allowlisted deterministic environment values
Hull action/BuildContext protocol version
```

The pre-run key identifies a candidate action. Because workspace observations may
be discovered through declared reads/listings, the committed result also carries
the full dependency manifest. Reuse requires validating that manifest. This is a
two-level action key, not an assertion that unknown dynamic dependencies can be
hashed before the plugin runs.

Plugins that request nondeterministic authority are non-cacheable and must be
explicitly classified. Network, wall clock, ambient environment, host randomness,
and arbitrary process execution are unavailable in v1.

## 11. Security invariants

1. The application being built cannot obtain a BuildContext: only a plugin process
   the host spawned holds a channel.
2. Plugin code cannot obtain filesystem, network, environment or database authority
   (its manifest preset refuses them, §9.1) or the legacy unrestricted build
   `tool.*` surface.
3. A plugin sees only planned source/artifact inputs and compiled workspace grants.
4. Artifact inputs are immutable and content-verified before plugin execution.
5. Plugin output is private until host validation and atomic commit.
6. Plugins cannot publish, link, embed, or install directly.
7. Tools are invoked by registered typed contracts, never shell text.
8. Bundled plugins have no hidden authority unavailable to application plugins.
9. Lua and JavaScript application source use one plugin path through semantic
   adapters; there is no duplicated per-language build pipeline.
10. Handles are action/generation scoped and fail after commit, abort, or reload.
11. Every committed result has complete producer, input, tool, target, and source
    provenance.
12. A failed action cannot partially replace the last valid dev/build generation.

## 12. Acceptance matrix

### Action transaction

- success commits exactly one immutable result;
- plugin error, diagnostic error, cancellation, timeout, invalid output, and tool
  failure each leave no visible result;
- crash/restart cleanup removes abandoned staging without touching committed data;
- commit is race-safe when two actions produce the same content or logical name;
- stale handles fail after commit/abort;
- allocator/fd/process accounting returns to zero on every injected failure.

### Workspace inputs

- undeclared path is denied;
- read/list/stat observations are recorded and repeatable inside one action;
- directory addition/removal invalidates a cached result;
- content change with unchanged mtime invalidates;
- mtime-only change does not invalidate under the default policy;
- contained subtree symlink behavior matches the compiled root policy;
- exact/pattern grants do not widen through symlinks;
- concurrent replacement never escapes the held root.

### Artifact inputs

- wrong kind/schema/target/hash is rejected before plugin code runs;
- a plugin cannot open an artifact absent from its planned set;
- blob/tree reads record dependencies automatically;
- malformed paths, special nodes, symlinks, and collisions are rejected;
- a two-stage `source -> artifact -> artifact` demonstration runs without
  reconstructing a workspace path for the intermediate artifact.

### Outputs

- multi-file output appears only as one committed result;
- partial writer failure rolls back the entire result;
- path escape, symlink, FIFO/device/socket, duplicate path, over-limit tree, and
  unclosed writer are rejected;
- committed bytes and hashes are computed from exact staged content;
- plugin code has no publication primitive.

### Tools

- undeclared tool and executable path are denied;
- unknown flag, shell/response-file injection, ambient env, inherited fd, and
  network attempt are denied;
- timeout/nonzero exit/oversized output aborts the action;
- tool identity or target change invalidates the result;
- output validation rejects unexpected or escaping files;
- the same invocation works for a bundled and application-local test plugin.

### Single pipeline and plugin parity

- one Lua-authored plugin consumes both Lua and JavaScript source selections via
  semantic adapters and produces the same canonical demonstration artifact;
- the same demonstration plugin written in JavaScript produces a byte-identical result
  and equivalent diagnostics over the same channel (§9.1), and neither runtime can send
  the host a value outside the channel's value model;
- a plugin manifest that declares a refused section or module fails to load as a
  plugin; a plugin process that crashes, trips its budget or breaks the protocol aborts
  its action and is replaced;
- no plugin parses project source or accesses a raw frontend AST;
- bundled and application-local copies of the same plugin produce byte-identical
  results and equivalent diagnostics under the same configuration/authority;
- neither plugin receives extra authority because of provenance;
- no JavaScript build pipeline or duplicate JavaScript plugin implementation is
  required.

## 13. Implementation checkpoints

Each checkpoint stops for review.

1. **Action core + `ctx.inputs`.** Opaque action lifecycle, compiled input grants,
   observation recording/repeatability, dependency manifest, cancellation, and
   failure-injection tests. No plugin loader yet.
2. **`ctx.outputs`.** Private result staging, complete validation, atomic action
   result commit/rollback, recovery of abandoned staging. No workspace
   publication API.
3. **Artifact input seam.** Minimal immutable blob/tree handle and schema envelope
   sufficient to prove `artifact -> action -> artifact`. This is the prerequisite
   for artifact-transform stages, not the full BuildArtifact product model.
4. **Constrained tool seam.** One harmless registered fixture tool first; prove
   typed argv, identity, limits, captured diagnostics, and staged output. Then
   adapt existing compiler/linker/wamrc services separately.
5. **Plugin runner + parity proof.** Plugins as Hull applications (§9.1): the channel
   and its value codec, the `hull/build-plugin` module and `app.transform` in both
   runtimes, the plugin manifest preset, the runner that spawns and supervises plugin
   processes, registration (§9.2: descriptors from plugin manifests, `hull.build.json`,
   the bundled list), identical bundled/application registration, and
   one demonstration plugin over both Lua and JavaScript semantic source selections,
   written in Lua and in JavaScript with byte-identical results.
6. **BuildArtifact + planner.** Canonical artifact schemas, explicit pipeline
   routes, DAG ordering/cycle rejection, cache integration, and the first real
   source-to-artifact-to-artifact chain.

Compute IR follows these checkpoints. Its source lowerer, linker, C/WASM/GPU
backends, and AOT postprocessor are ordinary source/artifact plugins using this
same action boundary.

### 13.1 Checkpoint 1 as built

Host-owned tool code, mostly Lua: `stdlib/cli/lua/hull/buildctx/action.lua` holds the action
state machine, the observation table, the limits, cancellation, the dependency manifest and its
revalidation. The one native piece is `hull.buildctx._fs` (`src/hull/runtime/lua/mod_buildctx.c`):
it compiles the grants and reads, stats and lists through the shipped fs policy, returning the
content and projection hashes - the part that has to hold descriptors. "Host-owned" (§1.1) means
the scheduler and transaction live in Hull's tool layer, not in `build.lua` and not in a plugin;
it does not mean C. Nothing in `hull` calls it yet: the module is registered in the tool VM when
the plugin runtime (checkpoint 5) drives actions. Tested by `tests/hull/buildctx/test_buildctx.c`
(every `make test`), which runs `buildctx/tests/test_action.lua` in a plain Lua state over the real
binding and a real workspace.

- **Grants** use the shipped fs grant syntax and compile with `hl_fs_policy_compile_manifest`
  (read set only), so authority is exactly the shipped policy: a SUBTREE follows links inside its
  anchor and clamps `..` and absolute targets at the anchor (not the workspace root); EXACT /
  PATTERN refuse links, at compile and when a link replaces a granted file later; the root's
  `.hull` stays reserved.
- **Three outcomes per op.** A *denial* (`permission`, `invalid_path`, `path_too_deep`,
  `reserved_path`) depends only on the grants: returned, not recorded. An *observation*
  (content, a type, or the workspace's answer `not_found` / `not_a_regular_file` /
  `is_a_directory` / `not_a_directory` / `symlink_denied` / `symlink_loop`) is recorded, and
  the first one of a (op, path, fields) is canonical: a repeat returns it, whatever the tree
  holds by then. A *failure* (`limit`, `io_error`, anything else) fails the op and the action:
  prepare answers `input_failed`, so no result can lack an input its plugin read.
- **Paths** are canonical (relative, no empty / `.` / `..` part, no trailing `/`) and valid
  UTF-8 with no control byte, so one file has one spelling and every path round-trips through
  the manifest; anything else is `invalid_path`.
- **What is recorded:** a read's SHA-256 and size; a list's names and types (sizes are not part
  of the projection - a file's size is its content's business); a stat's type plus only the
  fields asked for (`size`, `exec`). mtime is never offered or recorded.
- **Limits** (a missing field takes the default): one read 16 MiB, captured bytes per action
  256 MiB, distinct observations 65536.
- **State.** A method on a committed or aborted action answers `stale_handle`; on the wrong live
  state, `bad_state`. `cancel()` makes the next host call (an input op, prepare, commit) abort
  the action and answer `cancelled`.
- **Dependency manifest** (`deps()`, from preparing): compact JSON,
  `{"format":"hull-build-deps","version":1,"grants":[...],"observations":[...]}`, observations
  sorted by path, then op (list, read, stat), then fields, so the same observations made in any
  order give the same bytes. `action.check(root, manifest)` recompiles the recorded grants,
  replays every observation and answers fresh / stale; a malformed manifest or another version
  is an error, not a miss.
- **Commit** installs the action's result: §13.2.
- **Resources:** the binding holds a descriptor only inside one call; a Lua error that unwinds
  past a read or a listing leaves it to the next call or the finalizer. The test checks that
  open descriptors return to their count over repeated actions, aborted ones included.

### 13.2 Checkpoint 2 as built

`ctx.outputs`, diagnostics, the committed result and its lookup, in the same split: Lua tool
code (`hull.buildctx.outputs` for staging, `hull.buildctx.action` for the transaction) over one
more small native piece, `hull.buildctx._store` (in `mod_buildctx.c` beside `_fs`). Tested by the
`outputs_and_results` leg of `test_buildctx` (`buildctx/tests/test_outputs.lua`).

- **Staging is in memory**, bounded (a missing limit takes the default: 32 MiB of output bytes,
  4096 files - blobs plus tree entries - and 32 path components in a tree). A plugin builds
  payloads - `create_blob(name)` then `write` / `close`, or `create_tree(name)` then
  `write(path, bytes, "regular" | "executable")` / `close` - and declares them:
  `declare_generated_output({ name = "./app.ts" }, handle)`, `declare_artifact({ name, ... },
  handle)`. Generic constraints only (§6): canonical, collision-free names (a generated output
  may carry the `./` of its source name); a tree path is canonical, unique, and never both a file
  and a directory; descriptors are plain values (string-keyed maps, arrays, strings, integers,
  booleans - no floats), encoded canonically. Trees hold files only: no symlink, special file or
  mode beyond regular / executable can be expressed. There is no publication primitive.
- **Diagnostics** (§8): `emit({ severity, code, message, path?, range? })`. An error prevents
  PREPARING; warnings and infos are recorded in the result, in the order emitted.
- **PREPARING** refuses, and aborts the action on, a failed input (`input_failed`), an open writer
  (`open_writer`), a payload created but not declared (`undeclared_payload`) or an error
  diagnostic (`diagnostic_error`). It then stores every payload - the dependency manifest among
  them - in the store under its own SHA-256 (Hull hashes the staged bytes; nothing is taken from
  the plugin) and builds the result. A payload stored here is referenced by nothing until the
  result is installed, so nothing is visible.
- **The result** is canonical JSON: `{"deps":{"sha256"},"diagnostics":[...],
  "format":"hull-build-result","key","outputs":[...],"version":1}`, outputs sorted by kind then
  name, each with its descriptor and its payload (`{"type":"blob","sha256","size"}`, or a tree's
  entries sorted by path with mode, hash and size). The same outputs declared in any order give
  the same bytes.
- **COMMITTED** is one atomic put of the result under the action key (64 hex, chosen by the
  caller; the full key of §10 comes with the planner). A failed store write at prepare or commit
  aborts the action (`store_failed`) and leaves no result.
- **The store** (`_store.open(root)`) is a blob store whose every entry is sealed under the tool
  key (`$HOME/.hull/tool-cache.key`, which no app process loads), payloads and results in
  separate seal domains so a payload never verifies as a result; a put is atomic (temp + rename),
  and a key names one content, so two actions committing the same key race harmlessly. The
  sealed put now reports failure and accepts an empty payload (an erased file can be empty).
  Where the store lives in `~/.hull` is set when the pipeline first uses it (checkpoint 5), with
  its `hull cache` registry row.
- **Lookup** (`action.lookup(store, key, root)`): the result must be present and sealed, its
  dependency manifest must still hold for the workspace (`action.check`), and every payload must
  be present with its hash and size; it answers the outputs' bytes and the diagnostics, or
  `miss`, `stale` or `corrupt`.
- **Abandoned work**: staging is never on disk, so a crash leaves no staging to recover - only
  payloads no result references, which `hull cache prune` removes, and the blob store's own
  temporary files, which it sweeps when opened.

### 13.3 Checkpoint 5: the plugin side as built

The plugin half of §9.1: the channel, the value model, `app.transform` in both runtimes and the
manifest preset. The host half - the runner, registration (§9.2) and the host operations - is
§13.4.

- **The channel** is `shared/child_channel` (spawn with the socket as fd 3, stdin / stdout /
  stderr on `/dev/null` and a filtered environment; length-framed messages under deadlines;
  reap), the GPU compile helper's code moved out of `cap/gpu_helper.c`, which uses it unchanged.
  Plain POSIX: both ends are synchronous round trips, and the base stays Keel-free. Not on a cosmo
  APE or Windows.
- **The value model** is `shared/plain_value`: nil, booleans, int64, UTF-8 text, bytes, arrays,
  and maps with text keys in ascending byte order, so one value has one encoding (fuzzed by
  `fuzz_plain_value`). The Lua walker reads with raw accessors and allocates nothing in the VM, so
  no metamethod or finalizer runs during an encode; a Lua string is text, and bytes travel in a
  box (`chan.bytes`). The QuickJS walker reads own data properties of plain objects and arrays
  only, by class id: a Proxy, a getter, a Date or a boxed primitive is refused before any trap
  runs; an ArrayBuffer or typed array is bytes, and a string written as bytes becomes its UTF-8
  (`chan.bytes`, a lone surrogate refused). A float, a function, a hole or a `nil` / `null` in an
  array is refused; a `nil` / `null` map value is absent. Decoded, text and bytes are a Lua string;
  in JavaScript a string and an ArrayBuffer, an integer past 2^53 a BigInt.
- **The protocol** (`shared/build_plugin.h`, magic `HBP1`): the host sends ACTION `{ id, config,
  meta }`; the plugin answers each ctx call with CALL `{ op, args }` and reads REPLY `{ ok }` or
  `{ error }` with the same id; it ends the action with DONE `{}` or `{ error }`; STOP (or the
  host closing the socket) ends the loop and the process exits 0. Any other frame, a frame out of
  turn or a malformed value ends the channel, and the plugin exits non-zero.
- **The runner** (`serve.c`) takes fd 3 when `HULL_BUILD_PLUGIN_FD=3` is set, before any app
  code runs (as it takes the dev session key), and refuses to start when the variable names no
  socket. Once the module set is validated it applies the preset (`hl_build_plugin_preset_check`):
  no `fs`, `env`, `secrets`, `hosts`, `databases`, `kv`, `ssh`, `tui`, `state`, `gpu` or
  `compute`, and only the pure modules `hull/app`, `hull/build-plugin`, `hull/csv`,
  `hull/encoding`, `hull/json`, `hull/log`, `hull/path`, `hull/template`, `hull/validate` (no time
  or randomness). The plugin then runs under the shipped app sandbox, budget and run deadline.
- **`app.transform(fn)`** is the decoration `hull/build-plugin@1` installs: a few lines of C in each
  runtime hand the stdlib (`hull._build_plugin` in Lua, the script `hull/_build_plugin.js`) the live
  `app` and the channel - `next`, `call`, `done`, `bytes`, reachable only from that stdlib code. The
  stdlib registers `app.main` as the loop: read an action, call `fn(ctx)` (awaited in
  JavaScript), report DONE, repeat. Each action re-arms app.main's budget and run deadline, as a
  `tui.poll` event does; waits for the host are excluded from the binding meter, and each call
  pays `HL_WORK_PLUGIN_CALL_UNITS` plus its bytes. A REPLY is awaited at most the run's time left
  (60 s without a deadline).
- **The ctx API**, one call each, named for the host operation it sends:
  `ctx.meta`, `ctx.config`; `ctx.inputs.read / stat / list`;
  `ctx.outputs.create_blob / create_tree` (writers with `write` / `close`),
  `declare_generated_output`, `declare_artifact`; `ctx.diagnostics.emit`;
  `ctx.source.units / erased_source` (JavaScript: camelCase). A writer's host id is kept in a
  table private to the stdlib, so a handle cannot be made up. A refused call returns `nil, token`
  in Lua and throws an Error whose `code` is the token in JavaScript; the host defines the
  operations, their checks and their tokens.
- **Tests**: `test_plain_value`, `test_child_channel`, `test_build_plugin` (the preset, the
  descriptor), and `tests/e2e_build_plugin.sh` (`make e2e-build-plugin`): the same transform in
  Lua and JavaScript drives a fake host (`tests/e2e_build_plugin_drive.c`) with byte-identical
  calls and results over two actions on one process; a refused preset, an unknown frame, a run
  by hand and a bad descriptor each end as described.

### 13.4 Checkpoint 5: the host side as built

The host half of §9.1 and §9.2, in the same split as the action: Lua tool code
(`hull.buildctx.plugin` the runner, `hull.buildctx.registry` registration) over one more small
native piece, `hull.buildctx._plugin` (in `mod_buildctx.c`): spawn a plugin with the channel on its
fd 3, send a frame, receive one under a deadline, kill and reap - the part that holds a socket and
a pid. The three natives are loaded in the tool VM only, where a spawn is held to the tool
sandbox's grants like every other tool spawn (the executable for execute, the entry for read). The
first caller is `hull plugins run` (§13.5); the planner (checkpoint 6) is the next.

- **The runner** (`plugin.new({ exe, entry })`, `r:run(action, req)`, `r:close()`) spawns the hull
  binary on the plugin's entry with `-d :memory:` (an implicit `data.db` would be written into the
  plugin's directory and change its identity), `HULL_BUILD_PLUGIN_FD=3` and an environment of
  `HOME`, `TMPDIR`, `HULL_CACHE_DIR` and the `HULL_NO_*` cache switches only. `run` starts the
  action, sends ACTION, answers each CALL until DONE, then prepares and commits; it answers true or
  a token: `plugin_failed` (the transform raised; the process is reused), `plugin_crashed` (it
  exited or closed the channel), `plugin_timeout` (no DONE within `action_ms`, 120 s by default),
  `plugin_protocol` (a frame out of turn, a value outside the model, a malformed DONE),
  `spawn_failed`, `cancelled`, or what prepare / commit answer. A crash, a timeout or a protocol
  error kills the process; the next action spawns a fresh one, and a process that went while idle is replaced
  before the action starts. `close` sends STOP and waits for the exit.
- **The host operations** are defined once, in the runner: the op table maps each CALL to the
  action (`inputs.read` / `stat` / `list`, `outputs.create_blob` / `create_tree`, `blob.write` /
  `close`, `tree.write` / `close`, `outputs.declare_generated_output` / `declare_artifact`,
  `diagnostics.emit`) or to the caller's source selection (`source.units`, and
  `source.erased_source` when the caller's frontend session offers it - `unsupported_op`
  otherwise). Argument shapes are checked here (`invalid_args`, `invalid_handle`, `unknown_op`);
  everything else is the action's token. Bytes go back as bytes (`_plugin.bytes`), an empty list
  as an empty array (`_plugin.empty_array`, a marker the Lua walker encodes as `[]`, so a
  JavaScript plugin gets an array). A writer's id is a counter of the runner, kept in a table of
  the action, so an id from an earlier action is `invalid_handle`.
- **Registration** (`registry.load(root)`): the bundled list (empty until `stdlib:ts-erase@1`), then
  `hull.build.json`, read through an exact grant (a link there is refused): `{ "plugins": [ { path,
  config? } ] }`, unknown keys refused, each `path` canonical, not into `.hull`, named once, and a
  directory holding exactly one of `app.lua` / `app.js` (exact grants: no link on the way). The
  identity hash is over every file of the directory (`<path>\0<sha256>\n`, sorted, hashed; a link
  or special file in it is refused, 1024 files and 64 MiB at most); the manifest comes from the
  extraction child (`tool.extract_manifest_lua` / `_js`), and the preset is the runner's own
  check (`hl_build_plugin_preset_check`, through `_plugin.preset_check`, which reads the decoded
  manifest with `hl_manifest_extract_lua`). The descriptor (`build_plugin` / `buildPlugin`):
  `id`, `version`, `input` (`source` with `languages` and optional `annotations`, or `artifact` with
  `kinds`), `output`, and an optional `config` schema (`{ key = { type, required? } }`, types
  `string` / `integer` / `boolean` / `array` / `map`); unknown keys refused. The configuration is
  checked against it. A plugin claims `source:<language>:<annotation or *>` or `artifact:<kind>`,
  and two plugins claiming one kind refuse the load. `reg.config_sha256` is the file's hash, for
  the action key (§10).
- **Tests**: the `plugin_host` leg of `test_buildctx` runs real plugin processes
  (`tests/fixtures/build_host_{lua,js}`, every host operation) through real actions - a commit, a
  failing transform on a reused process, an undeclared payload, a cancelled action, a process
  that dies and is replaced, the host's deadline - with the Lua and JavaScript results
  byte-identical, and the protocol paths over a scripted process; the `registration` leg covers
  `hull.build.json`, links, the preset, descriptors, configuration and claims; and
  `lua_tool_vm.buildctx_registry_reads_real_plugin_manifests` runs registration in the real tool
  VM with the real extraction.
- **Not yet**: the bundled plugins' entries come with `stdlib:ts-erase@1`; the rest is §13.5.

### 13.5 Checkpoint 5: the demonstration as built

`hull plugins list|run [app_dir] [--json]` (`src/hull/commands/plugins.c`, the tool module
`hull.plugins`) over `hull.buildctx.pipeline`, the part of a pipeline run the planner will reuse.

- **list** loads the registry (§13.4) and prints each plugin's identity, language, input and output
  kinds, and claims; a registration error is exit 2.
- **run** (`pipeline.run(root)`): project discovery (`hull.project.analyze`; an invalid discovery
  stops the run), then for each source plugin its selection - every annotated declaration in a
  language it lists, carrying an annotation it lists (any, when it lists none), outside every
  plugin directory. A unit is a plain value, `{ id, language, path, kind, name, range, annotations
  = { { name, value } } }`, the same shape for every language; the action's grants are exactly the
  units' files. The action key is the SHA-256 of a canonical envelope (`hull-build-action-key`
  version 1: the channel protocol version, the plugin's identity, descriptor and configuration,
  the selection, `hull.build.json`'s hash). A committed result that `action.lookup` finds fresh is
  reported `cached`; otherwise the plugin runs (§13.4) and its result is `committed`, or the run
  reports `failed` with the runner's token and any diagnostics. A plugin with nothing selected is
  `no_input`; artifact plugins and bundled ones are `skipped` until the planner runs them. Exit 1
  when a plugin failed.
- **The result store** is the `build-results` cache kind (`~/.hull/blobs/runtime/build-results`, or
  under `HULL_CACHE_DIR`), sealed under the tool key; `HULL_NO_BUILD_CACHE` (or `HULL_NO_CACHE`)
  turns reuse off, never the commit. A result's key is the pre-run key, which a later run with
  other dependencies shares, so `put_result` replaces a stored result (a stale one); payloads stay
  content-addressed.
- **Plugin directories are not the app's**: `hull build` leaves every file under a directory
  `hull.build.json` names out of the binary (`hull.build.json` itself is embedded and signed), and
  the selection leaves them out of discovery's declarations; project discovery and `hull test`
  skip them too (§13.6).
- **The demonstration** (`tests/fixtures/plugin_demo`): an inventory plugin - it lists the `@demo`
  declarations it is shown, by name and annotation value, as one artifact, and reads each unit's
  file so the source is a dependency - written once in Lua and once in JavaScript, over the same
  app written in Lua and in JavaScript. `tests/e2e_plugins.sh` (`make e2e-plugins`) runs all four
  pairings through the hull binary and requires byte-identical artifacts and diagnostics; checks
  the cached rerun, a re-run after an edit outside the annotations (the dependency manifest, same
  bytes), a new selection after a new annotation and `HULL_NO_BUILD_CACHE`; reads a JavaScript
  plugin's manifest through the extraction child (`list`); refuses a missing required
  configuration and a plugin outside the preset; checks that nothing is written into the app and
  that `hull build` leaves the plugin's sources out.
- **Not yet**: `hull test` of a transform against a stubbed ctx, and discovery / `hull test`
  skipping plugin directories: §13.6.

### 13.6 Checkpoint 5: plugin tests as built

- **`test.run_transform(spec)` / `test.runTransform(spec)`** (`hull test` only) runs the transform
  `app.transform` registered against a stub host held in memory, through the same ctx and the same
  calls (`hull._build_plugin` / `hull:_build_plugin` route them to the stub instead of the channel),
  and answers `{ outputs, diagnostics, error? }` (JavaScript: a Promise). `spec`: `config`, `meta`,
  `units` (what `ctx.source.units()` returns), `inputs` (path -> contents: `read`, `stat` and
  `list` answer from it, `not_found` / `not_a_regular_file` / `not_a_directory` like the host),
  `erased` (unit -> contents, for `erased_source`). Each output carries `declared`, `descriptor`,
  `name` and the bytes (Lua: a string; JavaScript: an ArrayBuffer, and `text` when every write was a
  string), or a tree's entries. The stub is a test double, written once per stdlib language: the
  host's own checks (limits, names, grants, descriptors) are the real host's, which
  `hull plugins run` exercises. The runner is handed to the runtime by the stdlib and kept where
  only `test` reaches it (the Lua registry, the JS runtime's internal store); an app that is not a
  build plugin gets an error naming `hull/build-plugin@1`.
- **Plugin directories are skipped** by project discovery (`hull.project.analyze`, so
  `hull agent inspect` and `hull dev --agent` generations) and by `hull test` of the app
  (`hl_build_plugin_drop_dirs`, `shared/build_plugin.c`: the directories `hull.build.json` names,
  read leniently - a malformed file drops nothing; checking it is the registry's job). A plugin's
  own tests run with `hull test <plugin dir>`.
- **Tests**: the demonstration plugins carry their tests (`tests/fixtures/plugin_demo/inventory_*/
  tests/`), which `tests/e2e_plugins.sh` runs with `hull test` in both languages; the e2e also
  checks that the app's `hull test` and `hull agent inspect` leave the plugin out;
  `build_plugin.drop_dirs_leaves_plugins_out` covers the C filter.

## 14. Non-scope

- no `build.js`;
- no plugin runtime of its own: plugins are Hull applications on the shipped Lua and
  QuickJS runtimes, and no JavaScript-specific host operation, pipeline, or cache
  (§9.1);
- no duplicate Lua/JavaScript implementations of a bundled plugin;
- no Compute IR or Query IR;
- no generic process/network access;
- no direct plugin workspace publication;
- no universal AST;
- no raw frontend objects in artifacts;
- no automatic execution of every compatible plugin;
- no replacement of the existing build pipeline before the action core is proven
  additively.

