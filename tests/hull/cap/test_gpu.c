/*
 * test_hull_cap_gpu.c - Tests for GPU compute capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#ifdef HL_ENABLE_GPU

#include "hull/cap/gpu.h"  /* error codes; HL_GPU_* limits come transitively from limits/gpu.h */
#include <stdlib.h>
#include <string.h>

/* ── Unit tests ────────────────────────────────────────────────────── */

UTEST(hull_cap_gpu, init_destroy)
{
    HlGpuCtx ctx;
    int rc = hl_cap_gpu_init(&ctx, &hl_gpu_backend_wgpu);
    /* wgpu stub returns NOT_AVAILABLE - that's expected without a GPU */
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

UTEST(hull_cap_gpu, pipeline_two_stage)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }

    /* Stage 1: double each u32
     * Stage 2: add 10 to each u32
     * Combined: input * 2 + 10 */
    const char *double_wgsl =
        "@group(0) @binding(0) var<storage, read_write> data: array<u32>;\n"
        "@compute @workgroup_size(1)\n"
        "fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
        "    data[id.x] = data[id.x] * 2u;\n"
        "}\n";
    const char *add10_wgsl =
        "@group(0) @binding(0) var<storage, read_write> data: array<u32>;\n"
        "@compute @workgroup_size(1)\n"
        "fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
        "    data[id.x] = data[id.x] + 10u;\n"
        "}\n";

    int rc = hl_cap_gpu_compile(&ctx, -1, "pipe_double",
                                 double_wgsl, strlen(double_wgsl));
    ASSERT_EQ(HL_GPU_OK, rc);
    rc = hl_cap_gpu_compile(&ctx, -1, "pipe_add10",
                             add10_wgsl, strlen(add10_wgsl));
    ASSERT_EQ(HL_GPU_OK, rc);

    /* Input: [1, 2, 3, 4] → double → [2, 4, 6, 8] → add10 → [12, 14, 16, 18] */
    uint32_t input[4] = {1, 2, 3, 4};

    HlGpuBufferDesc bufs_s1[] = {
        { .name = "data", .data = input, .size = sizeof(input),
          .usage = HL_GPU_USAGE_READWRITE, .binding = -1 },
    };
    HlGpuBufferDesc bufs_s2[] = {
        { .name = "data", .usage = HL_GPU_USAGE_READWRITE, .binding = -1 },
    };

    HlGpuPipelineStage stages[] = {
        { .shader = "pipe_double", .buffers = bufs_s1, .buffer_count = 1,
          .workgroups = {4, 1, 1} },
        { .shader = "pipe_add10", .buffers = bufs_s2, .buffer_count = 1,
          .workgroups = {4, 1, 1} },
    };

    HlGpuPipelineOutput outputs[] = { { .stage = 1, .buffer = 0 } };

    HlGpuPipelineOpts opts = {
        .stages = stages,
        .stage_count = 2,
        .outputs = outputs,
        .output_count = 1,
        .device = -1,
    };

    HlGpuPipelineResult result;
    const char *err_msg = NULL;
    rc = hl_cap_gpu_pipeline(&ctx, &opts, &result, &err_msg);
    ASSERT_EQ(HL_GPU_OK, rc);
    ASSERT_EQ(1, result.count);
    ASSERT_TRUE(result.data[0] != NULL);
    ASSERT_EQ(sizeof(input), result.len[0]);

    uint32_t *out = (uint32_t *)result.data[0];
    ASSERT_EQ((uint32_t)12, out[0]);  /* 1*2+10 */
    ASSERT_EQ((uint32_t)14, out[1]);  /* 2*2+10 */
    ASSERT_EQ((uint32_t)16, out[2]);  /* 3*2+10 */
    ASSERT_EQ((uint32_t)18, out[3]);  /* 4*2+10 */

    hl_cap_gpu_pipeline_result_free(&result);
    hl_cap_gpu_destroy(&ctx);
}

UTEST(hull_cap_gpu, texture_create_destroy)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }

    int rc = hl_cap_gpu_texture_create(&ctx, -1, "test_tex",
                                        64, 64, HL_GPU_TEX_RGBA8, 0,
                                        HL_GPU_FILTER_NEAREST,
                                        HL_GPU_ADDRESS_CLAMP,
                                        HL_GPU_ADDRESS_CLAMP);
    ASSERT_EQ(HL_GPU_OK, rc);

    /* Idempotent create */
    rc = hl_cap_gpu_texture_create(&ctx, -1, "test_tex",
                                    64, 64, HL_GPU_TEX_RGBA8, 0,
                                    HL_GPU_FILTER_NEAREST,
                                    HL_GPU_ADDRESS_CLAMP,
                                    HL_GPU_ADDRESS_CLAMP);
    ASSERT_EQ(HL_GPU_OK, rc);

    /* Destroy */
    hl_cap_gpu_texture_destroy(&ctx, -1, "test_tex");

    /* Read after destroy should fail */
    void *data = NULL;
    size_t len = 0;
    uint32_t w, h;
    HlGpuTexFormat fmt;
    rc = hl_cap_gpu_texture_read(&ctx, -1, "test_tex",
                                  &data, &len, &w, &h, &fmt);
    ASSERT_EQ(HL_GPU_ERR_NOT_FOUND, rc);

    hl_cap_gpu_destroy(&ctx);
}

UTEST(hull_cap_gpu, texture_write_read_roundtrip)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }

    /* Create a small 2x2 RGBA8 texture */
    int rc = hl_cap_gpu_texture_create(&ctx, -1, "rt_tex",
                                        2, 2, HL_GPU_TEX_RGBA8, 1,
                                        HL_GPU_FILTER_NEAREST,
                                        HL_GPU_ADDRESS_CLAMP,
                                        HL_GPU_ADDRESS_CLAMP);
    ASSERT_EQ(HL_GPU_OK, rc);

    /* Write pixels: 4 RGBA pixels = 16 bytes */
    uint8_t pixels[16] = {
        255, 0, 0, 255,    /* red */
        0, 255, 0, 255,    /* green */
        0, 0, 255, 255,    /* blue */
        255, 255, 0, 255,  /* yellow */
    };
    rc = hl_cap_gpu_texture_write(&ctx, -1, "rt_tex", pixels, sizeof(pixels));
    ASSERT_EQ(HL_GPU_OK, rc);

    /* Read back */
    void *data = NULL;
    size_t len = 0;
    uint32_t w = 0, h = 0;
    HlGpuTexFormat fmt;
    rc = hl_cap_gpu_texture_read(&ctx, -1, "rt_tex",
                                  &data, &len, &w, &h, &fmt);
    ASSERT_EQ(HL_GPU_OK, rc);
    ASSERT_TRUE(data != NULL);
    ASSERT_EQ((uint32_t)2, w);
    ASSERT_EQ((uint32_t)2, h);
    ASSERT_EQ(HL_GPU_TEX_RGBA8, fmt);
    ASSERT_EQ(sizeof(pixels), len);

    /* Verify pixel data matches */
    ASSERT_EQ(0, memcmp(data, pixels, sizeof(pixels)));

    free(data);
    hl_cap_gpu_texture_destroy(&ctx, -1, "rt_tex");
    hl_cap_gpu_destroy(&ctx);
}

UTEST(hull_cap_gpu, texture_no_gpu)
{
    HlGpuCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    int rc = hl_cap_gpu_texture_create(&ctx, 0, "t", 64, 64,
                                        HL_GPU_TEX_RGBA8, 0, 0, 0, 0);
    ASSERT_EQ(HL_GPU_ERR_NOT_AVAILABLE, rc);
}

UTEST(hull_cap_gpu, texture_null_safety)
{
    int rc = hl_cap_gpu_texture_create(NULL, 0, "t", 64, 64,
                                        HL_GPU_TEX_RGBA8, 0, 0, 0, 0);
    ASSERT_EQ(HL_GPU_ERR_INTERNAL, rc);

    rc = hl_cap_gpu_texture_create(NULL, 0, NULL, 64, 64,
                                    HL_GPU_TEX_RGBA8, 0, 0, 0, 0);
    ASSERT_EQ(HL_GPU_ERR_INTERNAL, rc);

    /* Zero dimensions */
    HlGpuCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    rc = hl_cap_gpu_texture_create(&ctx, 0, "t", 0, 0,
                                    HL_GPU_TEX_RGBA8, 0, 0, 0, 0);
    ASSERT_EQ(HL_GPU_ERR_INTERNAL, rc);
}

#else /* !HL_ENABLE_GPU */

UTEST(hull_cap_gpu, disabled_placeholder)
{
    /* GPU disabled - this placeholder ensures the test binary compiles
     * and runs successfully even without HL_ENABLE_GPU. */
    ASSERT_TRUE(1);
}

#endif /* HL_ENABLE_GPU */

UTEST_MAIN();
