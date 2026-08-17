# `hull.source.lua` continuous libFuzzer harness (design)

Status: DESIGN (pre-implementation). The documented follow-up to the differential
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
  comparison — that stays the conformance gate's job (a `load()` call inside a
  high-throughput libFuzzer loop is heavier and less reproducible); the value here is
  millions of coverage-guided mutations under ASan/UBSan.

Together: the gate proves *correctness against ground truth*; the fuzzer proves
*robustness against adversarial input*.

## 2. What it asserts (the intrinsic oracle)

For each input `data[0..size)` treated as Lua source, `lua.parse(data, { limits })` must:
1. **Never raise** — the C-level call returns normally (the Lua `pcall` around
   `M.parse` returns `LUA_OK`); a Lua error escaping `parse()` is a contract breach →
   `abort()` (libFuzzer crash).
2. **Return a valid shape** — either a `unit` table with a `diagnostics` array (and an
   `ast`), or `(nil, err)` with a diagnostic-shaped/`string` `err`. Anything else →
   abort.
3. **Respect its bounds** — run with the DEFAULT limits; the parser's
   `max_bytes`/`max_tokens`/`max_depth` must bound work so no input hangs (libFuzzer
   `-timeout` catches a regression) or OOMs (a memory cap catches unbounded growth).
4. **Structural range sanity** (cheap post-check on a returned unit) — walk the AST and
   assert every node's range is in `[1, #src+1]`, non-inverted, and `unit:text(node)`
   slices without error. A violation → abort. (Reuses the conformance harness's range
   checker logic.)
5. **No sanitizer finding** — ASan (heap/stack overflow, UAF, leaks), UBSan (UB) run the
   whole thing; a finding is a crash libFuzzer records with the reproducer.

Note the parser is pure Lua, so "crash" surfaces as: a Lua **stack overflow** from deep
recursion (parser / `walk`) that the `max_depth` guard must prevent; **non-termination**
(a stall the guards must prevent); a raised error escaping `parse()`; or a memory error
in the vendored Lua VM itself exercised by pathological input.

## 3. Harness structure

A C file `fuzz/fuzz_lua_source.c` in the same mold as the existing `fuzz/fuzz_*.c`
harnesses — but it LINKS `$(LUA_OBJS)` and runs from the repo root (so `package.path`
resolves `stdlib/cli/lua`), the same scaffolding the conformance harness uses. (Unlike
the pure-C codec fuzzers — pgwire, sh_json — this one drives a `lua_State`.)

- **`LLVMFuzzerInitialize`** (once): create a persistent vanilla `lua_State`
  (`luaL_newstate` + `luaL_openlibs`, the same as the conformance harness), set
  `package.path` to `stdlib/cli/lua/?.lua`, `require("hull.source.lua")`, and stash the
  `M.parse` function + a small Lua driver in the registry. Reusing one state across
  inputs is essential for throughput (a fresh state per input would dominate runtime).
- **`LLVMFuzzerTestOneInput(data, size)`**:
  1. push the driver + the input string (`lua_pushlstring`, binary-safe) and `lua_pcall`
     it;
  2. the **Lua driver** does `local u, e = parse(input, LIMITS)`, then the shape +
     range checks (§2.2, §2.4), and returns a status; a check failure is signalled back
     to C (a distinguished return / a raised error) → C `abort()`s with a message;
  3. after the call, reset the stack (`lua_settop`) and periodically
     `lua_gc(L, LUA_GCCOLLECT)` (every N inputs) so accumulated per-parse tables don't
     grow memory unboundedly across the run.
- **Memory cap**: install a Lua allocator (or use Hull's tracking allocator) with a hard
  ceiling so a pathological input that tries to allocate unboundedly fails the parse
  (returned as a diagnostic / `(nil,err)`) rather than OOM-killing the fuzzer — turning
  "unbounded growth" into a catchable contract check rather than an ambiguous OOM.

The Lua driver lives inline in the C file as a string (or a tiny co-located
`fuzz_driver.lua`), keeping the "what to assert" in Lua next to `lua.parse` while C owns
the fuzz loop.

## 4. Corpus + dictionary

- **Seed corpus**: the repo's own `.lua` (the conformance corpus) + the parser/lexer
  test snippets, under `fuzz/corpus/lua_source/`. Seeding from valid Lua gives
  coverage-guided mutation a strong start.
- **Dictionary** (`fuzz/lua_source.dict`): Lua keywords, operators, long-bracket
  forms (`[[`, `]==]`), string escapes (`\u{`, `\x`, `\z`), numeral forms (`0x`, `p`,
  `..`), and `---@` so the mutator reaches annotation + edge lexer paths quickly.

## 5. Build + CI integration

Mirror the existing fuzz targets (`mk/tests.mk`, `FUZZ_CFLAGS`, `FUZZ_TIME ?= 60`):
- **Makefile**: a `fuzz/fuzz_lua_source: fuzz/fuzz_lua_source.c $(LUA_OBJS)` rule built
  with `$(FUZZ_CFLAGS)` (`-fsanitize=fuzzer,address,undefined`, clang) + the Lua include
  path, exactly like `fuzz/fuzz_pgwire` / `fuzz/fuzz_sh_json` but adding `$(LUA_OBJS)`.
  Run from repo root so `package.path` resolves.
- **CI**: add `lua_source` to the fuzz matrix in `.github/workflows/ci.yml` — a
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

- **Not differential vs `load()`** (§1) — the conformance gate owns that.
- **Not fuzzing the C `hl_cap_*` layer** — this targets `hull.source.lua` (the pure-Lua
  parser). The cap layer has its own harnesses.
- **Not a `make test` gate** — it is a continuous CI job (like the other fuzzers), not a
  unit test (the conformance gate is the in-`make test` robustness check).

## 8. Open decisions (for ratification)

1. **Range post-check in the hot loop.** Include the per-node range sanity check on
   every returned unit (§2.4) — a small constant cost that catches range bugs the
   never-raise check alone would miss — vs. never-raise + shape only for max throughput.
   Recommendation: **include it** (cheap, high-value; it's the same checker the gate
   uses).
2. **Memory cap mechanism.** A custom bounded Lua allocator in the harness vs. relying
   on libFuzzer's `-rss_limit_mb`. Recommendation: **a bounded allocator** — it turns
   "unbounded growth" into a clean, catchable `(nil, err)`/diagnostic contract check
   rather than an ambiguous RSS kill, and it exercises the parser's own OOM paths.
3. **Corpus location + size.** Seed from the full repo `.lua` corpus vs. a curated small
   set. Recommendation: **full corpus** (matches the conformance seed; more coverage),
   gitignored generated crash artifacts.
4. **CI time budget.** 60s (matches the other fuzzers) vs. longer. Recommendation:
   **60s** for parity; a nightly longer run is a later add.
