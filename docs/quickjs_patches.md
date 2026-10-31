# QuickJS patches

Hull vendors QuickJS directly in `vendor/quickjs/` (edited in tree, unlike
WAMR, which is a submodule patched into `build/wamr-patched/`; see
[wamr_patches.md](wamr_patches.md)). Every local change is marked in the source
with a `HULL PATCH` comment and listed here.

**Vendored version:** Bellard QuickJS **2026-06-04**
(`https://bellard.org/quickjs/quickjs-2026-06-04.tar.xz`, SHA-256
`b376e839b322978313d929fd20663b11ba58b75df5a46c126dd19ea2fa70ad2a`; upstream
publishes no checksum, so this is the hash of the tarball as fetched). The
version string lives in one place, `QJS_VERSION` in `mk/vendor/quickjs.mk`
(it feeds `CONFIG_VERSION` and the bytecode / template cache keys).

**On a QuickJS upgrade:** grep the new tree for `HULL PATCH`, confirm each entry
below is still needed against upstream, and re-apply the ones that are. Then
check the interrupt cadence (`JS_INTERRUPT_COUNTER_INIT` in `quickjs.c`,
`INTERRUPT_COUNTER_INIT` in `libregexp.c`) against `HL_JS_INTERRUPT_WEIGHT`
(`runtime/js/internal.h`), and any C code that switches on `JS_VALUE_GET_TAG`.

## Upgrade 2024-01-13 -> 2026-06-04 (audit 6 H5)

The reason for the upgrade: in 2024-01-13 a regular expression's
backtracking ran inside one native `RegExp.prototype.exec` call that never
polled the interrupt handler, so `/^(a+)+$/.test("a".repeat(40) + "!")`
held the event loop for exponential time, under any instruction limit.
QuickJS 2025-04-26 added the poll, and 2026-06-04 keeps it:
`libregexp.c` `lre_poll_timeout` decrements a counter on every backtracking
step and every `INTERRUPT_COUNTER_INIT` (10000) steps calls
`lre_check_timeout(opaque)`, which `quickjs.c` implements by calling the
runtime's interrupt handler; a non-zero answer returns `LRE_RET_TIMEOUT`,
and `js_regexp_exec` / `js_regexp_Symbol_replace` raise it through
`JS_ThrowInterrupted` (uncatchable). Hull's handler charges each of those
polls `HL_JS_INTERRUPT_WEIGHT` (10000), the same as a bytecode poll, so the
weight is unchanged: both pollers still run 10000 steps per poll.
Guard: `test_js.c`, `js_audit6.catastrophic_regexp_trips_the_instruction_limit`.

What else changed for Hull:

- `libbf.c` is gone (the bignum extensions were removed upstream) and
  `dtoa.c` is new; `CONFIG_BIGNUM` is no longer defined.
- QuickJS is compiled `-std=gnu11` (it uses the `asm` keyword) and `-fwrapv`,
  as upstream's own Makefile does; Hull's own code stays `-std=c11`.
- Uncatchability is now a property of the pending exception
  (`JS_SetUncatchableException(ctx, flag)`), not of the error object:
  `hl_js_budget_throw` uses it, and the 0003 export of
  `JS_SetUncatchableError` is no longer needed.
- Concatenated strings may be ropes (`JS_TAG_STRING_ROPE`); `JS_IsString`
  covers them, and the two tag switches in `mod_db.c` / `mod_db_udf.c` now
  take both tags (a concatenated SQL parameter used to bind as NULL).
- Patch 0001 is dropped: upstream now initialises `label_lvalue` in the array
  branch of `js_parse_destructuring_element` (the `enum_depth = 0` branch sets
  `label_lvalue = -1` alongside `opcode` / `scope`).

## Patch 0001 - dropped at 2026-06-04 (fixed upstream)

Initialised `label_lvalue` in the array-destructuring branch of
`js_parse_destructuring_element` (found by MSan, #547). Upstream 2026-06-04
sets it there itself, so the patch is gone; the MSan CI job remains its guard.

## Patch 0002 - a stack frame for bound-function calls

**File:** `vendor/quickjs/quickjs.c`, `js_call_bound_function`
**Found by:** the second capability audit (stdlib-identity spoofing)
**Upstream:** Hull-specific; not a QuickJS bug.

Hull decides whether a database call may touch the reserved `_hull_*` tables
by asking who called it: `JS_GetScriptOrModuleName(ctx, 1)` inside the native
binding names the immediate JS caller, and a `hull:` module is the stdlib.
`js_call_c_function` pushes a frame for the native function itself, so level 1
is its caller.

A bound function had no frame. Calling `f.bind(conn, sql)` ran the native
target with the bound function's CALLER as its level-1 frame. So a stdlib
helper that invokes an app-supplied callback - `retry.run(fn, { retryOn })`
calls `opts.retryOn(value)` - lent its stdlib identity to
`conn.exec.bind(conn)`, and app SQL ran against `_hull_*` tables.

The patch pushes a frame for the bound function in the non-constructor branch,
set up the way `js_call_c_function` sets up its own (`prev_frame`, `js_mode`,
`cur_func`, `arg_buf`, `arg_count`), and pops it on return. The frame's
function is the bound function object, which is not bytecode, so
`JS_GetScriptOrModuleName` returns no name for it and Hull treats the call as
app code. Frame walkers already handle non-bytecode frames (every native call
has one): backtraces print the bound function as a native frame, and the
strict/math-mode checks see the same zero `js_mode` a native frame carries.

The constructor branch (`new boundFn()`) is unchanged: a constructor call
cannot target a database binding.

**Guard:** `tests/hull/runtime/js/test_js.c`,
`js_cap.stdlib_helpers_do_not_lend_their_identity`. Without the patch the
bound-`retryOn` case runs the `_hull_*` statement and the test fails.

## Patch 0003 - an interrupt is polled again at the very next step

**File:** `vendor/quickjs/quickjs.c`, `JS_ThrowInterrupted` (since 2026-06-04;
it was in `__js_poll_interrupts` before)
**Found by:** the fifth runtime audit (instruction-limit bypass, H2)
**Upstream:** Hull-specific; upstream relies on the error being uncatchable.

The instruction limit is an interrupt: the handler returns 1 and QuickJS throws
an "uncatchable" error. That error skips `catch` blocks, but an async function
body and a promise reaction job turn ANY exception into a rejection
(`js_async_function_resume`, `promise_reaction_job`), and the code that called
them carries on. QuickJS reset its poll countdown to 10000 steps at every poll,
so the next poll landed inside the next async call too:

```js
const burn = async () => { for (;;) {} };
for (;;) burn();          // never interrupted: each poll lands in burn()
```

Hull's handler is sticky (once over the limit it returns 1 until the runtime
re-arms the budget at its next entry point). The patch sets the countdown to 1
when the handler interrupts, so the next call or backward jump - in the caller,
outside the async body - polls, finds the handler still tripped, and throws
there as well. Nothing can run more than a straight line of code after a trip,
and a resolve / reject call is itself a call, so a tripped run settles nothing
further: its promise jobs fail at their first step, which is how
`hl_js_run_jobs` discards them.

Since 2026-06-04 the patch sits in `JS_ThrowInterrupted`, which both
interrupt sources reach - the bytecode poll and the regexp backtracking poll
(`LRE_RET_TIMEOUT`) - so a trip inside a regexp is sticky the same way. The
earlier version also exported `JS_SetUncatchableError` for bindings that
re-raise a caught interrupt (a SQL UDF, a `compute.stream` callback); upstream
now provides `JS_SetUncatchableException`, which `hl_js_budget_throw` uses.

Audit 38: `JS_IteratorClose` with an exception pending - the error path of
every C loop over an iterator (`Iterator.prototype.some` / `every` / `find`
/ `forEach` / `reduce`, the helpers, `Array.from`, ...) - re-raised the
saved error with `JS_Throw`, which clears the uncatchable flag, so an
interrupt raised inside such a loop reached the caller's `catch`. It now
keeps the flag and, for an uncatchable error, calls no `return` method (as
the bytecode unwinder already skips iterator closing for one).

Audit 39: a regexp compile stopped by the budget (patch 0005's
`lre_check_work`) is raised through `JS_ThrowInterrupted` too, not as the
SyntaxError every other compile failure becomes. The same audit swept the
other places C code takes a pending exception and throws it again
(`JS_GetException` / `JS_Throw`): the async function and async generator
resumes, `promise_reaction_job`, the thenable and `finally` jobs, the
`Promise` combinators and module loading all turn it into a REJECTION, which
is the case this patch covers (the next call polls and finds the run still
tripped); the generator `throw` path throws the app's own value, and module
evaluation rethrows only to Hull's own loader. No C loop was found that
swallows an interrupt and keeps iterating.

**Guard:** `tests/hull/runtime/js/test_js.c`,
`js_audit5.async_bodies_do_not_escape_the_instruction_limit`; for the
audit 38 part, `js_audit38.iterator_helper_steps_are_charged` (the catch
never runs). Without the patch
the async-burn loop is never interrupted and the test hangs.

## Patch 0004 - build_backtrace holds its own reference to the error

**File:** `vendor/quickjs/quickjs.c`, `build_backtrace` (wrapping the original
body, renamed `build_backtrace1`)
**Found by:** the nightly deep JS-source fuzzer (`fuzz/fuzz_js_source.c`), on
QuickJS 2026-06-04, as an ASan SEGV in `find_own_property` under
`build_backtrace` -> `JS_DefinePropertyValue(..., JS_ATOM_stack, ...)`.
**Upstream:** not reported upstream yet.

Three callers pass `rt->current_exception` without a reference of their own:
`JS_CallInternal`'s exception path, the parser's error path and the module
loader's. `build_backtrace` allocates while it works (each frame's function
name, the stack string), and an allocation that runs out of memory throws "out
of memory", which replaces `rt->current_exception` and frees the error object
still being decorated. The final `JS_DefinePropertyValue` of `stack` then wrote
through freed memory. Hull's JS source analyzer runs in a heap-limited QuickJS
session, so malformed app source that exhausts it reached this path.

The patch makes `build_backtrace` a wrapper that takes a reference to the
error object for the duration of the call (`JS_DupValue` / `JS_FreeValue`).

**Guard:** `fuzz/corpus_js_source/regress_backtrace_oom_uaf`, the fuzzer's
crashing input, replayed by the per-PR JS-source fuzz job.

## Patch 0005 - charge work done inside one step

**Files:** `vendor/quickjs/quickjs.h` (`JSWorkHandler`, `JS_SetWorkHandler`),
`vendor/quickjs/quickjs.c` (the `work_*` runtime fields, `js_work_flush` /
`js_work_add` / `js_work_check`, and their call sites below),
`vendor/quickjs/libregexp.{c,h}` (since audit 39: `lre_check_work`, the cached
forward group-name scan)
**Found by:** the tenth runtime audit (H2)
**Upstream:** Hull-specific; the model is Lua patch 0004
([lua_patches.md](lua_patches.md)).

The instruction limit counts interrupt polls, one per
`JS_INTERRUPT_COUNTER_INIT` calls / backward jumps. A builtin that scans,
compares or copies a large operand does all of that inside one step, so the
limit did not bound a run's wall time:

```js
const s = 'a'.repeat(1 << 24);
for (;;) s.indexOf('b');                              // 16 MB per step
Array.prototype.indexOf.call({ length: 2 ** 53 - 1 }, 1);   // never returns
```

The patch adds a work handler next to the interrupt handler
(`JS_SetWorkHandler(rt, cb, opaque)`). Work is counted in 1/64 instruction
units ("bytes"): a byte scanned, compared, hashed or allocated costs one, an
element a generic array method visits `JS_WORK_ELEM` (64, one unit). Charges
accumulate in the runtime and are handed to the handler in batches of 1024
units (`JS_WORK_BATCH`); the handler (Hull's `js_budget_add`) adds them to the
run's count and answers whether the run is over. Two kinds of charge:

- **Deferred** (`js_work_add`): when the handler reports the run over, every
  context's `interrupt_counter` is set to 1, so the very next call or
  backward jump polls the interrupt handler, which is sticky in Hull and
  throws the uncatchable interrupt (as patch 0003 does after an interrupt).
  No error is raised where the charge is made, so it is safe anywhere -
  inside the allocator included. Used where the work is bounded by the heap
  and a trip can wait for the call to finish: every block the allocator
  hands out or grows (`js_malloc_rt`, `js_realloc_rt`: copies, string
  building, array growth, JSON output, typed-array allocation); a string
  equality / ordering (`js_string_eq`, `js_string_compare`,
  `js_string_rope_compare` - the bytes compared); a string made an atom
  (`__JS_NewAtom` hashes it whole) or a Map / Set key (`map_hash_key`);
  `trim`, `isWellFormed` / `toWellFormed`; a typed array's `fill`,
  `copyWithin`, `set`, `reverse`, `slice` and `sort` (about n log2 n
  comparisons, 8 per unit), and `indexOf` / `lastIndexOf` / `includes` (the
  elements it actually scanned); `ArrayBuffer.prototype.slice`;
  `JSON.parse` (its input, scanned once); a fast array's `shift` (its
  memmove) and `reverse`.
- **Checked** (`js_work_check`): charges, and when that finds the run over,
  throws the interrupt at once (`JS_ThrowInterrupted`) so the builtin
  returns through its error path. Used in loops no heap bounds: every
  generic array method's per-index loop (`indexOf`, `lastIndexOf`,
  `includes`, `fill`, `join` / `toString`, `reverse`, `slice` / `splice`,
  `concat`, `with`, `toReversed`, `toSpliced`, `toSorted`, `flat` /
  `flatMap`, `every` / `some` / `forEach` / `map` / `filter` - a sparse
  array calls no callback - `reduce` / `reduceRight`, `Array.from`, and
  `JS_CopySubArray`, behind `shift` / `unshift` / `splice` /
  `copyWithin`), a typed array's generic `join` / `slice` / `set` loops,
  each `sort` comparison, `JSON.stringify`'s array and object loops, and
  the string searches (`indexOf` / `lastIndexOf`, `includes` /
  `startsWith` / `endsWith`, and `string_indexof` behind `split` /
  `replace` / `replaceAll`), which are quadratic in the worst case
  (`('a'.repeat(1 << 16)).indexOf('a'.repeat(1 << 15) + 'b')`): each
  position charges the characters it compared. `string_cmp` reports that
  count, and `string_indexof` takes the context and returns -2 with the
  interrupt pending.

The eleventh audit added the work the first pass missed:

- **String to number** (deferred): `JS_ToCStringLen2` charges the string's
  length - its ASCII path scans the whole string and returns the string's own
  buffer, so no allocation charged it (`for (;;) +s` on a 16 MB string, and
  every Hull binding converting an app string). Since audit 23 a charge
  there that finds the run over budget FAILS the conversion with the
  interrupt (`js_work_check`, then the 0007 fail path): only counted, a C
  binding looping over an array that named one big string rescanned it once
  per element with nothing polled in between - `db.query(sql,
  new Array(32766).fill(s16MB))`, `http.fetch` headers, `tar.create` names
  - one call holding the loop for hours, past the budget and the run
  deadline. `ToNumber` /
  `JS_StringToBigInt` / `parseInt` / `parseFloat` charge the length again for
  the `skip_spaces` + `js_atof` parse.
- **RegExp flags** (deferred): `Symbol.match` / `matchAll` / `replace` /
  `split` and String `replaceAll` / `matchAll` scan `ToString(rx.flags)` - an
  app getter can return any string - for up to four letters
  (`js_flags_charge`: 4 x its length per call). A `"$<"` in a replacement
  with named groups and no `>` rescanned the rest of the replacement once per
  `"$<"` (quadratic in one call); `GetSubstitution` charges each scan
  (checked).
- **BigInt** (checked, before the work): `js_bigint_mul` charges
  `a->len * b->len` limb products, `js_bigint_divrem` `(na - nb + 1) * nb`,
  a decimal parse (`js_bigint_from_string`) `n_limbs^2 / 2` - each at
  `JS_WORK_LIMB` (8, 1/8 unit per limb multiply-add) - and `toString` in a
  radix that is not a power of two `len^2 / 2` limb divisions at
  `JS_WORK_LIMB_DIV` (32, 1/2 unit). A 2^20-bit square (2^28 products) is
  about 33M units. `js_bigint_pow` (`**`) is a chain of charged
  multiplications; it now frees its partial result when one fails (an
  upstream leak the charge made reachable).
- **Typed-array construction** (checked): `new TA(arrayLike)`
  (`js_typed_array_constructor_obj`), `TA.from` and `js_array_from_iterator`
  charge `JS_WORK_ELEM` per element, like `Array.from` -
  `new Uint8Array({ length: 5e7 })` ran 5e7 property reads in one step.

The thirty-eighth audit added the iterator protocol:

- **A native iterator step** (polled, as a call is): `JS_IteratorNext2`'s
  fast path calls a built-in iterator's `next` (an array iterator, an
  iterator helper, a wrap-for-valid-iterator, a Map / Set / string /
  regexp-string iterator) directly, with no `JS_Call`, so no step was
  counted. A C loop pulling from one allocated nothing and ran no bytecode:
  `Array.prototype.values.call({ length: 2 ** 53 - 1 }).drop(2 ** 53 - 1)
  .next()` ran 2^53 steps inside one builtin call, uncharged and blind to
  the run watchdog (patch 0006 polls only at steps; measured ~9 s per 3e8
  under `--max-run-ms 1000`, trip only after the return). The fast path now
  calls `js_poll_interrupts` before each step - one step, charged like a
  call and seeing the stop flag at once - so every consumer is covered in
  one place: the iterator helpers (`drop`, `filter`, `map`, `take`,
  `flatMap`, both levels), `some` / `every` / `find` / `forEach` /
  `reduce` / `toArray`, `Iterator.concat`, `Iterator.from`'s wrapper,
  `for...of`, spread and destructuring, `Array.from` / `TA.from`, the
  `Map` / `Set` / `WeakMap` / `WeakSet` constructors, `Object.fromEntries`,
  `Map.groupBy` / `Object.groupBy`, and `Promise.all` / `allSettled` /
  `any` / `race`. An iterator whose `next` is not native goes through
  `JS_Call`, which was already a counted step (a native callback such as
  `Array.isArray` is one too). The same audit found the interrupt made
  catchable on its way out of those loops: see patch 0003.

The thirty-ninth audit removed the last long single calls the run watchdog
could not stop:

- **Proxy `ownKeys` validation** (made linear, checked):
  `js_proxy_get_own_property_names` looked for duplicates with one
  `find_prop_key` scan per key, and matched every target key against the
  result the same way (non-configurable keys, a non-extensible target), so a
  trap returning n keys cost O(n^2) compares: 200k keys ran 8 s in one
  `Reflect.ownKeys`, 1M about 200 s within the 64 MB heap, the trip landing
  only after it returned. Both checks now go through an open-addressed index
  of the atoms (`JSAtomIndex`), and the key-collection, index-build and
  target loops each charge `JS_WORK_ELEM` per key (checked).
- **The `JSON.stringify` replacer array** (checked): its loop runs to the
  array's length - up to 2^53 - 1, not bounded by the heap - and charged
  nothing for a hole (`a.length = 1e8; JSON.stringify({}, a)` ran 4 s under a
  1 s deadline). Each index charges `JS_WORK_ELEM`.
- **A forward named backreference** (linear, checked): `\k<name>` before its
  group made `re_parse_captures` rescan the whole pattern, once per reference
  (`new RegExp('\\k<a>'.repeat(20000) + '(?<a>x)')` compiled for 5 s). The
  first forward reference now scans the pattern once into a list of every
  named group (`fwd_names`), which later ones search. Each lookup (backward
  or forward, up to 255 names of up to 127 bytes) and the scan are charged
  through a new libregexp hook, `lre_check_work(opaque, bytes)` (Hull's side:
  `js_work_add`); when it reports the run over, the compile fails with
  `LRE_INTERRUPTED_MSG`, which `js_compile_regexp` raises as the
  uncatchable interrupt rather than a SyntaxError.
- **`RegExp.prototype[Symbol.split]`** (checked): one exec per position of
  the input; the exec call counted one step for ~100 ns of work, so a 60M-
  character split ran 5 s inside a 100M budget. Each position now also
  charges `JS_WORK_ELEM`.
- **The `JSON.parse` reviver walk** (checked): `internalize_json_property`
  reads each array's length when it reaches it, and an earlier reviver call
  can set a later array's length to 2^32 - 1. Each element charges
  `JS_WORK_ELEM`.

Probed and left as they are (each one call stays bounded by the heap and
already charged, or is a counted step per iteration): typed-array `set` (its
generic loop was checked since audit 10), `Symbol.replace` / `Symbol.match`
(the fast path's `lre_exec` polls at every goto / backtrack; the generic loops
call `exec` through `JS_Call`, a step, and store every result), spread
(`js_append_enumerate`: the iterator protocol above, or a fast array whose
copy is allocated), `for...in` (its inner skip loop walks property lists the
heap holds; a prototype step polls), and `Date.parse` (one linear scan).

The allocator charge is in the core (`js_malloc_rt` / `js_realloc_rt`), not a
custom `JSMallocFunctions` table, so it can make the next step poll (an
allocator has no context) and keeps QuickJS's own slab allocator and memory
accounting. A runtime with no work handler pays one test per charge and
behaves as upstream (the JS source frontend's tooling session sets none).

Audit 39 added `JS_GetMallocSize(rt)`, an O(1) read of the bytes the runtime
has allocated. A binding that collects before retrying a refused call (a
handle cap held by objects the app dropped: `fs.mmap`, `blob`, `kv.open`,
`ws.connect`, compute buffers) runs a full `JS_RunGC`, whose cost grows with
the heap; Hull charges it from this size (`hl_js_gc_for_retry`, one unit per
64 bytes) BEFORE it collects - uncharged, a refused call in a loop over a
big heap cost ~8.6 ms for ~3,000 units.

**Guard:** `tests/hull/runtime/js/test_js.c`,
`js_audit10.builtin_work_is_charged` (each case trips a 1M budget within
seconds; several never return without the patch),
`js_audit11.builtin_work_is_charged` (each loop trips a 1M budget within
49 iterations, counted rather than timed), and the `js_audit38` cases
(`iterator_helper_steps_are_charged`: one helper call over a 2^53-element
array-like trips a 1M budget, uncatchably; `iterator_helper_steps_see_the_watchdog`:
with no limit, the run deadline stops that one call; the `drop` case never
returns without the patch), and the `js_audit39` cases
(`long_builtins_are_charged`: a holey replacer array, regexp compiles with
250 long group names and 300k backreferences either way, a 16M-position split
and a reviver-grown array each trip a 1M budget, uncatchably;
`long_builtins_see_the_watchdog`; `proxy_own_keys_is_linear`: 300k keys,
4.5e10 compares before, finish in milliseconds; `ordinary_use_still_works`).

## Patch 0006 - an embedder stop flag polled at every step

**Files:** `vendor/quickjs/quickjs.c` (`JSRuntime.stop_flag`,
`JSContext.stop_flag`, `JS_NewContextRaw`, `JS_SetStopFlag`,
`js_poll_interrupts`), `quickjs.h` (`JS_SetStopFlag`)
**Found by:** round-12 audit (G2: the run watchdog)
**Upstream:** Hull-specific.

Audits 9-12 kept finding work one step does that patch 0005 does not charge (a
property miss walking a deep prototype chain, a literal compared char by char,
a slow-array length shrink, ...; Proxy `ownKeys` validation and a regexp
compile rescanning for `\k<name>` were on this list until audit 39 made them
linear and charged). Charging each one never converges, so Hull
bounds every run by wall-clock time as well (`src/hull/cap/run_watchdog.c`): a
watchdog thread raises a per-VM stop flag at the run's deadline. The interrupt
handler only runs when a context's step counter runs out - every
`JS_INTERRUPT_COUNTER_INIT` (10000) calls / backward jumps - and each of those
steps may be one of the uncharged operations above.

The patch gives the runtime a pointer to an `int` the embedder owns
(`JS_SetStopFlag(rt, &flag)`, NULL = none), copied to every context (existing
ones by the setter, new ones by `JS_NewContextRaw`). `js_poll_interrupts`
reads it (an atomic relaxed load: another thread writes it) after decrementing
the counter, and when it is set polls the interrupt handler at this step.
Hull's handler (`runtime/js/runtime.c`, and the worker VM's in
`runtime/js/worker.c`) trips the run's budget, which patch 0003 keeps sticky
and uncatchable. The regexp engine's own poll (`lre_check_timeout`) calls the
same handler, so a backtracking regexp sees the flag too.

Cost: one load of a pointer kept beside the step counter and, when set, one
load of the flag, at every call and backward jump. A runtime with no flag (the
JS source frontend's tooling session) behaves as upstream.

**Limit:** the flag is seen at steps. One builtin call that runs long without
polling (one huge prototype walk, one regexp compile) runs to completion; the
watchdog bounds loops of them.

**Guard:** `tests/hull/runtime/js/test_js.c`, `js_run_watchdog.*` (loops of
round-12 triggers under a 200 ms deadline and no instruction limit are
stopped; the trip is not caught by try / catch; a worker.dispatch job is
stopped).

## Patch 0007 - a failed string conversion frees its reference

**Files:** `vendor/quickjs/quickjs.c` (`JS_ToCStringLen2`, the `fail:` label)
**Found by:** round-22 audit (G1)
**Upstream:** a bug in 2026-06-04; worth sending upstream.

`JS_ToCStringLen2` takes a reference to the string it converts (a
`JS_DupValue`, or `JS_ToString`'s result) and gives it back in
`JS_FreeCString`. When the UTF-8 copy of a non-ASCII or wide string cannot be
allocated it jumped to `fail:` and returned NULL with that reference still
held, so every conversion that hit the heap limit leaked its string for good.
A worker.dispatch pool thread reuses its JSRuntime across jobs, so the leaks
added up until every dispatch on that thread failed. The patch frees the
reference at `fail:` (`JS_FreeValue` of `JS_EXCEPTION`, the first `goto`, is a
no-op).

**Guard:** `tests/hull/runtime/js/test_js.c`,
`js_audit22.a_failed_string_conversion_leaks_nothing`.

## Patch 0008 - a job runs with the tag of the code that awaited

**Files:** `vendor/quickjs/quickjs.c` (`JSRuntime.job_tag`, `JSJobEntry.tag`,
`JSPromiseReactionData.tag`, `JS_EnqueueJob2`, `JS_ExecutePendingJob`,
`fulfill_or_reject_promise`, `perform_promise_then`, `JS_SetJobTag` /
`JS_GetJobTag` / `JS_GetPendingJobTag` / `JS_ExecutePendingJobTagged` /
`JS_DiscardPendingJob`),
`quickjs.h` (`JS_SetJobTag`, `JS_GetJobTag`, `JS_GetPendingJobTag`,
`JS_ExecutePendingJobTagged`, `JS_DiscardPendingJob`)
**Found by:** audit 30 (M: cross-request op attribution)
**Upstream:** Hull-specific.

QuickJS has one job queue per runtime, and Hull drains it inside whichever
entry is running (a request, a timer, a ws callback, a resume) with that
entry's request, life and teardown hook active - its own code after an
`await` must run there (audit 7 H1). But the drain also runs the jobs of
OTHER runs: a handler that awaited a promise Hull does not drive was ended
(its request answered), and when a later request resolved that promise, the
first handler's code ran inside the second's drain. An op it then started
(`hull.sleep`, `http.fetch`, `db.async`) was taken for the active request's:
it suspended that request's connection, and the second client waited for the
first handler's sleep.

The patch gives the runtime a 64-bit tag the embedder sets
(`JS_SetJobTag`, read back by `JS_GetJobTag`; 0 until set). A job is queued
with the tag current at that moment and run with it (the caller's tag is put
back after), and a promise reaction records the tag current when its handler
was registered - the `await` or `then` - and its job is queued with THAT tag,
not the one current when the promise was settled. So a job always carries the
tag of the code that waited for it: the AsyncContext model, for one value.

Hull (`runtime/js/runtime.c`, `hl_js_run_tag_*`) gives every entry a fresh
tag (`hl_js_budget_arm`, and app.main's arm), a resume re-enters the tag of
the continuation it resumes (`HlJsRunLink.run_tag`), and
`hl_js_async_cont_create` treats an op made under a tag other than the active
entry's as another run's: detached, attached to no request and chained into
no run (app.main's own run excepted, by its tag). `req.multipart()` reads are
refused there too.

Audit 31 adds `JS_GetPendingJobTag` (the tag the next pending job will run
with), so `hl_js_run_jobs` can tell where another run's stretch of jobs ends:
a transaction still open there is rolled back before the active entry's next
job runs, and that run is not continued past its await. It also adds
`JS_ExecutePendingJobTagged` (run the first pending job queued with a given
tag, leaving the ones ahead of it in order): while the active run is inside a
transaction across a microtask yield, the drain runs that run's own jobs
first and holds other runs' jobs back until the transaction closes, so they
never run inside it.

Audit 32 adds `JS_DiscardPendingJob` (drop the first pending job unrun, its
arguments freed): `hl_js_run_jobs` drops every job of a run that was failed
for holding a transaction across a wait, so its code after the await does not
run on in the same drain.

Audit 33 adds `JS_ForEachLiveJobTag` (report the tag of every pending job and
of every reaction on a pending promise; -1 while the collector runs). A failed
run's tag must outlive its request: a reaction it left on a promise Hull does
not drive is queued only when that promise settles, possibly from a much later
request. Hull keeps every failed tag in a sorted set and sweeps it, when it has
doubled, down to the tags this walk still reports - so a tag is forgotten only
once no code can run under it, and the set is bounded by the reactions and
jobs alive on the VM heap.

Cost: one 64-bit field per job and per pending reaction, and two stores per
job run. A runtime that never sets a tag (the worker.dispatch VM, the JS
source frontend's tooling session) behaves as upstream.

**Guard:** `tests/e2e_js_runs.sh`, "audit 30 M": without the patch (or the
Hull side) `/rb` answers after `/ra`'s 1.5 s sleep instead of at once.

## Patch 0009 - keyed atom and Map hashes; charged atom lookups

**Files:** `vendor/quickjs/quickjs.c` (`JSRuntime.hash_key`, the `JSSipState`
helpers that replace `hash_string8` / `hash_string16`, `hash_string`,
`hash_string_rope`, `js_atom_hash_str`, `js_atom_hash_ascii`, `__JS_NewAtom`,
`__JS_FindAtom`, `map_hash_key`, `JS_NewRuntimeKeyed`, `JS_NewRuntime2`,
`json_parse_value`; test hooks `JS_TestForceAtomHash`, `JS_TestAtomHash`,
`JS_TestAtomWidths` under `QJS_HULL_TEST_HOOKS`), `quickjs.h`
(`JS_HASH_KEY_SIZE`, `JS_NewRuntimeKeyed`, the test-hook prototypes)
**Found by:** audit 40
**Upstream:** Hull-specific.

Upstream hashes every atom string (property names, identifiers, JSON keys)
and every Map / Set string key with a fixed multiply-add, `h = h * 263 + c`,
seeded only by the atom type, and walks a bucket chain charging nothing. The
bucket of every string is therefore known in advance, and a chain walk is work
no step sees.

The patch:

- **Keys the string hash.** It is SipHash-1-3 under a 128-bit key held in
  `JSRuntime.hash_key`, the seed (the atom type, or 0 for a Map key) folded
  into the first key word. The input is the string's 16-bit code units,
  little-endian, so an 8-bit and a 16-bit string with the same text hash
  identically (atoms are compared across widths), and the state streams, so a
  rope hashes exactly as its flat text does. The atom hash keeps the 30 bits
  `JSString.hash` stores (`& JS_ATOM_HASH_MASK`); string atoms reserve no
  value (the reserved ones, 0 and `JS_ATOM_HASH_PRIVATE`, are meanings of the
  field for SYMBOL atoms, which are not hashed).
- **Sets the key before the predefined atoms exist.** `JS_NewRuntimeKeyed(mf,
  opaque, key)` stores the key before `JS_InitAtoms`; `JS_NewRuntime2` (and so
  `JS_NewRuntime`) is it with a NULL key, all zero - SipHash still, but
  unkeyed. Hull makes the main runtime and every worker.dispatch VM through
  `hl_js_new_runtime_keyed` (`runtime/js/runtime.c`), which draws the key from
  `hl_cap_crypto_random` and fails runtime init when that fails. The JS
  source frontend's tooling session (`frontend/js_session.c`) stays unkeyed.
- **Keys the Map / Set hash** for string keys (flat and rope), and also for
  number keys (one SipHash block over the float64 bits; ints are hashed as
  their double, so `1` and `1.0` still meet) and BigInt keys (one block per
  limb, charged per limb), which used the same unkeyed polynomial / multiply.
  Object and symbol keys hash their pointer, which a script does not choose.
- **Charges the chain walk.** `__JS_NewAtom` and `__JS_FindAtom` add
  `JS_WORK_ELEM` (one instruction) per chain step plus the bytes compared,
  through patch 0005's work charge, once the walk ends.
- **Polls in JSON.parse.** `json_parse_value` calls `js_poll_interrupts` once
  per object property and array element, so a tripped work charge (patch
  0005) or the run watchdog's stop flag (patch 0006) ends one long parse
  partway, uncatchably (patch 0003), instead of after it returns.

Considered and left: the **shape hash** (`shape_hash`, `get_shape_hash`). Its
inputs are object-pointer values and atom indexes the runtime assigns, not
bytes a script supplies, and a seed in a multiply-add would not be a key; a
keyed mix there would cost every property addition.

Not affected: bytecode (`JS_WriteObject` / `JS_ReadObject`) carries atoms as
strings and predefined atoms by index, never a hash, so code written by one
runtime (one key) loads in another and Hull's sealed bytecode cache is
unchanged; `JS_ResizeAtomHash` re-buckets by the stored `p->hash`, which is
the keyed value.

Lua needs no twin: Lua 5.4 seeds its string hash per state
(`luai_makeseed` in `vendor/lua/lstate.c`: the state's address, a stack
address, a function address and the time), and Hull does not override it.

**Test hooks** (compiled only into `build/qjs_quickjs_testhooks.o`, which only
`test_js` links; `mk/tests.mk`): `JS_TestForceAtomHash(rt, on)` makes every
atom hash to one constant (rehashing and re-bucketing the atoms already made,
so lookups keep working), `JS_TestAtomHash` reads one hash, and
`JS_TestAtomWidths` checks an 8-bit and a 16-bit copy of a text agree.

**Re-apply on upgrade:** replace the string hash functions with the keyed
`JSSipState` ones (every caller of `hash_string*` takes the runtime), keep the
key copy before `JS_InitAtoms` in the runtime constructor, re-add the chain
charge in the two atom-table walks and the two `js_poll_interrupts` calls at
the top of `json_parse_value`'s object and array loops, and re-key
`map_hash_key`'s string / number / BigInt cases. Check that nothing new stores
or serialises a string hash.

**Guard:** `tests/hull/runtime/js/test_js.c`, `js_audit40.*` (two runtimes
hash a text differently; 8-bit and 16-bit storage share an atom; bytecode and
the sealed module cache cross keyed runtimes; under `JS_TestForceAtomHash` a
JSON.parse of 3000 ordinary keys trips a 1M budget uncatchably and, with no
limit, the run deadline stops one of 60000; ordinary large parses fit the
default budget), and `lua_audit40.*` in `test_lua.c` (two Lua states order
string keys differently).
