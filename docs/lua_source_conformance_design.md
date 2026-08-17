# Lua source-analysis conformance harness

Status: IMPLEMENTED (`tests/hull/source/test_lua_source.c` +
`stdlib/cli/lua/hull/source/tests/test_conformance.lua`). Companion to
[lua_source_analysis_design.md](lua_source_analysis_design.md). This is step **A**
of the post-build-plan roadmap: validate `hull.source.lua` against real Lua 5.4 as
ground truth, systematically, before any consumer (`hull analyze`, `hull.query`,
`hull.compute`, codegen) is built on the AST.

## 1. Goal

Turn "the tests I hand-wrote pass" into "the parser agrees with real Lua 5.4 across
thousands of files, and its byte ranges are internally consistent." Concretely, catch:

1. **False rejects** — the parser emits a syntax diagnostic on input real Lua
   accepts. This is the highest-value bug class: real code the layer would choke on.
2. **False accepts** — the parser reports clean on input real Lua rejects for a
   *syntactic* reason (a missed syntax error).
3. **Range-integrity bugs** — a node whose half-open range is out of bounds, empty
   for a non-empty construct, or not nested within its parent.
4. **Never-raise breaches** — any input that makes `lua.parse` raise instead of
   returning `(unit, err)`.

## 2. Why `load()` is the oracle (and a good one)

The test harness (`tests/hull/source/test_lua_source.c`) runs co-located Lua scripts
in a **vanilla `lua_State`** built from the **same vendored Lua 5.4** (`LUA_OBJS`)
that Hull ships. That state's `load()` therefore drives the *exact* lexer/parser
Hull targets — not "some Lua," the same one. Properties that make it ideal:

- **Same version, in-process.** No subprocess, no version skew, no external Lua.
- **Compile-only.** `load(src, name, "t")` compiles and returns the function
  **without executing it** (mode `"t"` = text only, so a binary-chunk payload can
  never be loaded, and nothing runs). Pure accept/reject with zero side effects.
- **The module still never calls `load()`.** The locked contract
  ([design §…]) keeps dynamic compilation out of `hull.source.*`; the *test* calls
  `load()`. The corpus and the oracle live in the harness, exactly as reserved.

`load()` returns `(chunk, nil)` on accept, `(nil, errmsg)` on reject.

## 3. The core invariant — and its one-directional exception

`hull.source.lua` is a **syntactic recognizer**. Real Lua's `load()` is syntactic
**plus a thin layer of static-semantic checks** that a pure parser deliberately does
not perform.

**Hull's result is THREE-STATE, not two.** `hull_state(unit)` returns:
- **`reject`** — ≥1 `lua.syntax` / `lua.unsupported` diagnostic.
- **`clean`** — zero syntax diagnostics AND zero `lua.internal` / `lua.limit.*`.
- **`indeterminate`** — any `lua.internal` or `lua.limit.*` marker.

An internal/limit marker is **never** a clean parse and **never** enters semantic
classification (the locked guardrail). In this *required* gate an `indeterminate`
result **fails with a harness diagnostic** (raise the limits, or fix the never-raise
/ internal bug) — it is not silently read as acceptance. So the directional invariant
is over `reject` vs `clean` only:

- **`reject` ⟹ `load()` MUST reject.** A `reject` on load-accepted input is a
  **false-reject bug** (fail). This is the direction we most want to nail down.
- **`clean` USUALLY means `load()` accepts** — but `load()` may reject for a
  **semantic** reason the parser intentionally ignores. Those are *expected
  divergences*. A `load()` reject with a genuinely *syntactic* message on a `clean`
  parse is a **false-accept bug** (fail).

### 3.1 The semantic-exception set (expected `accept` / `load`-reject)

These constructs are syntactically valid (parser is `clean`) but statically rejected
by `load()`. They are pinned as EXPECTED divergences, each matched by a **precise**
Lua 5.4 message fragment (`SEMANTIC_REJECTS`):

| Construct | Example | Lua 5.4 message fragment |
|---|---|---|
| `break` outside a loop | `break` | `break outside loop` |
| `goto` to an undefined label | `goto x` | `no visible label` |
| `goto` into a local's scope | `goto s; local x=1; ::s:: return x` | `jumps into the scope` |
| `...` outside a vararg function | `function f() return ... end` | `cannot use '...' outside a vararg` |
| assign to a `<const>` / `<close>` | `local x <const> = 1; x = 2` | `attempt to assign to const variable` |
| too many locals (compiler limit) | 260 `local`s | `too many local variables` |

**Matching is honest SUBSTRING matching against the NORMALIZED message body** — the
`chunkname:line: ` prefix is stripped first (`normalize`) so a fragment matches the
semantic body, not an accidental chunkname/path — not full anchoring. Each fragment
is a precise vendored-Lua string and is pinned by a direct test (§5).

**Deliberately NOT in the allowlist:**
- **unknown attribute** (`local x <bad> = 1`). Hull validates attribute names itself
  (emits `lua.syntax` for anything but `const`/`close`), so an unknown attribute is a
  Hull **`reject`** that must AGREE with `load()`'s reject. If Hull ever parsed one
  `clean`, that is a **false-accept** and must fail — never be classified away. A
  direct test pins this (Hull `reject` + not classified).
- **broad `too many` / `constant overflow` / `control structure too long`.** The bare
  `too many` fragment could mask unrelated failures; only the precise, directly-
  testable `too many local variables` is kept. Upvalue/constant/control-structure
  limits need pathological inputs a mutation of a small file cannot reach, so they are
  omitted (a real occurrence would surface as a false-accept, correctly).

The set is small, closed, and each entry has a direct test.

**Guardrails (the classifier must not become a catch-all):**
- It applies in **exactly one situation**: Hull parses **cleanly** (0 syntax /
  unsupported diagnostics) **and** `load()` rejects. It is consulted nowhere else.
- It **never** excuses: (a) a Hull `reject` on `load()`-**accepted** input (that is a
  false-reject, always a failure); (b) a curated syntactic-negative mismatch (§5);
  (c) an `indeterminate` result — a `lua.internal` or `lua.limit.*` marker is not a
  `clean` parse, so the classifier is never even reached; an `indeterminate` in this
  gate is its own failure.
- Every allowed divergence is **reported by category and count** in the suite output,
  so an over-broad fragment surfaces as an anomalous tally instead of hiding.
- **Direct classifier tests** accompany the allowlist: for each fragment, a snippet
  that provably triggers its intended semantic error (asserting `load` rejects and
  `classify()` returns that category) **and** a nearby *syntax* error snippet that the
  classifier must **not** match (asserting `classify()` returns nil). This pins that
  each fragment recognizes only its semantic case and does not bleed onto adjacent
  syntax errors.

The `<bad>` attribute case is NOT a divergence: Hull emits its own `lua.syntax`
("unknown attribute…"), so it is a Hull `reject` that agrees with `load()`'s reject.
It is deliberately absent from the allowlist, and a direct test asserts Hull rejects
it (and that `classify()` does not match it) — a `clean` parse of an unknown attribute
would be a false-accept and must fail.

## 4. Input normalization (exactly one)

**Shebang.** Lua's *file* loader (`luaL_loadfilex`) skips a leading `#!…\n`; Lua's
*string* `load()` does not (`#` is the length operator, so `load("#!/x\n…")` is a
syntax error). `hull.source.lexer` follows *file* semantics (a leading `#!` line is a
shebang comment). To compare fairly, the harness strips a leading `#!…\n` (through
the first newline) from the source before handing it to `load()`, mirroring what the
parser does. This is the only normalization; everything else is byte-identical.

Empty input, trailing garbage, and every other case need no special handling: `load`
and the parser already agree (empty → accept; trailing tokens → both reject).

## 5. What the harness checks (three parts)

### Part 1 — Corpus, classified by the oracle (not assumed positive)
Do NOT assume every `.lua` file is valid — `tests/fixtures` and examples may hold
intentionally-broken inputs. The **oracle classifies each file** (after
shebang-normalization for `load()`), and the directional rules from §3 apply
uniformly:
- **`load()`-accepted files** (the bulk of committed code) enforce the anti-
  false-reject gate: `lua.parse` must yield **zero** `lua.syntax` / `lua.unsupported`
  diagnostics. This is the single most valuable check — real, shipping Hull code.
- **`load()`-rejected files** follow the same directional / semantic-divergence rules
  as any other reject: Hull must also reject (≥1 syntax diagnostic), OR the divergence
  is a pinned §3.1 semantic case (Hull clean + a matching fragment). These are
  **counted and reported separately** from the accepted-file tally, so a rejected
  fixture is surfaced, never silently passed.

### Part 2 — Range round-trip integrity
For every `clean` + load-accepted corpus file, walk the AST and assert, for every node:
- `1 <= range.start <= range.stop <= #src + 1` (in-bounds, non-inverted),
- **exact slice**: `unit:text(node) == src:sub(range.start, range.stop - 1)` (the
  round-trip actually holds, not just by construction),
- **non-empty for real constructs**: `range.stop > range.start` for every node
  EXCEPT the empty-source **chunk** root (an empty file's chunk is legitimately
  `[1, 1)`); a zero-width node of any other kind is a bug,
- each **AST child node**'s range is **nested within** its parent's `[start, stop)`.

**Nesting covers AST child nodes ONLY.** Attached annotations
(`node.annotation_list` / `node.annotations`) are *not* AST children and their ranges
intentionally **precede** the declaration they annotate (a `---@` comment sits
*above* the decl), so they are **excluded** from the nesting check — gathering
children skips the `annotation_list` / `annotations` fields (and only kind-bearing
tables are children, which annotation records are not, so `lua.walk` never visits
them anyway). Independent of accept/reject — catches range / `finish()` bugs directly.

### Part 3 — Curated negatives + classifier tests + seeded mutation fuzz
- **Curated syntactic negatives**: syntactically invalid snippets (`"1 +"`,
  `"function f("`, `"a.b."`, `"{,}"`, `"::"`, `"return return"`, …). Assert `load()`
  rejects AND Hull's verdict is `reject`.
- **Pinned semantic divergences**: the §3.1 constructs, asserted as `load()` rejects +
  Hull `clean` + `classify()` returns the expected category — so a behavior shift on
  either side is noticed.
- **Unknown-attribute guardrail**: `local x <bad> = 1` asserted as `load()` rejects +
  Hull `reject` + `classify()` returns nil (it must NOT be classified away).
- **Direct classifier tests** (§3.1 guardrail): each fragment's own good/bad pair.
- **Seeded mutation fuzz** (deterministic): FIXED `math.randomseed`, N small byte
  mutations (delete / duplicate / insert) per selected seed, each mutant run through
  both oracles asserting the §3 directional invariant with the three-state verdict:
  never-raise; `indeterminate` fails; no false-reject (`reject` + load-accept); no
  false-accept (`clean` + a *non-semantic* load-reject). Bounded (N=20/seed, 4000
  cap) so it stays sub-second in `make test`. Continuous/unbounded fuzzing is the §8
  follow-up.

## 6. Corpus enumeration (C-side, hermetic, deterministic)

Enumeration AND file reading happen in **C** (the vanilla `lua_State` has no portable
directory walk in pure Lua, and we will NOT shell out — `io.popen('find')` is
non-hermetic and shaky under the cosmo runner). A `dirent` walk over a fixed set of
repo-relative roots (`stdlib/lua`, `stdlib/cli/lua`, `examples`, `tests/fixtures`).

**C exposes `{path, source}` records, not paths.** C reads each file's bytes and
builds `HULL_LUA_CORPUS = { { path = <rel>, source = <bytes> }, … }`. Rationale
(a correction over the first draft): the Lua side does **no** `io` and does **no**
second filesystem pass, and — critically — the **oracle (`load()`) and the Hull
parser receive the identical bytes** C read (a re-open could race or resolve
differently). The script is pure compute over an injected table.

**Fail-closed (a gate must not silently degrade):**
- **Allocation failures propagate.** `sl_push` latches an `oom` flag on any
  `realloc`/`strdup` failure and **never inserts a NULL** (which would crash the
  sort); the runner aborts the whole leg (`return -1`) rather than test a truncated
  corpus.
- **`strdup` is validated** (a NULL result latches `oom`).
- **Full reads required.** `read_all` returns NULL on open/seek/alloc failure OR a
  short read (`fread` count ≠ file size); an enumerated file that fails to read
  **aborts the leg** — it is not skipped.
- **Counts reported.** The runner prints `enumerated N, read N`; because every
  enumerated file must read fully, `read == enumerated` always holds at success.

**Determinism (required for CI reproducibility):**
- **Regular files only.** Include an entry iff `lstat` reports `S_ISREG` and the name
  ends in `.lua`. No directories, devices, FIFOs.
- **Symlinks: explicit, never followed.** Use `lstat` (not `stat`); a symlink entry —
  file OR directory — is **skipped entirely**. This both fixes symlink handling and
  **avoids traversal loops** (no symlinked directory is ever descended), with a
  defensive recursion-depth cap as a backstop.
- **Sorted.** Collect normalized repo-relative paths and `qsort` them (byte-wise
  `strcmp`) before anything downstream selects from them, so ordering is stable
  across platforms and filesystem iteration order.
- **Deterministic seed scheduling.** When the corpus exceeds the mutation cap (§7),
  seeds are selected by a fixed stride over the sorted list (not by which files the
  FS yielded first), so the fuzzed subset is identical run to run.

The corpus is Hull's own committed Lua — always in sync, never embedded / stale.

## 7. Resource limits, determinism, CI

- Run the corpus with **generous limits** (raised `max_bytes` / `max_tokens` /
  `max_depth`) so `lua.limit.*` never trips on legitimate files. A `lua.limit.*` /
  `lua.internal` marker yields the `indeterminate` verdict (§3), which **fails the
  gate with a harness diagnostic** — it is never read as `clean`. A tripped limit on
  a legit file means "raise the limit"; a `lua.internal` means "fix a never-raise
  breach."
- **Deterministic**: one fixed seed, index-derived mutation choices, bounded budget —
  a CI failure reproduces exactly.
- Runs inside `make test` as a new UTEST leg (fast: low-thousands of parses,
  sub-second). No new external dependency.

## 8. Deliverables + follow-up

**This harness (one PR):**
- `docs/lua_source_conformance_design.md` (this file).
- C, in the existing `tests/hull/source/test_lua_source.c` (one vanilla Lua-state
  harness, one source-analysis target): a `dirent` corpus walk building `{path,
  source}` records + a runner that injects `HULL_LUA_CORPUS`, plus a **separate**
  `UTEST(lua_source, conformance)` leg driving its **own** Lua script — so a
  conformance failure stays isolated and readable, distinct from the lexer/parser/
  statements/annotations legs.
- Lua: `stdlib/cli/lua/hull/source/tests/test_conformance.lua` — the three-part
  differential (oracle-classified corpus, range round-trip over AST children only,
  curated negatives + pinned semantic divergences + seeded mutation fuzz) plus the
  direct classifier tests (§3.1); returns `{ pass, fail }` like the other suites.

**Follow-up (separate, later):** a continuous **libFuzzer never-raise harness**
`tests/fuzz_lua_source.c` (spin a `lua_State`, set `package.path`, feed
`LLVMFuzzerTestOneInput` bytes to `lua.parse`, assert it returns without raising) —
parallels Hull's existing pgwire / mysqlwire fuzzers and runs 60s in CI. The
differential-vs-`load` fuzz stays the seeded, bounded Lua-side loop in Part 3 (a
`load`-comparison inside libFuzzer is heavier and less reproducible).

## 9. Non-goals

- **Not** proving AST *equivalence* to Lua's internal parse tree (not exposed, and
  not needed — we prove accept/reject agreement + range integrity).
- **Not** semantic analysis (scopes, binding, const-checking) — that is a possible
  future slice for consumers that rewrite references, tracked separately.
- **Not** a second Lua implementation — `load()` is the oracle; the harness only
  compares.

## 10. Decisions (ratified)

1. **Semantic-exception classifier** — KEPT, with the §3.1 guardrails: anchored to
   vendored Lua 5.4 text, consulted only when Hull is clean and `load()` rejects,
   never excusing Hull-diagnostics-on-accepted / curated-negative mismatch /
   internal / limit diagnostics, every divergence reported by category + count, and
   direct classifier tests proving each fragment matches its case and not adjacent
   syntax errors.
2. **Harness placement** — extend `tests/hull/source/test_lua_source.c` (one vanilla
   Lua-state harness, one target), with conformance logic in its **own** Lua script
   under its **own** `UTEST` leg for isolated, readable failures.
3. **Mutation budget** — N=20 mutations per selected seed, ~4000-mutant ceiling,
   fixed seed; corpus enumeration deterministic per §6 (regular `.lua` only, symlinks
   skipped / no loops, sorted paths, fixed-stride seed scheduling above the cap).
