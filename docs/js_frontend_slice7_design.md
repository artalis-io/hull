<!--
SPDX-License-Identifier: AGPL-3.0-or-later
-->
# JS source-frontend Slice 7 - end-to-end mixed-language discovery + security boundary

Status: design (checkpoint approved with four amendments, folded in below).
Predecessors: Slices 1-6 (restricted QuickJS tooling runtime; byte lexer; parser;
JSDoc annotations; scope/binding resolver; frontend adapter + declaration_semantics;
C-owned generation/session manager). Design records: `docs/js_frontend_slice*.md`,
`docs/project_discovery_design.md`.

## 1. Purpose and scope

Slice 7 proves the COMPLETE ProjectDiscovery lifecycle carries a mixed Lua+JS
project correctly, on both a JS-analyzable build and a JS-less build, and locks
the security boundary of the JS tooling frontend. It is a wiring-verification,
lifecycle-completion, and boundary-test slice.

The lifecycle under test (already built across earlier work; Slice 7 verifies it
end to end with JS in the mix, and completes one ownership wire):

```
hull dev --agent  (supervisor)
  per (re)spawn: generation++ ; write .hull/dev.json {port,pid,session_pid}
  spawn child:   hull agent inspect <app> --generation=N --session-pid=P
                   -> analyze (mixed Lua+JS) -> project (neutral) -> atomic
                      write .hull/discovery.json  (tagged source=dev, gen, session_pid)
hull agent inspect <app>
  live fast path (C): discovery.json + dev.json session_pid match AND supervisor
                      alive (kill(pid,0)) AND document structurally valid
                      -> stream the live generation
  else: tool VM standalone analysis (source=standalone)
```

### 1.1 Non-scope (hard guardrail)

Slice 7 adds ZERO parser productions, ZERO lowering / IR, ZERO new semantic APIs,
and NEVER executes application code. No changes to the registry, analyzer, or
projection SEMANTICS. The only production code change is one integration wire
(section 3, manager shutdown into tool-VM teardown). Everything else is tests.

## 2. Contracts locked by this slice

- C1 - Neutral, handle-free wire. The published and standalone generations are
  frontend-neutral and handle-free. Semantic lowering
  (`analyze.declaration_semantics` / `analyze.scope`) is an in-process,
  retained-discovery-only capability that NEVER crosses the publish/read
  boundary. Both `hull.project.inspect` (standalone) and `hull.project.publish`
  analyze with `retain_frontend = false`, so the analyzer's finally-close always
  runs and the emitted lease is `{ retained=false, open=false, sessions={} }`.
  Slice 7 asserts this on the wire; it introduces no retain toggle.

- C2 - Session ownership bounded by the child. A publish/standalone process owns
  its JS generation session for exactly one analysis. It is torn down before the
  process returns to idle by two mechanisms working together: the analyzer's
  per-generation finally-close (`tool.frontend_close`), and an UNCONDITIONAL
  `hl_js_gen_shutdown()` wired into tool-VM teardown (section 3). No JS session
  outlives the tool VM; no session survives across the child's exit.

- C3 - Two distinct, separately gated security claims:
  - C3a - Application source is never executed. Application `.js` is read as
    bytes and parsed; its top level is never run. Proven by a fixture whose top
    level would throw / produce an observable effect IF executed; discovery still
    completes and reports the file analyzed, with no such effect.
  - C3b - Tooling JS has minimal authority. Reasserted THROUGH the
    generation-manager path via a TEST-ONLY probe (not a production frontend
    method, not a public API): in a manager session, `eval` and `Function` are
    unavailable; there is no `db`, `fs`, `http`, `env`, `crypto`, `process`, or
    application global; and no application module can be imported by tooling
    code. These are two independent claims (source-not-executed vs
    tooling-has-minimal-authority) and are gated by two independent tests.

- C4 - Availability is an honest build property, surfaced identically on every
  path. `HL_FRONTEND_JS` on -> JS analyzed end to end (standalone, publish,
  live-read). Off -> JS `unsupported` end to end. No path silently downgrades or
  misparses `.js` as Lua. CI pins BOTH sides (section 5).

- C5 - Generation identity + liveness (re-proven with JS present; no new
  mechanism). A generation is bound to its dev supervisor `session_pid`. A reader
  accepts a live generation only when `discovery.json` and `dev.json` session_pids
  match, the supervisor is alive, and the document is structurally valid; else it
  falls back to standalone. A reload supersedes the prior generation (monotonic
  counter). Slice 7 re-proves this with a `.js` in the fixture.

## 3. The one production wire: manager shutdown into tool-VM teardown

`hl_js_gen_shutdown()` (Slice 6) is lifecycle completion for the manager, not an
unrelated fix. Slice 7 wires it UNCONDITIONALLY into the tool VM's teardown
(`src/hull/tool.c`, alongside `hl_lua_free`), guarded by `#ifdef HL_FRONTEND_JS`
so a JS-less build compiles without the symbol. Ordering: shutdown the JS
generation manager AFTER the Lua tool VM has finished (the Lua side is the only
caller of `hl_js_gen_open`/`_close`), so no live proxy references remain.

Proven by unit tests (extend `tests/hull/frontend/test_js_generation.c`):
1. Normal default analysis (open -> analyze -> close) leaves zero live sessions.
2. A retained-but-unclosed session is defensively destroyed by
   `hl_js_gen_shutdown()` (open without a matching close, then shutdown, then
   assert zero live and no leak).
3. `hl_js_gen_shutdown()` does NOT reset `next_token` (a subsequent `open`
   returns a token strictly greater than any pre-shutdown token). This
   reasserts the Slice 6 invariant across the shutdown boundary.
4. ASan sees no leak across the shutdown path (Linux CI is the backstop; the
   deterministic live-count assertion is the primary gate, mirroring the
   shared-heap lifecycle testing rule).

## 4. Test-only authority probe (C3b)

A test-only C entry `hl_js_gen_probe(int64_t token, const uint8_t *probe_src,
size_t len, char **out_json, size_t *out_len)`, compiled ONLY under
`HL_JS_GEN_TESTING` (set in the test target's CFLAGS; absent from every shipped
build), runs a Hull-authored probe module (bytecode-precompiled, never `eval`)
in the manager session bound to `token` and returns a JSON report of the
authority surface. Production builds do not compile it, so it cannot widen the
attack surface. The probe module is Hull tooling source, not application code;
it enumerates:

- `typeof eval`, `typeof Function` (expect `"undefined"`);
- presence of `db`, `fs`, `http`, `env`, `crypto`, `process`, and a
  representative application global (expect all absent);
- the result of attempting to import a fake application module (expect a
  failure, not a load).

The test asserts every authority is absent and the import fails. This routes the
claim through the real manager session the frontend uses, per the amendment,
without exposing any probe surface in production.

## 5. Test plan

### 5.1 Isolated, freshly-built binaries (no stale-build recurrence)

To prevent the stale-build/hull failure class fixed in #365, Slice 7's e2e builds
TWO isolated binaries with verified clean rebuilds, and never reuses a stale
`build/hull`:
- `hull-full` from a clean `RUNTIME=all` build (JS analyzable, `HL_FRONTEND_JS`).
- `hull-lua` from a clean `RUNTIME=lua` build (JS-less).
Each leg names the binary it uses explicitly. A verified rebuild (assert the
binary's mtime is newer than the sources, or build into a per-leg temp prefix)
precedes each leg.

### 5.2 e2e (extend `tests/e2e_project_discovery.sh`)

Mixed-language dev generation (hull-full):
- Fixture: `app.lua` + application `client.js`, both annotated.
- Drive `hull dev --agent`; assert the live generation reports both languages
  `analyzable=true`, both sources `analyzed`, `complete=true`.
- `hull agent inspect` live-reads it (source=dev).
- A `.js` edit bumps the generation (monotonic; C5).

Live-vs-standalone SEMANTIC equivalence (hull-full):
- After the live read, STOP the dev supervisor and remove/invalidate the
  sidecars so the C live fast path cannot fire (liveness fails), then run
  `hull agent inspect` (standalone).
- Compare NORMALIZED semantic fields only: `frontends`, `sources`,
  `declarations` + indexes, `diagnostics`, `valid`, `complete`, `summary`.
  Exclude identity fields that legitimately differ: `source`, `generation`,
  `session_pid`. Byte-identity is explicitly NOT required.

Security boundary (hull-full):
- C3a: `evil.js` whose top level would throw if executed; assert discovery
  completes and the file is `analyzed`, with no throw surfaced (parse, not run).
- C3b: the test-only authority probe (section 4) via the manager path.
- Wire hygiene: recursively collect ALL JSON object KEYS (not substring match,
  since annotation text or paths may legitimately contain `handle`/`ast`) and
  assert the forbidden key set is absent:
  `lease`, `_frontend_lease`, `handle`, `handles`, `_handles`, `session_token`,
  `unit_id`, `decl_id`, `by_id`, `_by_source`, `ast`.

JS-less symmetry (hull-lua):
- The same lifecycle yields JS `unsupported` live and standalone; both languages'
  honest status is reported; `nm hull-lua` shows zero `JS_*` / `hl_js_gen_`.

### 5.3 CI gating (neither branch may silently skip)

- The full-build job builds `hull-full`, asserts
  `frontends[javascript].analyzable == true`, and FAILS if it is not; then runs
  the mixed + equivalence + boundary legs.
- The lua-only job builds `hull-lua`, asserts
  `frontends[javascript].analyzable == false`, and FAILS if it is analyzable;
  then runs the JS-less symmetry legs.
- Both are must-not-skip (mirrors the memory64 / shared-heap must-not-skip
  pattern): a skip is a failure.

## 6. Deliverables

1. This design record (committed first).
2. Production wire: `hl_js_gen_shutdown()` into tool-VM teardown (section 3),
   `#ifdef HL_FRONTEND_JS`.
3. Test-only manager probe `hl_js_gen_probe` under `HL_JS_GEN_TESTING`
   (section 4) + its Hull-authored probe module.
4. Unit tests: extend `tests/hull/frontend/test_js_generation.c` (section 3
   proofs 1-4) and add the authority-probe test (C3b).
5. e2e: extend `tests/e2e_project_discovery.sh` (section 5.2) + the CI legs
   (section 5.3).

No parser, lowering, semantic-API, or registry/analyzer/projection SEMANTIC
change lands in this slice.
