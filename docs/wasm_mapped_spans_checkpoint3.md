# Mapped spans — checkpoint 3 plan (public API, metadata delivery, SDK header)

**Status:** DONE (cut 1). This document was the plan; both cuts have shipped.
3a (windowed `fs.mmap`, named spans, `spans={}` bindings, invocation lifecycle
integration, the `SPAN_INFO` host-call — the D1/D2 locks below) merged via #313 /
#324 / #325 / #327 / #330 / #332 and the shared-heap AOT enablement (#326/#329);
3b (the `hull/wasm/span.h` SDK header shipped as `templates/hull_span.h`, the
`spanreader` example, header install/refresh, the interp+AOT e2e legs, and the
native-vs-WASM differential) merged via #324 and the follow-ups. The AOT span
lifecycle + differential are CI must-not-skip gates. Deferred, tracked, non-goals:
spans+segments composition in one call (D0.1 below), Memory64 span-metadata
dispatch (blocked on #318), and the orthogonal segment shared-heap descriptor leak
(#315, fixed separately). The turns-the-lifecycle-into-a-feature framing below is
retained as the design record.

**Was blocked on (resolved):** PR #309 (checkpoint 2) green + merged. This document
locked the two design decisions (§0) so 3a could start the instant #309 merged.

Reference: `docs/wasm_mapped_spans_design.md` (the WHAT — §§3.3, 3.5, 3.7),
`docs/wamr_shared_heap_guarded_subrange_design.md` (Design B / patch 0004),
`docs/wamr_shared_heap_destroy_design.md` (patch 0003).

## 0. The one hard constraint that drives the design

WAMR attaches **exactly one shared-heap chain per instance**: `e->shared_heap` is
a single pointer, and `wasm_runtime_attach_shared_heap_internal`
(`vendor/wamr/core/iwasm/common/wasm_memory.c`) returns `false` ("A shared heap is
already attached") if one is present. Today `compute.segment` chains are
module-scoped and re-attached each call (`cap/wasm.c:838`, via
`hl_wasm_attach_shared_heap`) and persist on pooled instances. Per-invocation span
chains are the opposite: they must **detach on every exit path** (the borrowed
buffers get released; the next pooled call has different spans or none) and must
**not** re-chain the module-shared segment chain (two concurrent instances
chaining onto one shared chain head is a data race on `chain_next`).

That constraint forces the two decisions below.

### D0.1 (LOCKED) — D1: spans and `compute.segment` are mutually exclusive per invocation

For cut 1, a single invocation uses **either** module segments **or** per-call
spans, never both:

- A `compute.call(..., {spans={...}})` on a module that currently has active
  `compute.segment` data (`mod->shared_data != NULL`) is **rejected** at the C
  layer with a distinct error (`"spans_with_segments"`), before any span is added
  or borrowed. The call does not run.
- A plain call (no `spans`) on a module that uses segments is **unaffected** — the
  existing segment path is byte-for-byte unchanged.
- The span set owns the **entire** per-call chain, attached fresh and torn down on
  every exit path (the checkpoint-2 lifecycle), so nothing touches the fragile
  module-shared segment chaining.

Rationale: the alternative (build a combined segment+span chain per call under
`mod->mutex`) would either serialize every call to the module for the whole WASM
execution or race the shared segment chain. Composition of spans + segments in one
call is a documented cut-1 **non-goal** (extends `wasm_mapped_spans_design.md` §6);
the structure allows adding it later behind a per-instance private-segment-copy
mechanism, out of scope here.

### D0.2 (LOCKED) — D2: index-based, versioned, self-identifying metadata record

`host_call`'s native signature is fixed at `(i32,i32,i32)->i32` — three data args,
un-growable without breaking every shipped plugin. So the §3.3 metadata query is
**index-based** and writes a **versioned, self-identifying, wire-explicit** record
into linear memory. The design's "resolve by name" (§3.3/§3.7) is realized
**SDK-side**: the record carries the span name, and `hull_span_setup()` reads each
record once at setup to build its own name→index map.

**Opcode:** `HL_WASM_OP_SPAN_INFO = 0x04` (new; extends the `host_call` opcode set
alongside `LOG=0x01`, `DATA_INFO=0x02`, `CALLBACK=0x10`, `STREAM=0x03`).
`tl_host_ctx` gains `const HlWasmSpanSet *spans` (an invocation-scoped snapshot,
mirroring `shared_data`).

**Wire ABI — `HlSpanMetaV1` (96 bytes, little-endian, packed, decode BY OFFSET).**
The guest MUST decode fields by explicit byte offset with `__builtin_memcpy` into a
local — never by casting linear memory to a native struct (alignment/aliasing UB,
and the wasm32/wasm64 host writes the same 64-bit-field layout regardless of guest
pointer width). All multi-byte integers are little-endian (WASM's native order).

| Offset | Size | Field         | Notes                                             |
|-------:|-----:|---------------|---------------------------------------------------|
| 0      | 2    | `version`     | `= 1`                                             |
| 2      | 2    | `struct_size` | `= 96` for v1 (the FULL required size)            |
| 4      | 4    | `flags`       | bit0 = read-only (cut 1: always set)              |
| 8      | 64   | `name[64]`    | NUL-terminated; matches `HlWasmSpan.name`         |
| 72     | 8    | `base`        | guest WASM address of the window (span `wasm_addr`)|
| 80     | 8    | `len`         | window length in bytes                            |
| 88     | 8    | `foffset`     | 64-bit logical file offset of `window[0]`         |

`base` is 64-bit on the wire on both targets; on wasm32 the host has validated
`base + len - 1` fits 32 bits before attach (design §3.3), so the guest uses the
low 32 bits. The 8-byte fields sit at 8-aligned offsets (8+64 = 72), so a
host-side `struct __attribute__((packed))` and a guest-side offset decoder agree.

**LOCKED call semantics** (`host_call(0x04, ptr, idx)` — `ptr` = record dest / `idx`
= span index):

- **Count:** `host_call(0x04, 0, -1)` returns the span count (`>= 0`). No record is
  written. (The sentinel index `-1` selects the count query; `ptr` is ignored.)
- **Valid index (`0 <= idx < count`) — the `cbSize` convention** (resolves
  "write the caller-supported size + return the full required size" against the
  fixed 3-arg `host_call`, since capacity cannot be a separate argument): the
  caller **pre-writes its own record capacity into the dest's `struct_size` field**
  (offset 2, a `uint16`) before the call. The host then: validates `[ptr, ptr+4)`
  and reads the advertised capacity `cap = dest.struct_size`; bounds `cap` to
  `[4, 4096]` (else malformed → `-1`); validates `[ptr, ptr+cap)`; writes
  `min(cap, sizeof(HlSpanMetaV1))` bytes of the record; and returns the **full
  required `struct_size`** (96 for v1). This never overruns an older/smaller
  caller buffer, and the return value always states the true record size:
  - a NEWER host (v2, `struct_size=128`) writing to an OLDER SDK (`cap=96`) writes
    only 96 bytes (a coherent v1 prefix — the v1 fields at fixed offsets never
    move; v2 only appends) and returns 128, so the old SDK learns a larger record
    exists without corruption;
  - a NEWER SDK (`cap=128`) reading an OLDER host (v1) sees the returned
    `struct_size = 96 < cap` and truncates its own read to the v1 fields.
  The SDK's `hull_span_setup()` sets `struct_size = HL_SPAN_META_V1_SIZE` before
  each query and validates `version`/`struct_size` on the result.
- **Out-of-range index (`idx >= count`, `idx < -1`):** returns `0` (no write). `0`
  is unambiguous vs. a successful write (which returns `struct_size >= 96`).
- **Invalid pointer / malformed query:** returns `-1` (**distinct negative**). This
  is ONLY: the initial `[ptr, ptr+4)` validate fails; OR the advertised `cap` is
  outside `[4, 4096]`; OR the `[ptr, ptr+cap)` validate fails. **There is no
  "undersized destination" error** — a `cap < 96` is a LEGAL prefix write (the
  cbSize rule above): the host writes `min(cap, required)` bytes and returns the
  full required size, never requiring 96 writable bytes. (The single validation
  rule is: validate 4 bytes → read `cap` → bound `cap` → validate exactly `cap`
  bytes → write `min(cap, required)`. The host NEVER validates or writes
  `sizeof(HlSpanMetaV1)` bytes unconditionally; that was an earlier contradictory
  draft and is removed.) Forward-compatible truncation is the SDK's job: the SDK
  MAY require the returned prefix to cover every v1 field it consumes, and errors
  if `struct_size` or the written length is short of that.
- **No active span set:** `tl_host_ctx.spans == NULL` → the count query returns `0`
  and any index returns `0` (indistinguishable from an empty set — correct: there
  is nothing to report).

**Pointer width — the destination record must live below 4 GiB (Memory64, cut 1).**
`host_call` is `(i32,i32,i32)->i32`; the C handler receives `int32_t ptr`. The
record's 64-bit `base`/`len`/`foffset` fields solve span-ADDRESS representation, but
NOT the address of the DESTINATION record itself. So for cut 1, with the fixed
signature retained:
- `ptr` is defined as an **unsigned 32-bit app offset**: the handler treats it as
  `(uint32_t)ptr` with **no sign extension**, and validates against linear memory as
  a 32-bit offset. A "negative" `int32_t` is simply a high unsigned offset and is
  rejected by the `validate_app_addr` gate like any other out-of-range offset — it
  is never sign-extended into a 64-bit address.
- A **Memory64 guest MUST place its `HlSpanMetaV1` scratch record below 4 GiB**;
  a destination at or above `UINT32_MAX` is unreachable through this opcode and is
  a documented cut-1 limitation (span DATA can sit anywhere in the 64-bit space;
  only the tiny metadata scratch is constrained). The span `base` reported IN the
  record is still full 64-bit, so a Memory64 plugin addresses its window normally.
- Tests: an AOT Memory64 case with a low (`< 4 GiB`) destination that succeeds AND a
  high-address destination that is cleanly rejected (`-1`, no sign-extension, no
  host crash).

**LOCKED set-build + lifecycle invariants (checkpoint 3a wiring, tied to §3.5/§3.7):**

- **Names validated + unique when building the set.** `hl_wasm_span_set_add` gains
  a `const char *name`: non-empty, `<= 63` bytes, and unique within the set (a
  duplicate name is rejected, distinct from the existing native-backing duplicate
  check). `HlWasmSpan` gains `char name[64]`.
- **Index order = `spans={}` declaration order.** The host adds spans in the order
  the caller listed them, so `idx` is the declaration position — deterministic and
  documented; the SDK does not depend on it beyond its own name→index scan.
- **`tl_host_ctx.spans` is cleared BEFORE teardown begins, on EVERY path.** Set
  `tl_host_ctx.spans = &set` after a successful `attach`; restore/NULL it before
  the first `hl_wasm_span_set_teardown` call on success, gas, trap, error, and
  every partial-build rollback path — so a metadata query can never observe a set
  that is mid-teardown or torn down (the snapshot goes away before any heap is
  detached/destroyed). This mirrors the existing `saved_ctx`/`tl_host_ctx` restore
  discipline around the `hull_process` call in `cap/wasm.c`.

## 1. Implementation surface (mapped to the design §§)

**A. Windowed `fs.mmap` binding (prerequisite; §1, §3.6).** The C API
`hl_cap_fs_mmap_window` exists (checkpoint 1). Expose it: `fs.mmap(path, {offset=…,
length=…})` selects the windowed constructor (Lua `lua_fs_mmap`
`src/hull/runtime/lua/mod_fs.c`, JS `js_fs_mmap` `src/hull/runtime/js/mod_fs.c`); a
bare path stays whole-file. Enforces `HL_FS_MMAP_MAX_WINDOW_BYTES` (1 GiB); clamps
to EOF. Small, isolated, independently testable.

**B. `HlWasmSpan` gains a name (§3.7).** `char name[64]`; `hl_wasm_span_set_add`
takes and validates it (see D2 lock). Pure extension of the checkpoint-2 struct.

**C. Public call opts + bindings (§1).** `HlWasmCallOpts` gains:

```c
const HlWasmSpanReq *spans;   /* array of requested spans, or NULL */
int                  span_count;
```

where

```c
typedef struct HlWasmSpanReq {
    const char     *name;     /* span name (validated, unique in the set) */
    HlMappedBuffer *buf;       /* the windowed mapping to attach read-only */
} HlWasmSpanReq;
```

Parse `opts.spans = {{name="source", buffer=mapped}, …}` in `mod_compute.c` (Lua,
around the existing `max_input`/`heap`/`gas` parse ~line 185) and the JS parallel
(`spans` with camelCase, resolving the `MappedBuffer` userdata/object to
`HlMappedBuffer*`). Accepted by `compute.call`, `compute.async.call`, and
`instance:call`.

**LOCKED public-option contract (validated in the binding, before any C-layer
work):**
- **`span_count` bounds + consistency.** `0 <= span_count <= HL_WASM_MAX_SPANS`
  (16). `spans == NULL` iff `span_count == 0` (a NULL array with a positive count,
  or a non-NULL array with count 0, is an internal error). A `span_count < 0` or
  `> 16` is rejected in the binding.
- **`spans={}` (empty or absent) is a PLAIN call**, not a `no_spans` error: no span
  set is created and the call runs exactly as today. `no_spans` is only the
  internal `hl_wasm_span_set_attach`-on-empty guard (unreachable from the public
  path, which never attaches an empty set).
- **Per-entry validation in the binding:** each entry must have a non-empty, valid
  name (see D2 name rules — `1..63` bytes, no embedded NUL, unique) and a live
  `MappedBuffer`. A **NULL entry**, a **closed buffer** (`buf->closed`), or a
  **whole-file mapping whose `map_len` is not page-aligned** is rejected with a
  clear error before submission. (The checkpoint-2 `hl_wasm_span_set_add` already
  fails `bad_buffer` on a non-page-aligned `map_len` / closed buffer; the binding
  rejects earlier with a user-facing message so the failure names the offending
  span.) A windowed `fs.mmap({offset,length})` always yields a page-aligned
  `map_len`; a whole-file `fs.mmap(path)` is only valid as a span when the file's
  length is already a page multiple — otherwise the caller must window it.

**D. Lifecycle integration in `cap/wasm.c` (§3.5).** Three call sites: sync
`hl_cap_wasm_call_buf`, the async worker path, and `instance:call`.

**D1 is checked ATOMICALLY against segment mutation.** The `mod->shared_data`
check and the span attach happen under `mod->mutex` in the same window that
`pool_acquire` already takes (it snapshots `shared_data`/`chain_head` under
`mod->mutex`). `compute.segment` mutation drains the pool + rewrites
`shared_data` under the SAME `mod->mutex`, so reading "no active segments" and
committing to a span attach cannot race a concurrent `compute.segment`. (The
attach itself and the WASM call run after the snapshot, as segments do today; D1
only needs the *decision* to be atomic with the segment presence it reads.)

**Sync + persistent path** (executing thread owns everything):
- under `mod->mutex`: enforce **D1** (reject if `mod->shared_data` active →
  `"spans_with_segments"`);
- `hl_wasm_span_set_init(&set, mod->is_memory64)` on the executing thread;
- add each requested span (validated name → borrow + RO heap), `attach`; on any
  add/attach failure, teardown-rollback and return;
- `tl_host_ctx.spans = &set` (snapshot); run `hull_process`;
- clear `tl_host_ctx.spans`, then **teardown on ALL exit paths** (success
  poolable/non-poolable, gas, trap, error) **before** `hl_wasm_pool_release`,
  including immediately after the call on the zero-copy poolable path (spans back
  the *input*, consumed by the call; the output buffer wraps linear memory with its
  own lifetime).

**Async path — submission-time ownership model (finding: the queued-worker race).**
`HlWasmCallOpts` is value-copied into the worker op, but a `HlWasmSpanReq` array
holds **borrowed** `name`/`HlMappedBuffer*` pointers, and the span set (its first
borrow) is created LATER on the worker. Between submission and worker execution,
Lua/JS can `close()` or GC the mapped buffer — invalidating the mapping AND the
pointer. So the binding must take ownership AT SUBMISSION, on the event-loop
thread, before the job is queued:

1. **Deep-copy** the request array into `HlWorkerWasmOp`: fixed `char
   names[HL_WASM_MAX_SPANS][64]` (copied, not borrowed) + `HlMappedBuffer
   *bufs[HL_WASM_MAX_SPANS]` + `int span_count`. No pointer into Lua/JS-owned
   memory survives submission.
2. **Submission pin:** `hl_cap_fs_mmap_borrow(buf)` on every buffer **before**
   `pool_submit`. The checkpoint-1 borrow refcount defers BOTH `munmap` AND the
   `HlMappedBuffer` free (`pending_free` + `borrow_count`): the struct and its
   mapping both stay alive while pinned, so a mid-flight `buffer:close()`/GC is
   safe. This is what keeps "the allocation itself alive, not merely its mapping."
3. **Rollback on any submission-time failure** (parse, deep-copy, op alloc, or
   `pool_submit` returns error): release every submission pin taken so far and free
   the op — no buffer left pinned, no partial submission.
4. **Worker span set:** the worker calls `hl_wasm_span_set_init` (owner = the
   worker thread) and `add`s each `bufs[i]` — this takes the **span-set's own**
   borrow (a SECOND, independent pin), attaches, runs, and tears down on every exit
   (success/trap), releasing the span-set borrows. The two pin layers are kept
   **separate and balanced**: submission pins are the event-loop's; span-set
   borrows are the worker's.
5. **Release submission pins after worker teardown** — on the event loop when the
   job is reaped, on BOTH success and **cancellation** (a cancelled job that never
   ran the worker span set still releases its submission pins). `tl_host_ctx.spans`
   is cleared before the worker teardown as in the sync path.

Net: a buffer is pinned continuously from submission → reap (submission layer),
with the span-set borrow nested inside worker execution; no window exists in which
the buffer can be unmapped/freed while the op references it. Test: a **queued
worker whose buffer is `close()`d/GC'd before execution starts** still runs and
reads correct bytes (the pre-worker race the plain `buffer:close()` mid-flight test
would miss).

**E. Metadata host-call (§3.3).** `HL_WASM_OP_SPAN_INFO = 0x04` in
`host_call_handler` (`cap/wasm.c`), writing `HlSpanMetaV1` per the D2 wire ABI via
`wasm_runtime_validate_app_addr` + `_addr_app_to_native` (exactly like the
LOG/CALLBACK paths). Invocation-scoped and fail-closed per the D2 semantics.

**F. SDK header `hull/wasm/span.h` (§3.2, §3.3).** Overflow-safe, alignment-safe
inline accessors (`__builtin_memcpy` into a local; `off <= len && w <= len - off`),
explicit-byte-order (`u/i/f{8,16,32,64}`, `le/be{16,32,64}`); the
`HullSpan{base,len,foffset,flags}` type; `HL_SPAN_META_V1_SIZE = 96`.

**LOCKED SDK ownership + API (freestanding — no malloc, caller-provided storage):**
- **`hull_span_setup()` discovers ALL spans in one pass**, not one requested name:
  it issues the count query `(0x04,0,-1)` once, then one record query per index,
  validates `version`/`struct_size`, and fills a caller-provided `HullSpan` array.
  Name resolution is a separate, cheap lookup over the cached array (below). This
  keeps "no host calls in the scan loop": setup runs once at plugin start; every
  subsequent access is pure inline check + native read.
- **Signature (caller-owned fixed-capacity storage):**
  ```c
  /* Fills out[0..min(count,out_cap)); returns the TRUE span count (>= 0),
   * or a negative HULL_SPAN_ERR_* on a version/struct_size/query failure.
   * A return value > out_cap means the caller's array was too small -- the
   * first out_cap spans are valid; the caller sized it too small. */
  int hull_span_setup(HullSpan *out, int out_cap);
  ```
  The plugin declares `HullSpan spans[HL_WASM_MAX_SPANS];` (or its own smaller
  cap) on its stack/bss and passes it in. The header allocates nothing.
- **More spans than capacity:** `hull_span_setup` writes the first `out_cap` and
  returns the true `count`; `count > out_cap` is the caller's signal it
  under-sized. No overflow, no truncation-in-silence.
- **Name lookup is a linear scan** over the (≤16) cached `HullSpan` records
  (`hull_span_find(const HullSpan *spans, int n, const char *name)` → index, or
  `-1` for an **unknown name**). ≤16 entries → a real hash map is unwarranted;
  linear is simpler in a freestanding header and the array is read once at setup.
  Each record carries its `name` (the D2 wire field), so the scan needs no extra
  host calls.
- **Per-query capacity handshake:** `hull_span_setup` sets `struct_size =
  HL_SPAN_META_V1_SIZE` in each scratch record before the query (the cbSize
  convention), checks the returned `struct_size` covers every v1 field it reads,
  and errors (`HULL_SPAN_ERR_VERSION`) otherwise.

Compiles **natively** over an `HlBufferView` for differential tests. Shipped as a
sibling of the per-module `hull_compute.h`, refreshable via `hull compute
refresh-header`.

## 2. Test plan (§5)

**Metadata host-call (`test_wasm_spans_call.c`), the D2 semantics:**
- count via `(0x04,0,-1)`; valid index writes a record and returns `struct_size`;
  out-of-range index returns `0`; **no active set / post-detach / `spans==NULL`
  query returns `0`** (proving `tl_host_ctx.spans` is cleared before teardown).
- **cbSize matrix:** advertised `cap` ∈ `{4, 8, 72, 95, 96, 128}` each writes
  `min(cap, 96)` bytes and returns `96` (a `cap<96` prefix write is legal, NOT an
  error); malformed `cap` (`<4`, `>4096`) → `-1`; a dest buffer **ending exactly at
  the linear-memory bound** (both the 4-byte pre-read and the `cap`-byte write at
  the boundary) — in-bounds succeeds, one past → `-1`, no host read/write OOB.
- **Memory64 dest width:** an AOT Memory64 case with a low (`<4 GiB`) destination
  succeeds; a high-address (`≥UINT32_MAX`) destination is cleanly rejected (`-1`,
  no sign-extension, host survives).
- **Metadata isolation across CONCURRENT calls** (not only post-detach): two
  in-flight invocations on two instances each see only their own spans by index +
  name; neither can query the other's set.

**Public option + set-build validation (binding + `hl_wasm_span_set_add`):**
- `spans={}` / absent → plain call (no set, no `no_spans`); `span_count` `<0`/`>16`
  rejected; `spans==NULL` with `count>0` (and vice versa) rejected.
- NULL entry, closed buffer, non-page-aligned whole-file mapping each rejected with
  the offending span named.
- **Names:** empty, `>63` bytes (overlong), **embedded NUL**, and duplicate-in-set
  all rejected; a valid unique set accepted.
- **Empty and maximum-sized (16) span lists**; the 17th rejected.
- **D1 atomicity:** a spans call racing a `compute.segment` mutation is
  deterministically either accepted (no segments) or `"spans_with_segments"`
  (segments present) — the decision taken under `mod->mutex`, never a torn read.

**Async ownership (the finding-1 core — `test_wasm_spans_async.c`):**
- **queued-worker race:** buffer `close()`/GC'd **before worker execution starts**
  → the call still runs and reads correct bytes (submission pin held).
- **two concurrent async calls sharing the SAME mapped buffer** → both read
  correctly; borrow count returns to baseline after both reap.
- **cancellation before worker start** AND **during execution** → submission pins
  released on both, no leak, no double-release, mapping freed only after reap.
- **partial async pinning failure** (Nth buffer fails to pin) and **submission
  failure** (`pool_submit` error) → all prior submission pins rolled back, op
  freed, buffers back to baseline.
- span-set borrow (worker) and submission pin (event loop) are **separately
  balanced** — assert `borrow_count` at each phase (submitted=1, executing=2,
  post-teardown=1, post-reap=0).

**`fs.mmap` window binding:** offset/length parse, EOF clamp, `>1 GiB` rejected,
non-page-aligned offset (Lua + JS).

**e2e (`tests/e2e_spans.sh`, both runtimes, interp + AOT):** a real plugin using
`hull/wasm/span.h` maps a file window via `spans={}`, `hull_span_setup` discovers
all spans, reads/verifies bytes, reads one past the window (traps → structured
error), `hull_span_find` on an unknown name → `-1`, and scans a `>4 GiB` file as
repeated windows checking `foffset`. Reuse the wamrc-AOT CI leg that already gates
the span AOT case.

**Differential:** the same `span.h` accessors native vs. WASM over identical bytes.

**Sanitizers/fuzz:** ASan/UBSan on the sync + async call paths; TSan on the two
concurrent-async cases; fuzz the metadata-record offset/cbSize math and the SDK
bounds check.

## 3. Sequencing (two cuts, each its own PR)

- **3a — internal→guest path (C + bindings, no SDK):** A (windowed `fs.mmap`), B
  (span name), C (opts + bindings), D (lifecycle integration + the D1/D2 locks), E
  (metadata host-call). Testable in C via raw `host_call`; a plugin *could* use it
  with hand-written host calls. The reviewable core.
- **3b — ergonomics + proof:** F (`hull/wasm/span.h`), the example plugin,
  `refresh-header` wiring, the full e2e (interp + AOT, both runtimes) and the
  differential test. Depends on 3a.

## 4. Risks / watch-items

- **Zero-copy output path** (`cap/wasm.c:947`) keeps the instance checked out —
  span teardown must run *before* that return and never entangle with the output
  buffer's instance lifetime.
- **Three teardown call sites** — sync `hl_cap_wasm_call_buf`, the async worker
  path, and `instance:call` each need their own clear-`tl_host_ctx.spans` +
  teardown-on-every-exit wiring. The checkpoint-2 audit's latent "no owner check in
  teardown" gap is where async teardown-thread correctness gets locked in (the
  worker that ran the job is the owner and the tearer-down).
- **D1 rejection is a clean, documented error**, never a silent fallback.
