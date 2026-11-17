# BuildContext action and authority design

Status: **checkpoint 1 implemented (§13.1); the rest design-only.** This document refines checkpoint 4
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
  `ctx.diagnostics`, `ctx.meta` and the source operations as calls over it. Each call
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
revalidation. The one native piece is `hull.buildctx._fs` (`src/hull/runtime/lua/mod_buildctx_fs.c`):
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
- **Commit** in this checkpoint only moves the action to committed: the result store and the
  atomic install are checkpoint 2.
- **Resources:** the binding holds a descriptor only inside one call; a Lua error that unwinds
  past a read or a listing leaves it to the next call or the finalizer. The test checks that
  open descriptors return to their count over repeated actions, aborted ones included.

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

