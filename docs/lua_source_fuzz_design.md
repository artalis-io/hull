# `hull.source.lua` continuous libFuzzer harness

Status: IMPLEMENTED (`fuzz/fuzz_lua_source.c`, `fuzz/lua_source.dict`,
`fuzz/corpus_lua_source/`; `make fuzz-lua-source`; CI "Fuzz lua source parser (60s)").
The documented follow-up to the differential
conformance gate ([lua_source_conformance_design.md](lua_source_conformance_design.md)
§8): a continuous **libFuzzer** harness that feeds arbitrary bytes to `lua.parse` and
asserts the parser's INTRINSIC invariants under sanitizers, mirroring Hull's existing
`fuzz_*` harnesses (sh_json, pgwire, mysqlwire, host_match) that each run 60s in CI.

## 1. Goal + relationship to the conformance gate

Two complementary oracles:
- **Conformance gate** (shipped): DIFFERENTIAL vs real Lua 5.4 `load()` over a fixed
  corpus + a bounded seeded mutation fuzz. Answers "does the parser AGREE with Lua on
  accept/reject + ranges?".
- **This harness** (new): INTRINSIC + continuous. Answers "can ANY byte string make the
  parser crash, hang, leak, raise, or violate a structural invariant?". No `load()`
  comparison - that stays the conformance gate's job (a `load()` call inside a
  high-throughput libFuzzer loop is heavier and less reproducible); the value here is
  millions of coverage-guided mutations under ASan/UBSan.

Together: the gate proves *correctness against ground truth*; the fuzzer proves
*robustness against adversarial input*.

## 2. What it asserts (the intrinsic oracle)

For each input `data[0..size)` treated as Lua source, `lua.parse(data, { limits })` must:
1. **Never raise** - the C-level call returns normally (the Lua `pcall` around
   `M.parse` returns `LUA_OK`); a Lua error escaping `parse()` is a contract breach →
   `abort()` (libFuzzer crash).
2. **Return a valid shape** - either a `unit` table with a `diagnostics` array (and an
   `ast`), or `(nil, err)` with a diagnostic-shaped/`string` `err`. Anything else →
   abort.
3. **Respect its bounds** - run with the DEFAULT limits; `max_bytes`/`max_tokens`/
   `max_depth` bound work so no input hangs (libFuzzer `-timeout` catches a regression).
   A per-input **bounded allocator** (§3.1) caps memory: exceeding it raises an EXPECTED
   `LUA_ERRMEM` (classified as resource exhaustion, not a never-raise breach), catching
   unbounded growth without an ambiguous RSS kill.
4. **Structural range sanity** (cheap post-check on a returned unit) - walk the AST and
   assert every node's range is in `[1, #src+1]`, non-inverted, and `unit:text(node)`
   slices without error. A violation → abort. (Reuses the conformance harness's range
   checker logic.)
5. **No sanitizer finding** - ASan (heap/stack overflow, UAF, leaks), UBSan (UB) run the
   whole thing; a finding is a crash libFuzzer records with the reproducer.

Note the parser is pure Lua, so "crash" surfaces as: a Lua **stack overflow** from deep
recursion (parser / `walk`) that the `max_depth` guard must prevent; **non-termination**
(a stall the guards must prevent); a raised error escaping `parse()`; or a memory error
in the vendored Lua VM itself exercised by pathological input.

## 3. Harness structure

A C file `fuzz/fuzz_lua_source.c` in the same mold as the existing `fuzz/fuzz_*.c`
harnesses - but it LINKS `$(LUA_OBJS)` and runs from the repo root (so `package.path`
resolves `stdlib/cli/lua`), the same scaffolding the conformance harness uses. (Unlike
the pure-C codec fuzzers - pgwire, sh_json - this one drives a `lua_State`.)

- **`LLVMFuzzerInitialize`** (once): create a persistent vanilla `lua_State`
  (`luaL_newstate` + `luaL_openlibs`, the same as the conformance harness), set
  `package.path` to `stdlib/cli/lua/?.lua`, `require("hull.source.lua")`, and stash the
  `M.parse` function + a small Lua driver in the registry. Reusing one state across
  inputs is essential for throughput (a fresh state per input would dominate runtime).
- **`LLVMFuzzerTestOneInput(data, size)`** - one input, one bounded parse:
  1. arm the per-input allocator allowance (§3.1): ceiling = `baseline + PER_INPUT_BYTES`;
  2. push the driver + input string (`lua_pushlstring`, binary-safe) and `lua_pcall` it;
  3. **classify the `pcall` result**:
     - `LUA_OK` → run the shape + range checks (§2.2/§2.4) via the driver's return; a
       check FAILURE is the only in-band way to `abort()` (a real robustness bug);
     - `LUA_ERRMEM` (from the bounded allocator) → **expected resource exhaustion, NOT a
       never-raise violation**: the per-input allowance was exceeded. Tolerated and
       counted - because an allocation failure can prevent `parse()` from even
       constructing its normal `(nil, err)` result, so its own `pcall` may re-raise
       `LUA_ERRMEM`. Not a crash.
     - **any other** Lua error (`LUA_ERRRUN`, `LUA_ERRERR`, …) → a raised error escaped
       `parse()` → `abort()` (a never-raise breach).
  4. **always**: reset the stack (`lua_settop(L, base)`) and force a full collection
     (`lua_gc(L, LUA_GCCOLLECT)`) BEFORE the next input, so per-parse garbage never bleeds
     across inputs and each input's allowance is measured from a clean baseline.

### 3.1 The bounded-allocator contract
A custom Lua allocator wrapping Hull's tracking allocator, with a hard ceiling:
- **Baseline**: captured ONCE, right after `LLVMFuzzerInitialize` finishes (state +
  `openlibs` + `require("hull.source.lua")` loaded), so the standing VM footprint is
  never charged against an input. The ceiling is `baseline + PER_INPUT_BYTES`.
- **Per-input allowance** (`PER_INPUT_BYTES`): a fixed budget - comfortably above any
  legitimate parse of a bounded input, low enough to catch runaway growth. Effectively
  reset each input by step 4's forced GC (which returns live memory toward baseline).
- **On exhaustion**: the allocator returns `NULL` → Lua raises `LUA_ERRMEM`, caught by
  the `pcall` and classified **expected** (step 3). This turns "unbounded growth" into a
  clean, catchable resource-exhaustion signal - not an ambiguous RSS kill, and never
  mislabeled a never-raise violation.

The Lua driver lives inline in the C file as a string (or a tiny co-located
`fuzz_driver.lua`), keeping the "what to assert" in Lua next to `lua.parse` while C owns
the fuzz loop + the allocator.

## 4. Corpus + dictionary

- **Checked-in seed (small + curated)**: a SMALL set of `.lua` snippets under
  `fuzz/corpus/lua_source/` covering tricky lexer/parser/annotation edge cases -
  **not** a duplicated snapshot of every repository `.lua` file.
- **Full corpus staged at run time**: the fuzz runner stages the repo's own `.lua` (the
  conformance corpus) DETERMINISTICALLY into a TEMPORARY corpus dir (e.g. copied under
  `build/fuzz-corpus/lua_source/`), unioned with the checked-in seed, and points
  libFuzzer there. The broad coverage seed is thus always fresh + in sync with the tree,
  with zero duplication committed to git.
- **Dictionary** (`fuzz/lua_source.dict`): Lua keywords, operators, long-bracket
  forms (`[[`, `]==]`), string escapes (`\u{`, `\x`, `\z`), numeral forms (`0x`, `p`,
  `..`), and `---@` so the mutator reaches annotation + edge lexer paths quickly.

## 5. Build + CI integration

Mirror the existing fuzz targets (`mk/tests.mk`, `FUZZ_CFLAGS`, `FUZZ_TIME ?= 60`):
- **Makefile**: a `fuzz/fuzz_lua_source: fuzz/fuzz_lua_source.c $(LUA_OBJS)` rule built
  with `$(FUZZ_CFLAGS)` (`-fsanitize=fuzzer,address,undefined`, clang) + the Lua include
  path, exactly like `fuzz/fuzz_pgwire` / `fuzz/fuzz_sh_json` but adding `$(LUA_OBJS)`.
  Run from repo root so `package.path` resolves.
- **CI**: add `lua_source` to the fuzz matrix in `.github/workflows/ci.yml` - a
  `-max_total_time=$(FUZZ_TIME)` (**60s**) run over the seed corpus with a per-input
  `-timeout` for hang detection, on clang/Linux (where the other fuzzers run). A finding
  fails CI with the crashing input attached.
- **Non-blocking on non-clang**: the fuzz job is clang-only (libFuzzer), already gated
  that way for the other harnesses; nothing changes for macOS/cosmo builds.

## 6. Determinism, triage, regressions

- A discovered crash is saved by libFuzzer as a reproducer file; the fix workflow is the
  same as the other fuzzers (add the reproducer to the corpus, fix, re-run).
- Because the harness is intrinsic (no `load()`), a finding is unambiguously a Hull
  parser robustness bug (raise / hang / OOM / OOB / bad shape), not an oracle
  disagreement.
- The seeded mutation fuzz inside the conformance gate stays (bounded, differential,
  in-`make test`); this harness is the UNBOUNDED, coverage-guided, sanitizer-backed
  complement that only runs in the fuzz CI job.

## 7. Non-goals

- **Not differential vs `load()`** (§1) - the conformance gate owns that.
- **Not fuzzing the C `hl_cap_*` layer** - this targets `hull.source.lua` (the pure-Lua
  parser). The cap layer has its own harnesses.
- **Not a `make test` gate** - it is a continuous CI job (like the other fuzzers), not a
  unit test (the conformance gate is the in-`make test` robustness check).

## 8. Decisions (ratified)

1. **Range post-check IN the hot loop** - every returned unit gets the per-node range
   sanity check (§2.4); cheap, high-value, reuses the gate's checker.
2. **Bounded allocator** (over `-rss_limit_mb`), with the §3.1 contract: post-init
   baseline, per-input allowance, forced GC + stack reset between inputs, and
   **`LUA_ERRMEM` classified as EXPECTED resource exhaustion** - any OTHER escaping Lua
   error is a crash (the distinction matters because allocation failure can prevent
   `parse()` from building its normal `(nil, err)`).
3. **Corpus**: a SMALL curated seed set + dictionary checked in; the FULL repo `.lua`
   corpus staged DETERMINISTICALLY into a temporary dir at run time - no duplicated
   snapshot committed to git. Generated crash artifacts are gitignored.
4. **60s CI budget** (`FUZZ_TIME`), matching the other fuzzers; a nightly longer run is
   a later add.
