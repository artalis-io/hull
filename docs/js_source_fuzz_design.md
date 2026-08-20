<!--
SPDX-License-Identifier: AGPL-3.0-or-later
-->
# JS source-frontend Slice A - continuous JS parser libFuzzer harness

Status: design (approved with three final contract corrections, folded in below).
Follows the seven-slice JavaScript source-analysis frontend (merged; commit 1fa98ca8).
Mirrors the Lua-side harness `fuzz/fuzz_lua_source.c` (`docs/lua_source_fuzz_design.md`),
but drives the JS parser through the restricted QuickJS tooling session. Companion:
Slice B (pinned parser-scoped Test262), designed separately after this merges.

## 1. Goal

Continuously fuzz `hull:source:parser`'s `parse(bytes, opts)` with arbitrary bytes and
assert its INTRINSIC robustness contract: it never raises across the session boundary,
never crashes, always yields a well-formed `SourceUnit` (or a host-classified limit), and
its ranges / slices / diagnostics are internally consistent. Complements the differential
conformance gate (correctness vs an oracle) with robustness vs adversarial input.

The parser is `hull:source:parser`:
`parse(bytes: Uint8Array, opts?: {path?, maxTokens?, maxDiagnostics?, maxDepth?}) -> SourceUnit`,
`SourceUnit = { ast, comments, diagnostics, linemap, valid }`, where
`ast = { type:"Program", body:[...], start, stop }`, comments are
`{ kind, start, stop, raw, text }`, annotations (attached to declaration nodes) are
`{ name, args, text, raw, range:{start,stop} }`, and diagnostics are
`{ severity, code, message, range:{start,stop}|null }` with codes `js.syntax` /
`js.unsupported` / `js.internal` / `js.limit.{depth,tokens,diagnostics}`. Ranges are
1-based half-open (`1 <= start <= stop <= n+1`). `parse()` is internally protected (it
contains every catchable exception as a `SourceUnit` carrying a `js.internal` diagnostic).

## 2. Do NOT fuzz through the frontend adapter

`frontend_entry.analyze()` (and the Slice-5 `frontend_javascript` adapter) RETAIN every
analyzed unit + declaration in session-module state so a later `declaration_semantics` /
`scope` call can resolve a handle. A persistent fuzzing session driven through that adapter
would accumulate retained units until it repeatedly hits the heap limit -- turning the
campaign into resource-exhaustion testing, not parser exploration.

Instead the harness drives a **test-only compact entry** that calls `parse()` directly (which
retains nothing), validates the whole `SourceUnit` in JS, and returns a tiny verdict.

## 3. The test-only compact entry: `hull:source:tests:fuzz_parse`

Lives at `stdlib/cli/js/hull/source/tests/fuzz_parse.js`. Under `.../source/tests/`, so the
production cli-js registry generator (which prunes any tests directory) never embeds it; it
is reachable only via the test/fuzz registry (`STDLIB_JS_CLI_TEST_REGISTRY_O`, the same
mechanism the Slice-7 authority probe uses). It registers
`globalThis.__hull_frontend = { fuzz }` matching the session call convention
`(ArrayBuffer src, path, opts)`.

`fuzz(srcBuf, path, opts)`:
1. `const u = parse(new Uint8Array(srcBuf), { path, maxDiagnostics: opts.maxDiagnostics })`;
2. validate the complete `SourceUnit` INTERNALLY (section 4);
3. retain NOTHING (no reference to `u`, its `ast`, or any node survives the call);
4. return a TINY verdict only: `{ schema_version:1, ok:true }` on success, or
   `{ schema_version:1, ok:false, reason:"<what>" }` on a validation breach. No AST, no
   declarations, no large payload -> nothing accumulates, and `max_result_bytes` is never
   stressed by legitimate output.

The entry adds NO production API. In particular it does not rely on a `unit:text(node)`
method (which does not exist in the JS `SourceUnit` contract and must not be added just for
fuzzing); raw-equality is checked with a reviewed reference decoder inside the entry
(section 4.3).

## 4. Internal validations (all in JS; a breach -> `{ok:false, reason}`)

### 4.1 Range integrality and bounds
Every range on every AST node, comment, annotation, and diagnostic: `start` and `stop` are
integers, `1 <= start <= stop <= n+1` (`n` = input byte length). A `null` diagnostic range
is allowed (a whole-unit diagnostic). Non-integral, inverted, or out-of-bounds -> breach.

### 4.2 Child nesting (SYNTAX-GATED exemption)
Each AST child node's range must nest within its parent's: `child.start >= parent.start` and
`child.stop <= parent.stop`. An escape is a BREACH, with ONE narrowly-scoped exemption:

- **Clean unit** (no `js.syntax` diagnostic): STRICT nesting for EVERY child, including
  zero-width nodes. A clean-parse range bug cannot hide.
- **Unsupported-only unit** (`js.unsupported` but no `js.syntax`): STRICT nesting -- an
  unsupported construct is consumed cleanly and produces no frontier markers.
- **Syntax-recovery unit** (contains a `js.syntax` diagnostic): non-empty, non-`Error`
  children must still nest; two recovery-artifact classes are EXEMPT because they intentionally
  anchor at the FAILURE FRONTIER (`cur.start`, the current not-yet-consumed token) rather than
  the parent's last CONSUMED token (`prev.stop`), so they can legitimately sit just past a
  parent's finalized stop -- `Error` recovery nodes (any width) and zero-width empty markers
  (`start === stop`), e.g. an `if`'s empty-consequent `ExpressionStatement` or an unterminated
  `ArrayPattern`'s missing element. They still pass the 4.1 in-bounds check. The exemption is
  COUNTED and bounded (`<= 2n + 64`) so recovery cannot mint unbounded synthetic escaping nodes.

The "recovery" signal is `js.syntax present OR js.limit.diagnostics present`: a diagnostic-
budget hit drops ordinary diagnostics (including `js.syntax`), so it is treated as possible
recovery -- safe, since escaping markers arise ONLY from syntax recovery. This gating prevents
a future clean-parse range bug from hiding behind a broad "all zero-width nodes are exempt"
rule while preserving the faithful recovery behavior. (The fuzzer found this while shaking out
the parser; it also found and motivated a real fix -- see below.)

### 4.2b Parser fix the fuzzer motivated (landed in this slice)
The fuzzer found an INVERTED range (`stop < start`) emitted by `fin()` / `fin2n()` on an
error-recovery path that consumed nothing after a node began (`prev.stop < node.start`).
`fin()`/`fin2n()`/`errNode()` now clamp `stop >= start` (a zero-width span when no progress),
preserving the 4.1 range invariant. Existing JS parser/frontend/conformance suites are
unaffected.

### 4.3 raw slice equality (non-tautological)
For every comment and every attached annotation, `raw` must EXACTLY equal the byte slice
`input[start-1 .. stop-1)` decoded the SAME way the lexer decodes it. The entry carries a
REVIEWED reference decoder `refSlice(bytes, start1, stop1)` that mirrors the lexer's
`sliceText`/`decode` byte-for-byte: ASCII (`<0x80`) verbatim; a valid 2/3/4-byte UTF-8
sequence -> its code point; any decode error -> a single `U+FFFD` advancing exactly one byte
(NOT `len` bytes) -- matching lexer.js. Comparing a locally recomputed slice against `raw`
is the real check; comparing a node's own slice with itself would be tautological. If the
lexer's decoder changes, this reviewed copy changes with it (a controlled pair).

### 4.4 Traversal termination + no cycles + size-bounded
Walk the whole AST with an explicit visited marker (a `Set` of node objects). A revisited
node -> cycle -> breach. The total visited-node count must stay under a bound derived from
input size (`MAX_NODES = 8 * n + 64`); exceeding it -> breach (runaway/quadratic construction).
The parser's AST is a tree and QuickJS is refcounted, so a cycle should be impossible -- this
is the defensive proof.

### 4.5 Diagnostic-budget behavior
`opts.maxDiagnostics` is derived DETERMINISTICALLY per input (section 5) and covers `0`,
small finite values, and the default. Assert: the count of ORDINARY diagnostics (code NOT
`js.limit.*`) is `<= maxDiagnostics`; and when the budget was hit, the terminal
`js.limit.diagnostics` diagnostic is still present (terminal limit diagnostics are always
kept even past the cap). Ordinary diagnostics exceeding the cap, or a swallowed terminal ->
breach.

### 4.6 Classification (reflects the real API)
The outcome is read from the returned `SourceUnit`:
- **clean** (no `severity:"error"` diagnostic), **syntax** (a `js.syntax` error), and
  **unsupported** (a `js.unsupported` error) are all VALID `rc==0` parser outcomes -> `ok:true`.
- **Parser-level `js.limit.depth` / `js.limit.tokens` / `js.limit.diagnostics`** may appear
  in an `rc==0` `SourceUnit` and are VALID bounded outcomes -> `ok:true`.
- **Any `js.internal` diagnostic** in the `SourceUnit` is a fuzz FAILURE -> `{ok:false,
  reason:"js.internal"}`. (A protected-parse internal error is a real defect, not an
  expected outcome.)

## 5. The C harness (`fuzz/fuzz_js_source.c`)

One process-lifetime `HlJsSession` created with tight-but-real `HlJsSessionLimits` (heap
64 MiB, stack 1 MiB, a finite instruction budget, `max_source_bytes` >= the fuzzer
`-max_len`, small `max_result_bytes` since the verdict is tiny). `LLVMFuzzerTestOneInput`:

1. Derive `maxDiagnostics` deterministically from the input, covering `0` / small / default:
   `n==0 -> 0`; else a small mix keyed off the first byte and length (e.g.
   `{0, 1, small, 4096}`), passed as the options JSON `{"maxDiagnostics":N}`.
2. `rc = hl_js_session_analyze(s, "hull:source:tests:fuzz_parse", "fuzz", data, n, "f.js",
   opts_json, opts_len, &out, &out_len)`.
3. Interpret (matches the real API's two channels):
   - `rc == 0`: parse the tiny verdict JSON. `{ok:true}` -> pass. `{ok:false}` (a validation
     breach OR a detected `js.internal`) -> `abort()` with the reason.
   - `rc != 0` with a host `js.limit.*` code in `out` -> EXPECTED resource exhaustion (the
     session's heap/stack/instruction/source/result limit); not a crash. Then run the
     session-reuse probe (step 4).
   - `rc != 0` with `js.internal`, malformed JSON, an unclassified error, or anything else
     -> `abort()`.
4. **Active session-reuse proof.** After ANY host-level `js.limit.*` (rc != 0), immediately
   invoke a FIXED tiny valid input (`"const a = 1;"`) in the SAME session and require a clean
   `{ok:true}` compact verdict. This catches a pending exception or poisoned session state
   the instant it happens. Then continue normally.
5. **Defensive hygiene.** Every `N = 512` inputs, destroy and recreate the session. The AST
   is acyclic and QuickJS is refcounted, so this is defense in depth, not a correctness
   requirement. There is no public GC operation and none is added for the fuzzer -- session
   recreation alone is sufficient.

`abort()` (or any ASan/UBSan trap) fails the campaign; libFuzzer writes the reproducer.

## 6. Corpus, dictionary, sanitizers, CI

- **No copied repo corpus.** Committed: a small ADVERSARIAL seed set
  `fuzz/corpus_js_source/` (deeply nested, unterminated, huge-number, mixed-UTF-8,
  regex/division-ambiguous, annotation-heavy inputs) and a dictionary `fuzz/js_source.dict`
  (JS keywords + punctuators + comment/annotation markers). At run time the `fuzz-js-source`
  target DETERMINISTICALLY stages the current repo `.js` / `.mjs` / `.cjs` files into a temp
  corpus dir (`build/fuzz-corpus/js_source/`), mirroring how `fuzz-lua-source` stages the
  `.lua` tree -- so real source shapes seed coverage without committing copies.
- **Sanitizer split** (the vendored-QuickJS exception, mirroring the MSAN QJS block):
  | Unit | Flags |
  |------|-------|
  | harness + `js_session.c` + registry (Hull-owned) | `-fsanitize=fuzzer,address,undefined` |
  | vendored QuickJS TUs | `-fsanitize=fuzzer-no-link,address` (coverage + ASan, NO UBSan) |
  | final link | `-fsanitize=fuzzer,address,undefined` |
  Retains libFuzzer coverage instrumentation + ASan on QuickJS while excluding only UBSan
  there; full sanitizers on Hull code.
- **Build/run:** `make fuzz-js-source CC=clang FUZZ_TIME=<s>` -> stage corpus, then
  `./fuzz/fuzz_js_source build/fuzz-corpus/js_source/ -dict=fuzz/js_source.dict
  -max_len=16384 -max_total_time=$(FUZZ_TIME)`. Add to the `fuzz` aggregate target.
- **CI:** a 60s `Fuzz js_source (60s)` step in the existing Fuzz job (`make fuzz-js-source
  CC=clang FUZZ_TIME=60`), mirroring the lua_source step.

## 7. Non-scope

No production API change (no `unit:text`, no GC hook). No parser behavior change. No grammar
change. Slice B (Test262) is a separate, independently-mergeable story.
