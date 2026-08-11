# WAMR patch 0004: guarded-subrange read-only shared heaps

Status: DESIGN (pre-implementation). Companion to patches 0001 (read-only
shared-heap enforcement), 0002 (`read_only` flag), and 0003
(`wasm_runtime_destroy_shared_heap`). This note is the prerequisite for
un-blocking the mapped-spans public API; it is written against Hull's vendored
WAMR (2.4.1-218-gc3a78cd1) and must be reviewed before any production change.

## 1. Problem

Mapped spans attach a windowed `HlMappedBuffer` to a WASM instance as a
read-only shared heap. Checkpoint 1 promised callers alignment-agnostic
windows: a window is `[addr, addr + len)` where

    addr    = map_base + slop,  slop = foffset % pagesize   (0 <= slop < page)
    map_len = round_up(slop + len, pagesize)

`map_base` / `map_len` are the page-aligned mmap used only for ownership and
`munmap`. The logical window the caller asked for is `[addr, addr + len)`.

WAMR's pre-allocated shared heap (`wasm_runtime_create_shared_heap`,
`core/iwasm/common/wasm_memory.c`) exposes a whole contiguous page-rounded
region to the guest:

- line 243: `size = align_uint(size, os_getpagesize())`
- lines 262-266: for a pre-allocated heap, `size != init_args->size` fails, so
  the caller-supplied `size` MUST already be page-aligned.
- line 269: `heap->base_addr = init_args->pre_allocated_addr` verbatim. There
  is NO alignment requirement on the base address itself; it is used only in
  the arithmetic `native = base_addr + (app_off - start_off)` (lines 806, 834,
  841) and `base_addr_adj = base_addr - start_off` (lines 508-537).

So WAMR constrains the SIZE to be page-aligned, not the BASE. The guest-visible
extent is `[base_addr, base_addr + size)` and the per-instance fast-path bound
is cached as

    shared_heap_end_off = start_off - 1 + size            (lines 507-537)

Today mapped spans pass `base_addr = map_base`, `size = map_len`. Two
consequences, both are intra-file over-reads:

1. PREFIX leak: when `slop > 0`, the guest can read `[map_base, addr)`, i.e.
   `slop` bytes BEFORE the requested window (adjacent file content).
2. SUFFIX leak: when `len` is not a page multiple, the guest can read
   `[addr + len, map_base + map_len)`, i.e. up to `page - 1` bytes AFTER the
   window (adjacent file content, or past-EOF pages which fault as SIGBUS).

The checkpoint-2 lifecycle does not reject these (its only alignment gate is
`map_len % page == 0`, which is always true). The fix is a WAMR-side guarded
subrange: reserve a page-rounded WASM range but permit access only to the
logical `valid_len`.

## 2. Non-goal / corrected assumption

WAMR does NOT require a page-aligned native base pointer. The earlier review
overstated this: only the pre-allocated SIZE is page-checked (lines 262-266).
The base is pure arithmetic. This is what makes design A (below) viable.

## 3. Design A (preferred): base = addr, upper-bound guard at valid_len

Pass the LOGICAL window base as the native backing base, reserve a page-rounded
WASM range for it, and store a separate logical length that bounds guest access.

Per span:

    pre_allocated_addr = buf->addr                 // logical offset start, no prefix
    reserved_size      = round_up(buf->len, page)  // page-aligned, satisfies WAMR
    valid_len          = buf->len                  // the only accessible extent

Guest-visible window becomes `[start_off, start_off + valid_len)` where
`start_off = ADDR_CEIL - reserved_size + 1`. The suffix
`[start_off + valid_len, start_off + reserved_size)` is reserved but
INACCESSIBLE. `base_addr = addr`, so app_off `start_off` maps to `addr` exactly:
no prefix is ever addressable.

### 3.1 WAMR changes

1. `WASMSharedHeap` (host struct, NOT part of the AOT ABI) gains a field:

       uint64 valid_size;   /* accessible bytes from base_addr; <= size */

   For every existing (non-span) heap `valid_size == size`, so behavior is
   unchanged unless a caller sets it.

2. `SharedHeapInitArgs` gains an optional `uint64 valid_size`. Semantics and
   validation, enforced in `create` (all fail closed):
   - `valid_size == 0` means "= size" (FULL heap). This is the ONLY back-compat
     path: every existing caller zero-initialises the args struct, so a zero
     field must preserve full-heap access byte for byte. An intentionally EMPTY
     span is never expressed as `valid_size == 0`; Hull's `hl_wasm_span_set_add`
     rejects a `len == 0` window BEFORE create, so 0 always and only means full.
   - reject `valid_size > size` (a window can never exceed its reserved range).
   - overflow-free by construction: `start_off = ADDR_CEIL - size + 1` and
     `end_off = start_off - 1 + valid_size`; with `valid_size <= size <=
     APP_HEAP_SIZE_MAX (1 GiB)` the sum is `<= ADDR_CEIL`, no wrap on mem32 or
     mem64.
   - a sub-full `valid_size < size` is permitted ONLY on a PRE-ALLOCATED heap
     (`pre_allocated_addr != NULL`, `heap_handle == NULL`). A runtime-managed
     heap (`heap_handle != NULL`) allocates across its whole range, so `create`
     forces `valid_size = size` for it; the two are mutually exclusive.

3. The per-instance cache builder `update_last_used_shared_heap`
   (wasm_memory.c:507-537, both INTERP and AOT arms) computes the fast-path
   upper bound from `valid_size` instead of `size`:

       shared_heap_end_off = start_off - 1 + cur->valid_size;

   This is the single load-bearing change. Both the interpreter hot path and
   the AOT-emitted hot path read `shared_heap_end_off` as a RUNTIME field
   (AOT: `aot_emit_memory.c:191`, loaded from `AOTModuleInstanceExtra` at the
   static-asserted offset 24). Lowering the stored VALUE tightens the bound for
   reads AND writes, scalar AND SIMD AND bulk, with no emitted-code change.

4. The slow / chain paths that use `size` directly must use `valid_size`:
   - `is_native_addr_in_shared_heap` (line 765): `> base_addr + cur->valid_size`
   - `wasm_runtime_check_and_update_last_used_shared_heap` (the AOT chain host
     call, `GET_AOT_FUNCTION(..., 7)` at aot_emit_memory.c:183) and the app-addr
     / string bound at line 1160 (`shared_heap_end_off`).
   - `start_off` / `start_off_mem32/64` stay derived from `size` (the reserved
     range), so chaining math and the high-address window are unchanged.

5. `reset_shared_heap_chain` (line 409, `memset(base_addr, 0, size)`) must key
   the zeroed length off `valid_size` too, but Hull never calls reset on a
   read-only span (it would fault), so this is defensive only.

### 3.1a Reserved vs valid: complete audit of every start_off / size use

Placement and overlap MUST use the page-rounded reserved `size`; guest access
checks and the per-instance cache MUST use `valid_size`. Reusing one bound for
both would let a later heap's slot be computed from a shortened range and
overlap the prior heap's reserved suffix. Audited against `wasm_memory.c`:

| Site | Purpose | Uses |
|------|---------|------|
| `:255-256` start_off = CEIL - size + 1 | PLACEMENT (heap's high-address slot) | `size` |
| `:344-345` chain: head.start_off = body.start_off - head.size | PLACEMENT (stacks slots contiguously; disjoint by construction) | `size` |
| `:371-372` reset re-derives start_off | PLACEMENT | `size` |
| `:175-220` runtime-managed mmap/munmap | PLACEMENT (runtime-owned only; N/A to spans) | `size` |
| `:507-535` update_last_used -> end_off | ACCESS (interp + AOT fast-path cache) | -> `valid_size` |
| `:729` is_app_addr_in_shared_heap chain-hit | ACCESS (which heap owns app_off; suffix gap must match NONE) | -> `valid_size` |
| `:765` is_native_addr_in_shared_heap | ACCESS (host native-pointer validation) | -> `valid_size` |
| `:409` reset memset length | WRITE (defensive; never hit for RO spans) | -> `valid_size` |

Because chaining stacks slots by `size` (`:344-345`), slots are contiguous with
NO inter-slot gap; the only gap is the within-slot suffix `[start_off +
valid_size, start_off + size)`. With `:729` switched to `valid_size`, an
app_offset in that suffix matches no heap in the chain walk and traps. Slot
placement is independent of `slop` (slop feeds only `base_addr`, never
`start_off`/`size`), so multiple spans with different slop have disjoint
reserved slots and independent valid bounds (point 7, confirmed).

### 3.2 The reserved-past-mapping proof obligation

With `base_addr = addr` and `reserved_size = round_up(len, page)`, the RESERVED
range `[addr, addr + reserved_size)` can extend PAST the live mmap
`[map_base, map_base + map_len)`. Example: `slop = 4095, len = 1, page = 4096`.
Then `map_len = round_up(4096) = 4096`, but `addr + reserved_size =
map_base + 4095 + 4096 = map_base + 8191`, which is `4095` bytes past the
mapping. Dereferencing there is SIGBUS.

This is safe iff nothing ever dereferences `[addr + valid_len, addr +
reserved_size)`:

- The guard (3.1.3) makes the GUEST unable to reach beyond `addr + valid_len`.
  And `addr + valid_len = map_base + slop + len <= map_base + map_len` always
  holds (`slop + len <= round_up(slop + len, page) = map_len`), so every
  accessible byte is within the live mapping. No SIGBUS on any permitted access,
  including the EOF-tail case.
- WAMR internals never touch a pre-allocated heap beyond bookkeeping: with
  `heap_handle == NULL`, `shared_heap_malloc/free` bail out; `reset` is never
  called by Hull. The reserved suffix is address arithmetic only.

CORRECTION (review round 2). The provable invariant is the WEAKER one on the
ACCESSIBLE range, not the reserved range:

    [addr, addr + valid_len) is contained in [map_base, map_base + map_len)
    proof: addr + valid_len = map_base + slop + len
                           <= map_base + round_up(slop + len, page)
                            = map_base + map_len       (since slop + len <= map_len)
    holds for EVERY slop in [0, page) and every len >= 1.

The STRONGER claim "[addr, addr + round_up(valid_len, page)) is within the
mapping" is FALSE. Counterexample: `slop = page-1, len = 1`. Then
`round_up(len, page) = page`, `addr + round_up(len,page) = map_base + (page-1) +
page = map_base + 2*page - 1`, but `map_len = round_up(page-1+1, page) = page`,
so the reserved range ends `page - 1` bytes PAST the mapping. So design A's
RESERVED range can exceed the live mmap and CANNOT be proven within it; only the
VALID range is provably within it.

Safety therefore rests on the GUARD, not on the reservation fitting: the guest
cannot reach `[addr + valid_len, addr + reserved_size)` (end_off = valid), and
WAMR never dereferences a pre-allocated heap's bytes (`heap_handle == NULL` ->
malloc/free bail; reset never called). This is a real design-A property, not a
gap, but it means EVERY access path must use `valid_size` (3.1a): a single path
left on `size` would let the guest reach the reserved tail, which may be
UNMAPPED (SIGBUS) rather than merely adjacent-in-file. Design B has no such tail
(reserved = map_len, always fits), so a missed guard path there degrades to an
in-mapping over-read, never an unmapped access. This asymmetry is the core A-vs-B
trade-off (see section 4a).

Boundary tests (mandatory for 0004): slop in {0, 1, page/2, page-1} crossed with
len in {1, page-1, page, page+1, 2*page-1}; for each assert the last valid byte
(`start_off + valid_len - 1`) reads, the first suffix byte (`start_off +
valid_len`) traps, a multi-byte / v128 access straddling the boundary traps, and
no access (permitted or trapped) raises SIGBUS, including an EOF-tail window.

## 4. Design B (fallback): base = map_base, two-sided sub-window

If a shipped architecture or a future WAMR path genuinely requires an aligned
native base (none found in 2.4.1), fall back to keeping `base_addr = map_base`
(page-aligned, `reserved_size = map_len`, always fits the mmap) and expressing
the window as a sub-range:

    valid_lo = slop
    valid_hi = slop + len

Guest access permitted only for app_off in `[start_off + valid_lo,
start_off + valid_lo + len)`; metadata returns `wasm_reserved_base + slop` as
the window base. This needs a two-sided guard (raise the LOWER bound by `slop`
in addition to lowering the upper bound), a slightly larger WAMR change than A's
single upper-bound tighten, but every reserved byte stays inside the live
mapping (no reserved-past-mapping obligation). Design A is preferred because it
gives the guest a base exactly at the logical offset and needs only the
one-sided tighten; B is the safety net.

Do NOT narrow the public contract to page-aligned offsets unless BOTH A and B
prove infeasible.

## 4a. A vs B: recommendation (review round 2)

Both are functionally correct; the probe proved A viable (non-aligned base
works). The decision is a robustness trade-off:

- A (base = addr): cleaner metadata (guest base == logical offset), single
  upper-bound tighten, ONE new field. But the reserved tail can exceed the
  mapping (3.2), so a missed guard path is an UNMAPPED access (SIGBUS / OOB).
- B (base = map_base): reserved == map_len always fits the mapping, so a missed
  guard path degrades to an in-mapping over-read. Cost: a two-sided guard
  (raise the lower bound by slop) and slop-carrying metadata.

Recommendation: proceed with A (as preferred), CONTINGENT on (i) all four
access sites in 3.1a switched to `valid_size`, (ii) the mandatory boundary +
arch test matrix green, and (iii) a hard `assert(valid_size <= size)` plus a
release-time check. If the implementation cannot make the path audit
exhaustive with confidence, fall back to B, whose failure mode is strictly
milder. This is the one open design choice for the reviewer; everything else is
determined.

## 5. AOT format version and code generation

DECISION: NOT required. This is settled on generated code + layout, not the
interpreter probe. Evidence in the AOT compiler and runtime:

- The AOT emits the shared-heap bound check by passing `func_ctx->
  shared_heap_end_off` (a value LOADED from `AOTModuleInstanceExtra` at run
  time, `aot_emit_memory.c:191`) to `wasm_runtime_check_and_update_last_used_
  shared_heap` / the inline compare. The reserved `size` is NEVER emitted as an
  immediate; the bound is a runtime field read.
- That field sits at the static-asserted offset 24 in `AOTModuleInstanceExtra`
  (`aot_runtime.c:63`), with `start_off` at 16 and `base_addr_adj` at 8. Patch
  0004 adds `valid_size` to the HOST `WASMSharedHeap` (runtime state, not the
  AOT ABI struct) and changes only the VALUE written to the existing `end_off`
  field via `update_last_used_shared_heap`. No field is inserted, no offset
  moves, no emitted instruction changes.

Therefore the AOT format version is unaffected. A bump (7 -> 8) becomes
necessary ONLY if the implementation deviates by adding a field to
`AOTModuleInstanceExtra` or altering the emitted check shape; the design forbids
both. AOT re-confirmation with `wamrc` (not built in the probe environment) is a
build-and-run check of the SAME already-emitted code, expected to match the
interpreter, not a version question.

### 5.1 HW-bound vs SW-bound, and unaligned native bases under AOT

- HW/SW modes: the AOT emits the shared-heap software check under
  `if (comp_ctx->enable_shared_heap)` (`aot_emit_memory.c:247, 323`),
  INDEPENDENT of the linear-memory bounds mode. The shared heap lives in the
  high address range OUTSIDE the linear-memory guard-page region, so it is
  always software-checked; the `valid_size` guard therefore applies in BOTH
  HW-bound and SW-bound builds. (A guest that reaches a shared-heap address in
  HW-bound mode still runs the emitted software compare against `end_off`.)
- Unaligned native base under AOT: WAMR sets the LLVM load/store alignment to
  the WASM instruction's declared alignment hint, `LLVMSetAlignment(1 << align)`
  (`aot_emit_memory.c:971, 986`). A span whose logical data starts at a non-N-
  aligned FILE offset produces a native address whose low bits equal `slop mod
  N` for an N-aligned WASM access. This is the SAME situation WAMR already
  handles for unaligned linear-memory accesses (the align field is a hint, not
  a guarantee; x86_64 and arm64 tolerate misaligned scalar loads/stores;
  strict-alignment targets take the runtime `check_memory_alignment` path). It
  is IDENTICAL under designs A and B, because the misalignment comes from the
  file offset (`slop`), not from the choice of base. Conclusion: not a new
  hazard and not a fix; it is a MUST-TEST on AOT for BOTH arm64 and Linux
  x86_64 with odd/2-mod/`page-1` slop and scalar + v128 accesses, to confirm the
  shipped targets behave as expected. Unaligned WASM accesses must remain valid
  (they are legal by spec).

## 6. Test matrix (patch 0004)

Reads AND writes, interpreter AND AOT, HW-bound AND SW-bound modes, scalar /
SIMD (v128) / bulk (memory.copy, memory.fill):

- Unaligned offsets (slop = 1 .. page-1) and partial final pages.
- Raw malicious reads of prefix (`base - 1`, `start_off - 1`) and suffix
  (`start_off + valid_len` .. `start_off + reserved_size - 1`): all trap.
- Exact last valid byte (`start_off + valid_len - 1`): succeeds; `+ valid_len`
  and `+ valid_len` for a multi-byte / v128 access straddling the boundary:
  traps.
- Zero-length bulk (memory.copy / fill with len 0) at the boundary: defined
  no-op, no trap.
- memory.copy with SOURCE crossing and DESTINATION crossing the valid_len
  boundary independently; memory.init with a shared-heap DESTINATION crossing
  the boundary (writes are separately blocked on RO spans by patch 0002, but the
  address bound must still be valid-based). memory.fill likewise.
- EOF-tail mapping (window ends mid-page at end of file): reads within
  valid_len succeed with no SIGBUS; the guard blocks the past-EOF page.
- Multiple spans with DIFFERENT slop values chained on one instance: disjoint
  reserved slots (placement by size) with independent valid bounds; an address
  in one span's suffix gap traps, does not spill into the neighbor.
- AOT on BOTH arm64 and Linux x86_64 (not only fast-interp): the full matrix
  above, to confirm the runtime-field `end_off` guard and the unaligned-base
  behavior on the shipped targets.
- Host survival: `is_native_addr_in_shared_heap` / string APIs respect
  valid_size. Writable (non-read-only) shared-heap compatibility: valid_size
  defaulting to size (and forced = size on runtime-managed heaps) leaves every
  existing writable / dynamic shared heap byte-for-byte unchanged.

## 7. Throwaway probe (before production code)

A scratch program (not committed to the product) validates the three
load-bearing assumptions against Hull's actual WAMR, so the design rests on
measured behavior, not reading:

- P1: a pre-allocated shared heap with a NON-page-aligned `base_addr` and a
  page-aligned `size` creates, attaches, and the guest reads byte 0 of the heap
  as the byte at `addr` (proves base need not be aligned).
- P2: with `reserved_size > len`, a guest read at `start_off + len`
  currently SUCCEEDS (demonstrates the suffix over-read the guard closes) and
  confirms today's `end_off = start_off - 1 + size`.
- P3: after lowering the cached `shared_heap_end_off` to
  `start_off - 1 + len`, the same read TRAPS (proves the guard mechanism works
  through the existing cached field) for the interpreter, and (if `wamrc` is
  present) for AOT with no recompile / version change.
- P4: an EOF-tail window (last partial page) reads its last valid byte without
  SIGBUS under the guard.

Probe results are recorded in section 8 before implementation begins.

## 8. Probe results

Run on macOS arm64 (16 KiB pages), Hull's vendored WAMR
(WAMR-2.4.1-218-gc3a78cd1), interpreter (fast-interp). Throwaway probe:
`build/probe_guarded_subrange.c` (gitignored, not committed). All pass:

- P1 CONFIRMED: `wasm_runtime_create_shared_heap` with a NON-page-aligned
  `pre_allocated_addr` (base = region + 64) and a page-aligned `size` creates
  and attaches, and the guest reads the heap's byte 0 as the byte at `addr`.
  WAMR does NOT require an aligned native base. **Design A is viable.**
- P2 CONFIRMED: with a 2-page reserved size, the guest reads a sentinel placed
  ~1 page + 3000 bytes into the reserved region (well past any sub-page logical
  window). Today the FULL page-rounded reserved region is guest-readable. The
  intra-file over-read is real; a byte-granular guard is required.
- P3 CONFIRMED: tightening the heap's derived extent to one page BEFORE attach
  makes a guest read in the 2nd reserved page TRAP, while the last in-window
  i32 still reads correctly.

KEY FINDING (sharpens sections 3.1 and 5): poking the per-instance cached
`shared_heap_end_off` ALONE does not hold. WAMR re-derives that field from the
heap on every cache miss (`update_last_used_shared_heap`, wasm_memory.c:507-537,
reached via the fast-interp / AOT chain-check on any access outside the cached
range). The guard MUST therefore live on the heap as a `valid_size` that the
derivation reads, not as a one-shot cache write. This is exactly the design-A
insertion point, and it confirms the "no AOT version bump" conclusion: the AOT
loads `end_off` at runtime, so only the derived VALUE changes, not any emitted
instruction or struct offset. (Interpreter proven here directly; the AOT
equivalent is to be re-confirmed with `wamrc` when patch 0004 is prototyped,
since `wamrc` is not built in this environment.)

Residual for implementation: the reserved-past-mapping obligation (section 3.2)
still holds -- with base = addr and a page-rounded reserved size, the reserved
tail can extend past the live mmap, so the guard (not merely the reservation)
is what guarantees no SIGBUS; the probe's P3 accesses stayed within the mapping
by construction and the EOF-tail case (section 6) must be exercised explicitly
in the 0004 test matrix.

## 9. Sequencing

1. This note reviewed.
2. Throwaway probe run; section 8 filled; assumptions confirmed or the design
   revised.
3. Patch 0004 implemented into `build/wamr-patched` (vendor stays immutable),
   carried by `scripts/wamr_apply_patches.sh` with a SHA, documented in
   `docs/wamr_patches.md`, with the full section-6 matrix and a WAMR-instrumented
   TSan/ASan pass.
4. Patch 0004 reviewed and merged.
5. Rebase checkpoint 2 (#309) onto it: pass `addr` + `valid_size` from the span
   layer, drop any reliance on `map_base`/`map_len` for guest exposure, add the
   unaligned-offset / out-of-window exposure tests, AND fix the destroy-failure
   borrow-retention plus the missing lifecycle tests. No temporary aligned-offset
   restriction is introduced at any point.
