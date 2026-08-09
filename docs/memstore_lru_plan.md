# In-stdlib O(1) LRU for `_memstore` — design + benchmark plan (no implementation)

**Status:** PLAN ONLY. No code until this plan's semantic-parity and
memory-impact review is approved. This is the sanctioned follow-up from the
concluded native-cache-store experiment
([kvmem_negative_result.md](kvmem_negative_result.md)): fix the ONE workload that
experiment improved (eviction-heavy, 30-90×) at its true root — an O(n) victim
scan that lives entirely inside the interpreter — WITHOUT paying the
scripting-boundary copy or the memory overhead that sank kvmem.

Scope: `stdlib/lua/hull/kv/_memstore.lua` and `stdlib/js/hull/kv/_memstore.js`
only. No C, no bindings, no capability change, no manifest/gate change. The
`_memstore` is the semantic oracle both `hull/kv` (memory) and `hull/cache`
(memory) depend on and that the retired kvmem was diffed against, so the bar is
**observational identity with today's `_memstore`**, plus a strict eviction
speedup and no memory regression.

## 1. The defect (exact, from the current code)

Both files are structurally identical. The victim search in `make_room`:

```lua
-- stdlib/lua/hull/kv/_memstore.lua:83-91  (JS: _memstore.js:55-64, identical)
while over() and self.items > 0 do
    local lru_k, lru_seq
    for k, e in pairs(self.data) do                 -- O(items) FULL SCAN
        if not lru_seq or e.seq < lru_seq then lru_k, lru_seq = k, e.seq end
    end
    if not lru_k then break end
    drop(self, lru_k, self.data[lru_k])
    self.st.evictions = self.st.evictions + 1
end
```

Recency is tracked by a monotonic `seq` counter: `get` (hit), `put` (new or
overwrite), and `incr` (existing) each do `self.seq = self.seq + 1; e.seq =
self.seq`. The LRU victim is the entry with the lowest `seq`. Finding it is an
O(items) scan, and under sustained over-cap inserts eviction fires on nearly
every insert, so the store degrades to O(n) per operation. This is the entire
performance gap; nothing else in `_memstore` is worse than O(1) amortized.

Everything ELSE `_memstore` does (values, TTL `exp`, byte/item accounting,
`stats`, `scan` order, error codes) is already O(1) per op and is **out of scope
to change**.

## 2. The fix: an intrusive doubly-linked LRU threaded through the entries

Keep a doubly-linked recency list whose nodes ARE the existing entry records (no
separate node objects, so no new per-entry allocation). The store gains `head`
(most-recently-used) and `tail` (least-recently-used) references. Each entry
record gains `prev`, `next`, and a self `k` (its own key, so the tail node can
delete itself from `data`). The `seq` field and the counter are **removed** —
list position replaces them.

- **touch (move-to-head):** on `get`-hit / `put` (new or overwrite) / `incr`
  (existing) — unlink the node from its current position and splice at head. O(1).
- **evict (unlink-tail):** `make_room` drops `self.tail` repeatedly until within
  caps. O(1) per eviction; total eviction work is O(evicted), not O(items).
- **drop / lazy-expiry / del / clear:** every existing `drop` also unlinks the
  node; `clear` resets `head=tail=nil`. O(1).

Per-entry record shape (conceptual fields; the PHYSICAL layout is
runtime-specific and chosen for memory in §6 — Lua uses positional/array-part
records, not named fields):

| today | after |
|-------|-------|
| `v, exp, bytes, seq` | `v, exp, bytes, k, prev, next` |

Net: remove one integer (`seq`), add one string ref (`k`) and two node refs
(`prev`,`next`). No new heap objects per entry. But field COUNT and per-runtime
storage are not free — see §6, where the naive named-field layout is shown to
fail the Lua RSS gate and a positional layout is required.

**Gate the list on `evict`.** A `hull/kv` memory store has `evict=false` and
NEVER evicts, so recency tracking is pure waste there. When `evict=false`, skip
ALL list maintenance AND the recency bump entirely: `get` becomes `live + stats`
(strictly less work than today's unconditional `seq` bump). The list exists only
for `evict=true` (cache) stores, exactly where it pays off. This is what keeps
the get/set workloads from regressing (§5) and even speeds up the KV path.

## 3. Semantic parity (Lua and QuickJS) — the correctness argument

The redesign must be **observationally indistinguishable** from today's
`_memstore` on every method's return value AND on `stats` AND on which keys
survive eviction. The parity argument, claim by claim:

1. **Victim identity is preserved.** Today's victim = the live entry with the
   minimum `seq` = the entry touched least recently, where "touch" is exactly
   {get-hit, put-new, put-overwrite, incr-existing}. An intrusive list that
   moves a node to head on exactly those same events has, at its tail, precisely
   the least-recently-touched entry. Since `seq` is strictly monotonic and unique
   per touch, the min is unique and equals the tail — no ties, no ambiguity. The
   list therefore evicts the SAME key the scan would, every time. `stats.evictions`
   and the surviving key set are identical.
2. **Non-touching operations stay non-touching.** `has`, `scan`, `cleanup`, a
   `get` MISS, and a `get` on an expired key do NOT bump `seq` today, so they must
   NOT move-to-head. (Concretely: `has` calls `live` but not the bump; a miss
   returns before the bump.) The plan preserves this exactly.
3. **Overwrite ordering is preserved.** Today `put` on an existing key bumps `seq`
   (MRU) FIRST, then calls `make_room(delta)`, so the key being written can never
   be its own victim. The list does move-to-head FIRST, then evict-from-tail — the
   just-promoted node is at head, unreachable as the tail victim. Identical
   protection. `incr` on an existing key has the same shape and the same fix.
4. **New-put ordering is preserved.** Today `make_room(nb, true)` runs BEFORE the
   new entry is inserted, so a fresh key can't evict itself. The list evicts from
   tail until room, THEN links the new node at head. Identical. The "value larger
   than the whole budget" path (evict everything, still over → raise
   `capacity_exceeded`) is reproduced by evicting until `items==0` then raising.
5. **`make_room` still ignores TTL when choosing a victim.** Today the scan does
   NOT skip expired-but-unswept entries; it evicts purely by `seq`. An expired
   entry is evicted only if it happens to be the LRU, and it is counted as an
   `eviction`, not an `expiration`. The list must NOT "optimize" by preferring
   expired entries — evicting strictly from tail reproduces the current
   eviction/expiration split exactly. (Explicitly called out because it is the
   easiest place to accidentally diverge.)
6. **`scan` order is untouched.** `scan` iterates `data` (Lua `pairs`, JS Map
   insertion order), NOT the recency list, and the plan keeps it that way. The
   list is never walked for `scan`, so scan output order is byte-identical.
7. **Lua/JS mirror each other.** Both files already implement the same algorithm
   line-for-line; the list is added identically (Lua table fields vs JS object
   properties; `data` stays a Lua table / JS `Map`). The cross-runtime kv/cache
   e2e already asserts Lua == JS; it must stay green.

**Verification (differential against the PRE-CHANGE oracle).** No implementation
or prototype exists yet; everything below is the verification the implementation
PR WILL run, not evidence already collected. Because we are changing the oracle
itself, the review needs the OLD behavior captured:

- **Golden-trace differential (planned):** vendor a frozen copy of today's
  `_memstore` (both runtimes) into the test tree as `_memstore_ref`. A property
  test will drive a long randomized op sequence (set/get/del/incr/cas/scan/clear,
  random keys, values, TTLs, and cap policies with `evict` both true and false)
  through BOTH the frozen reference and the redesigned store in one process and
  will assert, after every op: identical return value, identical `stats()`
  vector, identical live key set, and identical `scan` output. Same shape as the
  retired kvmem conformance oracle, now aimed at old-vs-new `_memstore`.
- **Independent victim check (planned):** a second assertion will recompute the
  expected LRU victim from a shadow `seq` clock and assert the redesigned store
  evicted that exact key — intended to prove claim (1) directly, not just
  transitively.
- **Existing suites (planned):** all current kv/cache unit tests and the `e2e_*`
  kv/cache scripts must pass with zero edits (any required edit is a parity break
  and blocks the change).
- The differential will run under ASan/UBSan (Lua) and the JS test runner; both
  runtimes.

## 4. Shared-namespace behavior

`REGISTRY[namespace] -> Store` is unchanged: two `get(ns)` calls in one process
return the SAME `Store`, and policy is fixed by the first open. The recency list
is per-`Store` state, so shared handles share ONE list — a `get` through handle A
promotes the node that handle B also sees as MRU. This matches today (handles
already share `data`, counters, and `stats`); the list is simply one more shared
field. No new cross-handle observable: the review item is to assert that a
promote/evict driven through handle A is reflected in handle B's subsequent
`stats`/eviction victims, and that opening the same namespace twice does not
double-maintain or fork the list. `_forget(namespace)` (the test/lifecycle
helper) drops the Store and with it the list.

## 5. Mixed-workload regression risk (the thing to prove does NOT regress)

Adding list maintenance to `get`/`put` costs a few pointer writes per touch. The
risk is a slowdown on get/set-heavy traffic. Controls:

- **`evict=false` (hull/kv memory):** no list, no recency bump at all → `get`/`put`
  do strictly LESS than today (today bumps `seq` unconditionally). Expect
  neutral-to-faster. The retired benchmark's mixed/ttl workloads ran on
  `evict=false`, so on those exact workloads this redesign cannot regress.
- **`evict=true` (hull/cache memory):** `get`-hit and `put` now do an
  unlink+splice-at-head (≈4-6 field writes) instead of two `seq` writes. This is
  the one place a small constant slowdown is possible. It must be MEASURED, not
  assumed. Acceptance bar for the review: get/set-heavy throughput on an
  `evict=true` store stays within a small margin of today's `_memstore` (target:
  no worse than ~10%, and ideally within noise), while eviction-heavy improves by
  the large factor in §6. If get-heavy on a cache store regresses beyond the
  margin, the change does not proceed.

Both must be benchmarked per runtime (Lua and QuickJS), never blended.

> **Measured outcome + fix.** The eager design DID regress the `evict=true`
> get/set path beyond the margin (mixed Lua +60%/+85%, JS +27%/+38%; ttl Lua
> +49%, JS +31%). §10 amends the design to a **lazily-activated exact LRU** that
> keeps the cheap `seq` path until an eviction actually occurs, directly removing
> this overhead from the fitting-cache case without changing any numeric gate.

## 6. Memory overhead

The list is threaded through the existing entry records (`prev`/`next` are
references to OTHER entry records, not new node wrappers), so there are ZERO new
heap objects per `put`. But the per-record field COUNT and how the runtime stores
those fields are not free, and the two runtimes behave very differently. The
naive "+2 named fields" framing is wrong for Lua; the layout is
runtime-specific and its RSS impact must be MEASURED per runtime, not assumed.

### 6.1 Lua — positional (array-part) entry records, NOT named fields

Measured, `collectgarbage("count")`, 40k entries, arm64 Darwin (this measurement
motivates the layout; the implementation PR re-measures at 1e6 for the gate):

| entry layout | bytes/entry | vs today |
|--------------|-------------|----------|
| named ×4 `{v, exp, bytes, seq}` (today) | 224 | — |
| named ×6 `{v, exp, bytes, k, prev, next}` (naive) | 320 | **+43%** |
| positional ×6 `[v, exp, bytes, k, prev, next]` | 178 | **−21%** |

Lua rounds a table's HASH part to a power of two, so a 4-named-field record
(hash size 4) becomes hash size 8 the moment it carries 5-6 named fields — +4
`Node` slots, ≈ +43% per entry, which fails the §7 RSS gate outright. A record
whose fields live in the ARRAY part (integer indices `1..6`) is a flat `TValue`
vector with no `Node` overhead and is LEANER than today's named ×4. So the Lua
entry record MUST be positional.

**Positional records must not become magic-number code.** The implementation is
required to define named index constants and small accessor/list helpers so the
positional layout stays readable, and to carry the original field semantics in
comments. Illustrative (implementation detail, not final code):

```lua
-- Entry record is a positional array; indices are named to preserve the
-- semantics the old named fields carried. Fields in the ARRAY part avoid Lua's
-- power-of-two hash-node overhead (docs/memstore_lru_plan.md §6.1).
local E_V, E_EXP, E_BYTES, E_K, E_PREV, E_NEXT = 1, 2, 3, 4, 5, 6
-- E_V     value bytes           (was e.v)
-- E_EXP   expiry ms | false     (was e.exp; false, not nil, so the slot exists)
-- E_BYTES accounted byte size   (was e.bytes)
-- E_K     the entry's own key   (new: lets a tail node delete itself from data)
-- E_PREV  more-recently-used nb (new: intrusive LRU link; false at head)
-- E_NEXT  less-recently-used nb (new: intrusive LRU link; false at tail)

-- List helpers keep splice/unlink in one audited place, never inline.
local function lru_unlink(s, e) ... end       -- O(1)
local function lru_push_head(s, e) ... end     -- O(1)
local function lru_touch(s, e) ... end         -- unlink + push_head
```

Note `exp` moves from `nil` to `false` when absent so the array slot always
exists (a `nil` in the middle of a Lua array truncates it). Accessors read
`e[E_EXP]` and treat `false` as "no expiry", preserving today's `exp == nil`
semantics exactly.

### 6.2 QuickJS — shape/inline-property assumptions, measured separately

QuickJS stores an object's properties in a shape (hidden class) shared by all
records of the same construction, with values in a contiguous inline array. Two
extra properties (dropping `seq`, adding `k`/`prev`/`next` → net +2) grow that
inline array by a small constant per object with no power-of-two jump, so the
relative hit is far smaller than Lua's naive case — but it is NOT zero and is NOT
assumed. Requirements: (a) construct every entry with the SAME property insertion
order so all records share one shape (a mixed order forks shapes and inflates
memory); (b) use `false`, not `undefined`, for empty `exp`/`prev`/`next` so the
slot is a stable own-property; (c) a positional JS array `[v, exp, bytes, k,
prev, next]` is an acceptable alternative if it measures leaner — decided by
measurement, not assumed. QuickJS is expected to be lower-risk than Lua, but it
has its OWN measured RSS gate (§7) and its own layout decision.

### 6.3 Store-level and leak guard

- Store-level: two references (`head`, `tail`). Negligible.
- This is the decisive contrast with kvmem, which added separate key+value
  mallocs (macOS `malloc` overhead ~16-32 B each) plus a Robin Hood table and
  cost +64% RSS. Here the backing structure (Lua table / JS `Map`) is unchanged
  and the Lua record gets LEANER; the goal is RSS at or below today, per runtime.
- **Leak guard:** an evicted / expired / deleted node's `prev`/`next` (and the
  store's `head`/`tail`) must hold no reference to it after drop, or it cannot be
  collected. The planned differential fuzz (§3) plus an explicit
  "evict-then-assert-collected" check will guard this (see §8).

## 7. Benchmark + RSS measurement plan

### 7.1 De-native-ized harness (frozen reference vs candidate)

The retired harness (`tests/fixtures/kvmem_bench/`, preserved on
`feat/kvmem-native`) compared a C `nativemem` store against `_memstore`. Reusing
it here requires REMOVING every native-backend assumption, or the numbers are
invalid:

- **Drop the native paths entirely:** the `nativemem.available` guard,
  `nativemem.get`, and — critically on the JS side — the `_nativemem.js`
  `ArrayBuffer` `toBuf`/`fromBuf` conversion. Both sides of THIS comparison are
  pure-interpreter byte-string stores; keeping the `ArrayBuffer` conversion would
  charge a boundary tax that `_memstore` never pays and mismeasure the candidate.
- **Compare frozen `_memstore_ref` vs the candidate `_memstore`,** both pure
  Lua/JS, loaded in one process. `_memstore_ref` is a byte-frozen copy of
  today's implementation pinned to a recorded baseline commit (§7.4).
- **Identical inputs and IDENTICAL operation counts** for both sides per cell.
  The native era used ASYMMETRIC op counts (native got ~5× the ops because it was
  faster and to fit the instruction budget); that hack is wrong when both stores
  are the same speed class. Same keys (pre-built arrays), same values, same op
  sequence, same count → a clean throughput ratio. Rename the `ref()`/`nat()`
  helpers to `old()`/`new()` over the two `_memstore` versions.

### 7.2 Throughput methodology (isolation, warm-up, repetition, variance)

One workload per process (the direct `hull app.lua` run path pins the 100M
instruction limit; keys pre-built into arrays; see kvmem_negative_result.md
§ Methodology). For every cell, per store, per runtime:

- **Each repetition runs in a FRESH Hull process** — never an in-process loop.
  Twelve executions cannot share one process without exceeding the 100M
  instruction budget, so the harness DRIVER performs 12 process invocations per
  cell, discards the first two as warm-up, validates that each of the remaining
  runs completed its full declared operation count, and reports the median and
  spread from the ten measured runs.
- **Warm-up:** the first 2 of the 12 invocations are discarded (warm the
  allocator / page cache) before timing.
- **Repetition:** 10 measured invocations per cell (the 3rd through 12th).
- **Forced GC:** `collectgarbage("collect")` twice (Lua) / best-effort GC (JS)
  immediately before each timed span, so a GC pause inside timing is minimized;
  report median to absorb the residual.
- **Report median PLUS spread:** median ops/ms and min-max (or stddev) across the
  10 reps — never a single number. Speedup per cell = median(new) / median(old),
  with both spreads shown.
- **Hard failure on truncation / incomplete op counts:** each run asserts it
  executed the FULL declared op count and populated the FULL declared key count;
  if a run hits the instruction limit or otherwise completes fewer ops/entries
  than declared, it is a HARD ERROR (non-zero exit, cell marked failed), NEVER a
  silently-reported partial number. (A silent partial run undercounted the
  pure-Lua baseline in the retired kvmem RSS measurement — this guard exists
  specifically to prevent that.)

Workload matrix (each measured old vs new, per runtime):

| Workload | Store policy | What it proves |
|----------|--------------|----------------|
| mixed get/set (80/20 and 50/50) | `evict=false` | KV path: no regression (expect neutral-to-faster) |
| mixed get/set (80/20 and 50/50) | `evict=true`  | cache path: the §5 regression bound (the one at-risk case) |
| ttl-churn | `evict=false` and `evict=true` | expiry bookkeeping unaffected; no regression |
| eviction-heavy (cap 500, sustained over-cap inserts) | `evict=true` | the target win: O(n)→O(1) |

Sizes: 1e3 / 1e5 / 1e6 keys, small and large values (per kvmem_design.md §11).

### 7.3 Resident memory (measured separately per runtime AND policy)

`mem.lua`-style populate of 1e6 small entries, `/usr/bin/time -l` peak RSS, one
store per process, `_memstore_ref` vs candidate. RSS is measured and gated
SEPARATELY for **each runtime (Lua, QuickJS) × each policy (`evict=false`,
`evict=true`)** — four independent RSS gates, never a single blended figure
(Lua's positional layout and QuickJS's shape layout have different memory
profiles, §6). Each populate run carries the same truncation / incomplete-count
hard-fail guard as §7.2: a run that does not populate the full 1e6 entries is a
hard error, not a partial RSS number.

### 7.4 Baseline pinning

Record, in the implementation PR: the `_memstore_ref` source commit (the `main`
SHA the frozen copy was taken from), the `hull` build config (flags), the
platform, and the tool versions. All old-vs-new numbers are reported against that
pinned baseline so a later reader can reproduce the exact comparison.

**Ship gates for the eventual implementation review (numeric; all required).**
Measured on arm64 Darwin, the redesigned `_memstore` vs today's `_memstore`,
each runtime measured separately and never blended. **These gates are UNCHANGED
by the §10 amendment** — the lazy-activation design must clear every one of them
exactly as written:

1. **Semantic differential — ZERO mismatches.** The golden-trace differential
   (§3) reports 0 observable differences vs the frozen reference across the full
   randomized op sequence (return values, `stats()`, live key set, `scan` output),
   for both Lua and QuickJS.
2. **Mixed workload — ≤ 10% regression per runtime.** Redesigned throughput is no
   more than 10% below today's `_memstore` on the mixed get/set workload (both
   80/20 and 50/50), for `evict=false` AND `evict=true`, Lua and QuickJS each.
3. **TTL-churn workload — ≤ 10% regression per runtime.** Same 10% bound on the
   ttl-churn workload, both policies, each runtime.
4. **Eviction-heavy — material improvement, ≥ 5×.** Redesigned throughput on the
   eviction-heavy workload (`evict=true`, sustained over-cap inserts) is at least
   5× today's `_memstore`, each runtime (this is the O(n)→O(1) payoff; the retired
   kvmem interpreter-side eviction wins were far higher, so 5× is a floor).
5. **Peak RSS — ≤ 10% above today, each of four (runtime × policy) measured
   independently.** Redesigned peak RSS (1e6 small entries, `/usr/bin/time -l`,
   §7.3) is no more than 10% above the pinned `_memstore_ref` in EACH of the four
   cells — {Lua, QuickJS} × {`evict=false`, `evict=true`} — never a blended
   figure. A failure in any one cell blocks the change.
6. **Existing KV/cache tests — ZERO regressions.** Every current kv/cache unit
   test and `e2e_*` kv/cache script passes with no edits.
7. **No new surface.** No new C, no new dependency, no capability/manifest/gate
   change; the pure `_memstore` remains the sole memory backend.

If any gate fails at implementation-review time, the redesign does not land and
today's `_memstore` stays as-is.

## 8. Implementation-review checklist (list-integrity edge cases)

Every item below must be exercised by a named test and confirmed at review. These
are the places a doubly-linked list most easily corrupts silently:

- **Unlink on lazy TTL expiry.** When `live()` finds an expired entry and calls
  `drop`, the node must be unlinked (not just removed from `data`). Test: expire
  a middle-of-list node, then drive eviction and assert the list has no dangling
  `prev`/`next` into the dropped node and the victim order is still correct.
- **Unlink on `del`.** `del` of a head node, a tail node, and a middle node each
  keep `head`/`tail` and both neighbors consistent. Test all three positions.
- **Unlink on `cleanup()` (eager sweep).** `cleanup` is a multi-`drop` sweep that
  removes an arbitrary, SCATTERED subset of nodes (every expired one) in a single
  pass. Test: expire a mix of head, tail, middle, and adjacent nodes, run
  `cleanup`, and assert the surviving list is fully coherent (`head`/`tail`
  correct, every survivor reachable both directions, no link into a swept node)
  and that a subsequent eviction picks the correct victim. This shares the `drop`
  unlink path with expiry/`del`/eviction, but its many-at-once scattered removal
  is a distinct failure surface.
- **Unlink + reset on `clear`.** `clear` empties `data`, zeroes counters, AND
  sets `head=tail=nil`. Test: `clear` then `put` and assert a fresh singleton
  list (not a stale head/tail into freed entries).
- **Repeated multi-victim eviction.** A single `put`/`incr` whose size forces
  MORE THAN ONE tail eviction in one `make_room` call walks the tail correctly N
  times (each unlink advances `tail` to its `prev`), stops exactly when within
  caps, and raises `capacity_exceeded` only when the store empties still over
  budget. Test with both `max_items` and `max_bytes` caps, and the
  "value-larger-than-budget" empties-the-store case.
- **Head/tail singleton transitions.** Correctness at the boundaries: empty list
  → first insert (`head==tail==node`, `prev==next==nil`); singleton → second
  insert; two nodes → delete one (survivor becomes `head==tail`); singleton →
  delete/evict/expire the only node (`head==tail==nil`). A move-to-head of the
  sole node, and of the current head (a no-op that must not corrupt links), are
  both covered.
- **Shared-handle touches.** With two `get(ns)` handles on one `Store`: a
  `get`/`put` promote through handle A is observed by handle B (its next eviction
  victim reflects A's promote), and interleaved touches through both handles keep
  a single coherent list (no fork, no double-maintenance). Assert via the shared
  store's `stats` and eviction victims.
- **Move-to-head of an already-head node** and **of the tail node** each leave a
  well-formed list (the two most common off-by-one splice bugs).
- **No retained reference after drop.** An evicted/expired/deleted node's
  `prev`/`next` (and the store's pointers) hold no reference to it, so it is
  collectable (the §6 leak guard).

## 9. Explicitly out of scope

- Any C, native store, binding, or `--with` packaging (that path is concluded —
  kvmem_negative_result.md).
- A QuickJS string fast-path or a C memory-layout redesign for kvmem — barred by
  the concluded experiment unless this stdlib O(1) LRU is first shown unable to
  meet the eviction requirement (it is expected to meet it).
- Any change to `scan` order, TTL semantics, byte accounting, error codes,
  capability advertisement, or the shared-namespace/registry contract.

## 10. Amendment: lazily-activated exact LRU (targets the measured §5 failure)

**Why this amendment.** The eager design above (maintain the intrusive list on
every touch of an `evict=true` store) was implemented and measured against the
full matrix. It PASSED the semantic differential (0 mismatches), the eviction
gate (Lua 11.7x, JS 26.4x), and every RSS cell (0.99-1.00x, the positional /
stable-shape layout held). It FAILED Gates 2/3 on the `evict=true` cache policy:
mixed Lua +60%/+85%, JS +27%/+38%; ttl Lua +49%, JS +31%. Root cause is
structural: an exact O(1) LRU does a move-to-head (~6 field writes) on EVERY
touch, versus the old store's single cheap `seq` bump, and on a cache whose
working set FITS (no eviction pressure) that is pure overhead with no offsetting
benefit. The list only earns its cost under sustained eviction.

This amendment removes that overhead from the fitting-cache case WITHOUT changing
any gate and without changing observable behavior: keep the old cheap `seq`
behavior until an eviction actually occurs, and only then pay a one-time cost to
switch the store to intrusive-list mode. **No numeric gate changes** (§7 stands
verbatim); the differential oracle is unchanged; this is purely a
performance-mode transition invisible to callers.

### 10.1 Two modes, one store

A store is in one of two recency modes:

- **SEQ mode (inactive, the default).** Exactly today's `_memstore`: a monotonic
  `seq` counter, bumped on each touch (`get`-hit, `put` new/overwrite, `incr`
  existing); NO links maintained. `del` and lazy-TTL `drop` are seq-only (there
  is no list to unlink). A store with `evict=false` NEVER leaves this mode (it
  never evicts), so the KV path is byte-for-byte today's behavior. A cache with
  headroom stays here too, for as long as it never actually evicts.
- **LIST mode (active).** The intrusive doubly-linked O(1) LRU of §2/§6: touch =
  move-to-head, eviction = tail unlink, `drop` unlinks. Once active, a store
  stays active until it empties or is `clear()`ed (§10.4).

The transition SEQ -> LIST happens exactly once, lazily, at the FIRST real
capacity eviction (§10.2). Fitting-cache mixed/TTL workloads never trigger it and
so keep the low-cost touch path; eviction-heavy workloads pay the one-time
ordering cost and then receive O(1) eviction.

### 10.2 Activation: reconstruct exact order from the unique `seq` values

Activation is invoked from inside `make_room`, at the point it determines a real
eviction is required (`over()` is true, `evict` is true, `items > 0`) and the
store is still inactive. It reconstructs the exact LRU order the seq counter
already encodes, then flips the store to LIST mode; `make_room` then evicts the
tail as usual.

Correctness of the reconstruction: today's oracle evicts the minimum-`seq` entry;
sorting the live entries by `seq` and making highest-`seq` the head / lowest the
tail yields a list whose tail is that same minimum-`seq` entry, and whose order
is the exact access-recency order. Because every touch bumps `seq` uniquely and
monotonically, the order is total and unambiguous, so the first list-mode victim
and every subsequent one match the oracle's min-`seq` selection (the same parity
argument as §3, now established once at activation and preserved thereafter).

### 10.3 Building the order with no large temporary allocation

The RSS gate (§7 gate 5) must stay meaningful, so activation must not allocate an
O(n) temporary (e.g. an array of all entries to `table.sort`). Instead it uses an
**intrusive stable merge sort over the existing link fields** — zero heap growth
beyond a handful of local pointers:

- The entry record keeps its 6 positional slots (§6.1); slots 5 and 6 are
  dual-purpose. In SEQ mode slot 5 (`E_SEQ`, aliasing `E_PREV`) holds the seq and
  slot 6 (`E_NEXT`) is `false`/unused. In LIST mode slot 5 is `prev` and slot 6
  is `next`. So the record size — and thus RSS — is identical in both modes and
  identical to the already-measured layout.
- **Collect:** iterate `data` once and thread every live entry onto a singly-
  linked list via slot 6 (`E_NEXT`), reading each entry's seq from slot 5. This
  writes only slot 6 (the unused slot); slot 5 (`seq`) is left intact.
- **Sort:** bottom-up (or top-down) stable merge sort of that singly-linked list
  by seq, using slot 6 as the "next" pointer. Merge sort on a linked list needs
  only O(log n) / O(1) auxiliary pointers — no per-entry temporary. Stable so
  that any equal keys (which cannot occur pre-rollover, §10.6) keep a defined
  order rather than a hash-iteration-dependent one.
- **Commit:** walk the sorted singly-linked list once and write the doubly-linked
  `prev`/`next` into slots 5 and 6 and set `head`/`tail`. This is the point slot 5
  changes from `seq` to `prev`; the seq value is no longer needed. Commit is O(n)
  pointer writes, allocation-free.

### 10.4 `clear()` and empty transitions reset to inactive

`clear()` empties `data`, zeroes the counters, sets `head=tail` empty, resets the
seq counter, and sets the store back to **SEQ mode** — a cleared store is a fresh
store and may lazily re-activate on a future eviction (§10.9 covers this in the
differential). A store that drains to zero items (via `del` / expiry) likewise
returns to SEQ mode, since there is no order left to maintain and the next fill
should get the cheap path until it again evicts. (Reactivation simply repeats
§10.2 when the next real eviction arrives.)

### 10.5 Pre- vs post-activation removal paths

- **Before activation:** `del` and lazy-TTL `drop` are seq-only — remove from
  `data`, adjust counters, do NOT touch links (there are none). Identical to
  today's oracle.
- **After activation:** `del`, lazy-TTL `drop`, `cleanup`, and eviction all
  unlink the node from the list (§2), O(1).

The `drop` helper therefore branches on the store's active flag: unlink only when
active. This keeps the §8 list-integrity obligations but scopes them to the
active state.

### 10.6 Counter rollover handling

`seq` is consulted only ONCE, at activation, to reconstruct order; after
activation the store never reads it again. Rollover / precision loss (Lua 5.4
integers wrap at 2^63; JS `Number` loses integer uniqueness past 2^53) can only
matter if a store performs that many touches while STILL inactive — i.e. an
`evict=true` store that never evicts across 2^53+ operations, or an `evict=false`
store (which never consults seq at all, so rollover is harmless there). To keep
uniqueness guaranteed at activation, the store treats a `seq` that reaches a
safe threshold (`SEQ_SAFE_MAX`, well below 2^53) as a trigger to **force
activation early** (run §10.2 now, from the still-unique seqs, and continue in
LIST mode, which does not use seq). This bounds `seq` and makes non-unique seqs
unreachable at activation. The differential exercises this with a lowered
test-only threshold (§10.9).

### 10.7 Shared handles observe one activation state

Activation state (`active`, `head`, `tail`, `seq`) lives on the single per-
namespace `Store`, exactly like `data` and the counters. Two `get(ns)` handles
share it: an eviction triggered through handle A that activates the store is
immediately observed by handle B (its subsequent touches take the list-mode
path). There is no per-handle mode. §4's shared-namespace contract is unchanged.

### 10.8 Activation is atomic; failure leaves the store and write unchanged

Activation must be all-or-nothing so a hypothetical failure cannot corrupt the
store or partially apply the triggering write. The structure above guarantees it:

- The collect + sort phases write only slot 6 (`E_NEXT`), which is unused in SEQ
  mode, and read slot 5 (`seq`) without modifying it. If any step raised before
  commit, the store is still a valid SEQ-mode store (its reads use slot 5;
  slot 6 is ignored in SEQ mode), `active` is still false, and no entry has been
  dropped. The build allocates nothing, so the realistic failure surface is
  empty; the invariant holds by construction regardless.
- `active`, `head`, and `tail` are assigned only in the final commit, which is
  allocation-free and cannot partially fail in a way that drops data.
- Because activation runs INSIDE `make_room` BEFORE any eviction `drop` and (for
  a new `put`) before the new entry is inserted, a raise propagates out of
  `make_room` -> `put`/`incr` with the store unchanged and the triggering write
  not applied (the same failure semantics as today's "value larger than budget"
  path). An overwrite that raises during its `make_room(delta)` leaves the old
  value in place (the value mutation happens only after `make_room` returns).

### 10.9 Added differential + integrity coverage (still 0 mismatches required)

The §3/§8 differential and integrity suites gain cases that specifically exercise
the transition, all asserted against the unchanged frozen oracle:

- **Activation boundary:** a workload that runs many touches with no eviction
  (stays SEQ) then crosses its cap once (activates), asserting the first
  post-activation victim and all subsequent ones equal the oracle's.
- **Multi-victim first eviction:** the activating `put`/`incr` is oversized so the
  first eviction removes several tail entries in one `make_room`; assert count and
  survivors match the oracle.
- **Expiry immediately before activation:** an entry lazily expires (seq-mode
  `drop`, no links) on the very operation that then triggers activation; assert
  the expired entry is excluded from the reconstructed order and the victim is
  correct.
- **Counter rollover:** with a lowered `SEQ_SAFE_MAX`, drive enough touches to
  cross it and force early activation; assert order is exact and parity holds.
- **clear / reactivation:** activate, `clear()` (back to SEQ), refill past the cap
  to reactivate; assert both activation episodes match the oracle and no stale
  links survive the clear.

### 10.10 Expected effect on the gates (to be MEASURED, not assumed)

Predicted, to be confirmed by re-running the §7 matrix on the amended
implementation:

- Gates 2/3 (mixed/ttl, `evict=true`, fitting working set): the store never
  activates, so it runs the cheap SEQ path == the oracle -> expected within the
  10% bound (near-parity), removing the +27%..+85% regression.
- Gate 4 (eviction-heavy): activates on the first eviction, pays the one-time
  O(n log n) sort, then O(1) evictions -> expected to keep the >=5x win (the
  one-time sort amortizes over sustained eviction).
- Gate 5 (RSS): entry record unchanged at 6 slots -> expected to stay 0.99-1.00x.
- Gate 1 / Gate 6: the transition is observably invisible -> expected 0
  mismatches and no existing-test regressions.

**Stop condition (unchanged discipline):** the amended implementation ships only
if EVERY numeric gate in §7 passes as written. If the lazy design still fails any
gate, the experiment is ARCHIVED as a second negative result (like
kvmem_negative_result.md); no third design is attempted.

## Related

- [kvmem_negative_result.md](kvmem_negative_result.md) — why the native store
  was concluded; this plan is its § Recommended path forward, elaborated.
- [kvmem_design.md](kvmem_design.md) — the concluded design spike (§11 workload
  matrix reused here).
- [kv_cache.md](kv_cache.md) — the `hull/kv` + `hull/cache` semantic layer.
