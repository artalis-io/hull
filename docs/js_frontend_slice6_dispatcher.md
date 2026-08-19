# JS frontend Slice 6 - C dispatcher + single-registry integration

Design record for Slice 6 of the JavaScript source frontend
(docs/javascript_source_frontend_design.md slice 6, sections 10-11; Q2/Q3/Q6/Q7/Q12). Wires the
Slice-5 JS adapter into the ONE existing Lua ProjectDiscovery builder through a C-owned
generation/session manager + a language-neutral dispatcher binding + a thin Lua proxy, WITHOUT
creating a second model. This is the DESIGN; implementation follows on approval. ASCII-only, no
em-dashes.

## 1. Goal + the invariant

Make `hull.project.analyze` produce a mixed Lua+JS ProjectDiscovery through the SAME
`hull.project.model`, where:

- Lua orchestrates + owns the one model (unchanged shape, Q2). No Lua code loads a parser,
  touches QuickJS, or interprets a JS AST.
- Lua performs frontend SELECTION from the canonical registry; C owns tooling-engine DISPATCH -
  QuickJS session create / invoke / limits / transport-validate / teardown. C keeps NO duplicate
  extension map. The JS frontend never sees Lua.

The seam is a single uniform `analyze_one(language, path, src, ctx) -> per_source` in the
analyzer; for `lua` it uses the in-process Lua frontend (as today), for `javascript` it makes ONE
C-bridge call returning the IDENTICAL facts shape (Slice 5) and builds JS `unit`/`declaration`
handle objects with the same accessor contract. Both feed the unchanged `model.build`, and the
`_handles` payload shape is identical for both (section 5).

Out of scope: the end-to-end mixed-language ProjectDiscovery through `hull dev` /
`hull dev --agent` / `hull agent inspect`, the generation publish/read lifecycle, and the full
security-boundary validation (Slice 7). This slice delivers the plumbing + a direct integration
test + the stale-token ABA test.

## 2. C generation/session manager (owns the QuickJS lifetime + monotonic tokens)

New C unit (`src/hull/frontend/js_generation.{c,h}`) owning the per-generation session lifecycle.
ONE session per project-analysis generation, shared across its JS files.

- **State:** a bounded table of LIVE generations, each `{ session_token, HlJsSession* }`, a
  strictly-monotonic `next_token`, and a MANAGER MUTEX. A small hard cap on concurrent live
  generations (e.g. 8) fails a further `open` closed.
- **Token contract (int64, positive).** A `session_token` is a POSITIVE `int64_t`, so it crosses
  Lua's signed-integer numbers exactly (never an arbitrary `uint64_t`). `token 0` is always
  invalid. `next_token` starts at 0 and pre-increments; if it would exceed `INT64_MAX` the
  manager is PERMANENTLY EXHAUSTED and every further `open` fails closed (rather than wrapping and
  risking reuse). At ~1 open/microsecond that is ~300000 years, so exhaustion is theoretical, but
  the contract is fail-closed, not wrap.
- **Serialization.** The manager mutex is held across `lookup + invocation` and across `close`,
  so a `close` can never destroy a session while an `analyze`/`semantics`/`scope` is using it
  (the tool VM is single-threaded today; the mutex makes the manager safe regardless).
- **`hl_js_gen_open(void) -> int64_t token`:** under the mutex, `token = ++next_token` (NEVER
  reused, even across close; fail closed at INT64_MAX); `session = hl_js_session_create(&limits)`;
  record `{ token, session }`; return the token (`0` = failure: cap reached, exhausted, or
  session-create OOM).
- **`hl_js_gen_analyze(token, src, len, path, opts, &out, &out_len) -> int`:** under the mutex,
  look up the LIVE session for `token`; if none (a closed/stale/never-issued/`<=0` token) return a
  deterministic operation-specific STALE result (section 8) WITHOUT dereferencing any freed state;
  else call `hl_js_session_analyze(session, "hull:source:frontend_entry", "analyze", src, len,
  path, opts, ...)` with the mutex held for the duration.
- **`hl_js_gen_declaration_semantics(token, decl_id, &out, ...)` / `hl_js_gen_scope(token,
  unit_id, ...)`:** same locked liveness check, then call the entry's `declarationSemantics` /
  `scope` method with the id passed as `{ "declId": N }` / `{ "unitId": N }` options JSON. Stale
  token -> the operation-specific stale shape (section 8).
- **`hl_js_gen_close(token) -> int`:** under the mutex, find the live entry;
  `hl_js_session_destroy(session)`; remove it. The token integer is now permanently dead (never
  reissued). Closing an unknown/already-closed/`<=0` token is a safe no-op (idempotent).

**Monotonic tokens defeat ABA (Q7 + Slice-5 correction).** Because `next_token` only increments
and closed tokens are never reissued, a stale `token` from a destroyed generation can never
match a LATER generation that happens to reuse the same heap slot / allocator address: the later
generation has a strictly higher token, so the stale token is absent from the live table and is
rejected deterministically. This is the property a bare session-relative `decl_id` cannot
provide alone (Slice 5 section 6); the manager supplies it at the `token` layer.

Teardown is EXPLICIT + C-owned: the manager never closes a session on its own; the analyzer
(section 5) calls `hl_js_gen_close` at generation end. On tool-VM shutdown a `hl_js_gen_shutdown`
destroys any still-live sessions (defensive; the analyzer should have closed them). CRITICAL:
`hl_js_gen_shutdown` MUST NOT reset `next_token` - it only destroys live sessions and clears the
live table. If shutdown reset the counter, a later `open` in the SAME process would reissue a low
token and reintroduce the ABA it was designed to prevent (an old handle from before shutdown could
match a new post-shutdown session). `next_token` is monotonic for the whole process lifetime, not
per-session-set.

## 3. Production entry module (registers the adapter on the session)

New `stdlib/cli/js/hull/source/frontend_entry.js` - the SHIPPED entry the session loads (the
production analog of the test-only `hull:source:lextest`). It imports the Slice-5 adapter and
registers `globalThis.__hull_frontend` with three methods matching the session's
`(ArrayBuffer src, path, options)` call convention:

- `analyze(srcBytes, path, opts)`   -> `frontend_javascript.analyze(new Uint8Array(srcBytes), path, opts)`
- `declarationSemantics(_, _, opts)` -> `frontend_javascript.declarationSemantics(opts.declId)`
- `scope(_, _, opts)`                -> `frontend_javascript.scope(opts.unitId)`

The adapter's session module-scope state (units/decls) persists across method calls WITHIN one
session (one generation), and is dropped by `JS_FreeRuntime` at `hl_js_gen_close`. The test-only
`__analyzeWithFailure` / `__mutate` are NOT registered by the production entry.

## 4. Language-neutral dispatcher binding (`tool.frontend_*`)

New C bindings (`src/hull/tool_frontend.c`, registered alongside the orchestration bindings)
exposing the manager to the tool VM. They are LANGUAGE-NEUTRAL: each takes a `language` argument
(only `"javascript"` is dispatched today; an unknown language is a clear error), so a future
language adds a manager + a dispatch branch, not a new binding. The bindings are ALWAYS PRESENT
(even in a build without the JS tooling engine - section 6), so Lua never calls a missing symbol.

- `tool.frontend_available(language) -> boolean` - true iff the tooling engine for `language` is
  compiled into this hull. FALSE in a JS-less build; the registry gates JS analyzability on it
  (section 6). Always present.
- `tool.frontend_open(language) -> token | nil, err` - opens a generation session (today:
  `hl_js_gen_open`). Returns `nil, err` when the engine is unavailable or `open` failed.
- `tool.frontend_analyze(language, token, path, src) -> facts_json` - the validated facts JSON
  string (the Lua proxy decodes it via `hull.json`). A stale/closed token yields the facts-shaped
  stale JSON (section 8).
- `tool.frontend_declaration_semantics(language, token, decl_id) -> record_json`.
- `tool.frontend_scope(language, token, unit_id) -> model_json`.
- `tool.frontend_close(language, token)` - closes the generation session (idempotent).

The bindings marshal Lua <-> C only (positive int64 tokens + byte strings + a returned JSON
string); they hold NO QuickJS value and expose NO pointer. Transport validation is the session's
(Slice 1: bytes in as an ArrayBuffer with the exact length, validated JSON out,
`js.limit.*`/`js.internal`, never-raise) plus the manager's token-liveness check. The C keeps no
extension map - selection is Lua's (section 6).

## 5. Lua proxy + the `analyze_one` seam

**The `_handles` shape is UNCHANGED and language-neutral.** The analyzer keeps storing exactly
`{ frontend = fe, unit = unit, declaration = d }` for BOTH languages, so `resolve_handle` has an
identical structure and no JS-specific hole opens in the frontend contract. The JS proxy supplies
JS-shaped `unit` and `declaration` OBJECTS that carry their private integers INTERNALLY:

```
unit        = { _session = token, _unit_id = facts.unit_id, _declarations = { ... } }
declaration = { _session = token, _decl_id = fact.decl_id, <normalized accessor fields> }
```

The unchanged contract calls then work verbatim: `fe.declaration_semantics(declaration)` reads
`declaration._session` + `declaration._decl_id`; `fe.scope(unit)` reads `unit._session` +
`unit._unit_id`. The proxy extracts its private integers; the analyzer never sees a token.

**Lua proxy `stdlib/cli/lua/hull/project/frontend_javascript.lua`** - implements the frontend
CONTRACT (same accessor surface as `frontend_lua`) by delegating to the C bridge; it NEVER parses
JS or touches QuickJS. `registry.load` returns it for a `javascript` row.

- `capabilities = { "declarations", "annotations", "source_ranges", "scope", "semantics" }`.
- `analyze_source(session, path, src) -> (unit, decls, diags)` - calls
  `tool.frontend_analyze("javascript", session, path, src)`, decodes the facts, and builds the JS
  `unit` object (`_session`/`_unit_id`/`_declarations`) + one JS `declaration` object per fact
  (`_session`/`_decl_id` + the normalized accessor fields). Used by `analyze_one`.
- `decl_name/decl_kind/decl_range/decl_group_range/decl_annotations/decl_is_method(d)` - read the
  normalized fields off the JS `declaration` object (same accessor contract as `frontend_lua`).
- `declaration_semantics(declaration)` - `tool.frontend_declaration_semantics("javascript",
  declaration._session, declaration._decl_id)`, decoded; `(record, nil)` or `(nil, diagnostic)`.
  Never raises.
- `scope(unit)` - `tool.frontend_scope("javascript", unit._session, unit._unit_id)`, decoded.

**`analyze_one(language, path, src, ctx) -> per_source`** added to `analyze.lua`, replacing the
inline per-file branch. `ctx` carries the generation's JS session token (opened lazily), the
handle table, and the lazy-open failure latch (section 8).

- `language == "lua"`: as today - `fe = frontend_lua`; `unit, diags = fe.parse(src, path)`;
  `collect_decls(fe, unit, ...)` via `fe.declarations` + `decl_*`; handle payload
  `{ frontend = fe, unit = unit, declaration = d }`.
- `language == "javascript"`: ensure the JS session is open (lazily, once); `fe = js_proxy`;
  `unit, decls, diags = fe.analyze_source(ctx.js_token, path, src)`; build facts via the SAME
  `fe.decl_*` accessors over each `declaration` object; handle payload
  `{ frontend = fe, unit = unit, declaration = d }` - IDENTICAL shape to Lua. The bridge-private
  `unit_id`/`decl_id` live INSIDE the `unit`/`declaration` objects, never on the ProjectDiscovery.

`collect_decls` stays unchanged in structure (it consumes `fe` + `unit` + the `declaration`
objects through the accessors). The two handle layers stay separate: the bridge-private ids live
only inside the retained handle objects; the public declaration carries the model `handle`; the
projection drops `_handles` (D6).

## 6. Registry activation

The JS rows gain an `engine` field that `analyze_one` dispatches on, and their analyzability is
CONDITIONAL on the tooling engine being compiled in:

```
lua = { language = "lua",        engine = "lua",        frontend_module = "hull.project.frontend_lua",        analyzable = true }
js  = { language = "javascript", engine = "javascript", frontend_module = "hull.project.frontend_javascript" }
mjs = { ... same as js ... }
cjs = { ... same as js ... }
```

`engine` distinguishes HOW `analyze_one` reaches the frontend (in-process Lua vs the C bridge),
keeping the registry the single selection point. `registry.load` returns `frontend_lua` /
`frontend_javascript` respectively.

**Availability gate (Slice 1 is build-gated).** The JS tooling engine (the QuickJS session +
`frontend/js_session.c` + the bundled cli-js) is not linked in every hull (e.g. a Lua-only
build). So a JS row is NOT unconditionally analyzable. `registry.for_ext` computes a JS row's
`analyzable` at query time as `tool.frontend_available("javascript") == true` (the binding is
always present and returns false in a JS-less build). When available, a `.js` app source is
analyzed; when NOT, JavaScript stays exactly as before - a KNOWN-but-unanalyzable language,
honestly reported `project.frontend.unsupported`, never parsed as Lua. `registry.frontends()`
reports JavaScript's `analyzable` flag + capabilities the same way. `known_exts` is unchanged.

## 7. Explicit teardown (per generation)

The analyzer OWNS open + close of the one JS session; consumers NEVER call `tool.frontend_close`
or touch a token. `retain_frontend` is normalized to a STRICT boolean (`opts.retain_frontend ==
true`; anything else is false).

- OPEN lazily on the first JS file (section 5), so a Lua-only project opens no session.
- DEFAULT (inspection) mode: in `analyze_unprotected`, AFTER `model.build` and BEFORE returning
  the discovery, if a JS session was opened, close it (`tool.frontend_close("javascript",
  ctx.js_token)`). Projection does NOT require a live session - the facts are already extracted -
  so the returned discovery's JS handles are intentionally DEAD/unresolvable, and the projection
  drops `_handles` anyway.
- RETAINED (`retain_frontend = true`) mode: the live session LEASE is attached to the discovery
  INTERNALLY (`disc._frontend_lease = { js_token = ctx.js_token }`, generation-internal, never
  serialized). The analyzer exposes ONE idempotent close operation:

  ```
  analyze.close(disc)   -- closes disc._frontend_lease's session if any; safe to call twice.
  ```

  A consumer that used semantics/scope calls `analyze.close(disc)` when finished. It is the ONLY
  public way to release the lease; `analyze.close` on a discovery with no lease, or a second call,
  is a no-op (double-close safe). Consumers must not reach for a token.
- FAILURE: the PUBLIC protected boundary (`M.analyze`) closes the session on EVERY failure path -
  a `pcall`-guarded cleanup that closes `ctx.js_token` if it was opened and no lease was handed
  out (or, in retain mode, still closes on a mid-analysis fault so a crash never leaks a live
  QuickJS session). `analyze.close` remains valid and idempotent afterward.

## 8. Transport validation (reaffirmed at the boundary)

Two independent gates, both fail-closed:

- The MANAGER validates the `token` is live before touching a session (section 2); a stale token
  never reaches QuickJS.
- The SESSION validates the crossing (Slice 1): source in as a length-exact ArrayBuffer (embedded
  NUL preserved, never a NUL-terminated C string); options in as length-aware JSON, fail-closed on
  malformed / NUL-bearing / trailing-garbage input; the result MUST be JSON; a resource breach is
  host-classified `js.limit.*`; the entry never raises.

**Operation-specific stale/error shapes.** A stale token, an unavailable engine, or a bridge
error is returned in the shape the OPERATION already uses, so the proxy/model treat it uniformly:

- `analyze`               -> facts-shaped `{ schema_version, status:"error", unit_id:-1,
                             declarations:[], diagnostics:[ Diagnostic ] }`.
- `declarationSemantics`  -> `{ error: Diagnostic }`.
- `scope`                 -> `{ ok:false, error: Diagnostic }`.

where `Diagnostic = { severity:"error", code:"javascript.internal", message, range:null }`. The
Lua proxy decodes the returned JSON with `hull.json` inside a protected boundary; a decode failure
becomes the same `javascript.internal` diagnostic on the frontend contract, never a raised error.

**Lazy-open failure is LATCHED once per generation.** If opening the JS session fails (engine
unavailable or `open` returned 0), the analyzer records the failure ONCE in `ctx`
(`ctx.js_open_failed = true` + the reason) and does NOT retry `open` for every subsequent JS file.
Each JS source in that generation is then reported with an honest per-source diagnostic
(`status:"error"`, `code:"javascript.internal"` / `project.frontend.unsupported` as appropriate),
so the discovery is honestly incomplete rather than silently empty or repeatedly retrying.

## 9. The stale-token ABA test (the Slice-5 deferral, now testable)

With the manager + tuple in place, the ABA case is tested directly in C:

1. `tokenA = hl_js_gen_open()`; analyze a file in A -> `decl_id 1` retained under A.
2. `hl_js_gen_close(tokenA)` - A's session destroyed.
3. `tokenB = hl_js_gen_open()` - a NEW session; it may reuse A's heap slot but gets a strictly
   higher token; analyze a file in B -> `decl_id 1` retained under B (session-relative counter).
4. `hl_js_gen_declaration_semantics(tokenA, 1)` - the OLD token + `1` - MUST be rejected as stale
   (tokenA is dead), NOT resolved against B's `decl_id 1`.
5. `hl_js_gen_declaration_semantics(tokenB, 1)` - the live token - resolves B's declaration.

So the full `{ session_token, unit_id, decl_id }` tuple is ABA-safe: the monotonic token
distinguishes generations even under slot reuse.

## 10. Testing plan

- **manager (C unit)**: open issues monotonic POSITIVE int64 tokens; analyze/semantics/scope on a
  live token work; close destroys the session; a stale/unknown/closed/`<=0` token is rejected
  deterministically (no dereference of freed state) with the OPERATION-SPECIFIC stale shape
  (section 8); the concurrent-generation cap fails closed; shutdown destroys stragglers.
- **stale-token ABA (section 9)**: old token + reused decl_id is rejected; the live token
  resolves.
- **dispatcher binding + availability**: `tool.frontend_*` marshal correctly;
  `tool.frontend_available("javascript")` is true in this build (false in a JS-less build - the
  registry gate); an unknown language errors; a stale token yields the operation-specific stale
  shape, not a crash.
- **neutral handle shape**: `_handles` payload is `{ frontend, unit, declaration }` for BOTH
  languages; `resolve_handle` has identical structure; the JS `unit`/`declaration` objects carry
  `_session`/`_unit_id`/`_decl_id` internally; `fe.declaration_semantics(declaration)` /
  `fe.scope(unit)` work verbatim.
- **Lua proxy**: `declaration_semantics` / `scope` decode the bridge JSON; a bridge error shape
  (per-operation) becomes a diagnostic; capabilities advertised.
- **registry activation**: `for_ext("js")` is analyzable with engine "javascript" WHEN available;
  `registry.frontends()` reports JavaScript analyzable with the proxy capabilities; a `.js` file
  is analyzed, not unsupported.
- **retained-generation ownership**: a DEFAULT discovery returns dead/unresolvable JS handles
  (semantics/scope through resolve_handle report stale); a RETAINED (`retain_frontend=true`)
  discovery resolves semantics + scope; `analyze.close(disc)` then invalidates them; a SECOND
  `analyze.close(disc)` is a safe no-op; a forced mid-analysis failure still closes the session
  (no leak); `retain_frontend` is a strict boolean.
- **lazy-open failure latch**: with the engine forced unavailable, a project with several JS files
  reports each honestly (`status:"error"`/unsupported) and opens (attempts) at most once.
- **analyze_one integration (the plumbing end to end)**: analyze a directory with a `.js` file
  through `hull.project.analyze` -> a ProjectDiscovery whose `sources` has the JS file
  `status:"analyzed"`, whose `declarations` carry the JS facts with model handles, and whose
  projection contains NO `_handles` / `decl_id` / `unit_id` / `session_token`; the JS session is
  opened once and closed at generation end (no leak).
- **public-projection proof**: the serialized projection (public JSON) never contains a
  bridge-private id (`decl_id`, `unit_id`, `session_token`) or any AST/handle.

All under ASan + UBSan; ASCII-only; no em-dashes.

## 11. Non-goals

No end-to-end mixed-language discovery through `hull dev` / `hull dev --agent` / `hull agent
inspect`, no generation publish/read sidecar lifecycle, no live-vs-standalone parity, and no full
security-boundary test (all Slice 7). No lowering (Query/Compute). No new JS language features. No
second ProjectDiscovery model. No execution of application JS.
