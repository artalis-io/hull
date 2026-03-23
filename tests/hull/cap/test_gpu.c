/*
 * test_hull_cap_gpu.c — Tests for GPU compute capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#ifdef HL_ENABLE_GPU

#include "hull/cap/gpu.h"
#include "hull/limits.h"
#include <stdlib.h>
#include <string.h>

/* ── Unit tests ────────────────────────────────────────────────────── */

UTEST(hull_cap_gpu, init_destroy)
{
    HlGpuCtx ctx;
    int rc = hl_cap_gpu_init(&ctx, &hl_gpu_backend_wgpu);
    /* wgpu stub returns NOT_AVAILABLE — that's expected without a GPU */
    ASSERT_TRUE(rc == HL_GPU_OK || rc == HL_GPU_ERR_NOT_AVAILABLE);

    if (rc == HL_GPU_OK)
        hl_cap_gpu_destroy(&ctx);
}

UTEST(hull_cap_gpu, available_no_gpu)
{
    HlGpuCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ASSERT_FALSE(hl_cap_gpu_available(&ctx));
    ASSERT_EQ(0, hl_cap_gpu_device_count(&ctx));
    ASSERT_TRUE(hl_cap_gpu_device_name(&ctx, 0) == NULL);
}

UTEST(hull_cap_gpu, null_safety)
{
    ASSERT_EQ(HL_GPU_ERR_INTERNAL, hl_cap_gpu_init(NULL, NULL));
    ASSERT_FALSE(hl_cap_gpu_available(NULL));
    ASSERT_EQ(0, hl_cap_gpu_device_count(NULL));
    ASSERT_TRUE(hl_cap_gpu_device_name(NULL, 0) == NULL);
}

UTEST(hull_cap_gpu, compile_no_gpu)
{
    HlGpuCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    int rc = hl_cap_gpu_compile(&ctx, 0, "test", "@compute fn main() {}", 22);
    ASSERT_EQ(HL_GPU_ERR_NOT_AVAILABLE, rc);
}

UTEST(hull_cap_gpu, dispatch_no_gpu)
{
    HlGpuCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    HlGpuDispatchOpts opts = {0};
    opts.device = -1;
    void *out = NULL;
    size_t out_len = 0;
    const char *err = NULL;

    int rc = hl_cap_gpu_dispatch(&ctx, "matmul", &opts, &out, &out_len, &err);
    ASSERT_EQ(HL_GPU_ERR_NOT_AVAILABLE, rc);
    ASSERT_STREQ("gpu_not_available", err);
}

UTEST(hull_cap_gpu, buffer_no_gpu)
{
    HlGpuCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    int rc = hl_cap_gpu_buffer_create(&ctx, 0, "weights", 1024, HL_GPU_USAGE_READ);
    ASSERT_EQ(HL_GPU_ERR_NOT_AVAILABLE, rc);
}

UTEST(hull_cap_gpu, dispatch_null_params)
{
    const char *err = NULL;
    int rc = hl_cap_gpu_dispatch(NULL, "test", NULL, NULL, NULL, &err);
    ASSERT_EQ(HL_GPU_ERR_INTERNAL, rc);
}

UTEST(hull_cap_gpu, error_codes)
{
    ASSERT_EQ(0, HL_GPU_OK);
    ASSERT_NE(HL_GPU_ERR_NOT_AVAILABLE, HL_GPU_ERR_SHADER);
    ASSERT_NE(HL_GPU_ERR_DISPATCH, HL_GPU_ERR_BUFFER);
    ASSERT_NE(HL_GPU_ERR_INTERNAL, HL_GPU_ERR_READBACK);
}

/* ── Real-GPU tests (skipped when no adapter available) ────────────── */

/* Helper: init GPU and skip test if no device available */
static int gpu_test_init(HlGpuCtx *ctx)
{
    int rc = hl_cap_gpu_init(ctx, &hl_gpu_backend_wgpu);
    if (rc != HL_GPU_OK || !hl_cap_gpu_available(ctx)) {
        if (rc == HL_GPU_OK) hl_cap_gpu_destroy(ctx);
        return 0; /* skip */
    }
    return 1; /* run */
}

UTEST(hull_cap_gpu, device_enumeration)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; } /* skip */

    ASSERT_TRUE(hl_cap_gpu_device_count(&ctx) > 0);
    const char *name = hl_cap_gpu_device_name(&ctx, 0);
    ASSERT_TRUE(name != NULL);
    ASSERT_TRUE(strlen(name) > 0);

    hl_cap_gpu_destroy(&ctx);
}

UTEST(hull_cap_gpu, compile_trivial_shader)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }

    const char *wgsl = "@compute @workgroup_size(1) fn main() {}";
    int rc = hl_cap_gpu_compile(&ctx, -1, "trivial", wgsl, strlen(wgsl));
    ASSERT_EQ(HL_GPU_OK, rc);

    /* Compiling again should be idempotent (already cached) */
    rc = hl_cap_gpu_compile(&ctx, -1, "trivial", wgsl, strlen(wgsl));
    ASSERT_EQ(HL_GPU_OK, rc);

    hl_cap_gpu_destroy(&ctx);
}

UTEST(hull_cap_gpu, compile_bad_shader)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }

    const char *bad_wgsl = "this is not valid wgsl at all";
    int rc = hl_cap_gpu_compile(&ctx, -1, "bad", bad_wgsl, strlen(bad_wgsl));
    ASSERT_EQ(HL_GPU_ERR_SHADER, rc);

    hl_cap_gpu_destroy(&ctx);
}

UTEST(hull_cap_gpu, dispatch_double)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }

    /* Compile a shader that doubles each u32 */
    const char *wgsl =
        "@group(0) @binding(0) var<storage, read_write> data: array<u32>;\n"
        "@compute @workgroup_size(1)\n"
        "fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
        "    data[id.x] = data[id.x] * 2u;\n"
        "}\n";

    int rc = hl_cap_gpu_compile(&ctx, -1, "double", wgsl, strlen(wgsl));
    ASSERT_EQ(HL_GPU_OK, rc);

    /* Input: [1, 2, 3, 4] */
    uint32_t input[4] = {1, 2, 3, 4};
    HlGpuBufferDesc buf = {
        .data = input,
        .size = sizeof(input),
        .usage = HL_GPU_USAGE_READWRITE,
        .binding = -1,
    };
    HlGpuDispatchOpts opts = {
        .buffers = &buf,
        .buffer_count = 1,
        .workgroups = {4, 1, 1},
        .output_buffer = 0,
        .device = -1,
    };

    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;

    rc = hl_cap_gpu_dispatch(&ctx, "double", &opts,
                              &output, &output_len, &err_msg);
    ASSERT_EQ(HL_GPU_OK, rc);
    ASSERT_TRUE(output != NULL);
    ASSERT_EQ(sizeof(input), output_len);

    uint32_t *result = (uint32_t *)output;
    ASSERT_EQ((uint32_t)2, result[0]);
    ASSERT_EQ((uint32_t)4, result[1]);
    ASSERT_EQ((uint32_t)6, result[2]);
    ASSERT_EQ((uint32_t)8, result[3]);

    free(output);
    hl_cap_gpu_destroy(&ctx);
}

UTEST(hull_cap_gpu, persistent_buffer_roundtrip)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }

    /* Create persistent buffer */
    int rc = hl_cap_gpu_buffer_create(&ctx, -1, "test_buf",
                                       16, HL_GPU_USAGE_READWRITE);
    ASSERT_EQ(HL_GPU_OK, rc);

    /* Write data to it */
    uint32_t data[4] = {10, 20, 30, 40};
    rc = hl_cap_gpu_buffer_write(&ctx, -1, "test_buf", data, sizeof(data), 0);
    ASSERT_EQ(HL_GPU_OK, rc);

    /* Read it back */
    void *read_data = NULL;
    size_t read_len = 0;
    rc = hl_cap_gpu_buffer_read(&ctx, -1, "test_buf", &read_data, &read_len);
    ASSERT_EQ(HL_GPU_OK, rc);
    ASSERT_TRUE(read_data != NULL);
    ASSERT_EQ(sizeof(data), read_len);

    uint32_t *result = (uint32_t *)read_data;
    ASSERT_EQ((uint32_t)10, result[0]);
    ASSERT_EQ((uint32_t)20, result[1]);
    ASSERT_EQ((uint32_t)30, result[2]);
    ASSERT_EQ((uint32_t)40, result[3]);

    free(read_data);

    /* Destroy buffer */
    hl_cap_gpu_buffer_destroy(&ctx, -1, "test_buf");

    /* Reading destroyed buffer should fail */
    rc = hl_cap_gpu_buffer_read(&ctx, -1, "test_buf",
                                 &read_data, &read_len);
    ASSERT_EQ(HL_GPU_ERR_NOT_FOUND, rc);

    hl_cap_gpu_destroy(&ctx);
}

#else /* !HL_ENABLE_GPU */

UTEST(hull_cap_gpu, disabled_placeholder)
{
    /* GPU disabled — this placeholder ensures the test binary compiles
     * and runs successfully even without HL_ENABLE_GPU. */
    ASSERT_TRUE(1);
}

#endif /* HL_ENABLE_GPU */

UTEST_MAIN();
