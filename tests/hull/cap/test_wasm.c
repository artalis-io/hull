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

/* VFS with embedded echo.wasm for testing */
static const HlEntry test_entries[] = {
    { "compute/echo.wasm", echo_wasm, echo_wasm_len },
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
                               &vfs, NULL, &err);
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
                               &vfs, NULL, &err);
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
                               &vfs, NULL, &err);
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
                               &vfs, NULL, &err);
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
                               &vfs, NULL, &err);
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
                                NULL, NULL, NULL, NULL, NULL, &err),
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
                               NULL, NULL, NULL, &vfs, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);
    ASSERT_EQ(output, NULL);

    /* Backslash in name */
    err = NULL;
    rc = hl_cap_wasm_call(&cache, "..\\secret",
                           "x", 1, &output, &output_len,
                           NULL, NULL, NULL, &vfs, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);

    /* Dot-prefixed name */
    err = NULL;
    rc = hl_cap_wasm_call(&cache, ".hidden",
                           "x", 1, &output, &output_len,
                           NULL, NULL, NULL, &vfs, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_NOT_FOUND);

    /* Simple slash */
    err = NULL;
    rc = hl_cap_wasm_call(&cache, "sub/module",
                           "x", 1, &output, &output_len,
                           NULL, NULL, NULL, &vfs, NULL, &err);
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
                               NULL, NULL, NULL, &vfs, NULL, &err);
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
                               NULL, NULL, NULL, NULL, NULL, &err);
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
                               &vfs, NULL, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(output_len, input_len);
    ASSERT_NE(output, NULL);
    ASSERT_EQ(memcmp(output, input, input_len), 0);

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
                               NULL, NULL, NULL, NULL, NULL, &err);
    ASSERT_EQ(rc, HL_WASM_ERR_INTERNAL);
    ASSERT_NE(err, NULL);
    ASSERT_STREQ(err, "internal_error");

    /* Load on uninitialized cache */
    ASSERT_EQ(hl_cap_wasm_load(&cache, "echo", NULL, NULL),
              HL_WASM_ERR_INTERNAL);
}

#else /* !HL_ENABLE_WASM */

UTEST(hl_cap_wasm, disabled_placeholder)
{
    /* WASM support not compiled — test passes as no-op */
    ASSERT_TRUE(1);
}

#endif /* HL_ENABLE_WASM */

UTEST_MAIN();
