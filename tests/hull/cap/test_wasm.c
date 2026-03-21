/*
 * test_hull_cap_wasm.c — Tests for WASM compute capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm.h"
#include "hull/limits.h"
#include "hull/vfs.h"
#include "hull/entry.h"
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Pre-compiled echo.wasm (135 bytes) — copies input to output */
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

/* Pre-compiled simd_dot_product_simd.wasm (906 bytes) — SIMD128 dot product.
 * Uses v128 types; requires AOT or SIMD-enabled interpreter to run. */
static const unsigned char simd_dot_wasm[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0d, 0x02, 0x60,
  0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x00, 0x01, 0x7f, 0x03,
  0x03, 0x02, 0x00, 0x01, 0x05, 0x05, 0x01, 0x01, 0x02, 0x80, 0x08, 0x06,
  0x08, 0x01, 0x7f, 0x01, 0x41, 0x80, 0x88, 0x04, 0x0b, 0x07, 0x28, 0x03,
  0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0c, 0x68, 0x75,
  0x6c, 0x6c, 0x5f, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73, 0x00, 0x00,
  0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f,
  0x6e, 0x00, 0x01, 0x0a, 0xf1, 0x04, 0x02, 0xe9, 0x04, 0x04, 0x02, 0x7f,
  0x01, 0x7b, 0x04, 0x7f, 0x01, 0x7c, 0x41, 0x7e, 0x21, 0x04, 0x02, 0x40,
  0x20, 0x01, 0x41, 0x04, 0x48, 0x0d, 0x00, 0x20, 0x03, 0x41, 0x08, 0x48,
  0x0d, 0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x22, 0x05, 0x41, 0x03, 0x74,
  0x41, 0x04, 0x72, 0x20, 0x01, 0x4a, 0x0d, 0x00, 0x20, 0x05, 0x41, 0x02,
  0x74, 0x21, 0x03, 0x02, 0x40, 0x02, 0x40, 0x20, 0x05, 0x41, 0x7c, 0x71,
  0x22, 0x01, 0x0d, 0x00, 0xfd, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x06,
  0x41, 0x00, 0x21, 0x01, 0x0c, 0x01, 0x0b, 0x20, 0x01, 0x41, 0x7f, 0x6a,
  0x22, 0x07, 0x41, 0x02, 0x76, 0x41, 0x01, 0x6a, 0x22, 0x04, 0x41, 0x03,
  0x71, 0x21, 0x08, 0x02, 0x40, 0x02, 0x40, 0x20, 0x01, 0x41, 0x0d, 0x4f,
  0x0d, 0x00, 0x41, 0x00, 0x21, 0x01, 0xfd, 0x0c, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x21, 0x06, 0x0c, 0x01, 0x0b, 0x20, 0x04, 0x41, 0xfc, 0xff, 0xff, 0xff,
  0x07, 0x71, 0x21, 0x09, 0x20, 0x01, 0x41, 0x73, 0x6a, 0x41, 0x70, 0x71,
  0x21, 0x0a, 0xfd, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x06, 0x20, 0x00,
  0x21, 0x01, 0x03, 0x40, 0x20, 0x06, 0x20, 0x01, 0x41, 0x04, 0x6a, 0xfd,
  0x00, 0x00, 0x00, 0x20, 0x01, 0x20, 0x03, 0x6a, 0x22, 0x04, 0x41, 0x04,
  0x6a, 0xfd, 0x00, 0x00, 0x00, 0xfd, 0xe6, 0x01, 0xfd, 0xe4, 0x01, 0x20,
  0x01, 0x41, 0x14, 0x6a, 0xfd, 0x00, 0x00, 0x00, 0x20, 0x04, 0x41, 0x14,
  0x6a, 0xfd, 0x00, 0x00, 0x00, 0xfd, 0xe6, 0x01, 0xfd, 0xe4, 0x01, 0x20,
  0x01, 0x41, 0x24, 0x6a, 0xfd, 0x00, 0x00, 0x00, 0x20, 0x04, 0x41, 0x24,
  0x6a, 0xfd, 0x00, 0x00, 0x00, 0xfd, 0xe6, 0x01, 0xfd, 0xe4, 0x01, 0x20,
  0x01, 0x41, 0x34, 0x6a, 0xfd, 0x00, 0x00, 0x00, 0x20, 0x04, 0x41, 0x34,
  0x6a, 0xfd, 0x00, 0x00, 0x00, 0xfd, 0xe6, 0x01, 0xfd, 0xe4, 0x01, 0x21,
  0x06, 0x20, 0x01, 0x41, 0xc0, 0x00, 0x6a, 0x21, 0x01, 0x20, 0x09, 0x41,
  0x7c, 0x6a, 0x22, 0x09, 0x0d, 0x00, 0x0b, 0x20, 0x0a, 0x41, 0x10, 0x6a,
  0x21, 0x01, 0x0b, 0x20, 0x07, 0x41, 0x7c, 0x71, 0x21, 0x04, 0x02, 0x40,
  0x20, 0x08, 0x45, 0x0d, 0x00, 0x20, 0x01, 0x41, 0x02, 0x74, 0x20, 0x00,
  0x6a, 0x41, 0x04, 0x6a, 0x21, 0x01, 0x03, 0x40, 0x20, 0x06, 0x20, 0x01,
  0xfd, 0x00, 0x00, 0x00, 0x20, 0x01, 0x20, 0x03, 0x6a, 0xfd, 0x00, 0x00,
  0x00, 0xfd, 0xe6, 0x01, 0xfd, 0xe4, 0x01, 0x21, 0x06, 0x20, 0x01, 0x41,
  0x10, 0x6a, 0x21, 0x01, 0x20, 0x08, 0x41, 0x7f, 0x6a, 0x22, 0x08, 0x0d,
  0x00, 0x0b, 0x0b, 0x20, 0x04, 0x41, 0x04, 0x6a, 0x21, 0x01, 0x0b, 0x20,
  0x06, 0xfd, 0x1f, 0x00, 0xbb, 0x20, 0x06, 0xfd, 0x1f, 0x01, 0xbb, 0xa0,
  0x20, 0x06, 0xfd, 0x1f, 0x02, 0xbb, 0xa0, 0x20, 0x06, 0xfd, 0x1f, 0x03,
  0xbb, 0xa0, 0x21, 0x0b, 0x02, 0x40, 0x20, 0x05, 0x20, 0x01, 0x4d, 0x0d,
  0x00, 0x20, 0x01, 0x41, 0x01, 0x6a, 0x21, 0x04, 0x02, 0x40, 0x20, 0x05,
  0x20, 0x01, 0x6b, 0x41, 0x01, 0x71, 0x45, 0x0d, 0x00, 0x20, 0x00, 0x41,
  0x04, 0x6a, 0x22, 0x08, 0x20, 0x01, 0x41, 0x02, 0x74, 0x22, 0x01, 0x6a,
  0x2a, 0x02, 0x00, 0xbb, 0x20, 0x08, 0x20, 0x03, 0x6a, 0x20, 0x01, 0x6a,
  0x2a, 0x02, 0x00, 0xbb, 0xa2, 0x20, 0x0b, 0xa0, 0x21, 0x0b, 0x20, 0x04,
  0x21, 0x01, 0x0b, 0x20, 0x05, 0x20, 0x04, 0x46, 0x0d, 0x00, 0x20, 0x05,
  0x20, 0x01, 0x6b, 0x21, 0x04, 0x20, 0x00, 0x20, 0x01, 0x41, 0x02, 0x74,
  0x6a, 0x21, 0x01, 0x03, 0x40, 0x20, 0x01, 0x41, 0x08, 0x6a, 0x22, 0x08,
  0x2a, 0x02, 0x00, 0xbb, 0x20, 0x01, 0x20, 0x03, 0x6a, 0x22, 0x09, 0x41,
  0x08, 0x6a, 0x2a, 0x02, 0x00, 0xbb, 0xa2, 0x20, 0x01, 0x41, 0x04, 0x6a,
  0x2a, 0x02, 0x00, 0xbb, 0x20, 0x09, 0x41, 0x04, 0x6a, 0x2a, 0x02, 0x00,
  0xbb, 0xa2, 0x20, 0x0b, 0xa0, 0xa0, 0x21, 0x0b, 0x20, 0x08, 0x21, 0x01,
  0x20, 0x04, 0x41, 0x7e, 0x6a, 0x22, 0x04, 0x0d, 0x00, 0x0b, 0x0b, 0x20,
  0x02, 0x20, 0x0b, 0x39, 0x03, 0x00, 0x41, 0x08, 0x21, 0x04, 0x0b, 0x20,
  0x04, 0x0b, 0x04, 0x00, 0x41, 0x01, 0x0b, 0x00, 0x55, 0x04, 0x6e, 0x61,
  0x6d, 0x65, 0x00, 0x1b, 0x1a, 0x73, 0x69, 0x6d, 0x64, 0x5f, 0x64, 0x6f,
  0x74, 0x5f, 0x70, 0x72, 0x6f, 0x64, 0x75, 0x63, 0x74, 0x5f, 0x73, 0x69,
  0x6d, 0x64, 0x2e, 0x77, 0x61, 0x73, 0x6d, 0x01, 0x1d, 0x02, 0x00, 0x0c,
  0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73,
  0x01, 0x0c, 0x68, 0x75, 0x6c, 0x6c, 0x5f, 0x76, 0x65, 0x72, 0x73, 0x69,
  0x6f, 0x6e, 0x07, 0x12, 0x01, 0x00, 0x0f, 0x5f, 0x5f, 0x73, 0x74, 0x61,
  0x63, 0x6b, 0x5f, 0x70, 0x6f, 0x69, 0x6e, 0x74, 0x65, 0x72, 0x00, 0x2f,
  0x09, 0x70, 0x72, 0x6f, 0x64, 0x75, 0x63, 0x65, 0x72, 0x73, 0x01, 0x0c,
  0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73, 0x65, 0x64, 0x2d, 0x62, 0x79,
  0x01, 0x0e, 0x48, 0x6f, 0x6d, 0x65, 0x62, 0x72, 0x65, 0x77, 0x20, 0x63,
  0x6c, 0x61, 0x6e, 0x67, 0x06, 0x31, 0x38, 0x2e, 0x31, 0x2e, 0x38, 0x00,
  0x35, 0x0f, 0x74, 0x61, 0x72, 0x67, 0x65, 0x74, 0x5f, 0x66, 0x65, 0x61,
  0x74, 0x75, 0x72, 0x65, 0x73, 0x03, 0x2b, 0x0f, 0x6d, 0x75, 0x74, 0x61,
  0x62, 0x6c, 0x65, 0x2d, 0x67, 0x6c, 0x6f, 0x62, 0x61, 0x6c, 0x73, 0x2b,
  0x08, 0x73, 0x69, 0x67, 0x6e, 0x2d, 0x65, 0x78, 0x74, 0x2b, 0x07, 0x73,
  0x69, 0x6d, 0x64, 0x31, 0x32, 0x38
};
static const unsigned int simd_dot_wasm_len = 906;

/* VFS with embedded echo.wasm and SIMD module for testing */
static const HlEntry test_entries[] = {
    { "compute/echo.wasm", echo_wasm, echo_wasm_len },
    { "compute/simd_dot.wasm", simd_dot_wasm, simd_dot_wasm_len },
    { 0, 0, 0 }
};

UTEST(hl_cap_wasm, init_destroy)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    ASSERT_EQ(cache.initialized, 1);
    ASSERT_EQ(cache.count, 0);
    hl_cap_wasm_destroy(&cache);
    ASSERT_EQ(cache.initialized, 0);
}

UTEST(hl_cap_wasm, load_from_vfs)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    int rc = hl_cap_wasm_load(&cache, "echo", &vfs, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(cache.count, 1);

    /* Second load should be a no-op (cached) */
    rc = hl_cap_wasm_load(&cache, "echo", &vfs, NULL);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(cache.count, 1);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, call_echo)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *input = "hello, wasm!";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, input_len,
                               &output, &output_len,
                               NULL, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_NE(output, NULL);
    ASSERT_EQ(memcmp(output, input, input_len), 0);

    free(output);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, call_empty_input)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               "", 0,
                               &output, &output_len,
                               NULL, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, (size_t)0);

    free(output);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, module_not_found)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "nonexistent",
                               "x", 1,
                               &output, &output_len,
                               NULL, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "not_found");
    ASSERT_EQ(output, NULL);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, input_size_limit)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* Set max_input to 4 bytes, then try to send 10 */
    HlWasmCallOpts opts = {0};
    opts.max_input = 4;

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               "0123456789", 10,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_INPUT);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "input_too_large");

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, gas_exhaustion)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* Set gas to 1 instruction — should fail */
    HlWasmCallOpts opts = {0};
    opts.gas = 1;

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               "hello", 5,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    /* Gas exhaustion should cause a call failure */
    ASSERT_NE(rc, 0);

    free(output);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, abi_version)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);
    ASSERT_EQ(cache.modules[0].abi_version, (uint32_t)1);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, null_safety)
{
    ASSERT_EQ(hl_cap_wasm_init(NULL), -1);

    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    ASSERT_EQ(hl_cap_wasm_load(&cache, NULL, NULL, NULL), HL_WASM_ERR_INTERNAL);

    const char *err = NULL;
    ASSERT_EQ(hl_cap_wasm_call(NULL, "x", "y", 1, NULL, NULL,
                                NULL, NULL, NULL, NULL, NULL, NULL, &err),
              HL_WASM_ERR_INTERNAL);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, path_traversal_rejected)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    /* Slash in name — path traversal */
    int rc = hl_cap_wasm_call(&cache, "../../../etc/passwd",
                               "x", 1, &output, &output_len,
                               NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);
    ASSERT_EQ(output, NULL);

    /* Backslash in name */
    err = NULL;
    rc = hl_cap_wasm_call(&cache, "..\\secret",
                           "x", 1, &output, &output_len,
                           NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);

    /* Dot-prefixed name */
    err = NULL;
    rc = hl_cap_wasm_call(&cache, ".hidden",
                           "x", 1, &output, &output_len,
                           NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);

    /* Simple slash */
    err = NULL;
    rc = hl_cap_wasm_call(&cache, "sub/module",
                           "x", 1, &output, &output_len,
                           NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);

    /* Also check load path */
    ASSERT_EQ(hl_cap_wasm_load(&cache, "../escape", &vfs, NULL),
              HL_WASM_ERR_NOT_FOUND);
    ASSERT_EQ(hl_cap_wasm_load(&cache, ".dotfile", &vfs, NULL),
              HL_WASM_ERR_NOT_FOUND);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, overlong_name_rejected)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* 256-char name exceeds the 255 limit */
    char long_name[257];
    memset(long_name, 'a', 256);
    long_name[256] = '\0';

    ASSERT_EQ(hl_cap_wasm_load(&cache, long_name, &vfs, NULL),
              HL_WASM_ERR_NOT_FOUND);

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, long_name,
                               "x", 1, &output, &output_len,
                               NULL, NULL, NULL, &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);
    ASSERT_EQ(output, NULL);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, empty_name_rejected)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    ASSERT_EQ(hl_cap_wasm_load(&cache, "", NULL, NULL),
              HL_WASM_ERR_NOT_FOUND);

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "",
                               "x", 1, &output, &output_len,
                               NULL, NULL, NULL, NULL, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, destroy_idempotent)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", &vfs, NULL), 0);

    hl_cap_wasm_destroy(&cache);
    ASSERT_EQ(cache.initialized, 0);
    ASSERT_EQ(cache.count, 0);

    /* Second destroy should be a safe no-op */
    hl_cap_wasm_destroy(&cache);
    ASSERT_EQ(cache.initialized, 0);

    /* Destroy NULL should also be safe */
    hl_cap_wasm_destroy(NULL);
}

UTEST(hl_cap_wasm, call_with_custom_opts)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts opts = {0};
    opts.max_input  = 1024;
    opts.max_output = 1024;
    opts.heap_size  = 512 * 1024;
    opts.stack_size = 32 * 1024;
    opts.gas        = 50000000;

    const char *input = "custom opts test";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, input_len,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_NE(output, NULL);
    ASSERT_EQ(memcmp(output, input, input_len), 0);

    free(output);
    hl_cap_wasm_destroy(&cache);
}

/* ── Pool tests ────────────────────────────────────────────────────── */

UTEST(hl_cap_wasm, pool_reuse)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* First call: cold instantiation */
    const char *input = "pool test";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, input_len,
                               &output, &output_len,
                               NULL, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_EQ(memcmp(output, input, input_len), 0);
    free(output);

    /* Second call: should reuse pooled instance */
    output = NULL;
    output_len = 0;
    err = NULL;
    rc = hl_cap_wasm_call(&cache, "echo",
                           input, input_len,
                           &output, &output_len,
                           NULL, NULL, NULL,
                           &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_EQ(memcmp(output, input, input_len), 0);
    free(output);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, pool_stress)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *input = "stress";
    size_t input_len = strlen(input);

    for (int i = 0; i < 100; i++) {
        void *output = NULL;
        size_t output_len = 0;
        const char *err = NULL;

        int rc = hl_cap_wasm_call(&cache, "echo",
                                   input, input_len,
                                   &output, &output_len,
                                   NULL, NULL, NULL,
                                   &vfs, NULL, NULL, &err);
        ASSERT_EQ(rc, 0);
        ASSERT_EQ(output_len, input_len);
        free(output);
    }

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, pool_error_no_reuse)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* Gas-exhausted call — instance should NOT be pooled */
    HlWasmCallOpts bad_opts = {0};
    bad_opts.gas = 1;

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               "hello", 5,
                               &output, &output_len,
                               &bad_opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_NE(rc, 0);
    free(output);

    /* Normal call should still work (fresh instance) */
    output = NULL;
    output_len = 0;
    err = NULL;
    rc = hl_cap_wasm_call(&cache, "echo",
                           "hello", 5,
                           &output, &output_len,
                           NULL, NULL, NULL,
                           &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, (size_t)5);
    ASSERT_EQ(memcmp(output, "hello", 5), 0);
    free(output);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, pool_size_mismatch)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *input = "mismatch";
    size_t input_len = strlen(input);

    /* Call with heap_size = 1 MB */
    HlWasmCallOpts opts1 = {0};
    opts1.max_input  = 1024;
    opts1.max_output = 1024;
    opts1.heap_size  = 1 * 1024 * 1024;
    opts1.stack_size = 32 * 1024;

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, input_len,
                               &output, &output_len,
                               &opts1, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    free(output);

    /* Call with different heap_size = 2 MB — pool miss, fresh instance */
    HlWasmCallOpts opts2 = {0};
    opts2.max_input  = 1024;
    opts2.max_output = 1024;
    opts2.heap_size  = 2 * 1024 * 1024;
    opts2.stack_size = 32 * 1024;

    output = NULL;
    output_len = 0;
    err = NULL;
    rc = hl_cap_wasm_call(&cache, "echo",
                           input, input_len,
                           &output, &output_len,
                           &opts2, NULL, NULL,
                           &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    free(output);

    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, call_uninitialized_cache)
{
    HlWasmCache cache;
    memset(&cache, 0, sizeof(cache));
    /* cache.initialized is 0 — not initialized */

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               "x", 1, &output, &output_len,
                               NULL, NULL, NULL, NULL, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_INTERNAL);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "internal_error");

    /* Load on uninitialized cache */
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", NULL, NULL),
              HL_WASM_ERR_INTERNAL);
}

/* ── SIMD: Interpreter gracefully handles v128 module ──────────────── */

UTEST(hl_cap_wasm, simd_interpreter_load)
{
    /* SIMD .wasm contains v128 types. WAMR interpreter without SIMDe
     * should either: (a) load but fail at call time, or (b) fail to load.
     * Either way, it must not crash. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    /* Attempt to load the SIMD module */
    int rc = hl_cap_wasm_load(&cache, "simd_dot", &vfs, NULL);

    if (rc == 0) {
        /* Module loaded — try calling it. WAMR with SIMD enabled can
         * load v128 modules even in interpreter mode. Call may succeed
         * (if SIMDe is available) or fail gracefully. */
        uint8_t input[4 + 4*4 + 4*4]; /* n=4, vec_a[4], vec_b[4] */
        memset(input, 0, sizeof(input));
        uint32_t n = 4;
        memcpy(input, &n, 4);
        /* a = {1,2,3,4}, b = {1,1,1,1} */
        float a[] = {1.0f, 2.0f, 3.0f, 4.0f};
        float b[] = {1.0f, 1.0f, 1.0f, 1.0f};
        memcpy(input + 4, a, 16);
        memcpy(input + 20, b, 16);

        void *output = NULL;
        size_t output_len = 0;
        const char *err = NULL;

        rc = hl_cap_wasm_call(&cache, "simd_dot",
                               input, sizeof(input),
                               &output, &output_len,
                               NULL, NULL, NULL,
                               &vfs, NULL, NULL, &err);
        /* Either succeeds with correct result, or fails gracefully */
        if (rc == 0) {
            ASSERT_EQ(output_len, (size_t)8);
            double result;
            memcpy(&result, output, 8);
            /* dot(a,b) = 1+2+3+4 = 10 */
            ASSERT_TRUE(result > 9.99 && result < 10.01);
        }
        /* If rc != 0, that's also fine — interpreter SIMD not supported */
        free(output);
    }
    /* If load failed, that's fine — confirms interpreter can't load v128 */

    hl_cap_wasm_destroy(&cache);
}

/* ── TC-1: Concurrent module loading (TS-2 regression) ─────────────── */

typedef struct {
    HlWasmCache *cache;
    HlVfs       *vfs;
    int          rc;
} ConcurrentLoadArg;

static void *concurrent_load_thread(void *arg)
{
    ConcurrentLoadArg *a = (ConcurrentLoadArg *)arg;
    const char *input = "concurrent";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    a->rc = hl_cap_wasm_call(a->cache, "echo",
                              input, input_len,
                              &output, &output_len,
                              NULL, NULL, NULL,
                              a->vfs, NULL, NULL, &err);
    if (a->rc == 0) {
        if (output_len != input_len || memcmp(output, input, input_len) != 0)
            a->rc = -99;
    }
    free(output);
    return NULL;
}

UTEST(hl_cap_wasm, concurrent_load)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    #define NUM_THREADS 8
    pthread_t threads[NUM_THREADS];
    ConcurrentLoadArg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        args[i].cache = &cache;
        args[i].vfs = &vfs;
        args[i].rc = -1;
        pthread_create(&threads[i], NULL, concurrent_load_thread, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    /* All threads should succeed */
    for (int i = 0; i < NUM_THREADS; i++)
        ASSERT_EQ(args[i].rc, 0);

    /* Module should be cached exactly once */
    ASSERT_EQ(cache.count, 1);
    #undef NUM_THREADS

    hl_cap_wasm_destroy(&cache);
}

/* ── TC-2: Callback provided but not invoked ───────────────────────── */

static int test_callback_fn(int id, const void *in, size_t in_len,
                             void *out_buf, size_t out_max, void *user_data)
{
    (void)id; (void)in; (void)in_len; (void)out_buf; (void)out_max;
    /* Mark that callback was invoked */
    int *called = (int *)user_data;
    *called = 1;
    return 0;
}

UTEST(hl_cap_wasm, callback_not_invoked)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    int callback_called = 0;
    const char *input = "callback test";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    /* Provide a callback, but echo.wasm never invokes host_call(CALLBACK) */
    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, input_len,
                               &output, &output_len,
                               NULL, test_callback_fn, &callback_called,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(callback_called, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_NE(output, NULL);
    ASSERT_EQ(memcmp(output, input, input_len), 0);

    free(output);
    hl_cap_wasm_destroy(&cache);
}

/* ── TC-3: Gas clamping edge cases ─────────────────────────────────── */

UTEST(hl_cap_wasm, gas_clamping_edge_cases)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    const char *input = "gas";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;
    int rc;

    /* gas = INT_MAX — should succeed (not clamped) */
    HlWasmCallOpts opts1 = {0};
    opts1.gas = INT_MAX;
    rc = hl_cap_wasm_call(&cache, "echo",
                           input, input_len,
                           &output, &output_len,
                           &opts1, NULL, NULL,
                           &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    free(output);

    /* gas = HL_WASM_MAX_GAS — should succeed (accepted as-is) */
    output = NULL;
    output_len = 0;
    err = NULL;
    HlWasmCallOpts opts2 = {0};
    opts2.gas = HL_WASM_MAX_GAS;
    rc = hl_cap_wasm_call(&cache, "echo",
                           input, input_len,
                           &output, &output_len,
                           &opts2, NULL, NULL,
                           &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    free(output);

    /* gas = HL_WASM_MAX_GAS + 1 — silently clamped, echo is fast so succeeds */
    output = NULL;
    output_len = 0;
    err = NULL;
    HlWasmCallOpts opts3 = {0};
    opts3.gas = HL_WASM_MAX_GAS + 1;
    rc = hl_cap_wasm_call(&cache, "echo",
                           input, input_len,
                           &output, &output_len,
                           &opts3, NULL, NULL,
                           &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    free(output);

    hl_cap_wasm_destroy(&cache);
}

/* ── Large allocation tests ─────────────────────────────────────────── */

/* Helper: generate deterministic input of given size and verify echo output.
 * Uses a simple PRNG to avoid allocating a second buffer for comparison —
 * we regenerate the expected bytes and compare in chunks. */
static uint32_t large_xorshift(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static void fill_deterministic(uint8_t *buf, size_t len, uint32_t seed)
{
    uint32_t state = seed;
    size_t i = 0;
    /* Fill in 4-byte chunks */
    for (; i + 4 <= len; i += 4) {
        state = large_xorshift(state);
        memcpy(buf + i, &state, 4);
    }
    /* Remaining bytes */
    if (i < len) {
        state = large_xorshift(state);
        memcpy(buf + i, &state, len - i);
    }
}

static int verify_deterministic(const uint8_t *buf, size_t len, uint32_t seed)
{
    uint32_t state = seed;
    size_t i = 0;
    for (; i + 4 <= len; i += 4) {
        state = large_xorshift(state);
        uint32_t expected;
        memcpy(&expected, buf + i, 4);
        if (expected != state) return 0;
    }
    if (i < len) {
        state = large_xorshift(state);
        uint8_t tmp[4];
        memcpy(tmp, &state, 4);
        if (memcmp(buf + i, tmp, len - i) != 0) return 0;
    }
    return 1;
}

UTEST(hl_cap_wasm, large_heap_instantiation)
{
    /* Verify that a 256 MB WASM heap can be instantiated and used.
     * This proves the raised HL_WASM_MAX_HEAP limit works end-to-end. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts opts = {0};
    opts.heap_size  = 256 * 1024 * 1024;  /* 256 MB */
    opts.max_input  = 1024;
    opts.max_output = 1024;

    const char *input = "large heap test";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, input_len,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_NE(output, NULL);
    ASSERT_EQ(memcmp(output, input, input_len), 0);

    free(output);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, large_io_32mb)
{
    /* Pass 32 MB through echo.wasm with a 128 MB heap.
     * Validates the full clamping chain and WASM linear memory
     * allocation for inputs well above the old 4 MB test ceiling. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    size_t io_size = 32 * 1024 * 1024;  /* 32 MB */
    uint8_t *input = malloc(io_size);
    ASSERT_NE(input, NULL);
    fill_deterministic(input, io_size, 0xDEADBEEF);

    HlWasmCallOpts opts = {0};
    opts.heap_size  = 128 * 1024 * 1024;  /* 128 MB — room for in + out */
    opts.max_input  = (uint32_t)io_size;
    opts.max_output = (uint32_t)io_size;

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, io_size,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, io_size);
    ASSERT_NE(output, NULL);
    /* Verify byte-exact match using deterministic pattern */
    ASSERT_TRUE(verify_deterministic(output, output_len, 0xDEADBEEF));

    free(output);
    free(input);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, large_io_128mb)
{
    /* Pass 128 MB through echo.wasm with a 512 MB heap.
     * Exercises the full path near the HL_WASM_MAX_IO_SIZE (256 MB) limit. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    size_t io_size = 128 * 1024 * 1024;  /* 128 MB */
    uint8_t *input = malloc(io_size);
    ASSERT_NE(input, NULL);
    fill_deterministic(input, io_size, 0xCAFEBABE);

    HlWasmCallOpts opts = {0};
    opts.heap_size  = 512 * 1024 * 1024;  /* 512 MB — room for in + out */
    opts.max_input  = (uint32_t)io_size;
    opts.max_output = (uint32_t)io_size;

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, io_size,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, io_size);
    ASSERT_NE(output, NULL);
    ASSERT_TRUE(verify_deterministic(output, output_len, 0xCAFEBABE));

    free(output);
    free(input);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, io_size_clamping)
{
    /* Verify that max_input/max_output beyond HL_WASM_MAX_IO_SIZE
     * gets silently clamped — the call should succeed with clamped limits. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts opts = {0};
    /* Request 512 MB I/O — exceeds HL_WASM_MAX_IO_SIZE (256 MB).
     * Should be silently clamped to 256 MB. Heap must be large enough
     * for the clamped output buffer allocation in WASM linear memory. */
    opts.max_input  = 512 * 1024 * 1024;
    opts.max_output = 512 * 1024 * 1024;
    opts.heap_size  = 768 * 1024 * 1024;  /* room for clamped 256 MB out */

    const char *input = "clamp test";
    size_t input_len = strlen(input);
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, input_len,
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_EQ(memcmp(output, input, input_len), 0);

    free(output);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, heap_size_clamping)
{
    /* Request heap > HL_WASM_MAX_HEAP — should be silently clamped. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts opts = {0};
    /* UINT32_MAX heap — exceeds HL_WASM_MAX_HEAP, clamped to ~4GB.
     * WAMR may fail to allocate 4GB, so we accept either success or
     * a graceful INTERNAL error. The point is: no crash, no truncation. */
    opts.heap_size = UINT32_MAX;
    opts.max_input = 1024;
    opts.max_output = 1024;

    const char *input = "huge heap";
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, strlen(input),
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    /* Either succeeds (system has enough memory) or fails gracefully */
    if (rc == 0) {
        ASSERT_EQ(output_len, strlen(input));
        ASSERT_EQ(memcmp(output, input, strlen(input)), 0);
    } else {
        /* Graceful failure — not a crash */
        ASSERT_EQ(rc, HL_WASM_ERR_INTERNAL);
    }

    free(output);
    hl_cap_wasm_destroy(&cache);
}

UTEST(hl_cap_wasm, input_exceeds_clamped_max)
{
    /* Set max_input to 16 bytes, then pass 32 bytes.
     * Even though the user could set max_input higher, with the small
     * limit in place, the call must reject the oversized input. */
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, test_entries, NULL);

    HlWasmCallOpts opts = {0};
    opts.max_input = 16;

    char input[32];
    memset(input, 'A', sizeof(input));
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;

    int rc = hl_cap_wasm_call(&cache, "echo",
                               input, sizeof(input),
                               &output, &output_len,
                               &opts, NULL, NULL,
                               &vfs, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_INPUT);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "input_too_large");
    ASSERT_EQ(output, NULL);

    hl_cap_wasm_destroy(&cache);
}

#else /* !HL_ENABLE_WASM */

UTEST(hl_cap_wasm, disabled_placeholder)
{
    /* WASM support not compiled — test passes as no-op */
    ASSERT_TRUE(1);
}

#endif /* HL_ENABLE_WASM */

UTEST_MAIN();
