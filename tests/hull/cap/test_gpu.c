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
        .data_len = sizeof(input),
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

/* Audit 35: a dispatch that uploaded an inline buffer and then failed
 * before its submit (an unknown texture) left the upload's staging copy in
 * wgpu's pending writes; a loop of them held gigabytes. The backend now
 * submits them on that path. The loop runs, the device is not stalled, and
 * a dispatch after it still computes from its own data. */
UTEST(hull_cap_gpu, a_failed_dispatch_flushes_its_uploads)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }
    const char *wgsl =
        "@group(0) @binding(0) var<storage, read_write> data: array<u32>;\n"
        "@compute @workgroup_size(1)\n"
        "fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
        "    data[id.x] = data[id.x] * 2u;\n"
        "}\n";
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_compile(&ctx, -1, "double", wgsl, strlen(wgsl)));

    const size_t big_len = (size_t)4 << 20;
    void *big = calloc(1, big_len);
    ASSERT_TRUE(big != NULL);
    HlGpuBufferDesc bbuf = {
        .data = big, .data_len = big_len, .size = big_len,
        .usage = HL_GPU_USAGE_READWRITE, .binding = -1,
    };
    HlGpuTextureDesc tex = { .name = "no_such_texture" };
    HlGpuDispatchOpts bad = {
        .buffers = &bbuf, .buffer_count = 1,
        .textures = &tex, .texture_count = 1,
        .workgroups = {1, 1, 1},
        .output_buffer = -1, .output_texture = -1, .device = -1,
    };
    for (int i = 0; i < 64; i++) {
        const char *err = NULL;
        EXPECT_EQ(HL_GPU_ERR_DISPATCH,
                  hl_cap_gpu_dispatch(&ctx, "double", &bad, NULL, NULL, &err));
        EXPECT_STREQ("texture_not_found", err);
    }
    free(big);
    int di = ctx.default_device >= 0 ? ctx.default_device : 0;
    EXPECT_EQ(0, ctx.devices[di].stalled);
    EXPECT_TRUE(ctx.devices[di].queued_write_bytes < HL_GPU_MAX_QUEUED_WRITE_BYTES);

    uint32_t input[4] = {1, 2, 3, 4};
    HlGpuBufferDesc buf = {
        .data = input, .data_len = sizeof(input), .size = sizeof(input),
        .usage = HL_GPU_USAGE_READWRITE, .binding = -1,
    };
    HlGpuDispatchOpts opts = {
        .buffers = &buf, .buffer_count = 1, .workgroups = {4, 1, 1},
        .output_buffer = 0, .output_texture = -1, .device = -1,
    };
    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_dispatch(&ctx, "double", &opts,
                                             &output, &output_len, &err));
    ASSERT_EQ(sizeof(input), output_len);
    EXPECT_EQ((uint32_t)8, ((uint32_t *)output)[3]);
    free(output);
    hl_cap_gpu_destroy(&ctx);
}

/* A buffer larger than its data gets the data and zeros - not whatever lies
 * past the data. Upload used to copy `size` bytes from `data`, so a script
 * asking for {data = "abcd", size = 1 << 20} read host memory into the GPU
 * buffer and back out as the dispatch's output. Here the bytes past data_len
 * are a known non-zero fill; a ragged data_len exercises the padded tail. */
UTEST(hull_cap_gpu, upload_reads_no_further_than_the_data)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }

    const char *wgsl =
        "@group(0) @binding(0) var<storage, read_write> data: array<u32>;\n"
        "@compute @workgroup_size(1)\n"
        "fn main() { data[0] = data[0]; }\n";
    int rc = hl_cap_gpu_compile(&ctx, -1, "touch", wgsl, strlen(wgsl));
    ASSERT_EQ(HL_GPU_OK, rc);

    uint8_t backing[32];
    memset(backing, 0xAB, sizeof(backing));
    for (int i = 0; i < 6; i++) backing[i] = (uint8_t)(i + 1);
    HlGpuBufferDesc buf = {
        .data = backing,
        .data_len = 6,
        .size = sizeof(backing),
        .usage = HL_GPU_USAGE_READWRITE,
        .binding = -1,
    };
    HlGpuDispatchOpts opts = {
        .buffers = &buf,
        .buffer_count = 1,
        .workgroups = {1, 1, 1},
        .output_buffer = 0,
        .device = -1,
    };

    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;
    rc = hl_cap_gpu_dispatch(&ctx, "touch", &opts,
                             &output, &output_len, &err_msg);
    ASSERT_EQ(HL_GPU_OK, rc);
    ASSERT_TRUE(output != NULL);
    ASSERT_EQ(sizeof(backing), output_len);

    const uint8_t *out = (const uint8_t *)output;
    for (int i = 0; i < 6; i++) EXPECT_EQ(out[i], (uint8_t)(i + 1));
    for (size_t i = 6; i < sizeof(backing); i++) EXPECT_EQ(out[i], 0);

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
    rc = hl_cap_gpu_buffer_read(&ctx, -1, "test_buf", 0, &read_data, &read_len);
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
    rc = hl_cap_gpu_buffer_read(&ctx, -1, "test_buf", 0,
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
        { .name = "data", .data = input, .data_len = sizeof(input),
          .size = sizeof(input),
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
    rc = hl_cap_gpu_texture_read(&ctx, -1, "test_tex", 0,
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
    rc = hl_cap_gpu_texture_read(&ctx, -1, "rt_tex", 0,
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

/* ── Audit 35: bindings the shader does not declare (real device) ─────
 *
 * A stage with one buffer more than its WGSL declares made wgpu-native
 * panic inside wgpuQueueSubmit ("failed to initiate panic, error 3,
 * aborting"), killing the process. Each case below must return an error,
 * and the next valid dispatch on the same device must still work. */

static const char *A35_ONE_BUF =
    "@group(0) @binding(0) var<storage, read_write> data: array<u32>;\n"
    "@compute @workgroup_size(1)\n"
    "fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
    "    data[id.x] = data[id.x] * 2u;\n"
    "}\n";

/* A valid dispatch of A35_ONE_BUF ("a35_one") after a refused call. */
static int a35_valid_dispatch(HlGpuCtx *ctx)
{
    uint32_t in[4] = {1, 2, 3, 4};
    HlGpuBufferDesc b = { .data = in, .data_len = sizeof(in),
                          .size = sizeof(in), .binding = -1 };
    HlGpuDispatchOpts o = { .buffers = &b, .buffer_count = 1,
                            .workgroups = {4, 1, 1}, .output_buffer = 0,
                            .output_texture = -1, .device = -1 };
    void *out = NULL;
    size_t len = 0;
    const char *err = NULL;
    int rc = hl_cap_gpu_dispatch(ctx, "a35_one", &o, &out, &len, &err);
    int ok = rc == HL_GPU_OK && len == sizeof(in) && out &&
             ((uint32_t *)out)[3] == 8;
    free(out);
    return ok;
}

static void a35_force_unknown_layout(HlGpuCtx *ctx, const char *name)
{
    HlGpuDevice *dev = &ctx->devices[ctx->default_device];
    for (int i = 0; i < dev->pipeline_count; i++)
        if (strcmp(dev->pipelines[i].name, name) == 0)
            dev->pipelines[i].layout_known = 0;
}

UTEST(hull_cap_gpu_audit35, extra_binding_refused_device_survives)
{
    HlGpuCtx *ctx = calloc(1, sizeof(*ctx));
    ASSERT_TRUE(ctx != NULL);
    if (!gpu_test_init(ctx)) { free(ctx); ASSERT_TRUE(1); return; }
    const char *zero = "@compute @workgroup_size(1) fn main() {}";
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "a35_one", A35_ONE_BUF,
                                            strlen(A35_ONE_BUF)));
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "a35_zero", zero,
                                            strlen(zero)));
    ASSERT_TRUE(a35_valid_dispatch(ctx));

    uint32_t in[4] = {1, 2, 3, 4};
    HlGpuBufferDesc two[2] = {
        { .data = in, .data_len = sizeof(in), .size = sizeof(in), .binding = -1 },
        { .data = in, .data_len = sizeof(in), .size = sizeof(in), .binding = -1 },
    };
    const char *err = NULL;
    void *out = NULL;
    size_t len = 0;

    /* gpu.pipeline: one buffer too many (the reported crash). */
    HlGpuPipelineStage st = { .shader = "a35_one", .buffers = two,
                              .buffer_count = 2, .workgroups = {4, 1, 1} };
    HlGpuPipelineOpts po = { .stages = &st, .stage_count = 1, .device = -1 };
    HlGpuPipelineResult res;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH, hl_cap_gpu_pipeline(ctx, &po, &res, &err));
    EXPECT_STREQ("binding_mismatch", err);
    EXPECT_TRUE(a35_valid_dispatch(ctx));

    /* gpu.dispatch: the same. */
    HlGpuDispatchOpts o = { .buffers = two, .buffer_count = 2,
                            .workgroups = {4, 1, 1}, .output_buffer = 0,
                            .output_texture = -1, .device = -1 };
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH,
              hl_cap_gpu_dispatch(ctx, "a35_one", &o, &out, &len, &err));
    EXPECT_STREQ("binding_mismatch", err);
    EXPECT_TRUE(out == NULL);
    EXPECT_TRUE(a35_valid_dispatch(ctx));

    /* A shader with no bindings at all, given one. */
    o.buffer_count = 1;
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH,
              hl_cap_gpu_dispatch(ctx, "a35_zero", &o, &out, &len, &err));
    EXPECT_STREQ("binding_mismatch", err);
    st.shader = "a35_zero";
    st.buffer_count = 1;
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH, hl_cap_gpu_pipeline(ctx, &po, &res, &err));
    EXPECT_TRUE(a35_valid_dispatch(ctx));

    /* Uniforms where the shader declares a storage buffer at 0. */
    static const uint32_t uni[4] = {1, 2, 3, 4};
    o.uniforms = uni;
    o.uniforms_len = sizeof(uni);
    o.buffer_count = 0;
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH,
              hl_cap_gpu_dispatch(ctx, "a35_one", &o, &out, &len, &err));
    EXPECT_STREQ("binding_mismatch", err);
    EXPECT_TRUE(a35_valid_dispatch(ctx));

    hl_cap_gpu_destroy(ctx);
    free(ctx);
}

/* The backend's own guard, with the source-level check out of the way: a
 * layout the reader could not follow, and a declared binding the entry
 * point never uses (WGSL drops it from the auto layout, so the source says
 * 2 bindings and the pipeline has 1). wgpu reports the invalid bind group
 * through the error callback; the backend must not submit it. */
UTEST(hull_cap_gpu_audit35, backend_refuses_invalid_bind_group)
{
    HlGpuCtx *ctx = calloc(1, sizeof(*ctx));
    ASSERT_TRUE(ctx != NULL);
    if (!gpu_test_init(ctx)) { free(ctx); ASSERT_TRUE(1); return; }
    const char *unused =
        "@group(0) @binding(0) var<storage, read_write> data: array<u32>;\n"
        "@group(0) @binding(1) var<storage, read_write> spare: array<u32>;\n"
        "@compute @workgroup_size(1)\n"
        "fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
        "    data[id.x] = data[id.x] + 1u;\n"
        "}\n";
    const char *zero = "@compute @workgroup_size(1) fn main() {}";
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "a35_one", A35_ONE_BUF,
                                            strlen(A35_ONE_BUF)));
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "a35_unused", unused,
                                            strlen(unused)));
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "a35_zero", zero,
                                            strlen(zero)));
    a35_force_unknown_layout(ctx, "a35_one");
    a35_force_unknown_layout(ctx, "a35_zero");

    uint32_t in[4] = {1, 2, 3, 4};
    HlGpuBufferDesc two[2] = {
        { .data = in, .data_len = sizeof(in), .size = sizeof(in), .binding = -1 },
        { .data = in, .data_len = sizeof(in), .size = sizeof(in), .binding = -1 },
    };
    const char *err = NULL;
    void *out = NULL;
    size_t len = 0;
    HlGpuDispatchOpts o = { .buffers = two, .buffer_count = 2,
                            .workgroups = {4, 1, 1}, .output_buffer = 0,
                            .output_texture = -1, .device = -1 };
    const char *shaders[3] = { "a35_one", "a35_unused", "a35_zero" };
    for (int k = 0; k < 3; k++) {
        o.buffer_count = k == 2 ? 1 : 2;
        err = NULL;
        EXPECT_EQ(HL_GPU_ERR_DISPATCH,
                  hl_cap_gpu_dispatch(ctx, shaders[k], &o, &out, &len, &err));
        EXPECT_STREQ("binding_mismatch", err);
        EXPECT_TRUE(a35_valid_dispatch(ctx));

        HlGpuPipelineStage st = { .shader = shaders[k], .buffers = two,
                                  .buffer_count = o.buffer_count,
                                  .workgroups = {4, 1, 1} };
        HlGpuPipelineOpts po = { .stages = &st, .stage_count = 1, .device = -1 };
        HlGpuPipelineResult res;
        err = NULL;
        EXPECT_EQ(HL_GPU_ERR_DISPATCH, hl_cap_gpu_pipeline(ctx, &po, &res, &err));
        EXPECT_STREQ("binding_mismatch", err);
        EXPECT_TRUE(a35_valid_dispatch(ctx));
    }

    /* Too few: the shader uses binding 0, the call binds nothing at it. A
     * pipeline stage with no buffers sets no bind group; the pass fails
     * validation at the encoder's finish, never at the submit. */
    HlGpuPipelineStage st0 = { .shader = "a35_one", .buffer_count = 0,
                               .workgroups = {4, 1, 1} };
    HlGpuPipelineOutput none_out = { .stage = 0, .buffer = 0 };
    HlGpuPipelineOpts po0 = { .stages = &st0, .stage_count = 1,
                              .outputs = &none_out,
                              .output_count = HL_GPU_OUTPUT_NONE, .device = -1 };
    HlGpuPipelineResult res0;
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH, hl_cap_gpu_pipeline(ctx, &po0, &res0, &err));
    EXPECT_STREQ("gpu_validation_error", err);
    EXPECT_TRUE(a35_valid_dispatch(ctx));

    hl_cap_gpu_destroy(ctx);
    free(ctx);
}

/* Device limits Hull's own caps allow past: refused with a named error
 * before wgpu sees them (each was an invalid object, then an abort in the
 * queue call that used it). */
UTEST(hull_cap_gpu_audit35, device_limits_refused_before_wgpu)
{
    HlGpuCtx *ctx = calloc(1, sizeof(*ctx));
    ASSERT_TRUE(ctx != NULL);
    if (!gpu_test_init(ctx)) { free(ctx); ASSERT_TRUE(1); return; }
    const char *uni_wgsl =
        "struct U { k: u32 };\n"
        "@group(0) @binding(0) var<uniform> u: U;\n"
        "@group(0) @binding(1) var<storage, read_write> data: array<u32>;\n"
        "@compute @workgroup_size(1)\n"
        "fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
        "    data[id.x] = data[id.x] * u.k;\n"
        "}\n";
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "a35_one", A35_ONE_BUF,
                                            strlen(A35_ONE_BUF)));
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "a35_uni", uni_wgsl,
                                            strlen(uni_wgsl)));

    uint32_t in[4] = {1, 2, 3, 4};
    HlGpuBufferDesc b = { .data = in, .data_len = sizeof(in),
                          .size = sizeof(in), .binding = -1 };
    HlGpuDispatchOpts o = { .buffers = &b, .buffer_count = 1,
                            .workgroups = {1u << 20, 1, 1},
                            .output_buffer = -1, .output_texture = -1,
                            .device = -1 };
    const char *err = NULL;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH,
              hl_cap_gpu_dispatch(ctx, "a35_one", &o, NULL, NULL, &err));
    EXPECT_STREQ("workgroups_exceed_limit", err);
    HlGpuPipelineStage st = { .shader = "a35_one", .buffers = &b,
                              .buffer_count = 1, .workgroups = {1, 1u << 20, 1} };
    HlGpuPipelineOpts po = { .stages = &st, .stage_count = 1,
                             .output_count = HL_GPU_OUTPUT_NONE, .device = -1 };
    HlGpuPipelineResult res;
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH, hl_cap_gpu_pipeline(ctx, &po, &res, &err));
    EXPECT_STREQ("workgroups_exceed_limit", err);
    EXPECT_TRUE(a35_valid_dispatch(ctx));

    /* Uniforms past maxUniformBufferBindingSize (64 KiB by default). */
    size_t big = (size_t)1 << 20;
    void *ubig = calloc(1, big);
    ASSERT_TRUE(ubig != NULL);
    o.workgroups = (HlGpuWorkgroups){4, 1, 1};
    o.uniforms = ubig;
    o.uniforms_len = big;
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH,
              hl_cap_gpu_dispatch(ctx, "a35_uni", &o, NULL, NULL, &err));
    EXPECT_STREQ("uniforms_too_large", err);
    free(ubig);
    EXPECT_TRUE(a35_valid_dispatch(ctx));

    /* A persistent buffer bound with a larger size than it has. */
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_create(ctx, -1, "a35_p", 16,
                                                  HL_GPU_USAGE_READWRITE));
    HlGpuBufferDesc pb = { .name = "a35_p", .size = 64, .binding = -1 };
    HlGpuDispatchOpts po2 = { .buffers = &pb, .buffer_count = 1,
                              .workgroups = {4, 1, 1}, .output_buffer = -1,
                              .output_texture = -1, .device = -1 };
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH,
              hl_cap_gpu_dispatch(ctx, "a35_one", &po2, NULL, NULL, &err));
    EXPECT_STREQ("buffer_size_mismatch", err);
    st = (HlGpuPipelineStage){ .shader = "a35_one", .buffers = &pb,
                               .buffer_count = 1, .workgroups = {4, 1, 1} };
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH, hl_cap_gpu_pipeline(ctx, &po, &res, &err));
    EXPECT_STREQ("buffer_size_mismatch", err);
    EXPECT_TRUE(a35_valid_dispatch(ctx));

    /* An inline storage buffer past maxStorageBufferBindingSize. */
    HlGpuBufferDesc huge = { .size = (size_t)512 << 20, .binding = -1 };
    o.uniforms = NULL;
    o.uniforms_len = 0;
    o.buffers = &huge;
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH,
              hl_cap_gpu_dispatch(ctx, "a35_one", &o, NULL, NULL, &err));
    EXPECT_STREQ("buffer_too_large", err);
    EXPECT_TRUE(a35_valid_dispatch(ctx));

    /* A persistent buffer past maxBufferSize (the live cap is 1 GiB) was an
     * invalid buffer that "succeeded"; its first write aborted. Either it is
     * refused, or the device really takes it and the write works. */
    int rc = hl_cap_gpu_buffer_create(ctx, -1, "a35_big", (size_t)768 << 20,
                                      HL_GPU_USAGE_READWRITE);
    if (rc == HL_GPU_OK) {
        EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_write(ctx, -1, "a35_big", in,
                                                     sizeof(in), 0));
        hl_cap_gpu_buffer_destroy(ctx, -1, "a35_big");
    }
    /* Likewise a texture past maxTextureDimension2D (8192 by default; Hull
     * admits 16384). */
    rc = hl_cap_gpu_texture_create(ctx, -1, "a35_wide", 16384, 4,
                                   HL_GPU_TEX_R8, 0, 0, 0, 0);
    if (rc == HL_GPU_OK) {
        uint8_t *px = calloc(16384, 4);
        ASSERT_TRUE(px != NULL);
        EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_texture_write(ctx, -1, "a35_wide",
                                                      px, 16384 * 4));
        free(px);
        hl_cap_gpu_texture_destroy(ctx, -1, "a35_wide");
    }
    EXPECT_TRUE(a35_valid_dispatch(ctx));

    hl_cap_gpu_buffer_destroy(ctx, -1, "a35_p");
    hl_cap_gpu_destroy(ctx);
    free(ctx);
}

#else /* !HL_ENABLE_GPU */

UTEST(hull_cap_gpu, disabled_placeholder)
{
    /* GPU disabled - this placeholder ensures the test binary compiles
     * and runs successfully even without HL_ENABLE_GPU. */
    ASSERT_TRUE(1);
}

#endif /* HL_ENABLE_GPU */

/* ── Bind-group slot accounting (base-resident, no device needed) ──── */

#include "hull/cap/gpu.h"

UTEST(hull_cap_gpu, tex_binding_slots)
{
    ASSERT_EQ(1, hl_gpu_tex_binding_slots(1));
    ASSERT_EQ(2, hl_gpu_tex_binding_slots(0));
}

/* Audit 10 H1: the entry array is sized from the descriptor's storage flag;
 * a resolved texture of the other kind must be refused, not written. */
UTEST(hull_cap_gpu, tex_binding_storage_mismatch_refused)
{
    const char *err = NULL;
    /* desc says storage (1 slot counted), texture is sampled (2 slots) */
    ASSERT_EQ(-1, hl_gpu_tex_binding_check(1, 0, 0, 1, &err));
    ASSERT_STREQ("texture_storage_mismatch", err);
    err = NULL;
    /* the reverse is refused too (it would leave a slot unwritten) */
    ASSERT_EQ(-1, hl_gpu_tex_binding_check(0, 1, 0, 2, &err));
    ASSERT_STREQ("texture_storage_mismatch", err);
    /* matching kinds pass and report their slot count */
    ASSERT_EQ(1, hl_gpu_tex_binding_check(1, 1, 0, 1, &err));
    ASSERT_EQ(2, hl_gpu_tex_binding_check(0, 0, 3, 5, &err));
    /* any nonzero flag is "storage" */
    ASSERT_EQ(1, hl_gpu_tex_binding_check(7, 1, 0, 1, &err));
}

UTEST(hull_cap_gpu, tex_binding_bounds_checked)
{
    const char *err = NULL;
    /* a sampled texture needs 2 slots; only 1 left */
    ASSERT_EQ(-1, hl_gpu_tex_binding_check(0, 0, 4, 5, &err));
    ASSERT_STREQ("too_many_bindings", err);
    err = NULL;
    ASSERT_EQ(-1, hl_gpu_tex_binding_check(1, 1, 5, 5, &err));
    ASSERT_STREQ("too_many_bindings", err);
    ASSERT_EQ(-1, hl_gpu_tex_binding_check(1, 1, -1, 5, &err));
    ASSERT_EQ(-1, hl_gpu_tex_binding_check(1, 1, 6, 5, &err));
    /* NULL err is allowed */
    ASSERT_EQ(-1, hl_gpu_tex_binding_check(1, 0, 0, 2, NULL));
}

/* ── Live-bytes reservation and read-back caps (audit 17) ─────────────
 *
 * Persistent buffers and textures were bounded only by their slot counts,
 * and a read-back was copied off the VM heap whatever its size. The
 * accounting is in the dispatch layer, so a mock backend (no device, no
 * allocation) exercises it on every CI runner. */

#include <pthread.h>

static int mock_buf_creates, mock_tex_creates, mock_reads, mock_fail_create;

static int mock_buffer_create(HlGpuDevice *dev, const char *name, size_t size,
                              int usage, HlGpuBuffer *out)
{
    (void)dev; (void)name; (void)size; (void)usage;
    mock_buf_creates++;
    if (mock_fail_create) return HL_GPU_ERR_BUFFER;
    out->handle = (void *)1;
    return HL_GPU_OK;
}

static void mock_buffer_destroy(HlGpuDevice *dev, HlGpuBuffer *buf)
{
    (void)dev;
    buf->handle = NULL;
}

static int mock_buffer_read(HlGpuDevice *dev, HlGpuBuffer *buf,
                            void **data, size_t *len)
{
    (void)dev;
    mock_reads++;
    *data = calloc(1, buf->size);
    *len = buf->size;
    return *data ? HL_GPU_OK : HL_GPU_ERR_INTERNAL;
}

static int mock_texture_create(HlGpuDevice *dev, uint32_t w, uint32_t h,
                               HlGpuTexFormat fmt, int storage, int filter,
                               int au, int av, HlGpuTexture *out)
{
    (void)dev; (void)filter; (void)au; (void)av;
    mock_tex_creates++;
    if (mock_fail_create) return HL_GPU_ERR_BUFFER;
    out->handle = (void *)1;
    out->width = w;
    out->height = h;
    out->format = fmt;
    out->storage = storage;
    return HL_GPU_OK;
}

static int mock_texture_read(HlGpuDevice *dev, HlGpuTexture *tex,
                             void **data, size_t *len)
{
    (void)dev;
    mock_reads++;
    size_t n = hl_gpu_tex_bytes(tex->width, tex->height, tex->format);
    *data = calloc(1, n);
    *len = n;
    return *data ? HL_GPU_OK : HL_GPU_ERR_INTERNAL;
}

static void mock_texture_destroy(HlGpuDevice *dev, HlGpuTexture *tex)
{
    (void)dev;
    tex->handle = NULL;
}

/* The live total the backend saw while a dispatch / pipeline ran, the
 * timeout it was handed, what it returns, and whether the queue is idle. */
static int mock_dispatches;
static size_t mock_live_during;
static uint32_t mock_timeout_seen;
static int mock_dispatch_rc;
static int mock_idle = 1;
static int mock_idle_asks;

static int mock_queue_idle(HlGpuDevice *dev)
{
    (void)dev;
    mock_idle_asks++;
    return mock_idle;
}

static int mock_dispatch(HlGpuDevice *dev, HlGpuPipeline *pipeline,
                         const HlGpuDispatchOpts *opts,
                         const HlGpuBuffer *pb, int pc,
                         const HlGpuTexture *pt, int ptc,
                         void **output, size_t *output_len,
                         const char **err_msg)
{
    (void)pipeline; (void)pb; (void)pc; (void)pt; (void)ptc;
    (void)output; (void)output_len; (void)err_msg;
    mock_dispatches++;
    mock_live_during = dev->live_bytes;
    mock_timeout_seen = opts->timeout_ms;
    return mock_dispatch_rc;
}

static int mock_dispatch_pipeline(HlGpuDevice *dev, HlGpuPipeline **pipelines,
                                  int stage_count, const HlGpuPipelineOpts *opts,
                                  const HlGpuBuffer *pb, int pc,
                                  const HlGpuTexture *pt, int ptc,
                                  HlGpuPipelineResult *result,
                                  const char **err_msg)
{
    (void)pipelines; (void)stage_count; (void)pb; (void)pc;
    (void)pt; (void)ptc; (void)result; (void)err_msg;
    mock_dispatches++;
    mock_live_during = dev->live_bytes;
    mock_timeout_seen = opts->timeout_ms;
    return mock_dispatch_rc;
}

/* Writes, copies and the queued-write flush (audit 34). */
static int mock_writes, mock_copies, mock_flushes;
static int mock_flush_rc, mock_read_rc;

static int mock_buffer_write(HlGpuDevice *dev, HlGpuBuffer *buf,
                             const void *data, size_t len, size_t offset)
{
    (void)dev; (void)buf; (void)data; (void)len; (void)offset;
    mock_writes++;
    return HL_GPU_OK;
}

static int mock_texture_write(HlGpuDevice *dev, HlGpuTexture *tex,
                              const void *data, size_t len)
{
    (void)dev; (void)tex; (void)data; (void)len;
    mock_writes++;
    return HL_GPU_OK;
}

static int mock_buffer_copy(HlGpuDevice *dev, HlGpuBuffer *src,
                            HlGpuBuffer *dst, size_t so, size_t dso, size_t n)
{
    (void)dev; (void)src; (void)dst; (void)so; (void)dso; (void)n;
    mock_copies++;
    return HL_GPU_OK;
}

static int mock_flush_writes(HlGpuDevice *dev, uint32_t timeout_ms)
{
    (void)dev; (void)timeout_ms;
    mock_flushes++;
    return mock_flush_rc;
}

/* A read that times out, as the backend reports a hung device. */
static int mock_buffer_read_rc(HlGpuDevice *dev, HlGpuBuffer *buf,
                               void **data, size_t *len)
{
    if (mock_read_rc != HL_GPU_OK) {
        mock_reads++;
        return mock_read_rc;
    }
    return mock_buffer_read(dev, buf, data, len);
}

static const HlGpuBackend MOCK_BACKEND = {
    .name              = "mock",
    .buffer_create     = mock_buffer_create,
    .buffer_write      = mock_buffer_write,
    .buffer_copy       = mock_buffer_copy,
    .texture_write     = mock_texture_write,
    .flush_writes      = mock_flush_writes,
    .buffer_read       = mock_buffer_read_rc,
    .buffer_destroy    = mock_buffer_destroy,
    .texture_create    = mock_texture_create,
    .texture_read      = mock_texture_read,
    .texture_destroy   = mock_texture_destroy,
    .dispatch          = mock_dispatch,
    .dispatch_pipeline = mock_dispatch_pipeline,
    .queue_idle        = mock_queue_idle,
};

static HlGpuCtx *mock_ctx(void)
{
    HlGpuCtx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->backend = &MOCK_BACKEND;
    ctx->device_count = 1;
    pthread_mutex_init(&ctx->devices[0].mutex, NULL);
    mock_buf_creates = mock_tex_creates = mock_reads = mock_fail_create = 0;
    mock_dispatch_rc = HL_GPU_OK;
    mock_idle = 1;
    mock_idle_asks = 0;
    mock_writes = mock_copies = mock_flushes = 0;
    mock_flush_rc = HL_GPU_OK;
    mock_read_rc = HL_GPU_OK;
    return ctx;
}

static void mock_free(HlGpuCtx *ctx)
{
    pthread_mutex_destroy(&ctx->devices[0].mutex);
    free(ctx);
}

UTEST(hull_cap_gpu_live, buffers_are_capped_by_bytes)
{
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    size_t half = HL_GPU_MAX_LIVE_BYTES / 2;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_create(ctx, -1, "a", half, HL_GPU_USAGE_READWRITE));
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_create(ctx, -1, "b", half, HL_GPU_USAGE_READWRITE));
    EXPECT_EQ(HL_GPU_MAX_LIVE_BYTES, ctx->devices[0].live_bytes);
    /* Over the ceiling: refused before the backend runs. */
    EXPECT_EQ(HL_GPU_ERR_BUFFER, hl_cap_gpu_buffer_create(ctx, -1, "c", 4, HL_GPU_USAGE_READWRITE));
    EXPECT_EQ(2, mock_buf_creates);
    /* Destroying one gives its bytes back. */
    hl_cap_gpu_buffer_destroy(ctx, -1, "a");
    EXPECT_EQ(half, ctx->devices[0].live_bytes);
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_create(ctx, -1, "c", 4, HL_GPU_USAGE_READWRITE));
    /* A create the backend fails reserves nothing. */
    mock_fail_create = 1;
    EXPECT_NE(HL_GPU_OK, hl_cap_gpu_buffer_create(ctx, -1, "d", 64, HL_GPU_USAGE_READWRITE));
    EXPECT_EQ(half + 4, ctx->devices[0].live_bytes);
    /* One larger than the ceiling on its own. */
    mock_fail_create = 0;
    hl_cap_gpu_buffer_destroy(ctx, -1, "b");
    hl_cap_gpu_buffer_destroy(ctx, -1, "c");
    EXPECT_EQ((size_t)0, ctx->devices[0].live_bytes);
    EXPECT_EQ(HL_GPU_ERR_BUFFER,
              hl_cap_gpu_buffer_create(ctx, -1, "e", HL_GPU_MAX_LIVE_BYTES + 1,
                                       HL_GPU_USAGE_READWRITE));
    mock_free(ctx);
}

UTEST(hull_cap_gpu_live, textures_reserve_their_bytes)
{
    EXPECT_EQ((size_t)16, hl_gpu_tex_bytes(2, 2, HL_GPU_TEX_RGBA8));
    EXPECT_EQ((size_t)4, hl_gpu_tex_bytes(2, 2, HL_GPU_TEX_R8));
    EXPECT_EQ((size_t)32, hl_gpu_tex_bytes(2, 2, HL_GPU_TEX_RGBA16F));
    EXPECT_EQ((size_t)16, hl_gpu_tex_bytes(2, 2, HL_GPU_TEX_R32F));

    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    /* 16384^2 x 8 = 2 GiB: refused, no backend create (it allocated the
     * whole texture with no upload). */
    EXPECT_EQ(HL_GPU_ERR_BUFFER,
              hl_cap_gpu_texture_create(ctx, -1, "big", 16384, 16384,
                                        HL_GPU_TEX_RGBA16F, 0, 0, 0, 0));
    EXPECT_EQ(0, mock_tex_creates);
    /* 16384 x 8192 x 4 = 512 MiB fits, twice; a third does not. */
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_texture_create(ctx, -1, "t1", 16384, 8192,
                                                   HL_GPU_TEX_RGBA8, 0, 0, 0, 0));
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_texture_create(ctx, -1, "t2", 16384, 8192,
                                                   HL_GPU_TEX_RGBA8, 0, 0, 0, 0));
    EXPECT_EQ(HL_GPU_ERR_BUFFER,
              hl_cap_gpu_texture_create(ctx, -1, "t3", 1, 1,
                                        HL_GPU_TEX_RGBA8, 0, 0, 0, 0));
    EXPECT_EQ(2, mock_tex_creates);
    hl_cap_gpu_texture_destroy(ctx, -1, "t1");
    hl_cap_gpu_texture_destroy(ctx, -1, "t2");
    EXPECT_EQ((size_t)0, ctx->devices[0].live_bytes);
    mock_free(ctx);
}

UTEST(hull_cap_gpu_live, readback_refused_past_max_len)
{
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    void *data = NULL;
    size_t len = 0;
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_create(ctx, -1, "b", 64, HL_GPU_USAGE_READWRITE));
    /* Larger than the caller's heap limit: no backend copy is made. */
    EXPECT_EQ(HL_GPU_ERR_BUFFER, hl_cap_gpu_buffer_read(ctx, -1, "b", 32, &data, &len));
    EXPECT_EQ(0, mock_reads);
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_read(ctx, -1, "b", 64, &data, &len));
    EXPECT_EQ((size_t)64, len);
    free(data);
    data = NULL;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_read(ctx, -1, "b", 0, &data, &len));
    free(data);
    data = NULL;

    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_texture_create(ctx, -1, "t", 4, 4,
                                                   HL_GPU_TEX_RGBA8, 0, 0, 0, 0));
    uint32_t w = 0, h = 0;
    HlGpuTexFormat fmt;
    mock_reads = 0;
    EXPECT_EQ(HL_GPU_ERR_BUFFER,
              hl_cap_gpu_texture_read(ctx, -1, "t", 63, &data, &len, &w, &h, &fmt));
    EXPECT_EQ(0, mock_reads);
    EXPECT_EQ(HL_GPU_OK,
              hl_cap_gpu_texture_read(ctx, -1, "t", 64, &data, &len, &w, &h, &fmt));
    EXPECT_EQ((size_t)64, len);
    free(data);
    hl_cap_gpu_buffer_destroy(ctx, -1, "b");
    hl_cap_gpu_texture_destroy(ctx, -1, "t");
    mock_free(ctx);
}

/* gpu.dispatch / gpu.pipeline's own buffers, uniforms and inline textures
 * were neither charged nor counted on the device's live total (audit 17):
 * they are now reserved for the call, and refused past the ceiling before
 * the backend runs. */
static void mock_add_shader(HlGpuCtx *ctx, const char *name)
{
    HlGpuDevice *dev = &ctx->devices[0];
    snprintf(dev->pipelines[dev->pipeline_count].name,
             sizeof(dev->pipelines[0].name), "%s", name);
    dev->pipeline_count++;
}

UTEST(hull_cap_gpu_live, dispatch_reserves_inline_buffers)
{
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    mock_add_shader(ctx, "sh");
    mock_dispatches = 0;
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_create(ctx, -1, "p", 64, HL_GPU_USAGE_READWRITE));
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_texture_create(ctx, -1, "pt", 2, 2,
                                                   HL_GPU_TEX_RGBA8, 0, 0, 0, 0));
    EXPECT_EQ((size_t)80, ctx->devices[0].live_bytes);

    HlGpuBufferDesc bufs[3] = {
        { .name = NULL, .size = 10, .binding = -1 },   /* word-rounded: 12 */
        { .name = "p",  .size = 100, .binding = -1 },  /* persistent: 0 */
        { .name = NULL, .size = 0, .binding = -1 },    /* at least a word: 4 */
    };
    HlGpuTextureDesc texs[2] = {
        { .name = NULL, .width = 2, .height = 2, .format = HL_GPU_TEX_RGBA8 },  /* 16 */
        { .name = "pt", .width = 2, .height = 2, .format = HL_GPU_TEX_RGBA8 },  /* persistent */
    };
    static const char uni[5] = "abcd";
    HlGpuDispatchOpts opts = {
        .buffers = bufs, .buffer_count = 3,
        .textures = texs, .texture_count = 2,
        .uniforms = uni, .uniforms_len = 5,   /* 16-byte rounded: 16 */
        .output_buffer = -1, .output_texture = -1, .device = -1,
    };
    EXPECT_EQ((size_t)48, hl_gpu_dispatch_transient_bytes(&ctx->devices[0], &opts));
    /* The binding's charge does not know which names are persistent. */
    EXPECT_EQ((size_t)(12 + 100 + 4 + 16 + 16 + 16), hl_gpu_dispatch_charge_bytes(&opts));

    const char *err = NULL;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_dispatch(ctx, "sh", &opts, NULL, NULL, &err));
    EXPECT_EQ(1, mock_dispatches);
    EXPECT_EQ((size_t)(80 + 48), mock_live_during);
    EXPECT_EQ((size_t)80, ctx->devices[0].live_bytes);   /* given back */

    /* Past the ceiling with what is live: refused before the backend. */
    bufs[0].size = HL_GPU_MAX_LIVE_BYTES - 64;
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_BUFFER, hl_cap_gpu_dispatch(ctx, "sh", &opts, NULL, NULL, &err));
    EXPECT_STREQ("device_memory_limit", err);
    EXPECT_EQ(1, mock_dispatches);
    EXPECT_EQ((size_t)80, ctx->devices[0].live_bytes);

    /* The charge saturates at the ceiling (a larger call is refused). */
    bufs[0].size = SIZE_MAX;
    EXPECT_EQ((size_t)SIZE_MAX, hl_gpu_dispatch_transient_bytes(NULL, &opts));
    EXPECT_EQ((size_t)HL_GPU_MAX_LIVE_BYTES, hl_gpu_dispatch_charge_bytes(&opts));

    hl_cap_gpu_buffer_destroy(ctx, -1, "p");
    hl_cap_gpu_texture_destroy(ctx, -1, "pt");
    EXPECT_EQ((size_t)0, ctx->devices[0].live_bytes);
    mock_free(ctx);
}

UTEST(hull_cap_gpu_live, pipeline_reserves_stage_buffers)
{
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    mock_add_shader(ctx, "s0");
    mock_add_shader(ctx, "s1");
    mock_dispatches = 0;
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_create(ctx, -1, "p", 64, HL_GPU_USAGE_READWRITE));

    HlGpuBufferDesc b0[2] = {
        { .name = "a",  .size = 8, .binding = -1 },
        { .name = NULL, .size = 3, .binding = -1 },    /* 4 */
    };
    HlGpuBufferDesc b1[2] = {
        { .name = "a",  .size = 40, .binding = -1 },   /* "a" once, at 40 */
        { .name = "p",  .size = 0, .binding = -1 },    /* persistent: 0 */
    };
    static const char uni[20] = "0123456789abcdefghi";
    HlGpuPipelineStage stages[2] = {
        { .shader = "s0", .buffers = b0, .buffer_count = 2,
          .uniforms = uni, .uniforms_len = 20 },        /* 32 */
        { .shader = "s1", .buffers = b1, .buffer_count = 2 },
    };
    HlGpuPipelineOpts opts = {
        .stages = stages, .stage_count = 2,
        .output_count = HL_GPU_OUTPUT_NONE, .device = -1,
    };
    EXPECT_EQ((size_t)(40 + 4 + 32), hl_gpu_pipeline_transient_bytes(&ctx->devices[0], &opts));
    /* Without the device, "p" counts as made (a word). */
    EXPECT_EQ((size_t)(40 + 4 + 32 + 4), hl_gpu_pipeline_charge_bytes(&opts));

    HlGpuPipelineResult result;
    const char *err = NULL;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_pipeline(ctx, &opts, &result, &err));
    EXPECT_EQ(1, mock_dispatches);
    EXPECT_EQ((size_t)(64 + 76), mock_live_during);
    EXPECT_EQ((size_t)64, ctx->devices[0].live_bytes);

    b1[0].size = HL_GPU_MAX_LIVE_BYTES;
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_BUFFER, hl_cap_gpu_pipeline(ctx, &opts, &result, &err));
    EXPECT_STREQ("device_memory_limit", err);
    EXPECT_EQ(1, mock_dispatches);
    EXPECT_EQ((size_t)64, ctx->devices[0].live_bytes);

    hl_cap_gpu_buffer_destroy(ctx, -1, "p");
    mock_free(ctx);
}


/* ── Audit 33: timeouts clamped in the cap layer; a timed-out call keeps
 * its memory reserved ──────────────────────────────────────────────── */

/* The JS bindings passed `timeout: -1` through as 2^32-1 ms (~49.7 days on
 * the event loop). The cap layer now clamps every caller's timeout. */
UTEST(hull_cap_gpu_audit33, dispatch_and_pipeline_timeouts_are_clamped)
{
    EXPECT_EQ((uint32_t)0, hl_gpu_timeout_clamp(0));
    EXPECT_EQ((uint32_t)10, hl_gpu_timeout_clamp(10));
    EXPECT_EQ((uint32_t)HL_GPU_TIMEOUT_MAX_MS, hl_gpu_timeout_clamp(UINT32_MAX));

    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    mock_add_shader(ctx, "sh");
    HlGpuDispatchOpts opts = {
        .output_buffer = -1, .output_texture = -1, .device = -1,
        .timeout_ms = UINT32_MAX,
    };
    const char *err = NULL;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_dispatch(ctx, "sh", &opts, NULL, NULL, &err));
    EXPECT_EQ((uint32_t)HL_GPU_TIMEOUT_MAX_MS, mock_timeout_seen);
    EXPECT_EQ((uint32_t)UINT32_MAX, opts.timeout_ms);   /* the caller's copy */
    opts.timeout_ms = 250;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_dispatch(ctx, "sh", &opts, NULL, NULL, &err));
    EXPECT_EQ((uint32_t)250, mock_timeout_seen);

    HlGpuPipelineStage stages[1] = { { .shader = "sh" } };
    HlGpuPipelineOpts popts = {
        .stages = stages, .stage_count = 1,
        .output_count = HL_GPU_OUTPUT_NONE, .device = -1,
        .timeout_ms = HL_GPU_TIMEOUT_MAX_MS + 1,
    };
    HlGpuPipelineResult result;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_pipeline(ctx, &popts, &result, &err));
    EXPECT_EQ((uint32_t)HL_GPU_TIMEOUT_MAX_MS, mock_timeout_seen);
    mock_free(ctx);
}

/* A timed-out dispatch is still running, and so is the memory its transient
 * buffers hold: it was given back at once, so repeated timed-out calls held
 * device memory past HL_GPU_MAX_LIVE_BYTES. It now stays reserved and the
 * device refuses new work until the queue has drained. */
UTEST(hull_cap_gpu_audit33, a_timed_out_dispatch_keeps_its_bytes_until_idle)
{
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    mock_add_shader(ctx, "sh");
    mock_dispatches = 0;
    HlGpuBufferDesc bufs[1] = { { .name = NULL, .size = 64, .binding = -1 } };
    HlGpuDispatchOpts opts = {
        .buffers = bufs, .buffer_count = 1,
        .output_buffer = -1, .output_texture = -1, .device = -1,
    };
    const char *err = NULL;

    mock_dispatch_rc = HL_GPU_ERR_TIMEOUT;
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, hl_cap_gpu_dispatch(ctx, "sh", &opts, NULL, NULL, &err));
    EXPECT_EQ((size_t)64, ctx->devices[0].live_bytes);     /* still held */
    EXPECT_EQ(1, ctx->devices[0].stalled);

    /* Busy: refused before the backend runs, nothing more reserved. */
    mock_dispatch_rc = HL_GPU_OK;
    mock_idle = 0;
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, hl_cap_gpu_dispatch(ctx, "sh", &opts, NULL, NULL, &err));
    EXPECT_STREQ("gpu_busy", err ? err : "");
    EXPECT_EQ(1, mock_dispatches);
    EXPECT_EQ((size_t)64, ctx->devices[0].live_bytes);
    HlGpuPipelineStage stages[1] = { { .shader = "sh", .buffers = bufs, .buffer_count = 1 } };
    HlGpuPipelineOpts popts = {
        .stages = stages, .stage_count = 1,
        .output_count = HL_GPU_OUTPUT_NONE, .device = -1,
    };
    HlGpuPipelineResult result;
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, hl_cap_gpu_pipeline(ctx, &popts, &result, &err));
    EXPECT_STREQ("gpu_busy", err ? err : "");
    EXPECT_EQ(1, mock_dispatches);

    /* Drained: the stalled bytes go back and the call runs. */
    mock_idle = 1;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_dispatch(ctx, "sh", &opts, NULL, NULL, &err));
    EXPECT_EQ(2, mock_dispatches);
    EXPECT_EQ((size_t)64, mock_live_during);               /* only this call's */
    EXPECT_EQ((size_t)0, ctx->devices[0].live_bytes);
    EXPECT_EQ(0, ctx->devices[0].stalled);

    /* A timed-out pipeline stalls the device the same way. */
    mock_dispatch_rc = HL_GPU_ERR_TIMEOUT;
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, hl_cap_gpu_pipeline(ctx, &popts, &result, &err));
    EXPECT_EQ((size_t)64, ctx->devices[0].live_bytes);
    EXPECT_EQ(1, ctx->devices[0].stalled);
    mock_dispatch_rc = HL_GPU_OK;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_pipeline(ctx, &popts, &result, &err));
    EXPECT_EQ((size_t)0, ctx->devices[0].live_bytes);
    mock_free(ctx);
}

/* ── Queued writes and the stall gate on every submitting entry (audit 34)
 *
 * wgpu holds every queued write's staging copy until a submission
 * completes, and nothing in a write path submitted: a loop of uploads held
 * 4.8 GB of device memory. And a stalled device refused only dispatch /
 * pipeline: a read waited out its timeout behind the hung work. */

UTEST(hull_cap_gpu_audit34, queued_writes_are_flushed_past_the_cap)
{
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    const size_t chunk = (size_t)16 << 20;
    void *data = calloc(1, chunk);
    ASSERT_TRUE(data != NULL);
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_create(ctx, -1, "b", chunk,
                                                   HL_GPU_USAGE_READWRITE));
    HlGpuDevice *dev = &ctx->devices[0];

    /* Under the cap: queued, nothing submitted. */
    int n = (int)(HL_GPU_MAX_QUEUED_WRITE_BYTES / chunk);
    for (int i = 0; i < n - 1; i++)
        EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_write(ctx, -1, "b", data, chunk, 0));
    EXPECT_EQ(0, mock_flushes);
    EXPECT_EQ((size_t)(n - 1) * chunk, dev->queued_write_bytes);

    /* Reaching it: the backend submits and waits; the count starts over. */
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_write(ctx, -1, "b", data, chunk, 0));
    EXPECT_EQ(1, mock_flushes);
    EXPECT_EQ((size_t)0, dev->queued_write_bytes);

    /* A loop of writes is bounded: one flush per cap's worth. */
    for (int i = 0; i < 10 * n; i++)
        EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_write(ctx, -1, "b", data, chunk, 0));
    EXPECT_EQ(11, mock_flushes);
    EXPECT_TRUE(dev->queued_write_bytes < HL_GPU_MAX_QUEUED_WRITE_BYTES);

    /* A completed read carried the queued writes with it. */
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_write(ctx, -1, "b", data, chunk, 0));
    void *out = NULL;
    size_t out_len = 0;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_read(ctx, -1, "b", 0, &out, &out_len));
    free(out);
    EXPECT_EQ((size_t)0, dev->queued_write_bytes);

    /* Texture writes count their (row-padded) staging copy. */
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_texture_create(ctx, -1, "t", 3, 2,
                                                    HL_GPU_TEX_RGBA8, 0, 0, 0, 0));
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_texture_write(ctx, -1, "t", data, 24));
    EXPECT_EQ((size_t)(256 * 2), dev->queued_write_bytes);

    /* A flush that times out stalls the device with the queued bytes held. */
    size_t live_before = dev->live_bytes;
    mock_flush_rc = HL_GPU_ERR_TIMEOUT;
    int rc = HL_GPU_OK;
    for (int i = 0; i < n && rc == HL_GPU_OK; i++)
        rc = hl_cap_gpu_buffer_write(ctx, -1, "b", data, chunk, 0);
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, rc);
    EXPECT_EQ(1, dev->stalled);
    EXPECT_EQ((size_t)0, dev->queued_write_bytes);
    EXPECT_TRUE(dev->stalled_bytes >= HL_GPU_MAX_QUEUED_WRITE_BYTES);
    EXPECT_EQ(live_before + dev->stalled_bytes, dev->live_bytes);

    /* Busy until the queue drains; then the held bytes go back. */
    mock_idle = 0;
    int writes = mock_writes;
    EXPECT_EQ(HL_GPU_ERR_BUSY, hl_cap_gpu_buffer_write(ctx, -1, "b", data, chunk, 0));
    EXPECT_EQ(writes, mock_writes);
    mock_idle = 1;
    mock_flush_rc = HL_GPU_OK;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_write(ctx, -1, "b", data, chunk, 0));
    EXPECT_EQ(0, dev->stalled);
    EXPECT_EQ(live_before, dev->live_bytes);

    free(data);
    mock_free(ctx);
}

UTEST(hull_cap_gpu_audit34, a_stalled_device_refuses_reads_copies_writes)
{
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_create(ctx, -1, "a", 64, HL_GPU_USAGE_READWRITE));
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_create(ctx, -1, "b", 64, HL_GPU_USAGE_READWRITE));
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_texture_create(ctx, -1, "t", 4, 4,
                                                    HL_GPU_TEX_RGBA8, 0, 0, 0, 0));
    HlGpuDevice *dev = &ctx->devices[0];
    dev->stalled = 1;
    mock_idle = 0;

    static const char px[64];
    void *out = NULL;
    size_t out_len = 0;
    EXPECT_EQ(HL_GPU_ERR_BUSY, hl_cap_gpu_buffer_read(ctx, -1, "a", 0, &out, &out_len));
    EXPECT_EQ(HL_GPU_ERR_BUSY, hl_cap_gpu_texture_read(ctx, -1, "t", 0, &out, &out_len,
                                                        NULL, NULL, NULL));
    EXPECT_EQ(HL_GPU_ERR_BUSY, hl_cap_gpu_buffer_copy(ctx, -1, "a", "b", 0, 0, 0));
    EXPECT_EQ(HL_GPU_ERR_BUSY, hl_cap_gpu_buffer_write(ctx, -1, "a", px, 64, 0));
    EXPECT_EQ(HL_GPU_ERR_BUSY, hl_cap_gpu_texture_write(ctx, -1, "t", px, 64));
    EXPECT_EQ(0, mock_reads);
    EXPECT_EQ(0, mock_copies);
    EXPECT_EQ(0, mock_writes);
    EXPECT_TRUE(out == NULL);

    /* Drained: each runs again. */
    mock_idle = 1;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_copy(ctx, -1, "a", "b", 0, 0, 0));
    EXPECT_EQ(1, mock_copies);
    EXPECT_EQ(0, dev->stalled);
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_read(ctx, -1, "a", 0, &out, &out_len));
    free(out);
    mock_free(ctx);
}

UTEST(hull_cap_gpu_audit34, a_read_that_times_out_stalls_the_device)
{
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_create(ctx, -1, "a", 1000, HL_GPU_USAGE_READWRITE));
    static const char px[64];
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_write(ctx, -1, "a", px, 64, 0));
    HlGpuDevice *dev = &ctx->devices[0];
    EXPECT_EQ((size_t)64, dev->queued_write_bytes);

    mock_read_rc = HL_GPU_ERR_TIMEOUT;
    void *out = NULL;
    size_t out_len = 0;
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, hl_cap_gpu_buffer_read(ctx, -1, "a", 0, &out, &out_len));
    EXPECT_EQ(1, dev->stalled);
    /* The staging copy of the read and the write it carried stay held. */
    EXPECT_EQ((size_t)(1000 + 64), dev->stalled_bytes);
    EXPECT_EQ((size_t)(1000 + 1000 + 64), dev->live_bytes);
    EXPECT_EQ((size_t)0, dev->queued_write_bytes);

    /* The next read is refused at once, without reaching the backend. */
    mock_idle = 0;
    int reads = mock_reads;
    EXPECT_EQ(HL_GPU_ERR_BUSY, hl_cap_gpu_buffer_read(ctx, -1, "a", 0, &out, &out_len));
    EXPECT_EQ(reads, mock_reads);

    mock_idle = 1;
    mock_read_rc = HL_GPU_OK;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_read(ctx, -1, "a", 0, &out, &out_len));
    free(out);
    EXPECT_EQ((size_t)1000, dev->live_bytes);
    EXPECT_EQ((size_t)0, dev->stalled_bytes);
    mock_free(ctx);
}

/* ── Audit 35: a dispatch / pipeline that queued its inline uploads and
 * failed before submitting them left their staging copies queued, and
 * the cap layer counted nothing for a failed call, so the audit-34 flush
 * never came (1.5 GB on Metal from a loop of failing dispatches). A failed
 * call now counts its transient bytes as queued writes. */

UTEST(hull_cap_gpu_audit35, a_failed_dispatch_counts_its_staged_uploads)
{
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    mock_add_shader(ctx, "sh");
    mock_add_shader(ctx, "s1");
    mock_dispatches = 0;
    HlGpuDevice *dev = &ctx->devices[0];

    const size_t chunk = (size_t)8 << 20;
    HlGpuBufferDesc bufs[1] = {
        { .name = NULL, .size = chunk, .binding = -1 },
    };
    HlGpuDispatchOpts opts = {
        .buffers = bufs, .buffer_count = 1,
        .output_buffer = -1, .output_texture = -1, .device = -1,
    };
    const char *err = NULL;

    /* A failure the backend reports before its submit: counted. */
    mock_dispatch_rc = HL_GPU_ERR_DISPATCH;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH, hl_cap_gpu_dispatch(ctx, "sh", &opts, NULL, NULL, &err));
    EXPECT_EQ(chunk, dev->queued_write_bytes);
    EXPECT_EQ((size_t)0, dev->live_bytes);   /* the reservation went back */
    EXPECT_EQ(0, mock_flushes);

    /* A loop of them is bounded: one flush per cap's worth. */
    int n = (int)(HL_GPU_MAX_QUEUED_WRITE_BYTES / chunk);
    for (int i = 0; i < 10 * n; i++)
        (void)hl_cap_gpu_dispatch(ctx, "sh", &opts, NULL, NULL, &err);
    EXPECT_EQ(10, mock_flushes);
    EXPECT_TRUE(dev->queued_write_bytes < HL_GPU_MAX_QUEUED_WRITE_BYTES);

    /* Success carried every queued write with its submission. */
    mock_dispatch_rc = HL_GPU_OK;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_dispatch(ctx, "sh", &opts, NULL, NULL, &err));
    EXPECT_EQ((size_t)0, dev->queued_write_bytes);

    /* The pipeline path too. */
    HlGpuBufferDesc b0[1] = { { .name = NULL, .size = chunk, .binding = -1 } };
    HlGpuPipelineStage stages[2] = {
        { .shader = "sh", .buffers = b0, .buffer_count = 1 },
        { .shader = "s1", .buffers = b0, .buffer_count = 1 },
    };
    HlGpuPipelineOpts popts = {
        .stages = stages, .stage_count = 2,
        .output_count = HL_GPU_OUTPUT_NONE, .device = -1,
    };
    HlGpuPipelineResult result;
    mock_dispatch_rc = HL_GPU_ERR_DISPATCH;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH, hl_cap_gpu_pipeline(ctx, &popts, &result, &err));
    EXPECT_EQ(hl_gpu_pipeline_transient_bytes(dev, &popts), dev->queued_write_bytes);
    EXPECT_TRUE(dev->queued_write_bytes >= chunk);

    /* A timeout is not double-counted: the queued writes stall the device. */
    mock_dispatch_rc = HL_GPU_ERR_TIMEOUT;
    size_t queued = dev->queued_write_bytes;
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, hl_cap_gpu_dispatch(ctx, "sh", &opts, NULL, NULL, &err));
    EXPECT_EQ((size_t)0, dev->queued_write_bytes);
    EXPECT_EQ(chunk + queued, dev->stalled_bytes);
    mock_free(ctx);
}

/* A write the backend refused may still have staged part of itself (whole
 * words, then a padded tail): counted all the same. */
static int mock_write_rc;
static int mock_buffer_write_rc(HlGpuDevice *dev, HlGpuBuffer *buf,
                                const void *data, size_t len, size_t offset)
{
    (void)mock_buffer_write(dev, buf, data, len, offset);
    return mock_write_rc;
}

UTEST(hull_cap_gpu_audit35, a_refused_write_is_still_counted)
{
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    HlGpuBackend be = MOCK_BACKEND;
    be.buffer_write = mock_buffer_write_rc;
    ctx->backend = &be;
    mock_write_rc = HL_GPU_ERR_BUFFER;
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_create(ctx, -1, "a", 64, HL_GPU_USAGE_READWRITE));
    static const char px[64];
    EXPECT_EQ(HL_GPU_ERR_BUFFER, hl_cap_gpu_buffer_write(ctx, -1, "a", px, 64, 0));
    EXPECT_EQ((size_t)64, ctx->devices[0].queued_write_bytes);
    hl_cap_gpu_buffer_destroy(ctx, -1, "a");
    mock_free(ctx);
}

/* ── Audit 35: the shader's declared layout (no device needed) ────────
 *
 * wgpu-native aborts the process on a submit whose bind group failed
 * validation, so the dispatch layer checks every binding a call makes
 * against the `@group(0) @binding(N)` declarations read at compile. */

static HlGpuPipeline a35_pipe;

static int a35_layout(const char *wgsl)
{
    memset(&a35_pipe, 0, sizeof(a35_pipe));
    return hl_gpu_wgsl_layout(wgsl, strlen(wgsl), &a35_pipe);
}

static const HlGpuBindingDecl *a35_decl(int binding)
{
    for (int i = 0; i < a35_pipe.decl_count; i++)
        if (a35_pipe.decls[i].binding == binding)
            return &a35_pipe.decls[i];
    return NULL;
}

UTEST(hull_cap_gpu_audit35, wgsl_layout_reads_declarations)
{
    ASSERT_EQ(0, a35_layout(
        "// @group(0) @binding(9) var<storage> commented: array<u32>;\n"
        "/* nested /* @group(0) @binding(8) var<uniform> c: u32; */ */\n"
        "struct U { k: u32, @size(16) pad: u32 };\n"
        "@group(0) @binding(0) var<uniform> u: U;\n"
        "@binding(1) @group(0) var<storage, read_write> a: array<u32>;\n"
        "@group(0)\n  @binding(0x2) var<storage,read> b: array<f32>;\n"
        "@group(0) @binding(3u) var t: texture_2d<f32>;\n"
        "@group(0) @binding(4i) var s: sampler;\n"
        "@group(0) @binding(5) var o: texture_storage_2d<r32float, write>;\n"
        "@group(1) @binding(0) var<storage> other_group: array<u32>;\n"
        "alias Img = texture_2d<f32>;\n"
        "@group(0) @binding(6) var img: Img;\n"
        "@group(0) @binding(7) var cube: texture_cube<f32>;\n"
        "var<private> p: u32;\n"
        "var<workgroup> w: array<u32, 64>;\n"
        "@compute @workgroup_size(8, 8)\n"
        "fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
        "    var local = 1u;\n"
        "}\n"));
    EXPECT_EQ(1, a35_pipe.layout_known);
    EXPECT_EQ(8, a35_pipe.decl_count);
    ASSERT_TRUE(a35_decl(0) && a35_decl(1) && a35_decl(2) && a35_decl(3));
    ASSERT_TRUE(a35_decl(4) && a35_decl(5) && a35_decl(6) && a35_decl(7));
    EXPECT_EQ(HL_GPU_BIND_UNIFORM, a35_decl(0)->kind);
    EXPECT_EQ(HL_GPU_BIND_STORAGE, a35_decl(1)->kind);
    EXPECT_EQ(HL_GPU_BIND_STORAGE, a35_decl(2)->kind);
    EXPECT_EQ(HL_GPU_BIND_TEXTURE, a35_decl(3)->kind);
    EXPECT_EQ(HL_GPU_BIND_SAMPLER, a35_decl(4)->kind);
    EXPECT_EQ(HL_GPU_BIND_STORAGE_TEXTURE, a35_decl(5)->kind);
    EXPECT_EQ(HL_GPU_TEX_R32F, a35_decl(5)->format);
    EXPECT_EQ(HL_GPU_BIND_UNKNOWN, a35_decl(6)->kind);
    EXPECT_EQ(HL_GPU_BIND_OTHER, a35_decl(7)->kind);
    EXPECT_TRUE(a35_decl(8) == NULL);   /* in a comment */
    EXPECT_TRUE(a35_decl(9) == NULL);

    /* No bindings at all is a known, empty layout. */
    ASSERT_EQ(0, a35_layout("@compute @workgroup_size(1) fn main() {}"));
    EXPECT_EQ(1, a35_pipe.layout_known);
    EXPECT_EQ(0, a35_pipe.decl_count);
}

UTEST(hull_cap_gpu_audit35, wgsl_layout_unknown_when_not_followed)
{
    /* A const-expression index: the reader cannot evaluate it. */
    EXPECT_EQ(-1, a35_layout(
        "const B = 1;\n@group(0) @binding(B) var<storage> a: array<u32>;\n"));
    EXPECT_EQ(0, a35_pipe.layout_known);
    /* group / binding not followed by a var. */
    EXPECT_EQ(-1, a35_layout("@group(0) @binding(0) fn f() {}"));
    EXPECT_EQ(-1, a35_layout("@group(0) var<storage> a: array<u32>;"));
    EXPECT_EQ(-1, a35_layout("@group(0) @binding(0)"));
    /* A duplicate binding (WGSL itself refuses it). */
    EXPECT_EQ(-1, a35_layout(
        "@group(0) @binding(0) var<storage> a: array<u32>;\n"
        "@group(0) @binding(0) var<storage> b: array<u32>;\n"));
    EXPECT_EQ(-1, a35_layout("@group(0) @binding(99999999999) var<storage> a: u32;"));
    EXPECT_EQ(-1, hl_gpu_wgsl_layout(NULL, 0, &a35_pipe));
    EXPECT_EQ(0, a35_pipe.layout_known);
}

UTEST(hull_cap_gpu_audit35, bindings_check_matches_hull_assignment)
{
    const char *err = NULL;
    HlGpuBufferDesc bufs[3];
    memset(bufs, 0, sizeof(bufs));
    HlGpuTextureDesc texs[2];
    memset(texs, 0, sizeof(texs));

    /* One storage buffer declared. */
    ASSERT_EQ(0, a35_layout("@group(0) @binding(0) var<storage, read_write> d: array<u32>;"));
    EXPECT_EQ(0, hl_gpu_bindings_check(&a35_pipe, NULL, 0, bufs, 1, NULL, 0, &err));
    EXPECT_EQ(-1, hl_gpu_bindings_check(&a35_pipe, NULL, 0, bufs, 2, NULL, 0, &err));
    EXPECT_STREQ("binding_mismatch", err);
    /* Uniforms take binding 0, which is a storage buffer here. */
    err = NULL;
    EXPECT_EQ(-1, hl_gpu_bindings_check(&a35_pipe, NULL, 1, bufs, 0, NULL, 0, &err));
    EXPECT_STREQ("binding_mismatch", err);
    /* A texture where nothing is declared. */
    EXPECT_EQ(-1, hl_gpu_bindings_check(&a35_pipe, NULL, 0, bufs, 1, texs, 1, &err));
    /* Fewer than declared is left to the backend (WGSL drops unused ones). */
    ASSERT_EQ(0, a35_layout(
        "@group(0) @binding(0) var<storage> a: array<u32>;\n"
        "@group(0) @binding(1) var<storage> b: array<u32>;\n"));
    EXPECT_EQ(0, hl_gpu_bindings_check(&a35_pipe, NULL, 0, bufs, 1, NULL, 0, &err));

    /* uniform + 2 buffers + a sampled texture + a storage texture. */
    ASSERT_EQ(0, a35_layout(
        "@group(0) @binding(0) var<uniform> u: vec4<f32>;\n"
        "@group(0) @binding(1) var<storage> a: array<u32>;\n"
        "@group(0) @binding(2) var<storage, read_write> b: array<u32>;\n"
        "@group(0) @binding(3) var t: texture_2d<f32>;\n"
        "@group(0) @binding(4) var s: sampler;\n"
        "@group(0) @binding(5) var o: texture_storage_2d<rgba8unorm, write>;\n"));
    texs[0].storage = 0;
    texs[1].storage = 1;
    texs[1].format = HL_GPU_TEX_RGBA8;
    EXPECT_EQ(0, hl_gpu_bindings_check(&a35_pipe, NULL, 1, bufs, 2, texs, 2, &err));
    /* Without the uniforms everything shifts down one: mismatched kinds. */
    EXPECT_EQ(-1, hl_gpu_bindings_check(&a35_pipe, NULL, 0, bufs, 2, texs, 2, &err));
    /* A third buffer lands on the texture's binding. */
    EXPECT_EQ(-1, hl_gpu_bindings_check(&a35_pipe, NULL, 1, bufs, 3, texs, 2, &err));
    /* The storage texture's format must be the declared one. */
    texs[1].format = HL_GPU_TEX_R32F;
    EXPECT_EQ(-1, hl_gpu_bindings_check(&a35_pipe, NULL, 1, bufs, 2, texs, 2, &err));
    texs[1].format = HL_GPU_TEX_RGBA8;
    /* Storage / sampled swapped. */
    texs[0].storage = 1;
    texs[1].storage = 0;
    EXPECT_EQ(-1, hl_gpu_bindings_check(&a35_pipe, NULL, 1, bufs, 2, texs, 2, &err));

    /* An alias-typed handle accepts a texture or a sampler, never a buffer. */
    ASSERT_EQ(0, a35_layout(
        "alias T = texture_2d<f32>; alias S = sampler;\n"
        "@group(0) @binding(0) var t: T;\n@group(0) @binding(1) var s: S;\n"));
    texs[0].storage = 0;
    EXPECT_EQ(0, hl_gpu_bindings_check(&a35_pipe, NULL, 0, NULL, 0, texs, 1, &err));
    EXPECT_EQ(-1, hl_gpu_bindings_check(&a35_pipe, NULL, 0, bufs, 1, NULL, 0, &err));

    /* An unknown layout passes: the backend validates. */
    a35_pipe.layout_known = 0;
    EXPECT_EQ(0, hl_gpu_bindings_check(&a35_pipe, NULL, 0, bufs, 3, NULL, 0, &err));
}

/* Through hl_cap_gpu_dispatch / _pipeline: a mismatched call never reaches
 * the backend. */
UTEST(hull_cap_gpu_audit35, mismatch_refused_before_backend)
{
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    mock_add_shader(ctx, "one");
    HlGpuPipeline *p = &ctx->devices[0].pipelines[0];
    const char *wgsl = "@group(0) @binding(0) var<storage, read_write> d: array<u32>;\n"
                       "@compute @workgroup_size(1) fn main() { d[0] = 1u; }\n";
    ASSERT_EQ(0, hl_gpu_wgsl_layout(wgsl, strlen(wgsl), p));
    mock_dispatches = 0;

    HlGpuBufferDesc bufs[2] = {
        { .size = 16, .binding = -1 }, { .size = 16, .binding = -1 },
    };
    HlGpuDispatchOpts o = { .buffers = bufs, .buffer_count = 2,
                            .output_buffer = -1, .output_texture = -1,
                            .device = -1 };
    const char *err = NULL;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH, hl_cap_gpu_dispatch(ctx, "one", &o, NULL, NULL, &err));
    EXPECT_STREQ("binding_mismatch", err);
    EXPECT_EQ(0, mock_dispatches);
    EXPECT_EQ((size_t)0, ctx->devices[0].live_bytes);

    HlGpuPipelineStage st[2] = {
        { .shader = "one", .buffers = bufs, .buffer_count = 1 },
        { .shader = "one", .buffers = bufs, .buffer_count = 2 },
    };
    HlGpuPipelineOpts po = { .stages = st, .stage_count = 2,
                             .output_count = HL_GPU_OUTPUT_NONE, .device = -1 };
    HlGpuPipelineResult res;
    err = NULL;
    EXPECT_EQ(HL_GPU_ERR_DISPATCH, hl_cap_gpu_pipeline(ctx, &po, &res, &err));
    EXPECT_STREQ("binding_mismatch", err);
    EXPECT_EQ(0, mock_dispatches);

    /* The matching calls go through. */
    o.buffer_count = 1;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_dispatch(ctx, "one", &o, NULL, NULL, &err));
    st[1].buffer_count = 1;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_pipeline(ctx, &po, &res, &err));
    EXPECT_EQ(2, mock_dispatches);
    mock_free(ctx);
}

UTEST_MAIN();
