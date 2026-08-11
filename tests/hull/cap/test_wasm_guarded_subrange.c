/*
 * WAMR patch 0004 (guarded-subrange read-only shared heaps, Design B) C-level
 * test. Drives Hull's patched WAMR directly: a pre-allocated shared heap carries
 * a valid sub-window (valid_offset, valid_size) inside its reserved [0, size)
 * region, so the guest can reach ONLY [valid_offset, valid_offset + valid_size).
 * Native backing = map_base, reserved size = map_len, so the reserved range
 * always sits inside the mapping (no unmapped tail; a missed guard would be an
 * in-mapping over-read, never a SIGBUS).
 *
 * Matrix: interpreter + AOT (SW-bound fixture, skipped when wamrc is absent),
 * scalar (i32/i64) + SIMD (v128, AOT) + bulk (memory.copy / memory.fill),
 * unaligned slop + partial final pages, prefix/suffix traps, exact last valid
 * byte + straddle, zero-length bulk, EOF-tail (no SIGBUS), multiple spans with
 * different slop, host-survival (validate app addr), and writable/full-heap
 * back-compat.
 *
 * Bound-mode note: the AOT fixture is built --bounds-checks=1 (SW-bound), like
 * the sibling read-only-heap AOT test. Hull's `hull build` ships HW-bound AOT by
 * default (wamrc --bounds-checks=0 on 64-bit), but HW-bound turns an out-of-heap
 * access into a guard-page fault that the FULL runtime converts to a trap; a bare
 * unit harness lacks that setup, so an out-of-window access that the guard
 * correctly rejects from the shared heap then falls to linear memory and faults
 * raw instead of trapping. The guard's shared-heap SOFTWARE check is emitted
 * identically in both modes (aot_emit_memory.c, under `enable_shared_heap`,
 * independent of the linear-memory bounds mode), so SW-bound here proves the
 * guard deterministically; HW-bound end-to-end is exercised by e2e-compute.
 *
 * See docs/wamr_shared_heap_guarded_subrange_design.md.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
# define _XOPEN_SOURCE 700
#endif

#include "utest.h"
#include "wasm_export.h"

#include "gen_gsub_aot_sw.h" /* build-generated: gsub_aot_sw[] + _len (or empty) */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* memory(1) + load8/load32/load64/loadv128(lane0) + fill + copy. Compiled from
 * tests/hull/fixtures/gsub.wat via wat2wasm. */
static const unsigned char gsub_wasm[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x11, 0x03, 0x60,
    0x01, 0x7f, 0x01, 0x7f, 0x60, 0x01, 0x7f, 0x01, 0x7e, 0x60, 0x03, 0x7f,
    0x7f, 0x7f, 0x00, 0x03, 0x07, 0x06, 0x00, 0x00, 0x01, 0x00, 0x02, 0x02,
    0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x3d, 0x07, 0x06, 0x6d, 0x65, 0x6d,
    0x6f, 0x72, 0x79, 0x02, 0x00, 0x05, 0x6c, 0x6f, 0x61, 0x64, 0x38, 0x00,
    0x00, 0x06, 0x6c, 0x6f, 0x61, 0x64, 0x33, 0x32, 0x00, 0x01, 0x06, 0x6c,
    0x6f, 0x61, 0x64, 0x36, 0x34, 0x00, 0x02, 0x08, 0x6c, 0x6f, 0x61, 0x64,
    0x76, 0x31, 0x32, 0x38, 0x00, 0x03, 0x04, 0x66, 0x69, 0x6c, 0x6c, 0x00,
    0x04, 0x04, 0x63, 0x6f, 0x70, 0x79, 0x00, 0x05, 0x0a, 0x3e, 0x06, 0x07,
    0x00, 0x20, 0x00, 0x2d, 0x00, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x28,
    0x02, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x29, 0x03, 0x00, 0x0b, 0x0b,
    0x00, 0x20, 0x00, 0xfd, 0x00, 0x04, 0x00, 0xfd, 0x1b, 0x00, 0x0b, 0x0b,
    0x00, 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0xfc, 0x0b, 0x00, 0x0b, 0x0c,
    0x00, 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0xfc, 0x0a, 0x00, 0x00, 0x0b,
};
static const unsigned int gsub_wasm_len = 168;

UTEST_MAIN();

static long PG;

/* reserved size a window of (slop + len) bytes needs (page-rounded). */
static uint32_t reserved_of(size_t slop, size_t len)
{
    uint64_t need = (uint64_t)slop + (uint64_t)len;
    uint64_t r = (need + (uint64_t)PG - 1) / (uint64_t)PG * (uint64_t)PG;
    if (r < 256) /* APP_HEAP_SIZE_MIN */
        r = ((uint64_t)PG > 256 ? (uint64_t)PG : 256);
    return (uint32_t)r;
}

/* start_off of a mem32 pre-allocated heap of `reserved` bytes (create stores
 * UINT32_MAX - reserved + 1). Lets the test avoid WAMR-internal headers. */
static uint64_t start_off_of(uint32_t reserved)
{
    return (uint64_t)UINT32_MAX - (uint64_t)reserved + 1;
}

/* a fresh instance from a module image (loader rewrites the buffer -> copy). */
static wasm_module_inst_t inst_new(const unsigned char *img, unsigned int len,
                                   wasm_module_t *out_mod, uint8_t **out_buf,
                                   wasm_exec_env_t *out_env)
{
    uint8_t *b = (uint8_t *)malloc(len);
    if (!b) return NULL;
    memcpy(b, img, len);
    char err[128] = { 0 };
    wasm_module_t mod = wasm_runtime_load(b, len, err, sizeof(err));
    if (!mod) { free(b); return NULL; }
    wasm_module_inst_t inst =
        wasm_runtime_instantiate(mod, 64 * 1024, 128 * 1024, err, sizeof(err));
    if (!inst) { wasm_runtime_unload(mod); free(b); return NULL; }
    *out_mod = mod; *out_buf = b;
    *out_env = wasm_runtime_create_exec_env(inst, 32 * 1024);
    return inst;
}
static void inst_free(wasm_module_inst_t i, wasm_module_t m, uint8_t *b,
                      wasm_exec_env_t e)
{
    if (e) wasm_runtime_destroy_exec_env(e);
    if (i) wasm_runtime_deinstantiate(i);
    if (m) wasm_runtime_unload(m);
    free(b);
}

/* create a guarded pre-allocated heap over [map_base, map_base+reserved) with a
 * valid sub-window [slop, slop+len). read_only selectable. */
static wasm_shared_heap_t guarded(uint8_t *map_base, uint32_t reserved,
                                  size_t slop, size_t len, bool ro)
{
    SharedHeapInitArgs a;
    memset(&a, 0, sizeof(a));
    a.pre_allocated_addr = map_base;
    a.size = reserved;
    a.read_only = ro;
    a.valid_offset = (uint64_t)slop;
    a.valid_size = (uint64_t)len;
    return wasm_runtime_create_shared_heap(&a);
}

/* guest call helpers: return 1 + *out on success, 0 on trap. */
static int call1(wasm_module_inst_t inst, wasm_exec_env_t env, const char *fn,
                 uint32_t a0, uint32_t *out)
{
    wasm_function_inst_t f = wasm_runtime_lookup_function(inst, fn);
    uint32_t argv[2] = { a0, 0 };
    wasm_runtime_set_exception(inst, NULL);
    if (!wasm_runtime_call_wasm(env, f, 1, argv)) return 0;
    if (out) *out = argv[0];
    return 1;
}
static int call3(wasm_module_inst_t inst, wasm_exec_env_t env, const char *fn,
                 uint32_t a0, uint32_t a1, uint32_t a2)
{
    wasm_function_inst_t f = wasm_runtime_lookup_function(inst, fn);
    uint32_t argv[3] = { a0, a1, a2 };
    wasm_runtime_set_exception(inst, NULL);
    return wasm_runtime_call_wasm(env, f, 3, argv) ? 1 : 0;
}

/* anon RW region of `pages` pages, filled with a verifiable pattern. */
static uint8_t *anon(size_t pages)
{
    uint8_t *p = mmap(NULL, (size_t)PG * pages, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    for (size_t i = 0; i < (size_t)PG * pages; i++) p[i] = (uint8_t)(i & 0xff);
    return p;
}

/* ============ the fixture-parameterised access matrix (interp + AOT) ======== */
static void access_matrix(int *utest_result,
                          const unsigned char *img, unsigned int len,
                          const char *label)
{
    /* Hull's fast interpreter is built with WASM_ENABLE_SIMDE=0, so it cannot
     * EXECUTE v128 at all (the SIMD-prefix handler is #if WASM_ENABLE_SIMDE).
     * SIMD runs only under AOT (native SSE/NEON). The guard itself is
     * width-agnostic (CHECK_SHARED_HEAP_OVERFLOW takes `bytes`; the scalar
     * 8-byte straddle below proves it in interp), so v128 is exercised on the
     * AOT fixtures only. */
    int is_aot = (label && strncmp(label, "aot", 3) == 0);
    wasm_module_t mod; uint8_t *mb; wasm_exec_env_t env;
    wasm_module_inst_t inst = inst_new(img, len, &mod, &mb, &env);
    ASSERT_TRUE(inst != NULL);

    /* unaligned slops x lengths (partial + full-page + over-page windows). */
    size_t slops[] = { 0, 1, 3, (size_t)PG / 2, (size_t)PG - 1 };
    size_t lens[]  = { 1, 100, (size_t)PG - 1, (size_t)PG, (size_t)PG + 1,
                       2 * (size_t)PG - 1 };
    for (size_t si = 0; si < sizeof(slops) / sizeof(slops[0]); si++) {
        for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
            size_t slop = slops[si], wlen = lens[li];
            uint32_t reserved = reserved_of(slop, wlen);
            size_t pages = (reserved / PG) + 2;
            uint8_t *region = anon(pages);
            ASSERT_TRUE(region != NULL);

            wasm_shared_heap_t h = guarded(region, reserved, slop, wlen, true);
            ASSERT_TRUE(h != NULL);
            ASSERT_TRUE(wasm_runtime_attach_shared_heap(inst, h));
            uint64_t base = start_off_of(reserved);      /* reserved base app addr */
            uint64_t win = base + slop;                  /* window base app addr */
            uint32_t v = 0;

            /* window base maps to addr = region + slop (the file's byte slop). */
            ASSERT_TRUE(call1(inst, env, "load8", (uint32_t)win, &v));
            ASSERT_EQ(v, (uint32_t)(region[slop]));

            /* last valid byte reads. */
            ASSERT_TRUE(call1(inst, env, "load8", (uint32_t)(win + wlen - 1), &v));
            ASSERT_EQ(v, (uint32_t)(region[slop + wlen - 1]));

            /* one past the window traps -- but only when a suffix exists inside
             * the reserved slot. When the window fills the slot exactly
             * (slop + wlen == reserved) the "one past" app address is
             * base + reserved, which for a mem32 heap wraps past UINT32_MAX into
             * low (linear-memory) space, so it is not a meaningful in-heap
             * suffix byte; the reserved-top and prefix checks cover that case. */
            if (slop + wlen < reserved)
                ASSERT_FALSE(call1(inst, env, "load8", (uint32_t)(win + wlen), &v));

            /* prefix (slop margin) traps when slop > 0. */
            if (slop > 0)
                ASSERT_FALSE(call1(inst, env, "load8", (uint32_t)(win - 1), &v));

            /* reserved base (bottom of the slot) traps when there is a prefix. */
            if (slop > 0)
                ASSERT_FALSE(call1(inst, env, "load8", (uint32_t)base, &v));

            /* the very top of the reserved slot traps when a suffix exists. */
            if (slop + wlen < reserved)
                ASSERT_FALSE(call1(inst, env, "load8", (uint32_t)(base + reserved - 1), &v));

            /* multi-byte straddle of the valid end traps; fully-inside reads. */
            if (wlen >= 4) {
                ASSERT_TRUE(call1(inst, env, "load32", (uint32_t)(win + wlen - 4), &v));
                ASSERT_FALSE(call1(inst, env, "load32", (uint32_t)(win + wlen - 2), &v));
            }
            if (wlen >= 8)
                ASSERT_FALSE(call1(inst, env, "load64", (uint32_t)(win + wlen - 4), &v));
            /* v128 (16-byte) straddle traps; fully-inside reads. AOT only. */
            if (is_aot && wlen >= 16) {
                ASSERT_TRUE(call1(inst, env, "loadv128", (uint32_t)win, &v));
                ASSERT_FALSE(call1(inst, env, "loadv128", (uint32_t)(win + wlen - 8), &v));
            }

            wasm_runtime_detach_shared_heap(inst);
            wasm_runtime_destroy_shared_heap(h);
            munmap(region, (size_t)PG * pages);
        }
    }

    /* ---- bulk: memory.copy / memory.fill honor the valid bound both ways ---- */
    {
        size_t slop = 32, wlen = 200;
        uint32_t reserved = reserved_of(slop, wlen);
        size_t pages = (reserved / PG) + 2;
        uint8_t *region = anon(pages);
        ASSERT_TRUE(region != NULL);
        /* WRITABLE guarded heap so memory.fill/copy dest is allowed to write. */
        wasm_shared_heap_t h = guarded(region, reserved, slop, wlen, false);
        ASSERT_TRUE(h != NULL);
        ASSERT_TRUE(wasm_runtime_attach_shared_heap(inst, h));
        uint64_t win = start_off_of(reserved) + slop;

        /* fill within the window: ok. crossing the valid end: trap. */
        ASSERT_TRUE(call3(inst, env, "fill", (uint32_t)win, 0x5a, 16));
        ASSERT_FALSE(call3(inst, env, "fill", (uint32_t)(win + wlen - 8), 0x5a, 32));
        /* zero-length fill inside the window: defined no-op, no trap. */
        ASSERT_TRUE(call3(inst, env, "fill", (uint32_t)(win + 8), 0x00, 0));

        /* copy DEST crossing the valid end traps; fully-inside ok. copy SOURCE
         * (linear mem 0) is a normal in-bounds address. */
        ASSERT_TRUE(call3(inst, env, "copy", (uint32_t)(win), 0, 32));
        ASSERT_FALSE(call3(inst, env, "copy", (uint32_t)(win + wlen - 4), 0, 16));

        wasm_runtime_detach_shared_heap(inst);
        wasm_runtime_destroy_shared_heap(h);
        munmap(region, (size_t)PG * pages);
    }

    inst_free(inst, mod, mb, env);
}

UTEST(wasm_guarded, interp)
{
    RuntimeInitArgs init; memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_System_Allocator;
    ASSERT_TRUE(wasm_runtime_full_init(&init));
    PG = sysconf(_SC_PAGESIZE); if (PG <= 0) PG = 4096;

    access_matrix(utest_result, gsub_wasm, gsub_wasm_len, "interp");

    wasm_runtime_destroy();
}

UTEST(wasm_guarded, aot_sw_bounds)
{
    if (gsub_aot_sw_len == 0)
        UTEST_SKIP("no wamrc-built SW-bound .aot fixture in this build leg");
    RuntimeInitArgs init; memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_System_Allocator;
    ASSERT_TRUE(wasm_runtime_full_init(&init));
    PG = sysconf(_SC_PAGESIZE); if (PG <= 0) PG = 4096;
    access_matrix(utest_result, gsub_aot_sw, gsub_aot_sw_len, "aot-sw");
    wasm_runtime_destroy();
}

/* ---- multiple spans with different slop chained on one instance ---- */
UTEST(wasm_guarded, multi_span_different_slop)
{
    RuntimeInitArgs init; memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_System_Allocator;
    ASSERT_TRUE(wasm_runtime_full_init(&init));
    PG = sysconf(_SC_PAGESIZE); if (PG <= 0) PG = 4096;

    wasm_module_t mod; uint8_t *mb; wasm_exec_env_t env;
    wasm_module_inst_t inst = inst_new(gsub_wasm, gsub_wasm_len, &mod, &mb, &env);
    ASSERT_TRUE(inst != NULL);

    /* two windows, different slop/len, each one reserved page. */
    uint8_t *r0 = anon(2), *r1 = anon(2);
    ASSERT_TRUE(r0 && r1);
    r0[7] = 0xAA;              /* window0 base byte (slop 7) */
    r1[100] = 0xBB;            /* window1 base byte (slop 100) */
    uint32_t res = (uint32_t)PG;
    wasm_shared_heap_t h0 = guarded(r0, res, 7, 64, true);
    wasm_shared_heap_t h1 = guarded(r1, res, 100, 64, true);
    ASSERT_TRUE(h0 && h1);
    /* chain h0(head, lower) below h1(body, higher). */
    wasm_shared_heap_t chain = wasm_runtime_chain_shared_heaps(h0, h1);
    ASSERT_TRUE(chain != NULL);
    ASSERT_TRUE(wasm_runtime_attach_shared_heap(inst, chain));

    /* each window reads its OWN backing byte. */
    uint64_t s1 = start_off_of(res);            /* body slot base */
    uint64_t s0 = s1 - res;                     /* head slot base (below body) */
    uint32_t v = 0;
    ASSERT_TRUE(call1(inst, env, "load8", (uint32_t)(s0 + 7), &v));
    ASSERT_EQ(v, 0xAAu);
    ASSERT_TRUE(call1(inst, env, "load8", (uint32_t)(s1 + 100), &v));
    ASSERT_EQ(v, 0xBBu);
    /* an address in head's slop prefix traps (does not spill into body). */
    ASSERT_FALSE(call1(inst, env, "load8", (uint32_t)(s0 + 0), &v));
    /* an address in head's suffix margin (past its 64-byte window) traps. */
    ASSERT_FALSE(call1(inst, env, "load8", (uint32_t)(s0 + 7 + 64), &v));

    wasm_runtime_detach_shared_heap(inst);
    wasm_runtime_unchain_shared_heaps(chain, true);
    wasm_runtime_destroy_shared_heap(h0);
    wasm_runtime_destroy_shared_heap(h1);
    munmap(r0, (size_t)PG * 2); munmap(r1, (size_t)PG * 2);
    inst_free(inst, mod, mb, env);
    wasm_runtime_destroy();
}

/* ---- host survival: wasm_runtime_validate_app_addr respects the valid bound -- */
UTEST(wasm_guarded, host_validate_app_addr)
{
    RuntimeInitArgs init; memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_System_Allocator;
    ASSERT_TRUE(wasm_runtime_full_init(&init));
    PG = sysconf(_SC_PAGESIZE); if (PG <= 0) PG = 4096;

    wasm_module_t mod; uint8_t *mb; wasm_exec_env_t env;
    wasm_module_inst_t inst = inst_new(gsub_wasm, gsub_wasm_len, &mod, &mb, &env);
    ASSERT_TRUE(inst != NULL);
    size_t slop = 48, wlen = 300;
    uint32_t reserved = reserved_of(slop, wlen);
    uint8_t *region = anon((reserved / PG) + 2);
    ASSERT_TRUE(region != NULL);
    wasm_shared_heap_t h = guarded(region, reserved, slop, wlen, true);
    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(wasm_runtime_attach_shared_heap(inst, h));
    uint64_t win = start_off_of(reserved) + slop;

    /* a host validate of an in-window range succeeds; prefix/suffix fail. */
    ASSERT_TRUE(wasm_runtime_validate_app_addr(inst, (uint64_t)win, 16));
    ASSERT_TRUE(wasm_runtime_validate_app_addr(inst, (uint64_t)(win + wlen - 1), 1));
    ASSERT_FALSE(wasm_runtime_validate_app_addr(inst, (uint64_t)(win - 1), 1));
    ASSERT_FALSE(wasm_runtime_validate_app_addr(inst, (uint64_t)(win + wlen), 1));
    /* app -> native for the window base returns addr = region + slop. */
    void *nat = wasm_runtime_addr_app_to_native(inst, (uint64_t)win);
    ASSERT_TRUE(nat == (void *)(region + slop));

    wasm_runtime_detach_shared_heap(inst);
    wasm_runtime_destroy_shared_heap(h);
    munmap(region, (size_t)PG * ((reserved / PG) + 2));
    inst_free(inst, mod, mb, env);
    wasm_runtime_destroy();
}

/* ---- EOF-tail: a window ending mid-page at end of a real file. Reads within
 *      the window succeed with no SIGBUS; the suffix (rest of the last page)
 *      traps. Design B keeps reserved == map_len, so no unmapped access. ------- */
UTEST(wasm_guarded, eof_tail_no_sigbus)
{
    RuntimeInitArgs init; memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_System_Allocator;
    ASSERT_TRUE(wasm_runtime_full_init(&init));
    PG = sysconf(_SC_PAGESIZE); if (PG <= 0) PG = 4096;

    char path[256];
    snprintf(path, sizeof(path), "/tmp/hull_gsub_eof_%d.bin", (int)getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ASSERT_TRUE(fd >= 0);
    size_t fsize = 100; /* window ends mid-page */
    unsigned char buf[100];
    for (size_t i = 0; i < fsize; i++) buf[i] = (unsigned char)(i & 0xff);
    ASSERT_EQ((size_t)write(fd, buf, fsize), fsize);

    /* map the whole (single) page the file lives in; the tail past EOF is
     * zero-filled by mmap, so reserved == map_len == PG stays inside it. */
    uint8_t *map_base = mmap(NULL, (size_t)PG, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    ASSERT_TRUE(map_base != MAP_FAILED);

    wasm_module_t mod; uint8_t *mb; wasm_exec_env_t env;
    wasm_module_inst_t inst = inst_new(gsub_wasm, gsub_wasm_len, &mod, &mb, &env);
    ASSERT_TRUE(inst != NULL);
    wasm_shared_heap_t h = guarded(map_base, (uint32_t)PG, 0, fsize, true);
    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(wasm_runtime_attach_shared_heap(inst, h));
    uint64_t win = start_off_of((uint32_t)PG);
    uint32_t v = 0;
    ASSERT_TRUE(call1(inst, env, "load8", (uint32_t)(win + fsize - 1), &v));
    ASSERT_EQ(v, (uint32_t)((fsize - 1) & 0xff));
    ASSERT_FALSE(call1(inst, env, "load8", (uint32_t)(win + fsize), &v)); /* suffix traps */

    wasm_runtime_detach_shared_heap(inst);
    wasm_runtime_destroy_shared_heap(h);
    munmap(map_base, (size_t)PG);
    unlink(path);
    inst_free(inst, mod, mb, env);
    wasm_runtime_destroy();
}

/* ---- back-compat: a full heap (valid_offset/size 0) is unchanged: the WHOLE
 *      reserved region is readable, exactly as before patch 0004. ------------- */
UTEST(wasm_guarded, full_heap_backcompat)
{
    RuntimeInitArgs init; memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_System_Allocator;
    ASSERT_TRUE(wasm_runtime_full_init(&init));
    PG = sysconf(_SC_PAGESIZE); if (PG <= 0) PG = 4096;

    wasm_module_t mod; uint8_t *mb; wasm_exec_env_t env;
    wasm_module_inst_t inst = inst_new(gsub_wasm, gsub_wasm_len, &mod, &mb, &env);
    ASSERT_TRUE(inst != NULL);
    uint32_t reserved = (uint32_t)PG;
    uint8_t *region = anon(1);
    ASSERT_TRUE(region != NULL);
    /* valid_offset == 0 && valid_size == 0 => full heap. */
    wasm_shared_heap_t h = guarded(region, reserved, 0, 0, true);
    ASSERT_TRUE(h != NULL);
    ASSERT_TRUE(wasm_runtime_attach_shared_heap(inst, h));
    uint64_t base = start_off_of(reserved);
    uint32_t v = 0;
    /* the top byte of the whole reserved region is readable (full access). */
    ASSERT_TRUE(call1(inst, env, "load8", (uint32_t)(base + reserved - 1), &v));
    ASSERT_EQ(v, (uint32_t)((reserved - 1) & 0xff));

    wasm_runtime_detach_shared_heap(inst);
    wasm_runtime_destroy_shared_heap(h);
    munmap(region, (size_t)PG);
    inst_free(inst, mod, mb, env);
    wasm_runtime_destroy();
}

/* ---- create() validation: the compatibility / reject rules ---- */
UTEST(wasm_guarded, create_validation)
{
    RuntimeInitArgs init; memset(&init, 0, sizeof(init));
    init.mem_alloc_type = Alloc_With_System_Allocator;
    ASSERT_TRUE(wasm_runtime_full_init(&init));
    PG = sysconf(_SC_PAGESIZE); if (PG <= 0) PG = 4096;
    uint8_t *region = anon(2);
    ASSERT_TRUE(region != NULL);
    uint32_t reserved = (uint32_t)PG;

    SharedHeapInitArgs a;
    /* nonzero offset with default (0) size is rejected. */
    memset(&a, 0, sizeof(a));
    a.pre_allocated_addr = region; a.size = reserved; a.read_only = true;
    a.valid_offset = 8; a.valid_size = 0;
    ASSERT_TRUE(wasm_runtime_create_shared_heap(&a) == NULL);

    /* valid_offset + valid_size > reserved is rejected. */
    memset(&a, 0, sizeof(a));
    a.pre_allocated_addr = region; a.size = reserved; a.read_only = true;
    a.valid_offset = reserved - 16; a.valid_size = 64;
    ASSERT_TRUE(wasm_runtime_create_shared_heap(&a) == NULL);

    /* a valid sub-window on a NON-pre-allocated (runtime-managed) heap rejected. */
    memset(&a, 0, sizeof(a));
    a.size = reserved; a.pre_allocated_addr = NULL;
    a.valid_offset = 0; a.valid_size = 64;
    ASSERT_TRUE(wasm_runtime_create_shared_heap(&a) == NULL);

    /* a legit sub-window succeeds. */
    memset(&a, 0, sizeof(a));
    a.pre_allocated_addr = region; a.size = reserved; a.read_only = true;
    a.valid_offset = 8; a.valid_size = 64;
    wasm_shared_heap_t h = wasm_runtime_create_shared_heap(&a);
    ASSERT_TRUE(h != NULL);
    wasm_runtime_destroy_shared_heap(h);

    munmap(region, (size_t)PG * 2);
    wasm_runtime_destroy();
}
