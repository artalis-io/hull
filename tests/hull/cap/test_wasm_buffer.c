/*
 * test_hull_cap_wasm_buffer.c - Tests for HlWasmBuffer zero-copy buffer
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm.h"
#include "hull/cap/wasm_buffer.h"
#include "hull/limits/wasm.h"
#include "hull/vfs.h"
#include "hull/entry.h"
#include <stdlib.h>
#include <string.h>

/* Pre-compiled echo.wasm (135 bytes) - copies input to output */
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

static const HlEntry test_entries[] = {
    { "compute/echo.wasm", echo_wasm, echo_wasm_len },
    { 0, 0, 0 }
};

/* ── OWNED buffer tests ───────────────────────────────────────────── */

UTEST(hl_wasm_buffer, create_owned)
{
    const char *data = "hello, buffer!";
    size_t len = strlen(data);

    HlWasmBuffer *buf = hl_wasm_buffer_create_owned(data, len, NULL);
    ASSERT_NE(buf, NULL);
    ASSERT_EQ((int)buf->kind, (int)HL_WASM_BUF_OWNED);
    ASSERT_EQ(hl_wasm_buffer_len(buf), len);
    ASSERT_EQ(memcmp(hl_wasm_buffer_data(buf), data, len), 0);
    ASSERT_EQ(buf->closed, 0);

    /* Data should be an independent copy */
    ASSERT_NE(hl_wasm_buffer_data(buf), (const void *)data);

    hl_wasm_buffer_destroy(buf);
    free(buf);
}

UTEST(hl_wasm_buffer, create_owned_empty)
{
    HlWasmBuffer *buf = hl_wasm_buffer_create_owned(NULL, 0, NULL);
    ASSERT_NE(buf, NULL);
    ASSERT_EQ((int)buf->kind, (int)HL_WASM_BUF_OWNED);
    ASSERT_EQ(hl_wasm_buffer_len(buf), (size_t)0);

    hl_wasm_buffer_destroy(buf);
    free(buf);
}

UTEST(hl_wasm_buffer, materialize)
{
    const char *data = "materialize me";
    size_t len = strlen(data);

    HlWasmBuffer *buf = hl_wasm_buffer_create_owned(data, len, NULL);
    ASSERT_NE(buf, NULL);

    size_t out_len = 0;
    void *copy = hl_wasm_buffer_materialize(buf, &out_len, NULL);
    ASSERT_NE(copy, NULL);
    ASSERT_EQ(out_len, len);
    ASSERT_EQ(memcmp(copy, data, len), 0);

    /* Copy should be independent of buffer */
    ASSERT_NE((const void *)copy, (const void *)hl_wasm_buffer_data(buf));

    free(copy);
    hl_wasm_buffer_destroy(buf);
    free(buf);
}

UTEST(hl_wasm_buffer, closed_returns_null)
{
    const char *data = "test";
    HlWasmBuffer *buf = hl_wasm_buffer_create_owned(data, 4, NULL);
    ASSERT_NE(buf, NULL);

    hl_wasm_buffer_destroy(buf);
    ASSERT_EQ(buf->closed, 1);
    ASSERT_EQ(hl_wasm_buffer_data(buf), NULL);
    ASSERT_EQ(hl_wasm_buffer_len(buf), (size_t)0);

    size_t out_len = 0;
    void *copy = hl_wasm_buffer_materialize(buf, &out_len, NULL);
    ASSERT_EQ(copy, NULL);
    ASSERT_EQ(out_len, (size_t)0);

    /* Double destroy should be safe */
    hl_wasm_buffer_destroy(buf);

    free(buf);
}

UTEST(hl_wasm_buffer, destroy_null_safe)
{
    hl_wasm_buffer_destroy(NULL);
    /* Should not crash */
    ASSERT_TRUE(1);
}

/* ── call_buf tests ───────────────────────────────────────────────── */

UTEST(hl_wasm_buffer, call_buf_basic)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *input = "hello, wasm buffer!";
    size_t input_len = strlen(input);
    HlWasmBuffer *out_buf = NULL;
    const char *err = NULL;

    int rc = hl_cap_wasm_call_buf(&cache, "echo",
                                   input, input_len,
                                   &out_buf, NULL, NULL, NULL,
                                   &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NE(out_buf, NULL);
    ASSERT_EQ(hl_wasm_buffer_len(out_buf), input_len);
    ASSERT_EQ(memcmp(hl_wasm_buffer_data(out_buf), input, input_len), 0);

    /* Default heap (2MB) is <= pool threshold (4MB), should be WASM-backed */
    ASSERT_EQ((int)out_buf->kind, (int)HL_WASM_BUF_WASM);

    hl_wasm_buffer_destroy(out_buf);
    free(out_buf);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_wasm_buffer, call_buf_non_poolable)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* Use heap > threshold to force OWNED buffer */
    HlWasmCallOpts opts = {0};
    opts.heap_size = HL_WASM_POOL_HEAP_THRESHOLD + (1024 * 1024);

    const char *input = "big heap test";
    size_t input_len = strlen(input);
    HlWasmBuffer *out_buf = NULL;
    const char *err = NULL;

    int rc = hl_cap_wasm_call_buf(&cache, "echo",
                                   input, input_len,
                                   &out_buf, &opts, NULL, NULL,
                                   &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NE(out_buf, NULL);
    ASSERT_EQ(hl_wasm_buffer_len(out_buf), input_len);
    ASSERT_EQ(memcmp(hl_wasm_buffer_data(out_buf), input, input_len), 0);

    /* Heap > threshold → OWNED (copied) */
    ASSERT_EQ((int)out_buf->kind, (int)HL_WASM_BUF_OWNED);

    hl_wasm_buffer_destroy(out_buf);
    free(out_buf);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_wasm_buffer, chain_same_module)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* First call: string → buffer */
    const char *input = "chain test data";
    size_t input_len = strlen(input);
    HlWasmBuffer *buf1 = NULL;
    const char *err = NULL;

    int rc = hl_cap_wasm_call_buf(&cache, "echo",
                                   input, input_len,
                                   &buf1, NULL, NULL, NULL,
                                   &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NE(buf1, NULL);
    ASSERT_EQ(hl_wasm_buffer_len(buf1), input_len);

    /* Second call: buffer → buffer (chaining) */
    HlWasmBuffer *buf2 = NULL;
    err = NULL;

    rc = hl_cap_wasm_call_buf(&cache, "echo",
                               hl_wasm_buffer_data(buf1),
                               hl_wasm_buffer_len(buf1),
                               &buf2, NULL, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NE(buf2, NULL);
    ASSERT_EQ(hl_wasm_buffer_len(buf2), input_len);
    ASSERT_EQ(memcmp(hl_wasm_buffer_data(buf2), input, input_len), 0);

    hl_wasm_buffer_destroy(buf2);
    free(buf2);
    hl_wasm_buffer_destroy(buf1);
    free(buf1);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_wasm_buffer, chain_different_opts)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* First call with heap=2MB, max_output=1024 */
    HlWasmCallOpts opts1 = {0};
    opts1.heap_size  = 2 * 1024 * 1024;
    opts1.max_output = 1024;

    const char *input = "different opts";
    size_t input_len = strlen(input);
    HlWasmBuffer *buf1 = NULL;
    const char *err = NULL;

    int rc = hl_cap_wasm_call_buf(&cache, "echo",
                                   input, input_len,
                                   &buf1, &opts1, NULL, NULL,
                                   &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NE(buf1, NULL);

    /* Second call with heap=3MB (different opts, buffer still works as input) */
    HlWasmCallOpts opts2 = {0};
    opts2.heap_size  = 3 * 1024 * 1024;
    opts2.max_output = 1024;

    HlWasmBuffer *buf2 = NULL;
    err = NULL;

    rc = hl_cap_wasm_call_buf(&cache, "echo",
                               hl_wasm_buffer_data(buf1),
                               hl_wasm_buffer_len(buf1),
                               &buf2, &opts2, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NE(buf2, NULL);
    ASSERT_EQ(hl_wasm_buffer_len(buf2), input_len);
    ASSERT_EQ(memcmp(hl_wasm_buffer_data(buf2), input, input_len), 0);

    hl_wasm_buffer_destroy(buf2);
    free(buf2);
    hl_wasm_buffer_destroy(buf1);
    free(buf1);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_wasm_buffer, call_buf_empty_input)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmBuffer *out_buf = NULL;
    const char *err = NULL;

    int rc = hl_cap_wasm_call_buf(&cache, "echo",
                                   "", 0,
                                   &out_buf, NULL, NULL, NULL,
                                   &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NE(out_buf, NULL);
    ASSERT_EQ(hl_wasm_buffer_len(out_buf), (size_t)0);

    hl_wasm_buffer_destroy(out_buf);
    free(out_buf);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_wasm_buffer, call_buf_null_safety)
{
    HlWasmBuffer *buf = NULL;
    const char *err = NULL;

    /* NULL cache */
    ASSERT_EQ(hl_cap_wasm_call_buf(NULL, "x", "y", 1, &buf,
                                    NULL, NULL, NULL, NULL, NULL, NULL, &err),
              HL_WASM_ERR_INTERNAL);

    /* NULL output_buf */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    ASSERT_EQ(hl_cap_wasm_call_buf(&cache, "x", "y", 1, NULL,
                                    NULL, NULL, NULL, NULL, NULL, NULL, &err),
              HL_WASM_ERR_INTERNAL);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_wasm_buffer, pool_return_on_destroy)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* Load module to access pool */
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);

    /* call_buf → WASM buffer (holds instance) */
    HlWasmBuffer *buf = NULL;
    const char *err = NULL;

    int rc = hl_cap_wasm_call_buf(&cache, "echo",
                                   "pool test", 9,
                                   &buf, NULL, NULL, NULL,
                                   &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_NE(buf, NULL);
    ASSERT_EQ((int)buf->kind, (int)HL_WASM_BUF_WASM);

    /* Pool should have 0 instances (one checked out by buffer) */
    ASSERT_EQ(cache.modules[0].pool.count, 0);

    /* Destroy buffer → instance returned to pool */
    hl_wasm_buffer_destroy(buf);
    free(buf);

    ASSERT_EQ(cache.modules[0].pool.count, 1);

    /* Subsequent call should reuse pooled instance */
    void *output = NULL;
    size_t output_len = 0;
    err = NULL;
    rc = hl_cap_wasm_call(&cache, "echo",
                           "reuse", 5,
                           &output, &output_len,
                           NULL, NULL, NULL,
                           &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, (size_t)5);
    ASSERT_EQ(memcmp(output, "reuse", 5), 0);
    free(output);

    hl_cap_wasm_destroy(&cache);
}

#else /* !HL_ENABLE_WASM */

UTEST(hl_wasm_buffer, disabled_placeholder)
{
    ASSERT_TRUE(1);
}

#endif /* HL_ENABLE_WASM */

UTEST_MAIN();
