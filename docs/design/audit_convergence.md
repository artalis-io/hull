# Design: Audit convergence - invariant ratchets and structural bounds

- **Status:** Accepted (phase 1 in progress)
- **Owner:** TBD
- **Related:** CLAUDE.md "Capability Enforcement Invariants", "The charging rule (audits 37-39)",
  "Run wall-clock watchdog"; audit rounds 1-41; docs/lua_patches.md, docs/quickjs_patches.md

## 1. Context

Forty-one audit rounds have hardened Hull area by area. Rounds 39-41 show two trends:

- **Converging areas** produce no High/Medium findings for several rounds in a row: core and
  tooling (serve, sandbox, signatures, build, deploy, agent/MCP); the stdlib auth, session,
  CSRF, JWT and OAuth modules; the Lua coroutine and transaction machinery; and memory safety
  and capability bypass across the C layer.
- **Non-converging classes** produce new Mediums every round:
  1. *Uncharged work* - a binding or cap path that does syscalls, network round trips or
     super-linear work without charging the run budget (kv, log, SQL text, mime.sniff, ws/sse
     frames, compute.stream output, WASM log).
  2. *Engine super-linearity* - one vendored builtin call doing quadratic work the watchdog
     cannot interrupt (atom hash flooding, normalize, regexp compile, Lua ephemerons).
  3. *MySQL session state* - Hull inferring server session state from SQL text and status flags.
  4. *Planted database files* - open-time blocklist checks on SQLite and DuckDB schemas.
  5. *GPU compile time* - lexical estimates of a black-box shader compiler's cost.
  6. *Fix regressions* - about two Mediums per round in the previous round's own fixes.

Per-call fixes for these classes do not converge: every new call, engine path or object kind is
a new gap by default.

## 2. Goals

- **G1.** A change that breaks an established invariant fails CI before review.
- **G2.** No finding from classes 1-5 is reachable by an unauthenticated remote client beyond a
  bounded, configurable per-request cost.
- **G3.** Classes 3-5 are removed by construction, so they no longer need to be tracked.
- **G4.** Fix regressions (class 6) drop to near zero per round.
- **G5.** Severity is rated against an explicit threat model, so the audit bar is stable.

## 3. Non-goals

- Making every engine builtin linear-time. This design bounds their cost instead.
- Hard real-time guarantees for app.main / CLI runs.
- Defending an app against its own author beyond resource bounds.

## 4. Threat model and severity rubric

Three attacker classes, rated separately:

| Attacker | Capabilities | High / Medium when | Otherwise |
|---|---|---|---|
| **A1. Unauthenticated remote client** | Sends arbitrary requests to a deployed app | It bypasses a capability, reads `_hull_*` / other users' data, executes code, or exceeds the per-request bound in §6.2 through ordinary app code | Low |
| **A2. Malicious repo / planted file** | Controls files in a cloned app tree or the deploy directory | It gains code execution, data exposure, or a weakened sandbox / signature check | Low (slow-downs, crashes of the local tool) |
| **A3. App author** | Writes the app's own Lua/JS/SQL | It escapes the sandbox or capability layer, or reads `_hull_*` data | Low (self-inflicted resource use bounded by the run deadline) |

Every audit report states which attacker reaches each finding. A finding reachable only by A3,
and bounded by the deadline, is Low by definition.

## 5. Part A - Invariant ratchets for converged areas

### 5.1 Regression test per finding (mandatory)

Every audit fix ships a test that fails on the pre-fix code, named `<area>_audit<N>.<case>`.
The fix PR template requires the reviewer to confirm the test failed before the fix
(a CI job verifies this by reverting the non-test diff and expecting a runtime failure;
a build failure counts only with the PR's headers kept and declared stubs linked, or under an
explicit `REGRESSION-GATE: build-only <reason>` opt-out - audit 43).

### 5.2 Invariant lint (`make check-invariants`, run in CI)

Each rule is a grep or a small libclang / tree-sitter query, with an allowlist file for
justified exceptions. Initial rules:

| ID | Rule |
|---|---|
| INV-1 | Every exported Lua/JS binding charges the run budget, through the central dispatcher (§6.1) or an explicit `HL_CHARGE_*` call before its first syscall |
| INV-2 | No `sqlite3_*` call outside `src/hull/cap/` (and its allowlisted agent readers) |
| INV-3 | Every JS operation that parks calls `hl_js_db_refuse_wait` before arming |
| INV-4 | Every JS entry ends through `hl_js_entry_end_txn_check` |
| INV-5 | Every disk loader (module, template, static, migration, shader, compute, entry) checks `hl_vfs_embedded` |
| INV-6 | All terminal, log and agent / MCP output goes through the shared escaper (C0, DEL, C1) |
| INV-7 | No `JS_RunGC` outside `hl_js_gc_for_retry` |
| INV-8 | Every new SQLite open registers with `hl_db_live_file_add` before reading |
| INV-9 | No new `lua_closethread(…, NULL)` / `lua_resume` without the resumer |
| INV-10 | No new vendored-engine change without a `HULL PATCH NNNN` marker and a docs/*_patches.md entry |

Rules come from the "a new X must Y" statements already in CLAUDE.md; any new such statement
must add a lint rule in the same PR.

### 5.3 Continuous fuzzing

A nightly job runs each libFuzzer target for at least 30 minutes, keeping a corpus between runs
(sql_kw, dsn parsers, pg/mysql wire, wgsl lexer, http_guard, log_escape, mime_sniff,
host_match, migrate_name, plus new targets for the stored-object classifiers). Crashes open
issues automatically.

### 5.4 Frozen-area review

PRs touching a converged area (CODEOWNERS paths) need a review against that area's invariant
checklist (docs/invariants/<area>.md) before merge.

### 5.5 Audit cadence

Converged areas get a diff-only review each round and a full sweep every fourth round.
An area leaves "converged" status after one High/Medium finding and re-enters after three clean
rounds.

## 6. Part B - Structural bounds for remote cost (classes 1 and 2)

### 6.1 Measured-time charging in the binding dispatcher

**Design.** All runtime bindings are invoked through one dispatcher (Lua: a C closure wrapper
registered by `luaL_setfuncs` replacement; JS: a `JSCFunction` trampoline). It reads
`CLOCK_MONOTONIC` before and after the call and charges
`max(declared_floor, elapsed_us × HL_WORK_UNITS_PER_US)` to the run budget, with
`HL_WORK_UNITS_PER_US = 64`.

- Explicit per-binding charges stay; the dispatcher charges only the excess of measured time
  over what the binding already charged (tracked in a per-run counter).
- Charging happens after the call, so the dispatcher also polls the budget/deadline before the
  call and raises the uncatchable trip if already exhausted.
- Clock cost: about 20-40 ns per call (vDSO); negligible relative to binding cost.

**Effect.** A missing floor can no longer make a call free; the worst case is one call's
duration before the trip lands.

### 6.2 Per-request wall budget

HTTP handler runs default to a 5 s wall deadline (`HL_RUN_HTTP_DEFAULT_MS`), configurable per
app and per route. app.main keeps 10 min; timers and ws callbacks 5 s. The existing watchdog
enforces it.

### 6.3 Preemption of long native calls

**Phase 1.** Run each app VM's request handlers on a dedicated VM thread, with the event loop
thread doing only I/O. The watchdog can then mark a stuck run as abandoned: the request is
answered 503, new requests are queued or refused, and the VM thread is recycled once the
native call returns, or the process restarts after a hard ceiling. One slow builtin degrades
one request, not the server.

**Phase 2 (optional).** A pool of VM threads per app for concurrency.

### 6.4 Concurrency and input caps (defaults on)

- Per-client-IP and per-route concurrency limits, with a default of 8 concurrent requests
  per IP.
- Input caps applied before engines see request data: body size, JSON nesting depth and key
  count (enforced in the JSON parser entry for request bodies), regex source length when the
  pattern comes from a request, and `normalize` / `localeCompare` input length.

### 6.5 Engine complexity tests

A CI job runs each listed builtin (string, regexp, JSON, array, typed array, Map/Set, Lua
table/string/GC operations) at sizes n and 2n on worst-case-shaped ordinary inputs and asserts
`t(2n)/t(n) < 2.5`. New QuickJS/Lua upgrades must pass it.

## 7. Part C - Remove classes by construction (classes 3-5)

### 7.1 MySQL / Postgres session state

- App SQL refuses session-altering statements: `SET [SESSION] autocommit`, `LOCK TABLES`,
  `UNLOCK TABLES`, `XA *`, `GET_LOCK` / `RELEASE_LOCK`, `FLUSH ... LOCK` / `FOR EXPORT`,
  backup locks, `LOCK INSTANCE`; on Postgres, session-level advisory locks and `SET ROLE` /
  `SET SESSION AUTHORIZATION`. Transactions use `db.batch` or `BEGIN` / `COMMIT` only.
- Escape hatch: `databases.named.<x>.session_control = true` opts a dedicated (non-shared)
  connection back in, documented as app-author responsibility.
- Every entry boundary issues `COM_RESET_CONNECTION` (or `DISCARD ALL`) on connections the
  entry used, unconditionally.
- Result: the trackers in `db_mysql.c` / `db_postgres.c` and their text classifiers are
  deleted.

### 7.2 Internal data separation

- `databases.internal` defaults to a Hull-owned SQLite file at `<app>/.hull/internal.db`
  (inside the reserved directory, unreachable by app fs grants), created and migrated by Hull.
- App database files never contain `_hull_*` tables, so a planted or cloned app database has
  no internal data to reach; the open-time stored-object checks become defence in depth.
- Migration: on first start with an existing `_hull_*` set in the app database, Hull copies the
  tables to the internal file and logs a one-time notice; `--keep-internal-in-app-db` keeps the
  old layout for one release.
- DuckDB / Postgres / MySQL default connections follow the same rule: internal tables live in
  the internal database, never the app connection.

### 7.3 GPU shader source

- In serve mode, `gpu.compile` / `gpu.load` accept only shaders embedded at build time
  (`shaders/` in the app VFS, pre-validated and pre-compiled where the backend allows).
- Runtime compilation of dynamic source requires `manifest.gpu.dynamic_shaders = true` and runs
  in a helper process with a hard kill at the compile deadline.
- Result: the lexical size / depth / expansion estimators become a fast pre-filter, not the
  security boundary.
- **Status: implemented.** Embedded-only compiles and the opt-in landed in #978
  (`audit_convergence_gpu_shaders.md`). The helper process is described in
  `gpu_compile_helper.md`: a supervisor spawned before the sandbox, a fresh sandboxed worker per
  compile killed at its deadline, and a verdict the server acts on before its own compile.
  `gpu.compile` stays synchronous; the wait is bounded by the deadlines and the run, and an
  async compile is a follow-up.

## 8. Part D - Fix-regression controls (class 6)

- Each audit fix PR gets an independent review pass that checks for regressions, false
  refusals of ordinary use, flavor builds and docs drift before merge.
- Fix batches are capped (for example, at most 5 PRs merged per round before CI on main is
  green); vendored-engine patches and protocol-state changes land alone.
- Feature freeze between an audit round's start and the merge of its fixes.

## 9. Rollout

| Phase | Contents | Exit criterion |
|---|---|---|
| 1 | §4 rubric, §5.1 tests, §5.2 lint (INV-1..10), §8 controls | Lint green on main; rubric used in next audit |
| 2 | §6.1 dispatcher charging, §6.2 HTTP deadline, §6.4 caps | No uncharged-work finding reachable by A1 in the next round |
| 3 | §7.1 session-statement refusal + reset, §7.3 build-time shaders | MySQL / GPU findings stop |
| 4 | §7.2 internal database separation with migration | Planted-file findings drop to defence-in-depth Lows |
| 5 | §6.3 VM thread / preemption, §6.5 complexity tests, §5.3 nightly fuzzing | Engine super-linearity bounded to one request |

## 10. Testing

- Lint rules have positive and negative fixtures.
- The dispatcher has unit tests proving that an uncharged slow binding still trips the budget
  and that its overhead stays under a fixed threshold.
- The HTTP deadline and concurrency caps get e2e tests with a slow handler and parallel clients,
  checking that `/ping` latency stays bounded.
- Session-statement refusals get e2e tests on MySQL 8.0/8.4 and MariaDB, plus the opt-in path.
- Internal database separation gets migration e2e tests (fresh app, existing app, rollback flag)
  for SQLite, Postgres and MySQL.
- Shader policy gets e2e tests for embedded-only acceptance, refusal of dynamic source, and the
  opt-in helper process killed at the deadline.

## 11. Risks and trade-offs

- **Measured-time charging** depends on wall time, so it varies under load; mitigated by
  charging only the excess over explicit charges and keeping explicit floors for
  determinism-sensitive tests.
- **A shorter HTTP deadline** may break slow legitimate handlers; it is configurable per route,
  documented, and rejected requests include a clear error.
- **Session-statement refusal** breaks apps relying on session state; the escape hatch and
  release notes cover it.
- **Moving internal tables** is a data migration; it is staged, reversible for one release, and
  backed by e2e tests.
- **VM threading (§6.3)** is the largest change and touches every runtime entry; it is
  scheduled last and can ship behind a flag.

## 12. Open questions

1. Should the dispatcher charge measured time in worker.dispatch and app.main runs too, or only
   in HTTP / timer / ws entries?
2. What is the right default HTTP deadline: 5 s, or 10 s?
3. Should DuckDB connections be confined to read-only access when they share a file with
   internal data, if any such configuration remains?
4. Do we keep `hull.ssh` / `hull.kv` network calls on the event loop once VM threading lands?
