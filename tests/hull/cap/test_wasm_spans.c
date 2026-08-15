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
#include "hull/cap/wasm_buffer.h"
#include "hull/cap/wasm_data.h"
#include "hull/worker_wasm.h"
#include "hull/cap/fs.h"
#include "hull/vfs.h"
#include "hull/utils/alloc.h"
#include "wasm_export.h"

#include "gen_ro_heap_span_aot.h" /* build-generated: ro_heap_span_aot[] + _len (or empty) */

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
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

/* echo hull_process(in,in_len,out,out_max): out_max<in_len -> -2, else copy
 * in->out and return in_len. Imports env.host_call. Used to drive the D.2 spans
 * CALL path (hl_cap_wasm_call_buf) -- the span set is attached to the instance
 * for the call and torn down after; echo does not read the span (attachment is
 * metadata-independent until item E). Same bytes as tests/hull/cap/test_wasm.c. */
static const unsigned char echo_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x14, 0x03, 0x60,
  0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f,
  0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f, 0x02, 0x11, 0x01, 0x03, 0x65, 0x6e,
  0x76, 0x09, 0x68, 0x6f, 0x73, 0x74, 0x5f, 0x63, 0x61, 0x6c, 0x6c, 0x00,
  0x00, 0x03, 0x03, 0x02, 0x01, 0x02, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07,
  0x28, 0x03, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0c,
  0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73,
  0x00, 0x01, 0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x76, 0x65, 0x72, 0x73,
  0x69, 0x6f, 0x6e, 0x00, 0x02, 0x0a, 0x20, 0x02, 0x19, 0x00, 0x20, 0x01,
  0x20, 0x03, 0x4b, 0x04, 0x40, 0x41, 0x7e, 0x0f, 0x0b, 0x20, 0x02, 0x20,
  0x00, 0x20, 0x01, 0xfc, 0x0a, 0x00, 0x00, 0x20, 0x01, 0x0b, 0x04, 0x00,
  0x41, 0x01, 0x0b
};
static const unsigned int echo_wasm_len = 135;

/* spanprobe.wasm: a compute plugin that exercises host_call(SPAN_INFO). Compiled
 * from a small C source with clang --target=wasm32 (see the E commit message).
 * hull_process queries the span count + span[0]'s HlSpanMetaV1 record + reads the
 * window's first byte via the reported base, and packs the results (7 LE u32) into
 * out for the host to verify. */
static const unsigned char spanprobe_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x10, 0x02, 0x60,
  0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f,
  0x01, 0x7f, 0x02, 0x11, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x09, 0x68, 0x6f,
  0x73, 0x74, 0x5f, 0x63, 0x61, 0x6c, 0x6c, 0x00, 0x00, 0x03, 0x02, 0x01,
  0x01, 0x04, 0x05, 0x01, 0x70, 0x01, 0x01, 0x01, 0x05, 0x03, 0x01, 0x00,
  0x02, 0x06, 0x08, 0x01, 0x7f, 0x01, 0x41, 0x80, 0x80, 0x04, 0x0b, 0x07,
  0x19, 0x02, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0c,
  0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73,
  0x00, 0x01, 0x0a, 0xdc, 0x03, 0x01, 0xd9, 0x03, 0x01, 0x05, 0x7f, 0x41,
  0x7e, 0x21, 0x04, 0x02, 0x40, 0x20, 0x01, 0x41, 0x0d, 0x48, 0x0d, 0x00,
  0x20, 0x03, 0x41, 0x8c, 0x01, 0x48, 0x0d, 0x00, 0x20, 0x00, 0x28, 0x00,
  0x04, 0x21, 0x05, 0x20, 0x00, 0x2d, 0x00, 0x00, 0x21, 0x03, 0x20, 0x00,
  0x2d, 0x00, 0x01, 0x21, 0x06, 0x20, 0x00, 0x28, 0x00, 0x08, 0x21, 0x01,
  0x41, 0x80, 0x80, 0x84, 0x80, 0x00, 0x21, 0x04, 0x41, 0x80, 0x80, 0x84,
  0x80, 0x00, 0x20, 0x00, 0x2d, 0x00, 0x0c, 0x41, 0x80, 0x01, 0xfc, 0x0b,
  0x00, 0x20, 0x03, 0x20, 0x06, 0x41, 0x08, 0x74, 0x72, 0x21, 0x07, 0x3f,
  0x00, 0x41, 0x10, 0x74, 0x21, 0x08, 0x41, 0xf0, 0xff, 0xff, 0xff, 0x07,
  0x21, 0x00, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40,
  0x20, 0x01, 0x41, 0x7f, 0x6a, 0x0e, 0x04, 0x04, 0x00, 0x01, 0x02, 0x03,
  0x0b, 0x41, 0x80, 0x80, 0x80, 0x80, 0x78, 0x21, 0x00, 0x0c, 0x03, 0x0b,
  0x20, 0x08, 0x20, 0x07, 0x41, 0xff, 0xff, 0x03, 0x71, 0x6b, 0x21, 0x04,
  0x0c, 0x01, 0x0b, 0x20, 0x08, 0x20, 0x07, 0x41, 0xff, 0xff, 0x03, 0x71,
  0x6b, 0x41, 0x01, 0x6a, 0x21, 0x04, 0x0b, 0x20, 0x04, 0x20, 0x06, 0x3a,
  0x00, 0x03, 0x20, 0x04, 0x20, 0x03, 0x3a, 0x00, 0x02, 0x20, 0x04, 0x21,
  0x00, 0x0b, 0x41, 0x00, 0x21, 0x03, 0x41, 0x04, 0x41, 0x00, 0x41, 0x7f,
  0x10, 0x80, 0x80, 0x80, 0x80, 0x00, 0x21, 0x04, 0x41, 0x04, 0x20, 0x00,
  0x20, 0x05, 0x10, 0x80, 0x80, 0x80, 0x80, 0x00, 0x21, 0x00, 0x02, 0x40,
  0x20, 0x01, 0x0d, 0x00, 0x20, 0x07, 0x41, 0xff, 0xff, 0x03, 0x71, 0x41,
  0xd0, 0x00, 0x49, 0x0d, 0x00, 0x20, 0x00, 0x41, 0xe0, 0x00, 0x47, 0x0d,
  0x00, 0x41, 0x00, 0x28, 0x02, 0xc8, 0x80, 0x84, 0x80, 0x00, 0x2d, 0x00,
  0x00, 0x21, 0x03, 0x0b, 0x20, 0x02, 0x20, 0x00, 0x3a, 0x00, 0x04, 0x20,
  0x02, 0x20, 0x04, 0x3a, 0x00, 0x00, 0x20, 0x02, 0x20, 0x00, 0x41, 0x18,
  0x76, 0x3a, 0x00, 0x07, 0x20, 0x02, 0x20, 0x00, 0x41, 0x10, 0x76, 0x3a,
  0x00, 0x06, 0x20, 0x02, 0x20, 0x00, 0x41, 0x08, 0x76, 0x3a, 0x00, 0x05,
  0x20, 0x02, 0x20, 0x04, 0x41, 0x18, 0x76, 0x3a, 0x00, 0x03, 0x20, 0x02,
  0x20, 0x04, 0x41, 0x10, 0x76, 0x3a, 0x00, 0x02, 0x20, 0x02, 0x20, 0x04,
  0x41, 0x08, 0x76, 0x3a, 0x00, 0x01, 0x41, 0x80, 0x7f, 0x21, 0x00, 0x03,
  0x40, 0x20, 0x02, 0x20, 0x00, 0x6a, 0x22, 0x04, 0x41, 0x88, 0x01, 0x6a,
  0x20, 0x00, 0x41, 0x80, 0x81, 0x84, 0x80, 0x00, 0x6a, 0x2d, 0x00, 0x00,
  0x3a, 0x00, 0x00, 0x20, 0x04, 0x41, 0x89, 0x01, 0x6a, 0x20, 0x00, 0x41,
  0x81, 0x81, 0x84, 0x80, 0x00, 0x6a, 0x2d, 0x00, 0x00, 0x3a, 0x00, 0x00,
  0x20, 0x04, 0x41, 0x8a, 0x01, 0x6a, 0x20, 0x00, 0x41, 0x82, 0x81, 0x84,
  0x80, 0x00, 0x6a, 0x2d, 0x00, 0x00, 0x3a, 0x00, 0x00, 0x20, 0x04, 0x41,
  0x8b, 0x01, 0x6a, 0x20, 0x00, 0x41, 0x83, 0x81, 0x84, 0x80, 0x00, 0x6a,
  0x2d, 0x00, 0x00, 0x3a, 0x00, 0x00, 0x20, 0x00, 0x41, 0x04, 0x6a, 0x22,
  0x00, 0x0d, 0x00, 0x0b, 0x20, 0x02, 0x41, 0x00, 0x3a, 0x00, 0x8b, 0x01,
  0x20, 0x02, 0x41, 0x00, 0x3b, 0x00, 0x89, 0x01, 0x20, 0x02, 0x20, 0x03,
  0x3a, 0x00, 0x88, 0x01, 0x41, 0x8c, 0x01, 0x21, 0x04, 0x0b, 0x20, 0x04,
  0x0b, 0x00, 0x46, 0x04, 0x6e, 0x61, 0x6d, 0x65, 0x00, 0x0f, 0x0e, 0x73,
  0x70, 0x61, 0x6e, 0x70, 0x72, 0x6f, 0x62, 0x65, 0x2e, 0x77, 0x61, 0x73,
  0x6d, 0x01, 0x1a, 0x02, 0x00, 0x09, 0x68, 0x6f, 0x73, 0x74, 0x5f, 0x63,
  0x61, 0x6c, 0x6c, 0x01, 0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x70, 0x72,
  0x6f, 0x63, 0x65, 0x73, 0x73, 0x07, 0x12, 0x01, 0x00, 0x0f, 0x5f, 0x5f,
  0x73, 0x74, 0x61, 0x63, 0x6b, 0x5f, 0x70, 0x6f, 0x69, 0x6e, 0x74, 0x65,
  0x72, 0x00, 0x2f, 0x09, 0x70, 0x72, 0x6f, 0x64, 0x75, 0x63, 0x65, 0x72,
  0x73, 0x01, 0x0c, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73, 0x65, 0x64,
  0x2d, 0x62, 0x79, 0x01, 0x0e, 0x48, 0x6f, 0x6d, 0x65, 0x62, 0x72, 0x65,
  0x77, 0x20, 0x63, 0x6c, 0x61, 0x6e, 0x67, 0x06, 0x32, 0x32, 0x2e, 0x31,
  0x2e, 0x38, 0x00, 0x94, 0x01, 0x0f, 0x74, 0x61, 0x72, 0x67, 0x65, 0x74,
  0x5f, 0x66, 0x65, 0x61, 0x74, 0x75, 0x72, 0x65, 0x73, 0x08, 0x2b, 0x0b,
  0x62, 0x75, 0x6c, 0x6b, 0x2d, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x2b,
  0x0f, 0x62, 0x75, 0x6c, 0x6b, 0x2d, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79,
  0x2d, 0x6f, 0x70, 0x74, 0x2b, 0x16, 0x63, 0x61, 0x6c, 0x6c, 0x2d, 0x69,
  0x6e, 0x64, 0x69, 0x72, 0x65, 0x63, 0x74, 0x2d, 0x6f, 0x76, 0x65, 0x72,
  0x6c, 0x6f, 0x6e, 0x67, 0x2b, 0x0a, 0x6d, 0x75, 0x6c, 0x74, 0x69, 0x76,
  0x61, 0x6c, 0x75, 0x65, 0x2b, 0x0f, 0x6d, 0x75, 0x74, 0x61, 0x62, 0x6c,
  0x65, 0x2d, 0x67, 0x6c, 0x6f, 0x62, 0x61, 0x6c, 0x73, 0x2b, 0x13, 0x6e,
  0x6f, 0x6e, 0x74, 0x72, 0x61, 0x70, 0x70, 0x69, 0x6e, 0x67, 0x2d, 0x66,
  0x70, 0x74, 0x6f, 0x69, 0x6e, 0x74, 0x2b, 0x0f, 0x72, 0x65, 0x66, 0x65,
  0x72, 0x65, 0x6e, 0x63, 0x65, 0x2d, 0x74, 0x79, 0x70, 0x65, 0x73, 0x2b,
  0x08, 0x73, 0x69, 0x67, 0x6e, 0x2d, 0x65, 0x78, 0x74
};
static const unsigned int spanprobe_wasm_len = 849;

static const HlEntry span_call_entries[] = {
    { "compute/echo.wasm", echo_wasm, echo_wasm_len },
    { "compute/spanprobe.wasm", spanprobe_wasm, spanprobe_wasm_len },
    { 0, 0, 0 }
};

/* One-shot: call "echo" with a single mapped span attached + `input`, returning
 * the string output via hl_cap_wasm_call. */
static int call_echo_with_span(HlWasmCache *cache, HlVfs *vfs, HlMappedBuffer *buf,
                               const char *name, const void *input, size_t in_len,
                               HlWasmCallOpts *opts_extra,
                               void **out, size_t *out_len, const char **err)
{
    HlWasmSpanReq req = { .name = name, .buf = buf };
    HlWasmCallOpts opts = {0};
    if (opts_extra) opts = *opts_extra;
    opts.spans = &req;
    opts.span_count = 1;
    return hl_cap_wasm_call(cache, "echo", input, in_len, out, out_len,
                            &opts, NULL, NULL, vfs, NULL, NULL, err);
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
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, "buf", &err), 0);
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
    ASSERT_EQ(hl_wasm_span_set_add(&set, b0, "b0", &err), 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b1, "b1", &err), 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b2, "b2", &err), 0);
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
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, "buf", &err), 0);
    /* same buffer again (distinct NAME, so it reaches the backing check rather
     * than the name-uniqueness check) -> duplicate backing, no second heap/borrow. */
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, "buf_again", &err), -1);
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
    ASSERT_EQ(hl_wasm_span_set_add(&set, big, "big", &err), 0);   /* total == 1 GiB */
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_add(&set, small, "small", &err), -1); /* would exceed cap */
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
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, "buf", &err), 0);   /* pins buf */
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
    ASSERT_EQ(hl_wasm_span_set_add(&a, b0, "b0", &err), 0);
    ASSERT_EQ(hl_wasm_span_set_attach(&a, inst, &err), 0);   /* inst now has a heap */

    /* set B built (heap created, borrowed), attach FAILS (inst already attached). */
    hl_wasm_span_set_init(&b, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&b, b1, "b1", &err), 0);
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
    a->add_rc = hl_wasm_span_set_add(a->set, a->buf, "a->buf", &err); /* wrong thread */
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
        ASSERT_EQ(hl_wasm_span_set_add(&set, b0, "b0", &err), 0);
        ASSERT_EQ(hl_wasm_span_set_add(&set, b1, "b1", &err), 0);
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
    ASSERT_EQ(hl_wasm_span_set_add(&set0, s0, "s0", &err), 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set1, s1, "s1", &err), 0);
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

/* ── cross-instance isolation, PROVEN BY EXECUTION: instance i1 cannot LOAD from
 *    an address that is a valid span only in i0. i0 gets two spans (so its lower
 *    slot sits below i1's single top slot); i1 has only the top slot. A guest load
 *    in i1 at i0's lower-span address is out of every range i1 has attached and
 *    TRAPS, while i1's own span and i0's own load of the same address both succeed
 *    -- so the trap is isolation, not a bogus address. ─────────────────────────── */
UTEST(wasm_spans, cross_instance_execution_isolation)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    wasm_module_t m0, m1; uint8_t *bb0, *bb1;
    wasm_module_inst_t i0 = make_instance(&m0, &bb0);
    wasm_module_inst_t i1 = make_instance(&m1, &bb1);
    ASSERT_TRUE(i0 && i1);
    wasm_exec_env_t e0 = wasm_runtime_create_exec_env(i0, 16 * 1024);
    wasm_exec_env_t e1 = wasm_runtime_create_exec_env(i1, 16 * 1024);
    ASSERT_TRUE(e0 && e1);

    /* i0 owns TWO page-aligned windows -> two chained slots (top + one below). */
    HlMappedBuffer *s0a = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    HlMappedBuffer *s0b = hl_cap_fs_mmap_window(&cfg, "a.bin", 8192, 4096, NULL, NULL);
    /* i1 owns ONE window -> a single slot at the top of the address space. */
    HlMappedBuffer *s1 = hl_cap_fs_mmap_window(&cfg, "a.bin", 16384, 4096, NULL, NULL);
    ASSERT_TRUE(s0a && s0b && s1);

    HlWasmSpanSet set0, set1; const char *err = NULL;
    hl_wasm_span_set_init(&set0, 0);
    hl_wasm_span_set_init(&set1, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set0, s0a, "s0a", &err), 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set0, s0b, "s0b", &err), 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set1, s1, "s1", &err), 0);
    ASSERT_EQ(hl_wasm_span_set_attach(&set0, i0, &err), 0);
    ASSERT_EQ(hl_wasm_span_set_attach(&set1, i1, &err), 0);

    /* s0a is i0's LOWER slot; with only a single (top) slot attached, that address
     * is not in any range i1 can reach. Confirm it is a real, reachable address in
     * i0 first, then that the SAME address traps in i1 -- pure isolation. */
    uint64_t s0a_addr = set0.spans[0].wasm_addr;
    uint32_t v = 0;
    ASSERT_TRUE(guest_load(i0, e0, s0a_addr, &v));            /* i0 owns it */
    ASSERT_FALSE(guest_load(i1, e1, s0a_addr, &v));           /* i1 cannot reach it */
    ASSERT_TRUE(guest_load(i1, e1, set1.spans[0].wasm_addr, &v)); /* i1's own span ok */

    hl_wasm_span_set_teardown(&set0);
    hl_wasm_span_set_teardown(&set1);
    wasm_runtime_destroy_exec_env(e0);
    wasm_runtime_destroy_exec_env(e1);
    hl_cap_fs_munmap(s0a); hl_cap_fs_munmap(s0b); hl_cap_fs_munmap(s1);
    free_instance(i0, m0, bb0);
    free_instance(i1, m1, bb1);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── distinct-buffer native-backing rejection: two DIFFERENT HlMappedBuffers that
 *    alias the same native mapping (or whose native ranges overlap) are rejected,
 *    exercising the map_base-equality and ranges_overlap arms of the duplicate
 *    check that the same-pointer case (duplicate_rejected) does not reach. The
 *    real windowed constructor never produces aliasing mappings (independent mmaps
 *    are disjoint), so a synthetic alias drives these defense-in-depth arms. The
 *    alias is rejected BEFORE any borrow/heap-create, so it is never pinned and
 *    needs no teardown. ──────────────────────────────────────────────────────── */
UTEST(wasm_spans, distinct_buffer_overlap_rejected)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *b0 = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(b0 != NULL);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b0, "b0", &err), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1);

    /* (a) distinct buffer, SAME native base -> map_base-equality arm. */
    HlMappedBuffer alias; memset(&alias, 0, sizeof(alias));
    alias.map_base = b0->map_base;
    alias.map_len = b0->map_len;
    alias.addr = b0->map_base;   /* voff = 0 */
    alias.len = 256;
    ASSERT_TRUE(&alias != b0);
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_add(&set, &alias, "alias", &err), -1);
    ASSERT_STREQ(err, "duplicate_span");

    /* (b) distinct buffer, DISTINCT base but OVERLAPPING native range -> the
     * ranges_overlap arm. Base is offset into b0's mapping so the ranges meet. */
    HlMappedBuffer overlap; memset(&overlap, 0, sizeof(overlap));
    overlap.map_base = (char *)b0->map_base + 2048; /* != b0->map_base, overlaps */
    overlap.map_len = b0->map_len;
    overlap.addr = overlap.map_base;
    overlap.len = 256;
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_add(&set, &overlap, "overlap", &err), -1);
    ASSERT_STREQ(err, "duplicate_span");

    /* neither synthetic alias was pinned or created a heap. */
    ASSERT_EQ(set.count, 1);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1);

    hl_wasm_span_set_teardown(&set);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    hl_cap_fs_munmap(b0);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── span name validation: NULL / empty / overlong rejected ("bad_name"), and a
 *    distinct buffer with a DUPLICATE name rejected ("duplicate_name", distinct
 *    from the same-backing "duplicate_span"). Each rejection happens before any
 *    borrow/heap-create, so no heap/borrow leaks. ─────────────────────────────── */
UTEST(wasm_spans, name_validation)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *b0 = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    HlMappedBuffer *b1 = hl_cap_fs_mmap_window(&cfg, "a.bin", 8192, 4096, NULL, NULL);
    ASSERT_TRUE(b0 && b1);

    HlWasmSpanSet set; const char *err = NULL;
    hl_wasm_span_set_init(&set, 0);

    /* NULL name -> internal_error (defensive; the binding never passes NULL). */
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_add(&set, b0, NULL, &err), -1);
    ASSERT_STREQ(err, "internal_error");
    /* empty name -> bad_name. */
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_add(&set, b0, "", &err), -1);
    ASSERT_STREQ(err, "bad_name");
    /* 64-byte name (>= sizeof name[64], no room for the NUL) -> bad_name. */
    char overlong[65];
    memset(overlong, 'x', 64);
    overlong[64] = '\0';
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_add(&set, b0, overlong, &err), -1);
    ASSERT_STREQ(err, "bad_name");
    /* 63-byte name is the max accepted. */
    char maxname[64];
    memset(maxname, 'y', 63);
    maxname[63] = '\0';
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_add(&set, b0, maxname, &err), 0);
    ASSERT_EQ(set.count, 1);
    ASSERT_STREQ(set.spans[0].name, maxname);

    /* a DISTINCT buffer with the SAME name -> duplicate_name (not duplicate_span:
     * different backing, colliding name). */
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_add(&set, b1, maxname, &err), -1);
    ASSERT_STREQ(err, "duplicate_name");
    ASSERT_EQ(set.count, 1);
    ASSERT_EQ(b1->borrow_count, 0);   /* the rejected add never borrowed */

    ASSERT_EQ(hl_wasm_span_set_teardown(&set), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    ASSERT_EQ(b0->borrow_count, 0);
    hl_cap_fs_munmap(b0); hl_cap_fs_munmap(b1);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── malformed span request guard (item D consumption entry): a request whose
 *    spans/span_count disagree is rejected "bad_spans" BEFORE module lookup /
 *    buffer borrow / instance acquisition, with NO heap / borrow / module-cache
 *    state change. Scripts can't build this (the binding sets both or neither);
 *    this drives it directly through the C API. ──────────────────────────────── */
UTEST(wasm_spans, guard_malformed_span_request)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);
    HlWasmSpanReq req = { .name = "src", .buf = buf };

    struct { const char *label; const HlWasmSpanReq *spans; int count; } cases[] = {
        { "spans set, count 0",   &req, 0 },
        { "spans NULL, count 1",  NULL, 1 },
        { "count negative",       &req, -1 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        HlWasmCallOpts opts = {0};
        opts.spans = cases[i].spans;
        opts.span_count = cases[i].count;
        void *out = (void *)0x1;
        size_t out_len = 99;
        const char *err = NULL;
        int rc = hl_cap_wasm_call(&cache, "nomod", "x", 1, &out, &out_len,
                                  &opts, NULL, NULL, NULL, NULL, NULL, &err);
        ASSERT_NE(rc, HL_WASM_OK);                 /* rejected */
        ASSERT_TRUE(err != NULL);
        ASSERT_STREQ(err, "bad_spans");
        ASSERT_TRUE(out == NULL);                  /* no output produced */
        ASSERT_EQ(out_len, (size_t)0);
        ASSERT_EQ(wasm_runtime_shared_heap_count(), base); /* no heap created */
        ASSERT_EQ(buf->borrow_count, 0);           /* no borrow taken */
        ASSERT_EQ(cache.count, 0);                 /* no module loaded/acquired */
    }

    /* a well-formed request passes the guard (then fails module lookup): proves
     * the guard rejects ONLY the malformed shapes. */
    HlWasmCallOpts ok_opts = {0};
    ok_opts.spans = &req;
    ok_opts.span_count = 1;
    void *out = NULL; size_t out_len = 0; const char *err = NULL;
    int rc = hl_cap_wasm_call(&cache, "nomod", "x", 1, &out, &out_len,
                              &ok_opts, NULL, NULL, NULL, NULL, NULL, &err);
    ASSERT_NE(rc, HL_WASM_OK);
    ASSERT_STREQ(err, "not_found");                /* past the guard, into lookup */
    ASSERT_EQ(buf->borrow_count, 0);

    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ══ D.2: pooled synchronous span consumption in hl_cap_wasm_call_buf ═══════════
 * These drive the real call path (echo hull_process) with a span attached, and
 * assert the whole cleanup discipline: every exit tears the span set down before
 * pool release / output ownership, restoring the shared-heap count and the mmap
 * borrow to baseline. The span is metadata-independent (echo never reads it).   */

/* ── success: a span attaches, the call runs, teardown restores baseline ─────── */
UTEST(wasm_spans, d2_success_attach_and_teardown)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);

    const char *input = "hello spans";
    void *out = NULL; size_t out_len = 0; const char *err = NULL;
    int rc = call_echo_with_span(&cache, &vfs, buf, "src", input, strlen(input),
                                 NULL, &out, &out_len, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(out_len, strlen(input));
    ASSERT_TRUE(out != NULL);
    ASSERT_EQ(memcmp(out, input, out_len), 0);   /* echo returned the input */
    free(out);

    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);  /* span heap torn down */
    ASSERT_EQ(buf->borrow_count, 0);                     /* borrow released */

    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── D1: a spans call on a module with an active segment is rejected, with no
 *    span heap created and no borrow taken (the segment's own heap is unaffected). */
UTEST(wasm_spans, d2_d1_rejection)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);

    /* load echo + install a segment so mod->shared_data != NULL. */
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);
    const char *derr = NULL;
    static const unsigned char seg[16] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "graph", seg, sizeof(seg),
                                    NULL, &vfs, NULL, &derr), 0);
    uint32_t after_seg = wasm_runtime_shared_heap_count(); /* base + the segment heap */
    ASSERT_TRUE(after_seg > base);

    const char *input = "x";
    void *out = (void *)0x1; size_t out_len = 9; const char *err = NULL;
    int rc = call_echo_with_span(&cache, &vfs, buf, "src", input, 1, NULL,
                                 &out, &out_len, &err);
    ASSERT_NE(rc, HL_WASM_OK);
    ASSERT_STREQ(err, "spans_with_segments");
    ASSERT_TRUE(out == NULL);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), after_seg); /* no span heap added */
    ASSERT_EQ(buf->borrow_count, 0);                        /* no borrow taken */

    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── gas exhaustion still tears the span set down (baseline restored) ────────── */
UTEST(wasm_spans, d2_gas_cleanup)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);

    HlWasmCallOpts extra = {0};
    extra.gas = 1;   /* exhaust immediately -> the call traps */
    void *out = NULL; size_t out_len = 0; const char *err = NULL;
    int rc = call_echo_with_span(&cache, &vfs, buf, "src", "hello", 5, &extra,
                                 &out, &out_len, &err);
    /* The call trapped (gas). What matters for D.2 is that the trap path tears the
     * span set down: exact error classification is orthogonal (WAMR emits
     * "instruction limit exceeded"; the cap layer maps it to call_failed). */
    ASSERT_NE(rc, HL_WASM_OK);
    ASSERT_TRUE(out == NULL);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);  /* torn down on trap */
    ASSERT_EQ(buf->borrow_count, 0);

    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── output alloc failure (tiny heap + big output) tears the span set down ───── */
UTEST(wasm_spans, d2_output_alloc_fail)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);

    HlWasmCallOpts extra = {0};
    extra.heap_size = 65536;              /* 64 KiB app heap */
    extra.max_output = 8 * 1024 * 1024;   /* 8 MiB output cannot fit -> alloc fails */
    void *out = NULL; size_t out_len = 0; const char *err = NULL;
    int rc = call_echo_with_span(&cache, &vfs, buf, "src", "hi", 2, &extra,
                                 &out, &out_len, &err);
    ASSERT_NE(rc, HL_WASM_OK);            /* output alloc failed */
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);  /* span torn down */
    ASSERT_EQ(buf->borrow_count, 0);

    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── pooled reuse: repeated span calls each return to baseline (per-call
 *    teardown keeps the pooled instance span-free between calls) ──────────────── */
UTEST(wasm_spans, d2_pooled_reuse)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);

    for (int i = 0; i < 50; i++) {
        HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
        ASSERT_TRUE(buf != NULL);
        void *out = NULL; size_t out_len = 0; const char *err = NULL;
        int rc = call_echo_with_span(&cache, &vfs, buf, "src", "abc", 3, NULL,
                                     &out, &out_len, &err);
        ASSERT_EQ(rc, HL_WASM_OK);
        ASSERT_EQ(out_len, (size_t)3);
        free(out);
        ASSERT_EQ(buf->borrow_count, 0);
        ASSERT_EQ(wasm_runtime_shared_heap_count(), base); /* baseline every call */
        hl_cap_fs_munmap(buf);
    }

    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── zero-copy output: the span is torn down BEFORE the returned WasmBuffer keeps
 *    the instance checked out; the output bytes stay valid; baseline restored ─── */
UTEST(wasm_spans, d2_zero_copy_lifetime)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);

    HlWasmSpanReq req = { .name = "src", .buf = buf };
    HlWasmCallOpts opts = {0};
    opts.spans = &req; opts.span_count = 1;
    HlWasmBuffer *ob = NULL; const char *err = NULL;
    int rc = hl_cap_wasm_call_buf(&cache, "echo", "wasmbuf", 7, &ob, &opts,
                                  NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_TRUE(ob != NULL);
    /* span already torn down even though the buffer keeps the instance out. */
    ASSERT_EQ(buf->borrow_count, 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    /* the zero-copy output still reads correctly. */
    ASSERT_EQ(hl_wasm_buffer_len(ob), (size_t)7);
    ASSERT_EQ(memcmp(hl_wasm_buffer_data(ob), "wasmbuf", 7), 0);
    hl_wasm_buffer_destroy(ob);
    hl_alloc_free(NULL, ob, sizeof(*ob));

    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── concurrency: workers call echo-with-span while a mutator toggles a segment.
 *    The D1 decision reads shared_data under mod->mutex (the same lock the mutator
 *    holds), so each call deterministically either SUCCEEDS or cleanly rejects
 *    "spans_with_segments" -- never a torn read, crash, or other error. Final
 *    heap count returns to baseline. TSan-validated. ─────────────────────────── */
#define D2R_NTHREAD 4
#define D2R_NITER   150
struct d2r_arg { HlWasmCache *cache; HlVfs *vfs; HlFsConfig *cfg; int ok; };
static void *d2r_worker(void *p)
{
    struct d2r_arg *a = (struct d2r_arg *)p;
    a->ok = 1;
    for (int i = 0; i < D2R_NITER; i++) {
        HlMappedBuffer *b = hl_cap_fs_mmap_window(a->cfg, "a.bin", 0, 4096, NULL, NULL);
        if (!b) { a->ok = 0; break; }
        void *out = NULL; size_t out_len = 0; const char *err = NULL;
        int rc = call_echo_with_span(a->cache, a->vfs, b, "src", "z", 1, NULL,
                                     &out, &out_len, &err);
        if (rc == HL_WASM_OK) {
            free(out);
        } else if (!(err && strcmp(err, "spans_with_segments") == 0)) {
            a->ok = 0; /* any OTHER error is a failure */
        }
        if (b->borrow_count != 0) a->ok = 0;   /* borrow always released */
        hl_cap_fs_munmap(b);
        if (!a->ok) break;
    }
    return NULL;
}
UTEST(wasm_spans, d2_segment_race_concurrency)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    uint32_t base = wasm_runtime_shared_heap_count();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    /* pre-load on the main thread (WAMR's module loader is not thread-safe). */
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);

    struct d2r_arg args[D2R_NTHREAD];
    pthread_t th[D2R_NTHREAD];
    for (int i = 0; i < D2R_NTHREAD; i++) {
        args[i].cache = &cache; args[i].vfs = &vfs; args[i].cfg = &cfg; args[i].ok = -1;
        ASSERT_EQ(pthread_create(&th[i], NULL, d2r_worker, &args[i]), 0);
    }
    /* mutator: toggle a segment on/off, racing the D1 decision. */
    static const unsigned char seg[16] = { 9, 9, 9, 9 };
    for (int r = 0; r < D2R_NITER; r++) {
        const char *derr = NULL;
        hl_cap_wasm_data_load(&cache, "echo", "graph", seg, sizeof(seg),
                              NULL, &vfs, NULL, &derr);
        hl_cap_wasm_data_load(&cache, "echo", "graph", NULL, 0,
                              NULL, &vfs, NULL, &derr); /* remove */
    }
    for (int i = 0; i < D2R_NTHREAD; i++) pthread_join(th[i], NULL);
    /* The proof: every call deterministically SUCCEEDED or cleanly rejected
     * "spans_with_segments" (never a torn read / unexpected error / crash), and
     * every span borrow was released -- so the D1 decision could not race the
     * segment mutation (mod->mutex serialises the read against the write), and no
     * span leaked under contention. TSan (make tsan-spans) covers the data-race
     * dimension. The mutator loop ends on a "remove", and all worker threads are
     * joined, so the runtime is quiescent with no segment installed and no span in
     * flight: since #315 the segment path also destroys its heap descriptors
     * (hl_wasm_free_segment via the drain-detach ordering), so the shared-heap
     * count is back to baseline -- the churn no longer leaks. */
    for (int i = 0; i < D2R_NTHREAD; i++) ASSERT_EQ(args[i].ok, 1);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);

    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── #315: the compute.segment path destroys its shared-heap descriptors on every
 *    teardown path, so the shared-heap-list count returns to baseline across
 *    load / remove / replace / remove-all / unload -- including after a call has
 *    attached the chain to a (now drained + detached) pooled instance. Purely
 *    single-threaded + deterministic, unlike d2_segment_race_concurrency. ─────── */
UTEST(wasm_spans, segment_lifecycle_baseline)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);
    uint32_t base = wasm_runtime_shared_heap_count();
    static const unsigned char seg[16]  = { 1, 2, 3, 4, 5, 6, 7, 8 };
    static const unsigned char seg2[16] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    const char *derr = NULL;

    /* A. load one segment, run a plain call (attaches the chain to a pooled
     *    instance), then remove -> the drain detaches and free destroys. */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "graph", seg, sizeof(seg),
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1);
    {
        void *out = NULL; size_t out_len = 0; const char *err = NULL;
        int rc = hl_cap_wasm_call(&cache, "echo", "x", 1, &out, &out_len,
                                  NULL, NULL, NULL, &vfs, NULL, NULL, &err);
        ASSERT_EQ(rc, HL_WASM_OK);
        free(out);
    }
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1); /* call did not leak */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "graph", NULL, 0,
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);     /* descriptor reclaimed */

    /* B. load / remove churn: back to baseline every cycle (no monotonic growth). */
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "g", seg, sizeof(seg),
                                        NULL, &vfs, NULL, &derr), 0);
        ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1);
        ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "g", NULL, 0,
                                        NULL, &vfs, NULL, &derr), 0);
        ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    }

    /* C. replace (same name, new data): the old heap is destroyed, one new heap
     *    created -> net +1, never +2. */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "graph", seg, sizeof(seg),
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "graph", seg2, sizeof(seg2),
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1); /* replaced, not doubled */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "graph", NULL, 0,
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);

    /* D. three segments (a chain), a call to attach the chain head, then
     *    remove-all (segment_name==NULL && data==NULL) -> unchain + destroy all. */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "a", seg, sizeof(seg),
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "b", seg, sizeof(seg),
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "c", seg, sizeof(seg),
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 3);
    {
        void *out = NULL; size_t out_len = 0; const char *err = NULL;
        int rc = hl_cap_wasm_call(&cache, "echo", "x", 1, &out, &out_len,
                                  NULL, NULL, NULL, &vfs, NULL, NULL, &err);
        ASSERT_EQ(rc, HL_WASM_OK);
        free(out);
    }
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", NULL, NULL, 0,
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);     /* whole chain reclaimed */

    /* E. reload a chain, then unload the module's shared data (hl_cap_wasm_data_unload)
     *    -> same drain + free_shared_data path -> baseline. */
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "a", seg, sizeof(seg),
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "b", seg, sizeof(seg),
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 2);
    hl_cap_wasm_data_unload(&cache, "echo");
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);

    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── #315: the destroy-FAILURE branch of the segment teardown. Pin a segment's
 *    heap onto a helper instance so wasm_runtime_destroy_shared_heap fails; the
 *    remove then RETAINS the descriptor + backing (never munmaps behind a live
 *    descriptor) and reports failure so the binding keeps its MappedBuffer pin.
 *    Clearing the block lets a retry finish cleanly, back to baseline. This is the
 *    path the normal/TSan/ASan matrix does not otherwise exercise. ────────────── */
UTEST(wasm_spans, segment_destroy_failure_retain)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);
    uint32_t base = wasm_runtime_shared_heap_count();

    wasm_module_t hmod; uint8_t *hmb;
    wasm_module_inst_t helper = make_instance(&hmod, &hmb);
    ASSERT_TRUE(helper != NULL);

    static const unsigned char seg[16] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const char *derr = NULL;
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "graph", seg, sizeof(seg),
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1);

    /* Reach the segment's heap and pin it onto the helper -> attached_count != 0
     * makes the patch-0003 destroy fail-closed. */
    HlWasmModule *m = hl_cap_wasm_module_lookup(&cache, "echo");
    ASSERT_TRUE(m != NULL && m->shared_data != NULL && m->shared_data->count == 1);
    wasm_shared_heap_t sh =
        (wasm_shared_heap_t)m->shared_data->segments[0].shared_heap;
    ASSERT_TRUE(sh != NULL);
    ASSERT_TRUE(wasm_runtime_attach_shared_heap(helper, sh));

    /* Remove cannot destroy the still-attached heap: reports failure and RETAINS
     * the segment (descriptor still on the list, slot kept, backing not freed). */
    derr = NULL;
    int rc = hl_cap_wasm_data_load(&cache, "echo", "graph", NULL, 0,
                                   NULL, &vfs, NULL, &derr);
    ASSERT_NE(rc, 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1);  /* retained, not leaked-dangling */
    m = hl_cap_wasm_module_lookup(&cache, "echo");
    ASSERT_TRUE(m->shared_data != NULL && m->shared_data->count == 1); /* slot kept */
    ASSERT_TRUE(m->shared_data->segments[0].shared_heap == (void *)sh); /* same heap */

    /* Clear the block and retry: destroy now succeeds -> clean removal, baseline. */
    wasm_runtime_detach_shared_heap(helper);
    derr = NULL;
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "graph", NULL, 0,
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    m = hl_cap_wasm_module_lookup(&cache, "echo");
    ASSERT_TRUE(m->shared_data == NULL);

    free_instance(helper, hmod, hmb);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── #315: a PERSISTENT instance attaches the segment chain at creation and, like
 *    a pooled instance, is not auto-detached by deinstantiate. Destroying it must
 *    detach so the segment's descriptor can later be reclaimed. Without the detach
 *    the heap stays attached, the subsequent remove's destroy fails, and both the
 *    shared_heap_count AND the HlWasmSharedData leak -- the latter only visible to
 *    LeakSanitizer (Linux), but the count assertion here catches it everywhere.
 *    Guards the d3_d1_rejection regression this fix first tripped in CI. ───────── */
UTEST(wasm_spans, segment_persistent_instance_baseline)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);
    uint32_t base = wasm_runtime_shared_heap_count();

    static const unsigned char seg[16] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const char *derr = NULL;
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "graph", seg, sizeof(seg),
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base + 1);

    /* Persistent instance attaches the chain at creation; a plain call exercises it. */
    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(&cache, "echo", NULL,
                                                     &vfs, NULL, NULL, &err);
    ASSERT_TRUE(pi != NULL);
    void *out = NULL; size_t out_len = 0; err = NULL;
    ASSERT_EQ(hl_cap_wasm_instance_call(pi, "x", 1, &out, &out_len,
                                        NULL, NULL, NULL, NULL, &err), HL_WASM_OK);
    free(out);

    /* Destroy the instance (must detach), then remove the segment: the descriptor
     * is now reclaimable, so the count returns to baseline and shared_data frees. */
    hl_cap_wasm_instance_destroy(pi);
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "graph", NULL, 0,
                                    NULL, &vfs, NULL, &derr), 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    HlWasmModule *m = hl_cap_wasm_module_lookup(&cache, "echo");
    ASSERT_TRUE(m->shared_data == NULL);

    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ══ D.3: persistent instance:call span consumption (hl_cap_wasm_instance_call) ══
 * Same cleanup guarantees as D.2, but the instance persists across calls: each
 * call attaches its span set and detaches on every exit so the instance is
 * chain-free for the next call. Output is always copied (no zero-copy path).    */

/* one-shot: call a persistent instance with a single span attached. */
static int call_inst_with_span(HlWasmInstance *pi, HlMappedBuffer *buf,
                               const char *sname, const void *input, size_t in_len,
                               HlWasmCallOpts *extra, void **out, size_t *out_len,
                               const char **err)
{
    HlWasmSpanReq req = { .name = sname, .buf = buf };
    HlWasmCallOpts opts = {0};
    if (extra) opts = *extra;
    opts.spans = &req; opts.span_count = 1;
    return hl_cap_wasm_instance_call(pi, input, in_len, out, out_len,
                                     &opts, NULL, NULL, NULL, err);
}

/* ── repeated calls with changing / no spans: each attaches + detaches, so the
 *    persistent instance returns to baseline (chain-free, borrow released) every
 *    call regardless of whether that call had a span. ────────────────────────── */
UTEST(wasm_spans, d3_repeated_changing_spans)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(&cache, "echo", NULL,
                                                     &vfs, NULL, NULL, &err);
    ASSERT_TRUE(pi != NULL);
    uint32_t base = wasm_runtime_shared_heap_count(); /* after create (no segs) */

    for (int i = 0; i < 20; i++) {
        void *out = NULL; size_t out_len = 0; err = NULL;
        if (i % 3 == 2) {
            /* a plain call (no spans) on the same instance */
            int rc = hl_cap_wasm_instance_call(pi, "plain", 5, &out, &out_len,
                                               NULL, NULL, NULL, NULL, &err);
            ASSERT_EQ(rc, HL_WASM_OK);
            ASSERT_EQ(out_len, (size_t)5);
            free(out);
            ASSERT_EQ(wasm_runtime_shared_heap_count(), base); /* unchanged */
        } else {
            /* a span call: fresh window each time (changing span) */
            HlMappedBuffer *b = hl_cap_fs_mmap_window(&cfg, "a.bin",
                                                      (uint64_t)(i * 4096) % 32768,
                                                      4096, NULL, NULL);
            ASSERT_TRUE(b != NULL);
            int rc = call_inst_with_span(pi, b, "src", "abcd", 4, NULL,
                                         &out, &out_len, &err);
            ASSERT_EQ(rc, HL_WASM_OK);
            ASSERT_EQ(out_len, (size_t)4);
            free(out);
            ASSERT_EQ(b->borrow_count, 0);                      /* detached */
            ASSERT_EQ(wasm_runtime_shared_heap_count(), base);  /* baseline */
            hl_cap_fs_munmap(b);
        }
    }

    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── busy: a span call on an instance with an async call "in flight" (busy=1) is
 *    rejected before any span borrow / heap creation. ────────────────────────── */
UTEST(wasm_spans, d3_busy_rejection)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(&cache, "echo", NULL,
                                                     &vfs, NULL, NULL, &err);
    ASSERT_TRUE(pi != NULL);
    uint32_t base = wasm_runtime_shared_heap_count();
    HlMappedBuffer *b = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(b != NULL);

    atomic_store(&pi->busy, 1);                 /* simulate async call in flight */
    void *out = (void *)0x1; size_t out_len = 9; err = NULL;
    int rc = call_inst_with_span(pi, b, "src", "x", 1, NULL, &out, &out_len, &err);
    ASSERT_NE(rc, HL_WASM_OK);
    ASSERT_STREQ(err, "instance_busy");
    ASSERT_TRUE(out == NULL);
    ASSERT_EQ(b->borrow_count, 0);              /* no borrow taken */
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);
    atomic_store(&pi->busy, 0);

    /* once no longer busy the same call succeeds. */
    out = NULL; out_len = 0; err = NULL;
    rc = call_inst_with_span(pi, b, "src", "x", 1, NULL, &out, &out_len, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    free(out);
    ASSERT_EQ(b->borrow_count, 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);

    hl_cap_fs_munmap(b);
    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── D1 on a persistent instance: a segment on the module rejects a span call. ─ */
UTEST(wasm_spans, d3_d1_rejection)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    /* install a segment BEFORE creating the instance (so it attaches the chain). */
    const char *derr = NULL;
    static const unsigned char seg[16] = { 1, 2, 3, 4 };
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);
    ASSERT_EQ(hl_cap_wasm_data_load(&cache, "echo", "graph", seg, sizeof(seg),
                                    NULL, &vfs, NULL, &derr), 0);
    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(&cache, "echo", NULL,
                                                     &vfs, NULL, NULL, &err);
    ASSERT_TRUE(pi != NULL);
    uint32_t before = wasm_runtime_shared_heap_count();
    HlMappedBuffer *b = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(b != NULL);

    void *out = (void *)0x1; size_t out_len = 9; err = NULL;
    int rc = call_inst_with_span(pi, b, "src", "x", 1, NULL, &out, &out_len, &err);
    ASSERT_NE(rc, HL_WASM_OK);
    ASSERT_STREQ(err, "spans_with_segments");
    ASSERT_TRUE(out == NULL);
    ASSERT_EQ(b->borrow_count, 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), before); /* no span heap */

    hl_cap_fs_munmap(b);
    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── gas trap on a persistent instance tears the span down; the instance stays
 *    usable for the next (successful) call. ──────────────────────────────────── */
UTEST(wasm_spans, d3_gas_cleanup_reusable)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(&cache, "echo", NULL,
                                                     &vfs, NULL, NULL, &err);
    ASSERT_TRUE(pi != NULL);
    uint32_t base = wasm_runtime_shared_heap_count();
    HlMappedBuffer *b = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(b != NULL);

    HlWasmCallOpts extra = {0};
    extra.gas = 1;                              /* trap */
    void *out = NULL; size_t out_len = 0; err = NULL;
    int rc = call_inst_with_span(pi, b, "src", "hello", 5, &extra,
                                 &out, &out_len, &err);
    ASSERT_NE(rc, HL_WASM_OK);                   /* trapped */
    ASSERT_EQ(b->borrow_count, 0);               /* span torn down */
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);

    /* instance is reusable: a normal span call now succeeds. */
    out = NULL; out_len = 0; err = NULL;
    rc = call_inst_with_span(pi, b, "src", "hey", 3, NULL, &out, &out_len, &err);
    ASSERT_EQ(rc, HL_WASM_OK);
    ASSERT_EQ(out_len, (size_t)3);
    free(out);
    ASSERT_EQ(b->borrow_count, 0);
    ASSERT_EQ(wasm_runtime_shared_heap_count(), base);

    hl_cap_fs_munmap(b);
    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── reentrancy proxy: if the instance already has a chain attached (as a
 *    re-entrant call from inside host_call would find), a span call's attach
 *    fails closed -- the call errors and its span borrow is released, leaving the
 *    pre-existing chain intact. ─────────────────────────────────────────────── */
UTEST(wasm_spans, d3_reentrancy_double_attach_fails_closed)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    const char *err = NULL;
    HlWasmInstance *pi = hl_cap_wasm_instance_create(&cache, "echo", NULL,
                                                     &vfs, NULL, NULL, &err);
    ASSERT_TRUE(pi != NULL);

    /* manually attach a span set to the instance (simulating a chain already
     * present when a re-entrant call arrives). */
    HlMappedBuffer *held = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(held != NULL);
    HlWasmSpanSet manual; err = NULL;
    hl_wasm_span_set_init(&manual, 0);
    ASSERT_EQ(hl_wasm_span_set_add(&manual, held, "held", &err), 0);
    ASSERT_EQ(hl_wasm_span_set_attach(&manual, pi->instance, &err), 0);
    uint32_t with_manual = wasm_runtime_shared_heap_count();

    /* a span call now finds the instance already chained -> its attach fails. */
    HlMappedBuffer *b = hl_cap_fs_mmap_window(&cfg, "a.bin", 8192, 4096, NULL, NULL);
    ASSERT_TRUE(b != NULL);
    void *out = (void *)0x1; size_t out_len = 9; err = NULL;
    int rc = call_inst_with_span(pi, b, "src", "x", 1, NULL, &out, &out_len, &err);
    ASSERT_NE(rc, HL_WASM_OK);                   /* attach failed, closed */
    ASSERT_EQ(b->borrow_count, 0);               /* the call's span was rolled back */
    ASSERT_EQ(wasm_runtime_shared_heap_count(), with_manual); /* manual chain intact */
    ASSERT_EQ(held->borrow_count, 1);            /* manual borrow untouched */

    /* tear the manual set down; the instance is chain-free again. */
    ASSERT_EQ(hl_wasm_span_set_teardown(&manual), 0);
    ASSERT_EQ(held->borrow_count, 0);

    hl_cap_fs_munmap(b); hl_cap_fs_munmap(held);
    hl_cap_wasm_instance_destroy(pi);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ══ D.4: async submission-pin ownership (HlWorkerWasmOp) ═══════════════════════
 * The binding deep-copies names + submission-pins buffers into the op before
 * submit (hl_worker_wasm_adopt_spans); the pins release in hl_worker_wasm_op_free
 * -- reached on completion, cancel-before-start, and cancel-during. These drive
 * that op-level lifecycle directly (deterministic, no event loop needed).       */

/* ── adopt pins + deep-copies; op_free releases. No Lua/JS pointer retained. ──── */
UTEST(wasm_spans, d4_op_pin_lifecycle)
{
    setup();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *b0 = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    HlMappedBuffer *b1 = hl_cap_fs_mmap_window(&cfg, "a.bin", 8192, 4096, NULL, NULL);
    ASSERT_TRUE(b0 && b1);

    /* names on a scratch buffer that we clobber after adopt, proving the op OWNS
     * its copy (no pointer into caller memory survives). */
    char nm0[64], nm1[64];
    snprintf(nm0, sizeof(nm0), "source");
    snprintf(nm1, sizeof(nm1), "landmarks");
    HlWasmSpanReq reqs[2] = { { nm0, b0 }, { nm1, b1 } };

    HlWorkerWasmOp *op = calloc(1, sizeof(HlWorkerWasmOp));
    ASSERT_TRUE(op != NULL);
    hl_worker_wasm_adopt_spans(op, reqs, 2);

    ASSERT_EQ(op->span_pins, 2);
    ASSERT_EQ(b0->borrow_count, 1);          /* submission-pinned */
    ASSERT_EQ(b1->borrow_count, 1);
    ASSERT_EQ(op->opts.span_count, 2);
    ASSERT_TRUE(op->opts.spans == op->span_reqs);      /* op-owned array */
    ASSERT_TRUE(op->span_reqs[0].name == op->span_names[0]); /* op-owned name */
    ASSERT_STREQ(op->span_names[0], "source");
    ASSERT_STREQ(op->span_names[1], "landmarks");
    ASSERT_TRUE(op->span_reqs[0].buf == b0);

    /* clobber the caller's name storage: the op copy is unaffected. */
    memset(nm0, 'Z', sizeof(nm0)); memset(nm1, 'Z', sizeof(nm1));
    ASSERT_STREQ(op->span_names[0], "source");

    hl_worker_wasm_op_free(op);
    ASSERT_EQ(op->span_pins, 0);
    ASSERT_TRUE(op->opts.spans == NULL);
    ASSERT_EQ(op->opts.span_count, 0);
    ASSERT_EQ(b0->borrow_count, 0);          /* released */
    ASSERT_EQ(b1->borrow_count, 0);
    free(op);

    hl_cap_fs_munmap(b0); hl_cap_fs_munmap(b1);
    teardown_dir();
}

/* ── empty list -> a plain call: no pins, opts.spans stays NULL. ─────────────── */
UTEST(wasm_spans, d4_op_empty_no_pins)
{
    setup();
    HlWorkerWasmOp *op = calloc(1, sizeof(HlWorkerWasmOp));
    ASSERT_TRUE(op != NULL);
    hl_worker_wasm_adopt_spans(op, NULL, 0);
    ASSERT_EQ(op->span_pins, 0);
    ASSERT_TRUE(op->opts.spans == NULL);
    ASSERT_EQ(op->opts.span_count, 0);
    hl_worker_wasm_op_free(op);
    free(op);
    teardown_dir();
}

/* ── closing/GC'ing the buffer immediately after submission is safe: the pin
 *    defers munmap until op_free (queued-worker race at the op level). ────────── */
UTEST(wasm_spans, d4_op_close_after_submit_defers_munmap)
{
    setup();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *b = hl_cap_fs_mmap_window(&cfg, "a.bin", 8192, 256, NULL, NULL);
    ASSERT_TRUE(b != NULL);
    char nm[64]; snprintf(nm, sizeof(nm), "src");
    HlWasmSpanReq req = { nm, b };

    HlWorkerWasmOp *op = calloc(1, sizeof(HlWorkerWasmOp));
    ASSERT_TRUE(op != NULL);
    hl_worker_wasm_adopt_spans(op, &req, 1);
    ASSERT_EQ(b->borrow_count, 1);

    /* close the buffer while the op holds it (as a handler might right after
     * compute.async.call): munmap is deferred, the bytes stay readable. */
    hl_cap_fs_munmap(b);
    ASSERT_EQ(b->pending_free, 1);
    ASSERT_EQ(b->closed, 0);
    ASSERT_EQ((int)((const unsigned char *)b->addr)[0], (int)(8192 & 0xff));

    hl_worker_wasm_op_free(op);   /* releases the last pin -> deferred munmap runs */
    free(op);
    teardown_dir();
}

/* ── two concurrent ops may share the same read-only buffer: pins stack, each
 *    op_free releases one, back to baseline. ─────────────────────────────────── */
UTEST(wasm_spans, d4_op_concurrent_shared_buffer)
{
    setup();
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *b = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(b != NULL);
    char nm[64]; snprintf(nm, sizeof(nm), "shared");
    HlWasmSpanReq req = { nm, b };

    HlWorkerWasmOp *op1 = calloc(1, sizeof(HlWorkerWasmOp));
    HlWorkerWasmOp *op2 = calloc(1, sizeof(HlWorkerWasmOp));
    ASSERT_TRUE(op1 && op2);
    hl_worker_wasm_adopt_spans(op1, &req, 1);
    hl_worker_wasm_adopt_spans(op2, &req, 1);
    ASSERT_EQ(b->borrow_count, 2);           /* both pin the same RO buffer */

    hl_worker_wasm_op_free(op1); free(op1);
    ASSERT_EQ(b->borrow_count, 1);
    hl_worker_wasm_op_free(op2); free(op2);
    ASSERT_EQ(b->borrow_count, 0);

    hl_cap_fs_munmap(b);
    teardown_dir();
}

/* ══ E: HL_WASM_OP_SPAN_INFO metadata query, driven from a real WASM guest ══════
 * spanprobe.wasm (compiled from C) is a PARAMETERISED driver: its input selects
 * the advertised cbSize capacity, the span index, the destination mode (scratch /
 * bad ptr / linear-memory boundary), and a scratch sentinel; its output returns
 * the count query, the index-query return code, the full 128-byte scratch (so the
 * host can check exact bytes written + untouched suffix), and the window's first
 * byte read THROUGH the reported base. This drives the whole D2 semantics matrix
 * from the guest side. */
static uint32_t rd32le(const unsigned char *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
       | (uint32_t)p[3] << 24; }
static uint64_t rd64le(const unsigned char *p)
{ uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v; }

/* Drive spanprobe once. reqs/nreq are the attached spans (nreq 0 => plain call).
 * Fills *count (count query), *rc (index-query return), scratch128 (the 128-byte
 * record buffer), *first (window byte via base). Returns the hl_cap_wasm_call rc. */
static int span_drive(HlWasmCache *cache, HlVfs *vfs,
                      const HlWasmSpanReq *reqs, int nreq,
                      uint16_t cap, int32_t idx, uint32_t mode, uint8_t sentinel,
                      int32_t *count, int32_t *rc, unsigned char scratch128[128],
                      uint32_t *first)
{
    unsigned char in[13];
    memset(in, 0, sizeof(in));
    in[0] = (unsigned char)(cap & 0xff);   in[1] = (unsigned char)(cap >> 8);
    in[4] = (unsigned char)(idx & 0xff);   in[5] = (unsigned char)((idx >> 8) & 0xff);
    in[6] = (unsigned char)((idx >> 16) & 0xff); in[7] = (unsigned char)(((uint32_t)idx >> 24) & 0xff);
    in[8] = (unsigned char)(mode & 0xff);  in[9] = (unsigned char)((mode >> 8) & 0xff);
    in[10] = (unsigned char)((mode >> 16) & 0xff); in[11] = (unsigned char)((mode >> 24) & 0xff);
    in[12] = sentinel;
    HlWasmCallOpts opts = {0};
    if (nreq > 0) { opts.spans = reqs; opts.span_count = nreq; }
    void *out = NULL; size_t out_len = 0; const char *err = NULL;
    int r = hl_cap_wasm_call(cache, "spanprobe", in, sizeof(in), &out, &out_len,
                             &opts, NULL, NULL, vfs, NULL, NULL, &err);
    if (r != HL_WASM_OK) { if (out) free(out); return r; }
    if (out_len != 140) { free(out); return -100; } /* driver contract */
    const unsigned char *o = (const unsigned char *)out;
    if (count) *count = (int32_t)rd32le(o + 0);
    if (rc) *rc = (int32_t)rd32le(o + 4);
    if (scratch128) memcpy(scratch128, o + 8, 128);
    if (first) *first = rd32le(o + 136);
    free(out);
    return HL_WASM_OK;
}

/* ── full record + read via base: count, struct_size, flags, name, len, foffset,
 *    and the window's first byte reached through the reported base. ──────────── */
UTEST(wasm_spans, e_span_info_record)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    const uint64_t off = 8195, wlen = 4096;
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", off, wlen, NULL, NULL);
    ASSERT_TRUE(buf != NULL);
    HlWasmSpanReq req = { .name = "source", .buf = buf };

    int32_t count = -9, rc = -9; unsigned char rec[128]; uint32_t first = 0;
    ASSERT_EQ(span_drive(&cache, &vfs, &req, 1, 96, 0, 0, 0xAA,
                         &count, &rc, rec, &first), HL_WASM_OK);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(rc, HL_SPAN_META_V1_SIZE);
    ASSERT_EQ((uint32_t)(rec[0] | rec[1] << 8), (uint32_t)1);          /* version */
    ASSERT_EQ((uint32_t)(rec[2] | rec[3] << 8), (uint32_t)HL_SPAN_META_V1_SIZE);
    ASSERT_EQ(rd32le(rec + 4), (uint32_t)HL_SPAN_META_FLAG_RO);         /* flags RO */
    ASSERT_STREQ((const char *)(rec + 8), "source");                   /* name */
    ASSERT_TRUE(rd64le(rec + 72) != 0);                                /* base set */
    ASSERT_EQ(rd64le(rec + 80), (uint64_t)wlen);                       /* len */
    ASSERT_EQ(rd64le(rec + 88), off);                                  /* foffset */
    ASSERT_EQ(first, (uint32_t)(off & 0xff));                          /* read via base */

    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── cbSize capacity matrix: for each advertised cap the host writes min(cap, 96)
 *    bytes (a prefix), returns the full 96, and leaves the suffix untouched. ──── */
UTEST(wasm_spans, e_span_info_capacity_matrix)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 4096, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);
    HlWasmSpanReq req = { .name = "src", .buf = buf };

    /* reference: the full 96-byte record (cap = 96). */
    unsigned char ref[128]; int32_t rc = 0;
    ASSERT_EQ(span_drive(&cache, &vfs, &req, 1, 96, 0, 0, 0xAA, NULL, &rc, ref, NULL),
              HL_WASM_OK);
    ASSERT_EQ(rc, 96);

    const uint16_t caps[] = { 4, 8, 72, 95, 96, 128 };
    for (size_t k = 0; k < sizeof(caps) / sizeof(caps[0]); k++) {
        uint16_t cap = caps[k];
        unsigned char rec[128]; rc = -9;
        ASSERT_EQ(span_drive(&cache, &vfs, &req, 1, cap, 0, 0, 0x5C,
                             NULL, &rc, rec, NULL), HL_WASM_OK);
        ASSERT_EQ(rc, 96);                       /* always returns full struct_size */
        size_t n = cap < 96 ? cap : 96;
        ASSERT_EQ(memcmp(rec, ref, n), 0);       /* exact bytes written */
        for (size_t i = n; i < 128; i++)         /* suffix untouched (sentinel) */
            ASSERT_EQ((int)rec[i], 0x5C);
    }

    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── malformed capacities (both sides): 0..3 and > 4096 all reject -1 with no
 *    record written. ──────────────────────────────────────────────────────────── */
UTEST(wasm_spans, e_span_info_malformed_cap)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);
    HlWasmSpanReq req = { .name = "src", .buf = buf };

    const uint16_t bad[] = { 0, 1, 2, 3, 4097, 8192, 65535 };
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        int32_t rc = -9, count = -9; unsigned char rec[128];
        ASSERT_EQ(span_drive(&cache, &vfs, &req, 1, bad[k], 0, 0, 0xAA,
                             &count, &rc, rec, NULL), HL_WASM_OK);
        ASSERT_EQ(count, 1);       /* the count query still worked */
        ASSERT_EQ(rc, -1);          /* malformed cap */
        for (int i = 8; i < 128; i++) ASSERT_EQ((int)rec[i], 0xAA); /* no record written */
    }

    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── invalid destination offsets (high / would-wrap-if-sign-extended) reject -1
 *    WITHOUT poisoning the guest's call. ──────────────────────────────────────── */
UTEST(wasm_spans, e_span_info_bad_dest)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);
    HlWasmSpanReq req = { .name = "src", .buf = buf };

    const uint32_t modes[] = { 1 /* 0x7ffffff0 */, 2 /* 0x80000000 */ };
    for (size_t k = 0; k < sizeof(modes) / sizeof(modes[0]); k++) {
        int32_t rc = -9, count = -9;
        /* the call returns cleanly (guest not trapped) with rc = -1. */
        ASSERT_EQ(span_drive(&cache, &vfs, &req, 1, 96, 0, modes[k], 0xAA,
                             &count, &rc, NULL, NULL), HL_WASM_OK);
        ASSERT_EQ(count, 1);
        ASSERT_EQ(rc, -1);
    }

    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── destination ending exactly at the linear-memory boundary succeeds; one past
 *    is rejected. ──────────────────────────────────────────────────────────────── */
UTEST(wasm_spans, e_span_info_boundary)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);
    HlWasmSpanReq req = { .name = "src", .buf = buf };

    int32_t rc = -9;
    ASSERT_EQ(span_drive(&cache, &vfs, &req, 1, 96, 0, 3 /* end-96 */, 0xAA,
                         NULL, &rc, NULL, NULL), HL_WASM_OK);
    ASSERT_EQ(rc, 96);                          /* [end-96, end) in bounds */
    rc = -9;
    ASSERT_EQ(span_drive(&cache, &vfs, &req, 1, 96, 0, 4 /* end-96+1 */, 0xAA,
                         NULL, &rc, NULL, NULL), HL_WASM_OK);
    ASSERT_EQ(rc, -1);                          /* one past end -> reject */

    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── out-of-range index returns 0. ─────────────────────────────────────────────── */
UTEST(wasm_spans, e_span_info_out_of_range)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);
    HlWasmSpanReq req = { .name = "src", .buf = buf };

    const int32_t idxs[] = { 1, 2, 99, -2 };   /* count == 1 */
    for (size_t k = 0; k < sizeof(idxs) / sizeof(idxs[0]); k++) {
        int32_t rc = -9, count = -9;
        ASSERT_EQ(span_drive(&cache, &vfs, &req, 1, 96, idxs[k], 0, 0xAA,
                             &count, &rc, NULL, NULL), HL_WASM_OK);
        ASSERT_EQ(count, 1);
        ASSERT_EQ(rc, 0);                       /* out of range -> 0 */
    }

    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── no active set (plain call) AND post-detach: a spans call sees count 1, and a
 *    SUBSEQUENT plain call on the same cache sees count 0 (tl_host_ctx.spans was
 *    cleared, not left dangling) -- distinct from a call that never had spans. ── */
UTEST(wasm_spans, e_span_info_no_set_and_post_detach)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    HlMappedBuffer *buf = hl_cap_fs_mmap_window(&cfg, "a.bin", 0, 4096, NULL, NULL);
    ASSERT_TRUE(buf != NULL);
    HlWasmSpanReq req = { .name = "src", .buf = buf };

    /* plain call, never had spans. */
    int32_t count = -9, rc = -9;
    ASSERT_EQ(span_drive(&cache, &vfs, NULL, 0, 96, 0, 0, 0xAA, &count, &rc, NULL, NULL),
              HL_WASM_OK);
    ASSERT_EQ(count, 0);
    ASSERT_EQ(rc, 0);

    /* a spans call -> count 1. */
    count = -9;
    ASSERT_EQ(span_drive(&cache, &vfs, &req, 1, 96, 0, 0, 0xAA, &count, &rc, NULL, NULL),
              HL_WASM_OK);
    ASSERT_EQ(count, 1);

    /* the NEXT plain call on the same cache -> count 0 (post-detach, not stale). */
    count = -9;
    ASSERT_EQ(span_drive(&cache, &vfs, NULL, 0, 96, 0, 0, 0xAA, &count, &rc, NULL, NULL),
              HL_WASM_OK);
    ASSERT_EQ(count, 0);
    ASSERT_EQ(rc, 0);

    hl_cap_fs_munmap(buf);
    hl_cap_wasm_destroy(&cache);
    teardown_dir();
}

/* ── concurrent invocations each see ONLY their own span's metadata (tl_host_ctx
 *    is thread-local). N workers each attach a span at a distinct file offset and
 *    assert the record's foffset is their own. TSan covers the race. ──────────── */
#define ESI_NTHREAD 4
#define ESI_NITER   120
struct esi_arg { HlWasmCache *cache; HlVfs *vfs; HlFsConfig *cfg; uint64_t off; int ok; };
static void *esi_worker(void *p)
{
    struct esi_arg *a = (struct esi_arg *)p;
    a->ok = 1;
    for (int r = 0; r < ESI_NITER && a->ok; r++) {
        HlMappedBuffer *b = hl_cap_fs_mmap_window(a->cfg, "a.bin", a->off, 4096, NULL, NULL);
        if (!b) { a->ok = 0; break; }
        HlWasmSpanReq req = { .name = "src", .buf = b };
        int32_t count = -9, rc = -9; unsigned char rec[128];
        int cr = span_drive(a->cache, a->vfs, &req, 1, 96, 0, 0, 0xAA,
                            &count, &rc, rec, NULL);
        if (cr != HL_WASM_OK || count != 1 || rc != 96
            || rd64le(rec + 88) != a->off)          /* MUST see its OWN foffset */
            a->ok = 0;
        if (b->borrow_count != 0) a->ok = 0;
        hl_cap_fs_munmap(b);
    }
    return NULL;
}
UTEST(wasm_spans, e_span_info_concurrent_isolation)
{
    setup();
    HlWasmCache cache; ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    HlVfs vfs; hl_vfs_init(&vfs, span_call_entries, NULL);
    ASSERT_EQ(write_file("a.bin", 40000), 0);
    /* pre-load on the main thread (WAMR's module loader is not thread-safe). */
    ASSERT_EQ(hl_cap_wasm_load(&cache, "spanprobe", &vfs, NULL), 0);

    struct esi_arg args[ESI_NTHREAD];
    pthread_t th[ESI_NTHREAD];
    for (int i = 0; i < ESI_NTHREAD; i++) {
        args[i].cache = &cache; args[i].vfs = &vfs; args[i].cfg = &cfg;
        args[i].off = (uint64_t)(4096 * (i + 1)) + i; /* distinct per worker */
        args[i].ok = -1;
        ASSERT_EQ(pthread_create(&th[i], NULL, esi_worker, &args[i]), 0);
    }
    for (int i = 0; i < ESI_NTHREAD; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < ESI_NTHREAD; i++) ASSERT_EQ(args[i].ok, 1);

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
        if (hl_wasm_span_set_add(&set, b0, "b0", &err) != 0
            || hl_wasm_span_set_add(&set, b1, "b1", &err) != 0
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
    for (int i = 0; i < HL_WASM_MAX_SPANS; i++) {
        char nm[16]; snprintf(nm, sizeof(nm), "s%d", i);
        ASSERT_EQ(hl_wasm_span_set_add(&set, bufs[i], nm, &err), 0);
    }
    ASSERT_EQ(set.count, HL_WASM_MAX_SPANS);
    /* the (max+1)-th add is rejected; no heap/borrow leaks. */
    err = NULL;
    ASSERT_EQ(hl_wasm_span_set_add(&set, bufs[HL_WASM_MAX_SPANS], "over", &err), -1);
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
    ASSERT_EQ(hl_wasm_span_set_add(&set, b0, "b0", &err), 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b1, "b1", &err), 0);
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
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, "buf", &err), 0);
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
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, "buf", &err), 0);
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
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, "buf", &err), 0);
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
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, "buf", &err), 0);
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
    ASSERT_EQ(hl_wasm_span_set_add(&set, b0, "b0", &err), 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b1, "b1", &err), 0);
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
    ASSERT_EQ(hl_wasm_span_set_add(&set, b0, "b0", &err), 0);
    ASSERT_EQ(hl_wasm_span_set_add(&set, b1, "b1", &err), 0);
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
    ASSERT_EQ(hl_wasm_span_set_add(&set, buf, "buf", &err), 0);
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
