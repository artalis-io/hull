/*
 * Hull-tree C-level test for the internal per-invocation mapped-span attachment
 * lifecycle (mapped-spans cut 1, checkpoint 2 -- cap/wasm_spans.c). Exercised
 * through Hull's own WAMR build. NO public spans API / metadata / SDK / bindings
 * are involved (held for review); this drives the internal HlWasmSpanSet directly.
 *
 * Covers: pin+account (logical + reserved bytes), duplicate/overlap/cap
 * rejection, transactional create+attach+chain, partial-attach + chain-failure
 * rollback, reverse-order teardown on every exit path, owning-thread enforcement,
 * close-while-borrowed, cross-instance isolation, zero-span, maximum span count,
 * and -- the load-bearing property -- the shared-heap list count returning to
 * baseline after repeated invocations (via WAMR patch 0003).
 *
 * Design B (WAMR patch 0004): each heap is created over the whole page-aligned
 * mapping with a valid sub-window (valid_offset = addr - map_base, valid_size =
 * len), so the guest reaches only the caller's logical window. Verified from the
 * WASM side: an unaligned window's logical base maps to addr, and a guest read in
 * the slop prefix or the page-rounding suffix (incl. the EOF-tail past-EOF page)
 * traps.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#if defined(__linux__) && !defined(_XOPEN_SOURCE)
# define _XOPEN_SOURCE 700
#endif

#include "utest.h"

#include "hull/cap/wasm.h"
#include "hull/cap/wasm_spans.h"
#include "hull/cap/fs.h"
#include "wasm_export.h"

#include "gen_ro_heap_span_aot.h" /* build-generated: ro_heap_span_aot[] + _len (or empty) */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

UTEST_MAIN();

static char test_dir[256];
static HlFsConfig cfg;

static void setup(void)
{
    snprintf(test_dir, sizeof(test_dir), "/tmp/hull_spans_%d", getpid());
    mkdir(test_dir, 0755);
    cfg.base_dir = test_dir;
    cfg.base_len = strlen(test_dir);
}
static void teardown_dir(void)
{
    /* best-effort: unlink the files we create + rmdir. */
    char p[512];
    const char *names[] = { "a.bin", "b.bin", "c.bin", "big.bin", "big2.bin" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", test_dir, names[i]);
        unlink(p);
    }
    rmdir(test_dir);
}

/* Write `n` bytes (pattern byte i == i&0xff) so a window is content-verifiable. */
static int write_file(const char *name, size_t n)
{
    unsigned char *b = (unsigned char *)malloc(n);
    if (!b) return -1;
    for (size_t i = 0; i < n; i++) b[i] = (unsigned char)(i & 0xff);
    int rc = hl_cap_fs_write(&cfg, name, (const char *)b, n, NULL);
    free(b);
    return rc;
}

/* Create a SPARSE file of `n` bytes (ftruncate; no disk for the holes). */
static int make_sparse(const char *name, off_t n)
{
    char full[512];
    snprintf(full, sizeof(full), "%s/%s", test_dir, name);
    int fd = open(full, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    int rc = ftruncate(fd, n);
    close(fd);
    return rc;
}

/* store_i32/load_i32 module with (memory 1) -- gives instances a linear memory. */
static const unsigned char ro_heap_wasm[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x02, 0x60,
    0x01, 0x7f, 0x00, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x03, 0x03, 0x02, 0x00,
    0x01, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x21, 0x03, 0x06, 0x6d, 0x65,
    0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x09, 0x73, 0x74, 0x6f, 0x72, 0x65,
    0x5f, 0x69, 0x33, 0x32, 0x00, 0x00, 0x08, 0x6c, 0x6f, 0x61, 0x64, 0x5f,
    0x69, 0x33, 0x32, 0x00, 0x01, 0x0a, 0x17, 0x02, 0x0d, 0x00, 0x20, 0x00,
    0x41, 0xc4, 0xe6, 0x88, 0x89, 0x01, 0x36, 0x02, 0x00, 0x0b, 0x07, 0x00,
    0x20, 0x00, 0x28, 0x02, 0x00, 0x0b
};
static const unsigned int ro_heap_wasm_len = 90;

/* Instantiate a fresh instance (owns its mutable module buffer). */
static wasm_module_inst_t make_instance(wasm_module_t *out_mod, uint8_t **out_buf)
{
    uint8_t *mbuf = (uint8_t *)malloc(ro_heap_wasm_len);
    if (!mbuf) return NULL;
    memcpy(mbuf, ro_heap_wasm, ro_heap_wasm_len);
    char err[128] = { 0 };
    wasm_module_t mod = wasm_runtime_load(mbuf, ro_heap_wasm_len, err, sizeof(err));
    if (!mod) { free(mbuf); return NULL; }
    wasm_module_inst_t inst =
        wasm_runtime_instantiate(mod, 16 * 1024, 64 * 1024, err, sizeof(err));
    if (!inst) { wasm_runtime_unload(mod); free(mbuf); return NULL; }
    *out_mod = mod; *out_buf = mbuf;
    return inst;
}
static void free_instance(wasm_module_inst_t inst, wasm_module_t mod, uint8_t *buf)
{
    if (inst) wasm_runtime_deinstantiate(inst);
    if (mod) wasm_runtime_unload(mod);
    free(buf);
}

/* Instantiate from arbitrary module bytes (interp .wasm or wamrc .aot); WAMR
 * picks the loader from the magic. Owns a mutable copy of the image. */
static wasm_module_inst_t inst_from(const unsigned char *img, unsigned int len,
                                    wasm_module_t *out_mod, uint8_t **out_buf)
{
    uint8_t *mbuf = (uint8_t *)malloc(len);
    if (!mbuf) return NULL;
    memcpy(mbuf, img, len);
    char err[128] = { 0 };
    wasm_module_t mod = wasm_runtime_load(mbuf, len, err, sizeof(err));
    if (!mod) { free(mbuf); return NULL; }
    wasm_module_inst_t inst =
        wasm_runtime_instantiate(mod, 16 * 1024, 64 * 1024, err, sizeof(err));
    if (!inst) { wasm_runtime_unload(mod); free(mbuf); return NULL; }
    *out_mod = mod; *out_buf = mbuf;
    return inst;
}

/* guest load_i32(app_addr): out set to the read value; returns 1 on success, 0 if
 * the guest trapped (bounds violation). Verifies the Design B window from the
 * WASM side. */
static int guest_load(wasm_module_inst_t inst, wasm_exec_env_t env,
                      uint64_t app_addr, uint32_t *out)
{
    wasm_function_inst_t f = wasm_runtime_lookup_function(inst, "load_i32");
    uint32_t argv[2] = { (uint32_t)app_addr, 0 };
    wasm_runtime_set_exception(inst, NULL);
    if (!wasm_runtime_call_wasm(env, f, 1, argv))
        return 0;
    if (out) *out = argv[0];
    return 1;
}

/* ── basic lifecycle: add one span, attach, teardown, count -> baseline ─────── */
UTEST(wasm_spans, lifecycle_basic)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);

    wasm_module_t mod; uint8_t *mb;
    wasm_module_inst_t inst = make_instance(&mod, &mb);
    ASSERT_TRUE(inst != NULL);

    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 8192, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0 /* wasm32 */);
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, &err), 0);
    ASSERT_EQ(set.count, 1);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1);   /* heap created */
    ASSERT_EQ(hl_wasm_span_set_attach(&set, inst, &err), 0);
    ASSERT_TRUE(set.spans[0].wasm_addr != 0);                /* address computed */

    hl_wasm_span_set_teardown(&set);
    ASSERT_EQ(set.count, 0);
    ASSERT_EQ(set.inst, NULL);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);       /* reclaimed */

    hl_cap_fs_munmap(buf);
    free_instance(inst, mod, mb);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── multiple spans: chained, non-overlapping WASM addresses ────────────────── */
UTEST(wasm_spans, multiple_spans)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);

    wasm_module_t mod; uint8_t *mb;
    wasm_module_inst_t inst = make_instance(&mod, &mb);
    ASSERT_TRUE(inst != NULL);

    /* three DISTINCT windows (distinct mmaps -> distinct map_base). */
    HlMappedBuffer *b0 = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    HlMappedBuffer *b1 = hl_cap_fs_mmap_window(&cfg, "a.bin", 8192, 4096, NULL, NULL);
    HlMappedBuffer *b2 = hl_cap_fs_mmap_window(&cfg, "a.bin", 16384, 4096, NULL, NULL);
    ASSERT_TRUE(b0 && b1 && b2);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b0, &err), 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b1, &err), 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b2, &err), 0);
    ASSERT_EQ(set.count, 3);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 3);
    ASSERT_EQ(hl_wasm_span_set_attach(&set, inst, &err), 0);

    /* Design B: wasm_addr is the guest LOGICAL base; the accessible windows
     * [wasm_addr, wasm_addr+len) are non-zero and strictly disjoint. */
    for (int i = 0; i < 3; i++) ASSERT_TRUE(set.spans[i].wasm_addr != 0);
    for (int i = 0; i < 3; i++) {
        uint64_t ai0 = set.spans[i].wasm_addr;
        uint64_t ai1 = ai0 + set.spans[i].buf->len - 1;
        for (int j = i + 1; j < 3; j++) {
            uint64_t aj0 = set.spans[j].wasm_addr;
            uint64_t aj1 = aj0 + set.spans[j].buf->len - 1;
            ASSERT_FALSE(ai0 <= aj1 && aj0 <= ai1);
        }
    }

    hl_wasm_span_set_teardown(&set);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    hl_cap_fs_munmap(b0); hl_cap_fs_munmap(b1); hl_cap_fs_munmap(b2);
    free_instance(inst, mod, mb);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── duplicate backing rejected; teardown rolls back cleanly ────────────────── */
UTEST(wasm_spans, duplicate_rejected)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, &err), 0);
    /* same buffer again -> duplicate backing, no second heap, no second borrow. */
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, &err), -1);
    ASSERT_STREQ(err, "duplicate_span");
    ASSERT_EQ(set.count, 1);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1);   /* only one heap */

    hl_wasm_span_set_teardown(&set);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── 1 GiB aggregate cap enforced (overflow-safe) ───────────────────────────── */
UTEST(wasm_spans, cap_enforced)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();

    /* a 1 GiB sparse file + a small one. */
    ASSERT_EQ(make_sparse("big.bin", (off_t)1 << 30), 0);
    ASSERT_EQ(write_file("b.bin", 8192), 0);

    /* one 1 GiB window fills the cap exactly (== APP_HEAP_SIZE_MAX). */
    HlMappedBuffer *big =
        hl_cap_fs_mmap_window(&cfg, "big.bin", 0, (uint64_t)1 << 30, NULL, NULL);
    ASSERT_TRUE(big != NULL);
    HlMappedBuffer *small = hl_cap_fs_mmap_window(&cfg, "b.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(small != NULL);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, big, &err), 0);   /* total == 1 GiB */
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_add(&set, small, &err), -1); /* would exceed cap */
    ASSERT_STREQ(err, "span_cap");
    ASSERT_EQ(set.count, 1);

    hl_wasm_span_set_teardown(&set);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    hl_cap_fs_munmap(big); hl_cap_fs_munmap(small);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── close-while-borrowed: the borrow pin defers munmap until teardown ──────── */
UTEST(wasm_spans, close_while_borrowed)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 8192, 256, NULL, NULL);
    ASSERT_TRUE(buf != NULL);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, &err), 0);   /* pins buf */
    ASSERT_EQ(buf->borrow_count, 1);

    /* close the buffer while the span borrows it: munmap is deferred. */
    hl_cap_fs_munmap(buf);
    ASSERT_EQ(buf->pending_free, 1);
    ASSERT_EQ(buf->closed, 0);
    ASSERT_EQ((int)((const unsigned char *)buf->addr)[0], (int)(8192 & 0xff));

    /* teardown releases the last borrow -> the deferred munmap + free run now. */
    hl_wasm_span_set_teardown(&set);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── partial attach failure: second set on an already-attached instance fails;
 *    teardown rolls back its heaps + borrows ─────────────────────────────────── */
UTEST(wasm_spans, partial_attach_failure)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    wasm_module_t mod; uint8_t *mb;
    wasm_module_inst_t inst = make_instance(&mod, &mb);
    ASSERT_TRUE(inst != NULL);

    HlMappedBuffer *b0 = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    HlMappedBuffer *b1 = hl_cap_fs_mmap_window(&cfg, "a.bin", 8192, 4096, NULL, NULL);
    ASSERT_TRUE(b0 && b1);

    HlWasmSpanSet a, b; const char *err = NULL;
    hl_wasm_span_set_init(&a, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&a, b0, &err), 0);
    ASSERT_EQ(hl_wasm_span_set_attach(&a, inst, &err), 0);   /* inst now has a heap */

    /* set B built (heap created, borrowed), attach FAILS (inst already attached). */
    hl_wasm_span_set_init(&b, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&b, b1, &err), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 2);
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_attach(&b, inst, &err), -1);
    ASSERT_STREQ(err, "attach_failed");
    ASSERT_EQ(b.inst, NULL);
    /* teardown B: rolls back its heap + borrow (unchained, un-attached). */
    hl_wasm_span_set_teardown(&b);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1);   /* only A's heap left */

    hl_wasm_span_set_teardown(&a);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    hl_cap_fs_munmap(b0); hl_cap_fs_munmap(b1);
    free_instance(inst, mod, mb);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── owning-thread rule: add/attach from another thread is rejected ─────────── */
struct wt_arg { HlWasmSpanSet *set; HlMappedBuffer *buf; int add_rc; };
static void *wt_worker(void *p)
{
    struct wt_arg *a = (struct wt_arg *)p;
    const char *err = NULL;
    a->add_rc = hl_wasm_span_set_add(a->set, a->buf, &err); /* wrong thread */
    return NULL;
}
UTEST(wasm_spans, owning_thread_enforced)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);

    HlWasmSpanSet set;
    hl_wasm_span_set_init(&set, 0);   /* owner = this thread */
    struct wt_arg a = { &set, buf, 99 };
    pthread_t t; ASSERT_EQ(pthread_create(&t, NULL, wt_worker, &a), 0);
    pthread_join(t, NULL);
    ASSERT_EQ(a.add_rc, -1);          /* rejected on the wrong thread */
    ASSERT_EQ(set.count, 0);

    hl_wasm_span_set_teardown(&set);
    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── repeated invocations: list count returns to baseline every time (0003) ─── */
UTEST(wasm_spans, repeated_invocations_baseline)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    wasm_module_t mod; uint8_t *mb;
    wasm_module_inst_t inst = make_instance(&mod, &mb);
    ASSERT_TRUE(inst != NULL);

    for (int r = 0; r < 2000; r++) {
        HlMappedBuffer *b0 = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
        HlMappedBuffer *b1 = hl_cap_fs_mmap_window(&cfg, "a.bin", 8192, 4096, NULL, NULL);
        ASSERT_TRUE(b0 && b1);
        HlWasmSpanSet set; const char *err = NULL;
        hl_wasm_span_set_init(&set, 0);
        ASSERT_EQ(hl_wasm_span_set_add(&set, b0, &err), 0);
        ASSERT_EQ(hl_wasm_span_set_add(&set, b1, &err), 0);
        ASSERT_EQ(hl_wasm_span_set_attach(&set, inst, &err), 0);
        hl_wasm_span_set_teardown(&set);
        hl_cap_fs_munmap(b0); hl_cap_fs_munmap(b1);
        /* the load-bearing invariant: no shared-heap growth per invocation. */
        ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    }
    free_instance(inst, mod, mb);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── cross-instance isolation: two sets on two instances, independent ───────── */
UTEST(wasm_spans, cross_instance_isolation)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    wasm_module_t m0, m1; uint8_t *bb0, *bb1;
    wasm_module_inst_t i0 = make_instance(&m0, &bb0);
    wasm_module_inst_t i1 = make_instance(&m1, &bb1);
    ASSERT_TRUE(i0 && i1);

    HlMappedBuffer *s0 = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    HlMappedBuffer *s1 = hl_cap_fs_mmap_window(&cfg, "a.bin", 8192, 4096, NULL, NULL);
    ASSERT_TRUE(s0 && s1);

    HlWasmSpanSet set0, set1; const char *err = NULL;
    hl_wasm_span_set_init(&set0, 0);
    hl_wasm_span_set_init(&set1, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set0, s0, &err), 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set1, s1, &err), 0);
    ASSERT_EQ(hl_wasm_span_set_attach(&set0, i0, &err), 0);
    ASSERT_EQ(hl_wasm_span_set_attach(&set1, i1, &err), 0);   /* independent */

    /* tear down set0 first; set1 stays valid, then tear it down. */
    hl_wasm_span_set_teardown(&set0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1);    /* set1's heap remains */
    hl_wasm_span_set_teardown(&set1);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);

    hl_cap_fs_munmap(s0); hl_cap_fs_munmap(s1);
    free_instance(i0, m0, bb0);
    free_instance(i1, m1, bb1);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── concurrent invocations: N threads each drive their OWN instance + span-set
 *    build/attach/teardown, racing on the shared heap list. Validated under the
 *    WAMR-instrumented TSan (make tsan-spans). Final count returns to baseline. ─ */
#define SPAN_NTHREAD 4
#define SPAN_NITER   400
/* Each worker drives its OWN pre-made instance (WAMR's module loader is not
 * thread-safe -- a global handle_table -- so modules/instances are created on the
 * main thread; only the span-set lifecycle, which touches the shared_heap_list
 * under 0003's lock, runs concurrently). */
struct ci_arg { HlFsConfig *cfg; wasm_module_inst_t inst; int ok; };
static void *ci_worker(void *p)
{
    struct ci_arg *a = (struct ci_arg *)p;
    a->ok = 1;
    for (int r = 0; r < SPAN_NITER; r++) {
        HlMappedBuffer *b0 = hl_cap_fs_mmap_window(a->cfg, "a.bin", 0, 4096, NULL, NULL);
        HlMappedBuffer *b1 = hl_cap_fs_mmap_window(a->cfg, "a.bin", 8192, 4096, NULL, NULL);
        if (!b0 || !b1) { a->ok = 0; if (b0) hl_cap_fs_munmap(b0); if (b1) hl_cap_fs_munmap(b1); break; }
        HlWasmSpanSet set; const char *err = NULL;
        hl_wasm_span_set_init(&set, 0);
        if (hl_wasm_span_set_add(&set, b0, &err) != 0
            || hl_wasm_span_set_add(&set, b1, &err) != 0
            || hl_wasm_span_set_attach(&set, a->inst, &err) != 0)
            a->ok = 0;
        hl_wasm_span_set_teardown(&set);
        hl_cap_fs_munmap(b0); hl_cap_fs_munmap(b1);
    }
    return NULL;
}
UTEST(wasm_spans, concurrent_invocations)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);

    /* pre-create one instance per worker on THIS thread (loader/instantiate are
     * not thread-safe); each worker then owns exactly one instance. */
    wasm_module_t mod[SPAN_NTHREAD]; uint8_t *mb[SPAN_NTHREAD];
    struct ci_arg args[SPAN_NTHREAD];
    pthread_t th[SPAN_NTHREAD];
    for (int i = 0; i < SPAN_NTHREAD; i++) {
        args[i].inst = make_instance(&mod[i], &mb[i]);
        ASSERT_TRUE(args[i].inst != NULL);
        args[i].cfg = &cfg; args[i].ok = -1;
    }
    for (int i = 0; i < SPAN_NTHREAD; i++)
        ASSERT_EQ(pthread_create(&th[i], NULL, ci_worker, &args[i]), 0);
    for (int i = 0; i < SPAN_NTHREAD; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < SPAN_NTHREAD; i++) ASSERT_EQ(args[i].ok, 1);

    /* every span set was torn down on its own thread -> list back to baseline. */
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    for (int i = 0; i < SPAN_NTHREAD; i++) free_instance(args[i].inst, mod[i], mb[i]);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── zero-span: attach on an empty set is rejected; teardown is a clean no-op ── */
UTEST(wasm_spans, zero_span)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    wasm_module_t mod; uint8_t *mb;
    wasm_module_inst_t inst = make_instance(&mod, &mb);
    ASSERT_TRUE(inst != NULL);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0);
    ASSERT_EQ(hl_wasm_span_set_attach(&set, inst, &err), -1);
    ASSERT_STREQ(err, "no_spans");
    hl_wasm_span_set_teardown(&set);          /* empty set: no-op */
    ASSERT_EQ(set.count, 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);

    free_instance(inst, mod, mb);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── maximum span count (HL_WASM_MAX_SPANS): the (max+1)-th add is rejected ──── */
UTEST(wasm_spans, max_spans)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 4096;
    /* one sparse file, MAX+2 page-aligned windows so each has a distinct slot. */
    ASSERT_EQ(make_sparse("big.bin", (off_t)pg * (HL_WASM_MAX_SPANS + 2)), 0);

    HlMappedBuffer *bufs[HL_WASM_MAX_SPANS + 1];
    for (int i = 0; i < HL_WASM_MAX_SPANS + 1; i++) {
        bufs[i] = hl_cap_fs_mmap_window(&cfg, "big.bin", (uint64_t)pg * i, 256,
                                        NULL, NULL);
        ASSERT_TRUE(bufs[i] != NULL);
    }

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0);
    for (int i = 0; i < HL_WASM_MAX_SPANS; i++)
        ASSERT_EQ(hl_wasm_span_set_add(&set, bufs[i], &err), 0);
    ASSERT_EQ(set.count, HL_WASM_MAX_SPANS);
    /* the (max+1)-th add is rejected; no heap/borrow leaks. */
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_add(&set, bufs[HL_WASM_MAX_SPANS], &err), -1);
    ASSERT_STREQ(err, "too_many_spans");
    ASSERT_EQ(set.count, HL_WASM_MAX_SPANS);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + HL_WASM_MAX_SPANS);

    ASSERT_EQ(hl_wasm_span_set_teardown(&set), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);   /* list back to baseline */
    for (int i = 0; i < HL_WASM_MAX_SPANS; i++)
        ASSERT_EQ(bufs[i]->borrow_count, 0);             /* borrows back to baseline */
    ASSERT_EQ(bufs[HL_WASM_MAX_SPANS]->borrow_count, 0); /* the rejected add never borrowed */
    for (int i = 0; i < HL_WASM_MAX_SPANS + 1; i++) hl_cap_fs_munmap(bufs[i]);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── chain-failure rollback: a chain that fails mid-attach is torn down cleanly.
 *    Force the failure by attaching one of the set's heaps to a helper instance
 *    first, so its attached_count != 0 makes wasm_runtime_chain_shared_heaps fail. */
UTEST(wasm_spans, chain_failure_rollback)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    wasm_module_t mod, hmod; uint8_t *mb, *hmb;
    wasm_module_inst_t inst = make_instance(&mod, &mb);
    wasm_module_inst_t helper = make_instance(&hmod, &hmb);
    ASSERT_TRUE(inst && helper);

    HlMappedBuffer *b0 = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    HlMappedBuffer *b1 = hl_cap_fs_mmap_window(&cfg, "a.bin", 8192, 4096, NULL, NULL);
    ASSERT_TRUE(b0 && b1);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b0, &err), 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b1, &err), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 2);

    /* pin span0's heap onto the helper so the internal chain(span0, span1) fails. */
    ASSERT_TRUE(wasm_runtime_attach_shared_heap(
        helper, (wasm_shared_heap_t)set.spans[0].shared_heap));
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_attach(&set, inst, &err), -1);
    ASSERT_STREQ(err, "chain_failed");
    ASSERT_EQ(set.inst, NULL);

    /* release span0 from the helper, then teardown rolls the set back to baseline. */
    wasm_runtime_detach_shared_heap(helper);
    ASSERT_EQ(hl_wasm_span_set_teardown(&set), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);   /* list back to baseline */
    ASSERT_EQ(b0->borrow_count, 0);                       /* borrows back to baseline */
    ASSERT_EQ(b1->borrow_count, 0);

    hl_cap_fs_munmap(b0); hl_cap_fs_munmap(b1);
    free_instance(inst, mod, mb);
    free_instance(helper, hmod, hmb);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── unaligned window: a non-page-aligned file offset (slop > 0). The guest's
 *    logical base maps to buf->addr and reads the right file bytes. ──────────── */
UTEST(wasm_spans, unaligned_window)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    wasm_module_t mod; uint8_t *mb;
    wasm_module_inst_t inst = make_instance(&mod, &mb);
    ASSERT_TRUE(inst != NULL);
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 16 * 1024);
    ASSERT_TRUE(env != NULL);

    const uint64_t off = 100; /* deliberately not page-aligned -> slop = 100 */
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", off, 64, NULL, NULL);
    ASSERT_TRUE(buf != NULL);
    /* the window really is unaligned: addr = map_base + slop, slop != 0. */
    ASSERT_TRUE((uintptr_t)buf->addr > (uintptr_t)buf->map_base);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, &err), 0);
    ASSERT_EQ(hl_wasm_span_set_attach(&set, inst, &err), 0);

    /* logical base reads file bytes [off..off+4) (pattern byte i == i&0xff). */
    uint32_t v = 0;
    ASSERT_TRUE(guest_load(inst, env, set.spans[0].wasm_addr, &v));
    uint32_t exp = (uint32_t)((off) & 0xff) | (uint32_t)((off + 1) & 0xff) << 8
                 | (uint32_t)((off + 2) & 0xff) << 16
                 | (uint32_t)((off + 3) & 0xff) << 24;
    ASSERT_EQ(v, exp);

    hl_wasm_span_set_teardown(&set);
    wasm_runtime_destroy_exec_env(env);
    hl_cap_fs_munmap(buf);
    free_instance(inst, mod, mb);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── explicit prefix/suffix guest-read rejection: the guest can read the window
 *    but a read in the slop prefix or the page-rounding suffix traps. ────────── */
UTEST(wasm_spans, guest_prefix_suffix_reject)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    wasm_module_t mod; uint8_t *mb;
    wasm_module_inst_t inst = make_instance(&mod, &mb);
    ASSERT_TRUE(inst != NULL);
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 16 * 1024);
    ASSERT_TRUE(env != NULL);

    const uint64_t off = 200, len = 64; /* slop = 200, window < reserved slot */
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", off, len, NULL, NULL);
    ASSERT_TRUE(buf != NULL);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, &err), 0);
    ASSERT_EQ(hl_wasm_span_set_attach(&set, inst, &err), 0);
    uint64_t win = set.spans[0].wasm_addr;   /* guest logical base */
    uint32_t v = 0;

    /* in-window: last valid i32 reads. */
    ASSERT_TRUE(guest_load(inst, env, win + len - 4, &v));
    /* prefix (slop, just below the window) traps. */
    ASSERT_FALSE(guest_load(inst, env, win - 4, &v));
    /* suffix (page-rounding padding, just past the window) traps. */
    ASSERT_FALSE(guest_load(inst, env, win + len, &v));

    hl_wasm_span_set_teardown(&set);
    wasm_runtime_destroy_exec_env(env);
    hl_cap_fs_munmap(buf);
    free_instance(inst, mod, mb);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── EOF-tail: a window that ends mid-page at end of file. Reads within the
 *    window succeed with no SIGBUS; the suffix (past-EOF page tail) traps. ────── */
UTEST(wasm_spans, eof_tail)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    const size_t fsize = 100; /* whole file is one partial page */
    ASSERT_EQ(write_file("a.bin", fsize), 0);
    wasm_module_t mod; uint8_t *mb;
    wasm_module_inst_t inst = make_instance(&mod, &mb);
    ASSERT_TRUE(inst != NULL);
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 16 * 1024);
    ASSERT_TRUE(env != NULL);

    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, fsize, NULL, NULL);
    ASSERT_TRUE(buf != NULL);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, &err), 0);
    ASSERT_EQ(hl_wasm_span_set_attach(&set, inst, &err), 0);
    uint64_t win = set.spans[0].wasm_addr;
    uint32_t v = 0;

    /* last valid i32 in the window reads with no SIGBUS. */
    ASSERT_TRUE(guest_load(inst, env, win + fsize - 4, &v));
    /* the suffix (rest of the page, past EOF) traps -- the guard blocks it. */
    ASSERT_FALSE(guest_load(inst, env, win + fsize, &v));

    hl_wasm_span_set_teardown(&set);
    wasm_runtime_destroy_exec_env(env);
    hl_cap_fs_munmap(buf);
    free_instance(inst, mod, mb);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── teardown return status + retry under an INJECTED destroy failure. Pin a
 *    span's heap onto a helper so wasm_runtime_destroy_shared_heap fails; teardown
 *    returns -1 and RETAINS the span (heap handle + borrow reachable, not a
 *    permanent pin). Releasing the helper then lets a retry finish cleanly, with
 *    both the list count AND the borrow count back to baseline. ──────────────── */
UTEST(wasm_spans, destroy_failure_retry)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    wasm_module_t hmod; uint8_t *hmb;
    wasm_module_inst_t helper = make_instance(&hmod, &hmb);
    ASSERT_TRUE(helper != NULL);

    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, &err), 0);
    ASSERT_EQ(buf->borrow_count, 1);            /* pinned by add */

    /* pin the span's heap onto the helper -> its attached_count != 0 makes destroy
     * fail-closed (patch 0003). */
    ASSERT_TRUE(wasm_runtime_attach_shared_heap(
        helper, (wasm_shared_heap_t)set.spans[0].shared_heap));

    /* teardown cannot destroy the still-attached heap: returns -1, RETAINS it. */
    ASSERT_EQ(hl_wasm_span_set_teardown(&set), -1);
    ASSERT_EQ(set.count, 1);                    /* span retained, reachable */
    ASSERT_TRUE(set.spans[0].shared_heap != NULL);
    ASSERT_TRUE(set.spans[0].buf == buf);
    ASSERT_EQ(buf->borrow_count, 1);            /* borrow RETAINED, not released */
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1); /* heap still on the list */

    /* clear the block, then retry: now destroy succeeds -> full teardown. */
    wasm_runtime_detach_shared_heap(helper);
    ASSERT_EQ(hl_wasm_span_set_teardown(&set), 0);
    ASSERT_EQ(set.count, 0);
    ASSERT_EQ(buf->borrow_count, 0);            /* borrow back to baseline */
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);

    hl_cap_fs_munmap(buf);
    free_instance(helper, hmod, hmb);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── wasm32 address accounting: reserved total = sum(map_len), and the reserved
 *    slots sit at the TOP of the 32-bit space (below UINT32_MAX). ─────────────── */
UTEST(wasm_spans, addr_accounting_wasm32)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    wasm_module_t mod; uint8_t *mb;
    wasm_module_inst_t inst = make_instance(&mod, &mb);
    ASSERT_TRUE(inst != NULL);
    HlMappedBuffer *b0 = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    HlMappedBuffer *b1 = hl_cap_fs_mmap_window(&cfg, "a.bin", 8192, 4096, NULL, NULL);
    ASSERT_TRUE(b0 && b1);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0 /* wasm32 */);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b0, &err), 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b1, &err), 0);
    ASSERT_EQ(set.total_reserved,
              (uint64_t)b0->map_len + (uint64_t)b1->map_len);
    ASSERT_TRUE(set.total_logical < set.total_reserved
                || set.total_logical == set.total_reserved);
    ASSERT_EQ(hl_wasm_span_set_attach(&set, inst, &err), 0);
    /* logical bases are in the top of the 32-bit space and never exceed it. */
    for (int i = 0; i < 2; i++) {
        ASSERT_TRUE(set.spans[i].wasm_addr
                    > (uint64_t)UINT32_MAX - set.total_reserved);
        ASSERT_TRUE(set.spans[i].wasm_addr + set.spans[i].buf->len - 1
                    <= (uint64_t)UINT32_MAX);
    }

    ASSERT_EQ(hl_wasm_span_set_teardown(&set), 0);
    ASSERT_EQ(b0->borrow_count, 0); ASSERT_EQ(b1->borrow_count, 0);
    hl_cap_fs_munmap(b0); hl_cap_fs_munmap(b1);
    free_instance(inst, mod, mb);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── memory64 address accounting: same reserved total, but the address ceiling is
 *    UINT64_MAX, so the logical bases sit at the TOP of the 64-bit space. The heap
 *    descriptors are width-agnostic, so the accounting is validated independently
 *    of guest execution (a mem64 guest needs an AOT mem64 module, out of scope for
 *    the lifecycle accounting). ──────────────────────────────────────────────── */
UTEST(wasm_spans, addr_accounting_memory64)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    wasm_module_t mod; uint8_t *mb;
    wasm_module_inst_t inst = make_instance(&mod, &mb);
    ASSERT_TRUE(inst != NULL);
    HlMappedBuffer *b0 = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    HlMappedBuffer *b1 = hl_cap_fs_mmap_window(&cfg, "a.bin", 8192, 4096, NULL, NULL);
    ASSERT_TRUE(b0 && b1);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 1 /* memory64 */);
    ASSERT_EQ(set.is_memory64, 1);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b0, &err), 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b1, &err), 0);
    /* reserved accounting is width-independent. */
    ASSERT_EQ(set.total_reserved,
              (uint64_t)b0->map_len + (uint64_t)b1->map_len);
    ASSERT_EQ(hl_wasm_span_set_attach(&set, inst, &err), 0);
    /* logical bases are in the top of the 64-bit space (ceiling = UINT64_MAX),
     * confirming the memory64 ceiling selection independent of wasm32. */
    for (int i = 0; i < 2; i++) {
        ASSERT_TRUE(set.spans[i].wasm_addr
                    > UINT64_MAX - set.total_reserved);
    }

    ASSERT_EQ(hl_wasm_span_set_teardown(&set), 0);
    ASSERT_EQ(b0->borrow_count, 0); ASSERT_EQ(b1->borrow_count, 0);
    hl_cap_fs_munmap(b0); hl_cap_fs_munmap(b1);
    free_instance(inst, mod, mb);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── AOT span lifecycle: the full add/attach/guest-read/teardown path against a
 *    wamrc-built AOT instance of the same module, so the span lifecycle + the
 *    Design B window are exercised under AOT, not only the interpreter. Skips when
 *    no wamrc-built fixture is present; the wasm-readonly-heap-aot CI job builds
 *    wamrc and asserts this case is NOT skipped. ──────────────────────────────── */
UTEST(wasm_spans, aot_span_lifecycle)
{
    if (ro_heap_span_aot_len == 0)
        UTEST_SKIP("no wamrc-built .aot fixture in this build leg");
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t hbase = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);

    wasm_module_t mod; uint8_t *mb;
    wasm_module_inst_t inst = inst_from(ro_heap_span_aot, ro_heap_span_aot_len, &mod, &mb);
    ASSERT_TRUE(inst != NULL);
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 16 * 1024);
    ASSERT_TRUE(env != NULL);

    const uint64_t off = 100, len = 64; /* unaligned window (slop = 100) */
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", off, len, NULL, NULL);
    ASSERT_TRUE(buf != NULL);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, &err), 0);
    ASSERT_EQ(hl_wasm_span_set_attach(&set, inst, &err), 0);
    uint64_t win = set.spans[0].wasm_addr;
    uint32_t v = 0;

    /* under AOT: logical base reads the file bytes; prefix + suffix trap. */
    ASSERT_TRUE(guest_load(inst, env, win, &v));
    uint32_t exp = (uint32_t)((off) & 0xff) | (uint32_t)((off + 1) & 0xff) << 8
                 | (uint32_t)((off + 2) & 0xff) << 16
                 | (uint32_t)((off + 3) & 0xff) << 24;
    ASSERT_EQ(v, exp);
    ASSERT_FALSE(guest_load(inst, env, win - 4, &v));       /* slop prefix traps */
    ASSERT_FALSE(guest_load(inst, env, win + len, &v));     /* suffix traps */

    ASSERT_EQ(hl_wasm_span_set_teardown(&set), 0);
    ASSERT_EQ(buf->borrow_count, 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), hbase);
    wasm_runtime_destroy_exec_env(env);
    hl_cap_fs_munmap(buf);
    free_instance(inst, mod, mb);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}
