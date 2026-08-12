# Mapped spans — checkpoint 3 plan (public API, metadata delivery, SDK header)

**Status:** PLAN. Turns the reviewed internal lifecycle (checkpoint 2 —
`cap/wasm_spans.c` `HlWasmSpanSet` + WAMR patch 0004 guarded-subrange RO shared
heaps, PR #309) into a usable feature: `compute.call(..., {spans={...}})` lets a
WASM plugin read host-mapped file windows at native speed, learning each window's
address via a versioned host-call query, with a safe SDK header.

**Blocked on:** PR #309 (checkpoint 2) green + merged. No checkpoint-3 code lands
before that. This document locks the two design decisions (§0) so 3a can start the
instant #309 merges.

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
- **Invalid pointer / undersized destination / malformed query:** returns a
  **distinct negative** error (`-1`). "Invalid pointer" = `wasm_runtime_validate_app_addr(inst,
  ptr, sizeof(HlSpanMetaV1))` fails; "undersized" is subsumed by that validate
  (the dest must admit a full record); "malformed" = any other unexpected arg
  combination.
- **No active span set:** `tl_host_ctx.spans == NULL` → the count query returns `0`
  and any index returns `0` (indistinguishable from an empty set — correct: there
  is nothing to report).

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

**D. Lifecycle integration in `cap/wasm.c` (§3.5).** In `hl_cap_wasm_call_buf` (+
the async and persistent-instance variants — three call sites): when `opts->spans`
present →
- enforce **D1** (reject if `mod->shared_data` active → `"spans_with_segments"`);
- `hl_wasm_span_set_init(&set, mod->is_memory64)` on the executing thread
  (event-loop for sync, the worker for async, the owner for persistent — matches
  the checkpoint-2 owner model + the async borrow-pin);
- add each requested span (validated name → borrow + RO heap), `attach` to the
  instance; on any add/attach failure, teardown-rollback and return;
- `tl_host_ctx.spans = &set` (snapshot);
- run `hull_process`;
- clear `tl_host_ctx.spans`, then **teardown on ALL exit paths** (success
  poolable/non-poolable, gas, trap, error) **before** `hl_wasm_pool_release`,
  including immediately after the call on the zero-copy poolable path (spans back
  the *input*, consumed by the call; the output buffer wraps linear memory with its
  own lifetime). Async teardown runs on the worker at job completion (success or
  trap); the borrow-pin defers `munmap` across a mid-flight `buffer:close()`.

**E. Metadata host-call (§3.3).** `HL_WASM_OP_SPAN_INFO = 0x04` in
`host_call_handler` (`cap/wasm.c`), writing `HlSpanMetaV1` per the D2 wire ABI via
`wasm_runtime_validate_app_addr` + `_addr_app_to_native` (exactly like the
LOG/CALLBACK paths). Invocation-scoped and fail-closed per the D2 semantics.

**F. SDK header `hull/wasm/span.h` (§3.2, §3.3).** Overflow-safe, alignment-safe
inline accessors (`__builtin_memcpy` into a local; `off <= len && w <= len - off`),
explicit-byte-order (`u/i/f{8,16,32,64}`, `le/be{16,32,64}`); the
`HullSpan{base,len,foffset,flags}` type; `HL_SPAN_META_V1_SIZE = 96`; a
`hull_span_setup()` that issues the §3.3 query once per span (count query, then one
record per index), validates `version`/`struct_size`, caches into `HullSpan`, and
builds the name→index map — no host calls in the scan loop. Compiles **natively**
over an `HlBufferView` for differential tests. Shipped as a sibling of the
per-module `hull_compute.h`, refreshable via `hull compute refresh-header`.

## 2. Test plan (§5)

- **C-level (`test_wasm_spans.c` / new `test_wasm_spans_call.c`):**
  `HlWasmCallOpts.spans` plumbing end-to-end through `hl_cap_wasm_call_buf`;
  metadata query — count via `(0x04,0,-1)`; a valid index writes exactly a record
  and returns `struct_size`; out-of-range index returns `0`; invalid/undersized
  dest returns `-1`; version/struct_size the SDK checks; **post-detach /
  `spans==NULL` query returns 0** (proving `tl_host_ctx.spans` is cleared before
  teardown); D1 `"spans_with_segments"` rejection; duplicate-name rejection;
  async pinning across `buffer:close()`; reentrancy rejection (spans on an
  in-flight instance). The isolation / rollback / borrow-baseline invariants are
  already covered by checkpoint 2.
- **`fs.mmap` window binding:** offset/length parse, EOF clamp, `> 1 GiB` rejected,
  non-page-aligned offset (Lua + JS).
- **e2e (`tests/e2e_spans.sh`, both runtimes, interp + AOT):** a real plugin using
  `hull/wasm/span.h` maps a file window via `spans={}`, reads/verifies bytes, reads
  one past the window (traps → structured error), scans a `> 4 GiB` file as
  repeated windows checking `foffset`. Reuse the wamrc-AOT CI leg that already gates
  the span AOT case.
- **Differential:** the same `span.h` accessors native vs. WASM over identical
  bytes.
- **Sanitizers/fuzz:** ASan/UBSan on the call path; fuzz the metadata-record offset
  math and the SDK bounds check.

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
