/*
 * test_hull_cap_gpu.c - Tests for GPU compute capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#ifdef HL_ENABLE_GPU

#include "hull/cap/gpu.h"  /* error codes; HL_GPU_* limits come transitively from limits/gpu.h */
#include <stdio.h>
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
    int rc = hl_cap_gpu_compile(&ctx, 0, "test", "@compute fn main() {}", 21);
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
    /* The failing calls bind a texture the shader declares, so the
     * dispatch layer's binding check passes them (audit 35) and the
     * backend fails after the upload, on the texture lookup. */
    const char *wgsl_tex =
        "@group(0) @binding(0) var<storage, read_write> data: array<u32>;\n"
        "@group(0) @binding(1) var tex: texture_2d<f32>;\n"
        "@group(0) @binding(2) var smp: sampler;\n"
        "@compute @workgroup_size(1)\n"
        "fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
        "    let c = textureSampleLevel(tex, smp, vec2<f32>(0.0, 0.0), 0.0);\n"
        "    data[id.x] = data[id.x] * 2u + u32(c.x * 0.0);\n"
        "}\n";
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_compile(&ctx, -1, "double_tex", wgsl_tex,
                                            strlen(wgsl_tex)));

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
                  hl_cap_gpu_dispatch(&ctx, "double_tex", &bad, NULL, NULL, &err));
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
#include "hull/cap/gpu_helper.h"
#include "hull/shared/work_charge.h"

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

/* ── Audit 36: shaders naga would recurse over until the stack overflows ──
 *
 * A long chain of operators or accessors in one expression is parsed into a
 * tree as deep as the chain; naga's later passes walk it recursively, and a
 * few thousand terms overflowed the event loop's stack (SIGSEGV). The cap
 * layer refuses such a source before any wgpu call. */

/* A shader whose one expression is `head` followed by `n` copies of `rep`
 * and then `tail`. Freed by the caller. */
static char *a36_chain(const char *head, const char *rep, size_t n,
                       const char *tail)
{
    size_t hl = strlen(head), rl = strlen(rep), tl = strlen(tail);
    char *s = malloc(hl + rl * n + tl + 1);
    if (!s) return NULL;
    char *p = s;
    memcpy(p, head, hl); p += hl;
    for (size_t i = 0; i < n; i++) { memcpy(p, rep, rl); p += rl; }
    memcpy(p, tail, tl); p += tl;
    *p = '\0';
    return s;
}

static size_t a36_depth(const char *wgsl)
{
    return hl_gpu_wgsl_complexity(wgsl, strlen(wgsl));
}

#define A36_ADD_HEAD "@group(0) @binding(0) var<storage, read_write> o: array<f32>;\n" \
                     "@compute @workgroup_size(1) fn main() { var y = o[0]; o[0] = y"
#define A36_SWZ_HEAD "@group(0) @binding(0) var<storage, read_write> o: array<vec4f>;\n" \
                     "@compute @workgroup_size(1) fn main() { o[0] = o[1]"

static char *a36_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(1 << 16);
    size_t n = buf ? fread(buf, 1, (1 << 16) - 1, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = '\0';
    *len = n;
    return buf;
}

static const char *const a36_examples[] = {
    "examples/gpu_search/shaders/cosine_similarity.wgsl",
    "examples/gpu_texture/shaders/invert.wgsl",
    "examples/gpu_pipeline/shaders/normalize.wgsl",
    "examples/gpu_pipeline/shaders/weight.wgsl",
    "examples/gpu_pipeline/shaders/reduce.wgsl",
    "examples/compute_gpu_chain/shaders/double.wgsl",
};

UTEST(hull_cap_gpu_audit36, ordinary_shaders_are_shallow)
{
    for (size_t k = 0; k < sizeof(a36_examples) / sizeof(a36_examples[0]); k++) {
        size_t len = 0;
        char *src = a36_read_file(a36_examples[k], &len);
        ASSERT_TRUE(src != NULL);
        size_t d = hl_gpu_wgsl_complexity(src, len);
        EXPECT_TRUE(d > 0);
        EXPECT_TRUE(d < 32);
        free(src);
    }
    EXPECT_EQ((size_t)0, hl_gpu_wgsl_complexity(NULL, 0));
    EXPECT_EQ((size_t)0, a36_depth(""));
    /* Every binary operator may be a level, the assignment included. */
    EXPECT_EQ((size_t)3, a36_depth("let x = a + b * c;"));   /* = + * */
    EXPECT_EQ((size_t)3, a36_depth("let x = -v.x;"));      /* bin '=' + '-' + '.' */
    /* Comments, literals with exponents and hex are single operands. */
    EXPECT_EQ((size_t)1, a36_depth("// a+b+c+d\n/* x /* - - */ */ let x = 1.5e-3;"));
    EXPECT_EQ((size_t)1, a36_depth("let x = 0x1p-4f;"));
}

UTEST(hull_cap_gpu_audit36, long_chains_are_deep)
{
    /* The crash forms: chained binary operators, swizzles, && terms. */
    char *add = a36_chain(A36_ADD_HEAD, "+y", 20000, "; }");
    char *swz = a36_chain(A36_SWZ_HEAD, ".wzyx", 20000, "; }");
    char *band = a36_chain(A36_ADD_HEAD, " && y == 1.0", 6000, "; }");
    char *cst = a36_chain("const c = 1", "+1", 20000,
                          ";\n@compute @workgroup_size(1) fn main() {}");
    ASSERT_TRUE(add && swz && band && cst);
    EXPECT_TRUE(a36_depth(add) > HL_GPU_WGSL_EXPR_DEPTH_MAX);
    EXPECT_TRUE(a36_depth(swz) > HL_GPU_WGSL_EXPR_DEPTH_MAX);
    EXPECT_TRUE(a36_depth(band) > HL_GPU_WGSL_EXPR_DEPTH_MAX);
    EXPECT_TRUE(a36_depth(cst) > HL_GPU_WGSL_EXPR_DEPTH_MAX);
    free(add); free(swz); free(band); free(cst);

    /* Nesting adds up: ten groups of 500 each stay deep, though no single
     * bracket level holds more than 500 operators. */
    char *nest = a36_chain("let x = ", "(", 10, "y");
    ASSERT_TRUE(nest != NULL);
    size_t nl = strlen(nest);
    char *full = malloc(nl + 10 * (500 * 2 + 1) + 2);
    ASSERT_TRUE(full != NULL);
    memcpy(full, nest, nl);
    char *p = full + nl;
    for (int g = 0; g < 10; g++) {
        for (int k = 0; k < 500; k++) { *p++ = '+'; *p++ = 'y'; }
        *p++ = ')';
    }
    *p++ = ';';
    *p = '\0';
    EXPECT_TRUE(a36_depth(full) > HL_GPU_WGSL_EXPR_DEPTH_MAX);
    free(nest); free(full);

    /* So does nesting alone. */
    char *paren = a36_chain("let x = ", "(", 5000, "1;");
    ASSERT_TRUE(paren != NULL);
    EXPECT_TRUE(a36_depth(paren) > HL_GPU_WGSL_EXPR_DEPTH_MAX);
    free(paren);
}

UTEST(hull_cap_gpu_audit36, wide_but_shallow_is_fine)
{
    /* A 5000-element constant table: commas restart the count. */
    char *lut = a36_chain("const lut = array<f32, 5000>(-1.0", ", -1.0", 4999,
                          ");\n@compute @workgroup_size(1) fn main() {}");
    /* An unrolled 256-term dot product: every '+' and '*' counts (~513),
     * still well under the limit. */
    char *dot = a36_chain(A36_ADD_HEAD, " + o[1] * o[2]", 256, "; }");
    /* Statements reset the count: 5000 short statements. */
    char *many = a36_chain("@compute @workgroup_size(1) fn main() { var y = 1.0; ",
                           "y = y + y * y; ", 5000, "}");
    ASSERT_TRUE(lut && dot && many);
    EXPECT_TRUE(a36_depth(lut) < 16);
    EXPECT_TRUE(a36_depth(dot) < 600);
    EXPECT_TRUE(a36_depth(many) < 16);
    free(lut); free(dot); free(many);
}

/* Through hl_cap_gpu_compile: refused before the backend is called. */
static int mock_compiles;
static int mock_compile(HlGpuDevice *dev, const char *name, const char *wgsl,
                        size_t wgsl_len, HlGpuPipeline *out)
{
    (void)dev; (void)name; (void)wgsl; (void)wgsl_len; (void)out;
    mock_compiles++;
    return HL_GPU_OK;
}

UTEST(hull_cap_gpu_audit36, compile_refuses_before_backend)
{
    HlGpuBackend be = MOCK_BACKEND;
    be.compile = mock_compile;
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    ctx->backend = &be;
    mock_compiles = 0;

    char *add = a36_chain(A36_ADD_HEAD, "+y", 20000, "; }");
    ASSERT_TRUE(add != NULL);
    EXPECT_EQ(HL_GPU_ERR_SHADER, hl_cap_gpu_compile(ctx, -1, "deep", add, strlen(add)));
    free(add);

    /* Larger than HL_GPU_WGSL_MAX_BYTES: refused, however shallow. */
    size_t big = HL_GPU_WGSL_MAX_BYTES + 1;
    char *src = malloc(big + 1);
    ASSERT_TRUE(src != NULL);
    memset(src, ' ', big);
    src[big] = '\0';
    EXPECT_EQ(HL_GPU_ERR_SHADER, hl_cap_gpu_compile(ctx, -1, "big", src, big));
    free(src);
    EXPECT_EQ(0, mock_compiles);
    EXPECT_EQ(0, ctx->devices[0].pipeline_count);

    const char *ok = "@compute @workgroup_size(1) fn main() { let x = 1 + 2; }";
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "ok", ok, strlen(ok)));
    EXPECT_EQ(1, mock_compiles);
    mock_free(ctx);
}

#ifdef HL_ENABLE_GPU
/* On a real device (skipped without an adapter): the source that crashed
 * the process is refused, and the process goes on to compile the examples'
 * shaders. */
UTEST(hull_cap_gpu_audit36, real_device_survives)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }

    char *add = a36_chain(A36_ADD_HEAD, "+y", 20000, "; }");
    char *swz = a36_chain(A36_SWZ_HEAD, ".wzyx", 20000, "; }");
    ASSERT_TRUE(add && swz);
    EXPECT_EQ(HL_GPU_ERR_SHADER, hl_cap_gpu_compile(&ctx, -1, "add", add, strlen(add)));
    EXPECT_EQ(HL_GPU_ERR_SHADER, hl_cap_gpu_compile(&ctx, -1, "swz", swz, strlen(swz)));
    free(add); free(swz);

    for (size_t k = 0; k < sizeof(a36_examples) / sizeof(a36_examples[0]); k++) {
        size_t len = 0;
        char *src = a36_read_file(a36_examples[k], &len);
        ASSERT_TRUE(src != NULL);
        char name[16];
        snprintf(name, sizeof(name), "ex%zu", k);
        EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(&ctx, -1, name, src, len));
        free(src);
    }
    hl_cap_gpu_destroy(&ctx);
}
#endif /* HL_ENABLE_GPU */

/* ── Audit 37: the pre-checks read the source exactly as naga does ────
 *
 * WGSL ends a line comment at VT, FF, NEL, LS and PS as well as LF / CR.
 * A reader that ended it only at LF / CR skipped the rest of such a line
 * while naga parsed it, so a deep chain hidden there crashed the process.
 * Also: `else if` chains nest without braces (block depth), and alias /
 * struct chains nest types across declarations (type depth). */

static const char *const a37_terms[] = {
    "\v", "\f", "\xC2\x85", "\xE2\x80\xA8", "\xE2\x80\xA9",
};

#define A37_N (sizeof(a37_terms) / sizeof(a37_terms[0]))

/* The audit-36 crash chain, hidden from a reader that does not end a line
 * comment at @p term. */
static char *a37_hidden(const char *term, size_t terms)
{
    char head[512];
    snprintf(head, sizeof(head),
             "@group(0) @binding(0) var<storage, read_write> o: array<f32>;\n"
             "@compute @workgroup_size(1) fn main() { var y = o[0]; "
             "// hidden%s o[0] = y", term);
    return a36_chain(head, " + y", terms, "; }");
}

/* An `if` followed by @p links `else if` links, in main(). */
static char *a37_elseif(size_t links)
{
    return a36_chain("@group(0) @binding(0) var<storage, read_write> o: array<f32>;\n"
                     "@compute @workgroup_size(1) fn main() { "
                     "if (o[0] > 0.0) { o[0] = 1.0; }",
                     " else if (o[0] > 0.0) { o[0] = 1.0; }", links, " }");
}

/* `alias A0 = f32; alias A1 = array<A0, 1>; ...` up to A<links>, in
 * declaration order or reversed. */
static char *a37_aliases(size_t links, int reversed)
{
    size_t cap = 64 * (links + 4);
    char *s = malloc(cap);
    if (!s) return NULL;
    size_t off = 0;
    if (!reversed) off += (size_t)snprintf(s + off, cap - off, "alias A0 = f32;\n");
    for (size_t k = 1; k <= links; k++) {
        size_t i = reversed ? links + 1 - k : k;
        off += (size_t)snprintf(s + off, cap - off,
                                "alias A%zu = array<A%zu, 1>;\n", i, i - 1);
    }
    if (reversed) off += (size_t)snprintf(s + off, cap - off, "alias A0 = f32;\n");
    snprintf(s + off, cap - off,
             "@compute @workgroup_size(1) fn main() { var x: A%zu; }\n", links);
    return s;
}

/* Valid WGSL using every blankspace character, comments ended by each line
 * break, and a non-ASCII identifier. */
static const char a37_blank_src[] =
    "@group(0) @binding(0) var<storage, read_write> o: array<f32>;\n"
    "@compute @workgroup_size(1) fn main() { "
    "var\xC2\x85y\xE2\x80\x8E=\xE2\x80\x8Fo[0];\v\f"
    "// c1\v o[0] = y;// c2\f o[0] = y;// c3\xC2\x85 o[0] = y;"
    "// c4\xE2\x80\xA8 o[0] = y;// c5\xE2\x80\xA9 o[0] = y;\r"
    "/* \"quoted\" #x $y ? \\ ` ' */ let \xC3\xA9t\xC3\xA9 = y; "
    "o[0] = \xC3\xA9t\xC3\xA9;\xE2\x80\xA8\xE2\x80\xA9 }";

static int a37_check(const char *wgsl)
{
    const char *why = NULL;
    return hl_gpu_wgsl_precheck(wgsl, strlen(wgsl), &why);
}

UTEST(hull_cap_gpu_audit37, line_comment_terminators_hide_nothing)
{
    for (size_t k = 0; k < A37_N; k++) {
        char *src = a37_hidden(a37_terms[k], 20000);
        ASSERT_TRUE(src != NULL);
        EXPECT_TRUE(a36_depth(src) > HL_GPU_WGSL_EXPR_DEPTH_MAX);
        const char *why = NULL;
        EXPECT_EQ(-1, hl_gpu_wgsl_precheck(src, strlen(src), &why));
        EXPECT_TRUE(why != NULL);
        free(src);
    }
    /* A comment ends at the terminator: what follows is code, counted. */
    for (size_t k = 0; k < A37_N; k++) {
        char src[128];
        snprintf(src, sizeof(src), "// c%slet x = a + b * c;", a37_terms[k]);
        EXPECT_EQ((size_t)3, a36_depth(src));
    }
    /* ...but U+200E / U+200F are blankspace, not line breaks. */
    EXPECT_EQ((size_t)0, a36_depth("// c\xE2\x80\x8E let x = a + b * c;"));
    EXPECT_EQ((size_t)0, a36_depth("// c\xE2\x80\x8F let x = a + b * c;"));
}

UTEST(hull_cap_gpu_audit37, layout_sees_past_every_terminator)
{
    for (size_t k = 0; k < A37_N; k++) {
        char src[256];
        snprintf(src, sizeof(src),
                 "@group(0) @binding(0) var<storage> a: array<u32>; // c%s"
                 "@group(0) @binding(1) var<storage> b: array<u32>;\n",
                 a37_terms[k]);
        ASSERT_EQ(0, a35_layout(src));
        EXPECT_EQ(2, a35_pipe.decl_count);
        EXPECT_TRUE(a35_decl(1) != NULL);
    }
    /* Blankspace of every kind between the tokens, non-ASCII names. */
    ASSERT_EQ(0, a35_layout(
        "@group(0)\xC2\x85@binding(0)\xE2\x80\x8Evar<storage>\xE2\x80\x8F"
        "donn\xC3\xA9" "es\xE2\x80\xA8:\xE2\x80\xA9" "array<u32>;\v\f"
        "@group(0) @binding(1) var t\xC3\xA9x: texture_2d<f32>;\n"));
    EXPECT_EQ(2, a35_pipe.decl_count);
    ASSERT_TRUE(a35_decl(0) && a35_decl(1));
    EXPECT_EQ(HL_GPU_BIND_STORAGE, a35_decl(0)->kind);
    EXPECT_EQ(HL_GPU_BIND_TEXTURE, a35_decl(1)->kind);
}

UTEST(hull_cap_gpu_audit37, legitimate_blankspace_accepted)
{
    EXPECT_EQ(0, a37_check(a37_blank_src));
    EXPECT_TRUE(a36_depth(a37_blank_src) < 8);
    /* The examples' shader files too. */
    for (size_t k = 0; k < sizeof(a36_examples) / sizeof(a36_examples[0]); k++) {
        size_t len = 0;
        char *src = a36_read_file(a36_examples[k], &len);
        ASSERT_TRUE(src != NULL);
        const char *why = "unset";
        EXPECT_EQ(0, hl_gpu_wgsl_precheck(src, len, &why));
        EXPECT_TRUE(why == NULL);
        free(src);
    }
}

/* Every WGSL shader embedded in the gpu context doc (```wgsl fences)
 * passes. The example apps' shaders moved to shaders/ files (audit
 * convergence 7.3: a built binary compiles only those) and are checked
 * through a36_examples. */
static const char *const a37_hosts[] = {
    "stdlib/context/gpu.md",
};

UTEST(hull_cap_gpu_audit37, embedded_example_shaders_accepted)
{
    static const char *const opens[]  = { "[[\n", "`\n", "```wgsl\n" };
    static const char *const closes[] = { "\n]]", "\n`", "\n```" };
    size_t found = 0;
    for (size_t h = 0; h < sizeof(a37_hosts) / sizeof(a37_hosts[0]); h++) {
        size_t len = 0;
        char *src = a36_read_file(a37_hosts[h], &len);
        ASSERT_TRUE(src != NULL);
        for (size_t m = 0; m < 3; m++) {
            const char *p = src;
            while ((p = strstr(p, opens[m])) != NULL) {
                p += strlen(opens[m]);
                const char *e = strstr(p, closes[m]);
                if (!e) break;
                size_t n = (size_t)(e - p);
                char *body = malloc(n + 1);
                ASSERT_TRUE(body != NULL);
                memcpy(body, p, n);
                body[n] = '\0';
                if (strstr(body, "fn ") && (strstr(body, "@compute") ||
                                            strstr(body, "@group"))) {
                    const char *why = NULL;
                    EXPECT_EQ(0, hl_gpu_wgsl_precheck(body, n, &why));
                    if (why) printf("%s: %s\n", a37_hosts[h], why);
                    found++;
                }
                free(body);
                p = e + strlen(closes[m]);
            }
        }
        free(src);
    }
    EXPECT_TRUE(found >= 1);
}

UTEST(hull_cap_gpu_audit37, unclassifiable_bytes_refused)
{
    static const struct { const char *s; size_t len; } bad[] = {
#define A37_S(x) { x, sizeof(x) - 1 }
        A37_S("let x = \"s\";"), A37_S("let x = 'c';"), A37_S("#define X 1"),
        A37_S("let $x = 1;"), A37_S("let x = a ? b : c;"),
        A37_S("let x = a \\ b;"), A37_S("let x = `t`;"),
        A37_S("let x = 1;\x01"), A37_S("let x = 1;\x7f"),
        A37_S("let x\0 = 1;"),                      /* NUL outside a comment */
        A37_S("let x = 1; \xFF"), A37_S("let \xC0\x80 = 1;"),
        A37_S("let \xED\xA0\x80 = 1;"), A37_S("let x = 1; \xE2\x80"),
        A37_S("// \xFF in a comment\n"),            /* not UTF-8 at all */
#undef A37_S
    };
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
        EXPECT_EQ(HL_GPU_WGSL_UNREADABLE, hl_gpu_wgsl_complexity(bad[k].s, bad[k].len));
        EXPECT_EQ(HL_GPU_WGSL_UNREADABLE, hl_gpu_wgsl_block_depth(bad[k].s, bad[k].len));
        EXPECT_EQ(HL_GPU_WGSL_UNREADABLE, hl_gpu_wgsl_type_depth(bad[k].s, bad[k].len));
        const char *why = NULL;
        EXPECT_EQ(-1, hl_gpu_wgsl_precheck(bad[k].s, bad[k].len, &why));
    }
    /* Those characters inside a comment are only comment text. */
    EXPECT_EQ((size_t)0, a36_depth("// \"q\" 'c' #x $y ? \\ ` \x01\n/* \x7f */"));
}

UTEST(hull_cap_gpu_audit37, else_if_chains_nest)
{
    /* fn body 1, if body 2, each link one deeper. */
    char *c500 = a37_elseif(500);
    char *c5500 = a37_elseif(5500);
    ASSERT_TRUE(c500 && c5500);
    EXPECT_EQ((size_t)502, hl_gpu_wgsl_block_depth(c500, strlen(c500)));
    EXPECT_TRUE(hl_gpu_wgsl_block_depth(c5500, strlen(c5500)) >
                HL_GPU_WGSL_BLOCK_DEPTH_MAX);
    /* The expression estimate never saw it. */
    EXPECT_TRUE(a36_depth(c5500) < 16);
    /* At the depth bound c500 is past the function size bounds
     * (audit 40); a chain as deep in fewer words passes. */
    EXPECT_EQ(-1, a37_check(c500));
    EXPECT_EQ(-1, a37_check(c5500));
    free(c500); free(c5500);
    char *small = a36_chain("@compute @workgroup_size(1) fn main() { if (true) {}",
                            " else if (true) {}", 500, " }");
    ASSERT_TRUE(small != NULL);
    EXPECT_EQ((size_t)502, hl_gpu_wgsl_block_depth(small, strlen(small)));
    EXPECT_EQ(0, a37_check(small));
    free(small);

    EXPECT_EQ((size_t)0, hl_gpu_wgsl_block_depth(NULL, 0));
#define A37_BD(s) hl_gpu_wgsl_block_depth(s, strlen(s))
    EXPECT_EQ((size_t)1, A37_BD("fn f() {}"));
    EXPECT_EQ((size_t)3, A37_BD("fn f() { { {} } }"));
    /* else: the if's level; else if: one deeper; comments in between. */
    EXPECT_EQ((size_t)2, A37_BD("fn f() { if a {} else {} }"));
    EXPECT_EQ((size_t)3, A37_BD("fn f() { if a {} /* c */ else if b {} }"));
    EXPECT_EQ((size_t)4, A37_BD("fn f() { if a {} else if b {} else if c {} else {} }"));
    /* A chain inside nested blocks adds to them. */
    EXPECT_EQ((size_t)5, A37_BD("fn f() { loop { if a {} else if b {} else if c {} } }"));
    /* Separate statements do not. */
    EXPECT_EQ((size_t)2, A37_BD("fn f() { if a {} if b {} if c {} }"));
#undef A37_BD
}

UTEST(hull_cap_gpu_audit37, type_chains_nest)
{
#define A37_TD(s) hl_gpu_wgsl_type_depth(s, strlen(s))
    EXPECT_EQ((size_t)0, hl_gpu_wgsl_type_depth(NULL, 0));
    EXPECT_EQ((size_t)0, A37_TD("fn main() {}"));
    EXPECT_EQ((size_t)1, A37_TD("alias F = f32;"));
    EXPECT_EQ((size_t)3, A37_TD("alias M = array<array<f32, 4>, 4>;"));
    EXPECT_EQ((size_t)2, A37_TD("struct S { a: f32, @align(16) b: u32 }"));
    /* A member named like a type is not a reference to it. */
    EXPECT_EQ((size_t)2, A37_TD("struct S { S: f32 }"));
    /* A comparison inside a size expression does not close a template. */
    EXPECT_EQ((size_t)2, A37_TD("alias V = array<f32, (2 > 1)>;"));
    EXPECT_TRUE(A37_TD("struct L { c: vec3<f32> } "
                       "struct Scene { l: array<L, 4>, n: u32 }") < 8);
    /* Mutual recursion (WGSL refuses it) counts as too deep. */
    EXPECT_TRUE(A37_TD("struct A { b: B } struct B { a: A }") >
                HL_GPU_WGSL_TYPE_DEPTH_MAX);

    /* A<k> nests k + 1 levels, in either declaration order. */
    for (int rev = 0; rev <= 1; rev++) {
        char *ok = a37_aliases(HL_GPU_WGSL_TYPE_DEPTH_MAX - 1, rev);
        char *deep = a37_aliases(HL_GPU_WGSL_TYPE_DEPTH_MAX, rev);
        char *huge = a37_aliases(5000, rev);
        ASSERT_TRUE(ok && deep && huge);
        EXPECT_EQ((size_t)HL_GPU_WGSL_TYPE_DEPTH_MAX, A37_TD(ok));
        EXPECT_EQ(0, a37_check(ok));
        EXPECT_TRUE(A37_TD(deep) > HL_GPU_WGSL_TYPE_DEPTH_MAX);
        EXPECT_EQ(-1, a37_check(deep));
        EXPECT_EQ(-1, a37_check(huge));
        free(ok); free(deep); free(huge);
    }
    /* A struct chain likewise. */
    char st[8192];
    size_t off = (size_t)snprintf(st, sizeof(st), "struct S0 { v: f32 }\n");
    for (int k = 1; k <= 100; k++)
        off += (size_t)snprintf(st + off, sizeof(st) - off,
                                "struct S%d { v: S%d }\n", k, k - 1);
    EXPECT_TRUE(A37_TD(st) > HL_GPU_WGSL_TYPE_DEPTH_MAX);
#undef A37_TD
}

UTEST(hull_cap_gpu_audit37, compile_refuses_before_backend)
{
    HlGpuBackend be = MOCK_BACKEND;
    be.compile = mock_compile;
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    ctx->backend = &be;
    mock_compiles = 0;

    char *srcs[A37_N + 2];
    for (size_t k = 0; k < A37_N; k++) srcs[k] = a37_hidden(a37_terms[k], 20000);
    srcs[A37_N] = a37_elseif(5500);
    srcs[A37_N + 1] = a37_aliases(2000, 1);
    for (size_t k = 0; k < A37_N + 2; k++) {
        ASSERT_TRUE(srcs[k] != NULL);
        EXPECT_EQ(HL_GPU_ERR_SHADER,
                  hl_cap_gpu_compile(ctx, -1, "bad", srcs[k], strlen(srcs[k])));
        free(srcs[k]);
    }
    const char *quote = "fn main() { let x = \"s\"; }";
    EXPECT_EQ(HL_GPU_ERR_SHADER, hl_cap_gpu_compile(ctx, -1, "q", quote, strlen(quote)));
    EXPECT_EQ(0, mock_compiles);

    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "blank", a37_blank_src,
                                            strlen(a37_blank_src)));
    EXPECT_EQ(1, mock_compiles);
    mock_free(ctx);
}

#ifdef HL_ENABLE_GPU
/* On a real device (skipped without an adapter): each source that crashed
 * naga or held the loop is refused and the process survives; the valid
 * blankspace shader and a type chain at the limit still compile. */
UTEST(hull_cap_gpu_audit37, real_device_survives)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }

    char *srcs[A37_N + 2];
    for (size_t k = 0; k < A37_N; k++) srcs[k] = a37_hidden(a37_terms[k], 20000);
    srcs[A37_N] = a37_elseif(5500);
    srcs[A37_N + 1] = a37_aliases(2000, 0);
    for (size_t k = 0; k < A37_N + 2; k++) {
        ASSERT_TRUE(srcs[k] != NULL);
        char name[16];
        snprintf(name, sizeof(name), "bad%zu", k);
        EXPECT_EQ(HL_GPU_ERR_SHADER,
                  hl_cap_gpu_compile(&ctx, -1, name, srcs[k], strlen(srcs[k])));
        free(srcs[k]);
    }
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(&ctx, -1, "blank", a37_blank_src,
                                            strlen(a37_blank_src)));
    char *ok = a37_aliases(HL_GPU_WGSL_TYPE_DEPTH_MAX - 1, 1);
    ASSERT_TRUE(ok != NULL);
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(&ctx, -1, "types", ok, strlen(ok)));
    free(ok);
    hl_cap_gpu_destroy(&ctx);
}
#endif /* HL_ENABLE_GPU */

/* ── audit 38: declaration reference chains ───────────────────────── */

enum { A38_CONST, A38_OVERRIDE, A38_FN, A38_N };

/* @p links declarations of one kind, each naming the one before (in
 * declaration order or reversed), and an entry point naming the last:
 * a chain of links + 1 declarations. Compact, so 50000 links fit the
 * 1 MiB source cap. */
static const char *a38_name(size_t i, char buf[16])
{
    static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char tmp[16];
    size_t n = 0;
    do { tmp[n++] = digits[i % 36]; i /= 36; } while (i > 0);
    buf[0] = 'C';
    for (size_t k = 0; k < n; k++) buf[1 + k] = tmp[n - 1 - k];
    buf[1 + n] = '\0';
    return buf;
}

static char *a38_chain(int kind, size_t links, int reversed)
{
    size_t cap = 40 * (links + 4) + 256;
    char *s = malloc(cap);
    if (!s) return NULL;
    size_t off = 0;
    char a[16], b[16];
    for (size_t k = 0; k < links; k++) {
        size_t i = reversed ? links - 1 - k : k;
        const char *fmt0, *fmt;
        switch (kind) {
        case A38_CONST:    fmt0 = "const C0=1u;";      fmt = "const %s=%s;"; break;
        case A38_OVERRIDE: fmt0 = "override C0=1u;";   fmt = "override %s=%s;"; break;
        default:           fmt0 = "fn C0(){}";         fmt = "fn %s(){%s();}"; break;
        }
        if (i == 0) off += (size_t)snprintf(s + off, cap - off, "%s", fmt0);
        else off += (size_t)snprintf(s + off, cap - off, fmt,
                                     a38_name(i, a), a38_name(i - 1, b));
    }
    if (kind == A38_FN)
        snprintf(s + off, cap - off,
                 "\n@compute @workgroup_size(1) fn main(){%s();}\n",
                 a38_name(links - 1, a));
    else
        snprintf(s + off, cap - off,
                 "\n@group(0) @binding(0) var<storage, read_write> o: array<u32>;\n"
                 "@compute @workgroup_size(1) fn main(){o[0]=%s;}\n",
                 a38_name(links - 1, a));
    return s;
}

static const size_t a38_crash_links[A38_N] = { 30000, 40000, 50000 };

UTEST(hull_cap_gpu_audit38, chains_counted)
{
#define A38_DC(s) hl_gpu_wgsl_decl_chain(s, strlen(s))
    EXPECT_EQ((size_t)0, hl_gpu_wgsl_decl_chain(NULL, 0));
    EXPECT_EQ((size_t)0, A38_DC("// nothing\n"));
    EXPECT_EQ((size_t)1, A38_DC("fn main() {}"));
    EXPECT_EQ((size_t)2, A38_DC("const a = 1; fn main() { let x = a; }"));
    EXPECT_EQ((size_t)3, A38_DC("fn main() { b(); } fn b() { c(); } fn c() {}"));
    /* Attribute arguments name what they use; attribute names do not. */
    EXPECT_EQ((size_t)2, A38_DC("override WG = 64u; "
                                "@compute @workgroup_size(WG) fn main() {}"));
    EXPECT_EQ((size_t)1, A38_DC("const compute = 1; @compute @workgroup_size(1) fn main() {}"));
    /* Types, and var / let / alias / struct / const_assert. */
    EXPECT_EQ((size_t)4, A38_DC("const N = 4u; alias V = array<f32, N>; "
                                "struct S { v: V } var<private> g: S;"));
    EXPECT_EQ((size_t)2, A38_DC("const N = 4; const_assert N > 1;"));
    EXPECT_EQ((size_t)1, A38_DC("var<storage> storage: f32;"));   /* template */
    /* Member names are not references. */
    EXPECT_EQ((size_t)2, A38_DC("struct S { a: f32 } fn a() -> f32 { var s: S; return s.a; }"));
    /* Shadowing: a parameter, a let in a block (to its block's end), a for
     * header's var (to the loop's end), and an initializer seeing the
     * outer name. None of these is a cycle. */
    EXPECT_EQ((size_t)2, A38_DC("fn a() { let b = 1; } fn b() { a(); }"));
    EXPECT_EQ((size_t)2, A38_DC("fn a(b: u32) -> u32 { return b; } fn b() -> u32 { return a(1u); }"));
    EXPECT_EQ((size_t)2, A38_DC("fn b() {} fn a() { { let b = 1; } b(); }"));
    EXPECT_EQ((size_t)2, A38_DC("fn b() {} fn a() { for (var b = 0; b < 1; b++) { } b(); }"));
    EXPECT_EQ((size_t)2, A38_DC("const x = 1; fn a() { let x = x + 1; }"));
    EXPECT_EQ((size_t)1, A38_DC("const b = 1; fn a() { var<function> b = 0; { let y = b; } }"));
    /* Cycles (WGSL refuses them) count as too long. */
    EXPECT_TRUE(A38_DC("const a = b; const b = a;") > HL_GPU_WGSL_DECL_CHAIN_MAX);
    EXPECT_TRUE(A38_DC("fn a() { a(); }") > HL_GPU_WGSL_DECL_CHAIN_MAX);
    EXPECT_TRUE(A38_DC("struct A { b: B } struct B { a: A }") > HL_GPU_WGSL_DECL_CHAIN_MAX);
    /* The lexer's refusals carry over. */
    EXPECT_EQ(HL_GPU_WGSL_UNREADABLE, A38_DC("const a = \"s\";"));
#undef A38_DC
}

UTEST(hull_cap_gpu_audit38, chains_at_the_bound)
{
    for (int kind = 0; kind < A38_N; kind++) {
        for (int rev = 0; rev <= 1; rev++) {
            /* links + 1 declarations with the entry point. */
            char *ok = a38_chain(kind, HL_GPU_WGSL_DECL_CHAIN_MAX - 1, rev);
            char *over = a38_chain(kind, HL_GPU_WGSL_DECL_CHAIN_MAX, rev);
            ASSERT_TRUE(ok && over);
            EXPECT_EQ((size_t)HL_GPU_WGSL_DECL_CHAIN_MAX,
                      hl_gpu_wgsl_decl_chain(ok, strlen(ok)));
            EXPECT_EQ((size_t)HL_GPU_WGSL_DECL_CHAIN_MAX + 1,
                      hl_gpu_wgsl_decl_chain(over, strlen(over)));
            const char *why = NULL;
            if (kind == A38_FN) {
                /* 2048 functions are past the module size bound (audit 40). */
                EXPECT_EQ(-1, hl_gpu_wgsl_precheck(ok, strlen(ok), &why));
                EXPECT_TRUE(why && strstr(why, "too large"));
            } else {
                EXPECT_EQ(0, a37_check(ok));
            }
            EXPECT_EQ(-1, hl_gpu_wgsl_precheck(over, strlen(over), &why));
            EXPECT_TRUE(why && strstr(why, "declarations"));
            /* Every other estimate stays shallow. */
            EXPECT_TRUE(hl_gpu_wgsl_complexity(over, strlen(over)) < 16);
            EXPECT_TRUE(hl_gpu_wgsl_block_depth(over, strlen(over)) < 4);
            EXPECT_TRUE(hl_gpu_wgsl_type_depth(over, strlen(over)) < 4);
            free(ok); free(over);
        }
        char *crash = a38_chain(kind, a38_crash_links[kind], 1);
        ASSERT_TRUE(crash != NULL);
        EXPECT_TRUE(strlen(crash) <= HL_GPU_WGSL_MAX_BYTES);
        EXPECT_EQ(-1, a37_check(crash));
        free(crash);
    }
}

UTEST(hull_cap_gpu_audit38, compile_refuses_before_backend)
{
    HlGpuBackend be = MOCK_BACKEND;
    be.compile = mock_compile;
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    ctx->backend = &be;
    mock_compiles = 0;
    for (int kind = 0; kind < A38_N; kind++) {
        char *src = a38_chain(kind, HL_GPU_WGSL_DECL_CHAIN_MAX, 0);
        ASSERT_TRUE(src != NULL);
        EXPECT_EQ(HL_GPU_ERR_SHADER,
                  hl_cap_gpu_compile(ctx, -1, "bad", src, strlen(src)));
        free(src);
    }
    EXPECT_EQ(0, mock_compiles);
    /* A const chain: a function chain this long is past the module size
     * bound (audit 40). */
    char *ok = a38_chain(A38_CONST, HL_GPU_WGSL_DECL_CHAIN_MAX - 1, 1);
    ASSERT_TRUE(ok != NULL);
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "ok", ok, strlen(ok)));
    EXPECT_EQ(1, mock_compiles);
    free(ok);
    mock_free(ctx);
}

#ifdef HL_ENABLE_GPU
/* On a real device (skipped without an adapter): the chains that crashed
 * naga are refused and the process survives; chains at the bound compile. */
UTEST(hull_cap_gpu_audit38, real_device_survives)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }
    for (int kind = 0; kind < A38_N; kind++) {
        char name[16];
        char *crash = a38_chain(kind, a38_crash_links[kind], 1);
        char *over = a38_chain(kind, HL_GPU_WGSL_DECL_CHAIN_MAX, 1);
        char *ok = a38_chain(kind, HL_GPU_WGSL_DECL_CHAIN_MAX - 1, kind == A38_FN);
        ASSERT_TRUE(crash && over && ok);
        snprintf(name, sizeof(name), "crash%d", kind);
        EXPECT_EQ(HL_GPU_ERR_SHADER,
                  hl_cap_gpu_compile(&ctx, -1, name, crash, strlen(crash)));
        snprintf(name, sizeof(name), "over%d", kind);
        EXPECT_EQ(HL_GPU_ERR_SHADER,
                  hl_cap_gpu_compile(&ctx, -1, name, over, strlen(over)));
        snprintf(name, sizeof(name), "ok%d", kind);
        /* A function chain at the bound is past the module size bound
         * (audit 40). */
        EXPECT_EQ(kind == A38_FN ? HL_GPU_ERR_SHADER : HL_GPU_OK,
                  hl_cap_gpu_compile(&ctx, -1, name, ok, strlen(ok)));
        free(crash); free(over); free(ok);
    }
    hl_cap_gpu_destroy(&ctx);
}
#endif /* HL_ENABLE_GPU */

/* ── Audit 39: inferred nesting, expansion, the compile thread ────── */

static HlGpuWgslInferred a39_inf;

static int a39_infer(const char *wgsl)
{
    return hl_gpu_wgsl_inferred(wgsl, strlen(wgsl), &a39_inf);
}

/* `links` declarations of @p kind, each wrapping the one before:
 *   A39_LOCAL_CONST / A39_LOCAL_LET: `const aK = array(aK-1);` in main
 *   A39_MOD_CONST:                   the same at module scope
 *   A39_EXP_CONST / A39_EXP_LOCAL:   `const aK = array(aK-1, aK-1);`
 *   A39_LET_CHAIN:                   `let aK = aK-1;` in main */
enum { A39_LOCAL_CONST, A39_LOCAL_LET, A39_MOD_CONST, A39_EXP_CONST,
       A39_EXP_LOCAL, A39_LET_CHAIN };

static char *a39_shape(int kind, size_t links)
{
    size_t cap = 48 * (links + 4) + 256;
    char *s = malloc(cap);
    if (!s) return NULL;
    int local = kind != A39_MOD_CONST && kind != A39_EXP_CONST;
    const char *kw = (kind == A39_LOCAL_LET || kind == A39_LET_CHAIN) ? "let" : "const";
    size_t off = 0;
    off += (size_t)snprintf(s + off, cap - off, "%s%s a0 = 1u;\n",
                            local ? "@compute @workgroup_size(1) fn main() {\n" : "",
                            kw);
    for (size_t k = 1; k <= links; k++) {
        if (kind == A39_LET_CHAIN)
            off += (size_t)snprintf(s + off, cap - off, "%s a%zu = a%zu;\n",
                                    kw, k, k - 1);
        else if (kind == A39_EXP_CONST || kind == A39_EXP_LOCAL)
            off += (size_t)snprintf(s + off, cap - off,
                                    "%s a%zu = array(a%zu, a%zu);\n",
                                    kw, k, k - 1, k - 1);
        else
            off += (size_t)snprintf(s + off, cap - off, "%s a%zu = array(a%zu);\n",
                                    kw, k, k - 1);
    }
    snprintf(s + off, cap - off, "%s_ = a%zu;\n}\n",
             local ? "" : "@compute @workgroup_size(1) fn main() {\n", links);
    return s;
}

UTEST(hull_cap_gpu_audit39, inferred_depth)
{
    memset(&a39_inf, 0xff, sizeof(a39_inf));
    EXPECT_EQ(0, hl_gpu_wgsl_inferred(NULL, 0, &a39_inf));
    EXPECT_EQ((size_t)0, a39_inf.depth);
    EXPECT_EQ(-1, hl_gpu_wgsl_inferred("x", 1, NULL));
    ASSERT_EQ(0, a39_infer("const a = 1;"));
    EXPECT_EQ((size_t)0, a39_inf.depth);
    /* Constructor calls and template lists nest; a call does not. */
    ASSERT_EQ(0, a39_infer("const a = array(array(1));"));
    EXPECT_EQ((size_t)2, a39_inf.depth);
    ASSERT_EQ(0, a39_infer("const a = array<array<f32, 2>, 1>(array<f32, 2>(1., 2.));"));
    EXPECT_EQ((size_t)2, a39_inf.depth);
    ASSERT_EQ(0, a39_infer("const v = vec4f(1.); const m = mat2x2f(1., 2., 3., 4.);"));
    EXPECT_EQ((size_t)1, a39_inf.depth);
    ASSERT_EQ(0, a39_infer("fn f(x: f32) -> f32 { return x; } "
                           "fn main() { let a = f(f(f(1.))); let b = sqrt(max(a, 1.)); }"));
    EXPECT_EQ((size_t)0, a39_inf.depth);
    /* Through declarations, by inference: module scope, const and let. */
    for (int kind = A39_LOCAL_CONST; kind <= A39_MOD_CONST; kind++) {
        char *s = a39_shape(kind, 10);
        ASSERT_TRUE(s != NULL);
        ASSERT_EQ(0, a39_infer(s));
        EXPECT_EQ((size_t)10, a39_inf.depth);
        free(s);
    }
    /* An accessor takes a level off; a struct is one over its members. */
    ASSERT_EQ(0, a39_infer("const a = array(array(1)); const b = array(a[0]); "
                           "const c = array(a[0][0]);"));
    EXPECT_EQ((size_t)2, a39_inf.depth);
    ASSERT_EQ(0, a39_infer("struct S { x: f32 } struct T { s: S } "
                           "const t = T(S(1.)); const u = array(t);"));
    EXPECT_EQ((size_t)3, a39_inf.depth);
    ASSERT_EQ(0, a39_infer("alias A = array<f32, 2>; fn main() { var x: A; "
                           "let y = array(x); }"));
    EXPECT_EQ((size_t)2, a39_inf.depth);
    /* Parameters and return types carry their types' depth. */
    ASSERT_EQ(0, a39_infer("alias A = array<array<f32, 2>, 2>; "
                           "fn f(p: A) -> A { let q = array(p); return p; } "
                           "fn main() { let r = array(f(A())); }"));
    EXPECT_EQ((size_t)3, a39_inf.depth);
    /* Shadowing: a local hides a module name to the end of its block. */
    ASSERT_EQ(0, a39_infer("const a = array(array(array(1))); "
                           "fn main() { let a = 1; let b = array(a); }"));
    EXPECT_EQ((size_t)3, a39_inf.depth);
    ASSERT_EQ(0, a39_infer("const a = array(array(array(1))); "
                           "fn main() { { let a = 1; } let b = array(a); }"));
    EXPECT_EQ((size_t)4, a39_inf.depth);
    ASSERT_EQ(0, a39_infer("const a = array(array(array(1))); "
                           "fn main() { let a = array(a); }"));   /* the outer one */
    EXPECT_EQ((size_t)4, a39_inf.depth);
    ASSERT_EQ(0, a39_infer("const i = array(array(array(1))); fn main() { "
                           "for (var i = 0; i < 4; i++) { let b = array(i); } }"));
    EXPECT_EQ((size_t)3, a39_inf.depth);
    /* A cycle counts as too deep; unreadable bytes fail. */
    ASSERT_EQ(0, a39_infer("const a = array(b); const b = array(a);"));
    EXPECT_TRUE(a39_inf.depth > HL_GPU_WGSL_INFERRED_DEPTH_MAX);
    EXPECT_EQ(-1, a39_infer("const a = \"s\";"));
    EXPECT_TRUE(a39_inf.depth > HL_GPU_WGSL_INFERRED_DEPTH_MAX);
}

UTEST(hull_cap_gpu_audit39, inferred_expansion)
{
    /* No copies: literals, arithmetic operands, a LUT read by index. */
    ASSERT_EQ(0, a39_infer("const a = array(1, 2, 3, 4); const b = a[1] + a[2]; "
                           "const c = b * b;"));
    EXPECT_EQ((uint64_t)0, a39_inf.expanded);
    ASSERT_EQ(0, a39_infer("const T = array<u32, 4>(1u, 2u, 3u, 4u); "
                           "fn main() { let x = array(T, T); let y = T[0]; }"));
    EXPECT_EQ((uint64_t)0, a39_inf.expanded);   /* written by name */
    /* A local const copied as a whole argument. */
    ASSERT_EQ(0, a39_infer("fn main() { const a = array(1, 2, 3); "
                           "const b = array(a, a); }"));
    EXPECT_EQ((uint64_t)6, a39_inf.expanded);
    /* Doubling: aK = 2^(K+1) - 1 nodes, 3 written. */
    char *s = a39_shape(A39_EXP_CONST, 3);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ(0, a39_infer(s));
    EXPECT_EQ((uint64_t)(4 + 12), a39_inf.expanded);
    free(s);
    for (int kind = A39_EXP_CONST; kind <= A39_EXP_LOCAL; kind++) {
        s = a39_shape(kind, 10);
        ASSERT_TRUE(s != NULL);
        ASSERT_EQ(0, a39_infer(s));
        EXPECT_TRUE(a39_inf.expanded > 2000 && a39_inf.expanded < 5000);
        free(s);
        s = a39_shape(kind, 64);       /* saturates, no overflow */
        ASSERT_TRUE(s != NULL);
        ASSERT_EQ(0, a39_infer(s));
        EXPECT_TRUE(a39_inf.expanded > HL_GPU_WGSL_EXPAND_MAX);
        free(s);
    }
    /* The charge follows the expansion. */
    s = a39_shape(A39_EXP_CONST, 10);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ(0, a39_infer(s));
    HlGpuWgslSize a39_sz;
    ASSERT_EQ(0, hl_gpu_wgsl_size(s, strlen(s), &a39_sz));
    EXPECT_EQ(hl_work_bytes_units(strlen(s)) +
              a39_inf.expanded * HL_GPU_WGSL_EXPAND_UNITS +
              a39_sz.module_words * HL_GPU_WGSL_WORD_UNITS +
              a39_sz.module_stmts * HL_GPU_WGSL_STMT_UNITS +
              a39_sz.decls_sq * HL_GPU_WGSL_DECL_SQ_UNITS,
              hl_gpu_wgsl_compile_units(s, strlen(s)));
    free(s);
}

UTEST(hull_cap_gpu_audit39, local_chains)
{
    char *ok = a39_shape(A39_LET_CHAIN, HL_GPU_WGSL_DECL_CHAIN_MAX - 1);
    char *over = a39_shape(A39_LET_CHAIN, HL_GPU_WGSL_DECL_CHAIN_MAX);
    ASSERT_TRUE(ok && over);
    ASSERT_EQ(0, a39_infer(ok));
    EXPECT_EQ((size_t)HL_GPU_WGSL_DECL_CHAIN_MAX, a39_inf.chain);
    EXPECT_EQ((size_t)0, a39_inf.depth);
    /* 2048 locals are past the per-function size bounds (audit 40), which
     * now bound a local chain first. */
    const char *why0 = NULL;
    EXPECT_EQ(-1, hl_gpu_wgsl_precheck(ok, strlen(ok), &why0));
    EXPECT_TRUE(why0 && strstr(why0, "function"));
    char *fit = a39_shape(A39_LET_CHAIN, HL_GPU_WGSL_FN_DECLS_MAX - 1);
    ASSERT_TRUE(fit != NULL);
    EXPECT_EQ(0, a37_check(fit));
    free(fit);
    ASSERT_EQ(0, a39_infer(over));
    EXPECT_EQ((size_t)HL_GPU_WGSL_DECL_CHAIN_MAX + 1, a39_inf.chain);
    const char *why = NULL;
    EXPECT_EQ(-1, hl_gpu_wgsl_precheck(over, strlen(over), &why));
    EXPECT_TRUE(why && strstr(why, "function-local"));
    free(ok); free(over);
    /* Module-scope chains are hl_gpu_wgsl_decl_chain's: a chain at its
     * bound, read from main, still passes. */
    char *mod = a38_chain(A38_CONST, HL_GPU_WGSL_DECL_CHAIN_MAX - 1, 0);
    ASSERT_TRUE(mod != NULL);
    EXPECT_EQ(0, a37_check(mod));
    free(mod);
}

/* The round-39 shapes: refused before the backend sees them. */
static char *a39_round39(int which)
{
    switch (which) {
    case 0: return a39_shape(A39_LOCAL_CONST, 10000);   /* crashed naga */
    case 1: return a39_shape(A39_EXP_CONST, 16);        /* 6 s */
    case 2: return a39_shape(A39_EXP_CONST, 20);        /* 109 s */
    case 3: return a39_shape(A39_EXP_LOCAL, 18);
    default: return a39_shape(A39_LOCAL_LET, 10000);
    }
}

UTEST(hull_cap_gpu_audit39, round39_shapes_refused)
{
    for (int w = 0; w < 5; w++) {
        char *s = a39_round39(w);
        ASSERT_TRUE(s != NULL);
        EXPECT_TRUE(strlen(s) <= HL_GPU_WGSL_MAX_BYTES);
        const char *why = NULL;
        EXPECT_EQ(-1, hl_gpu_wgsl_precheck(s, strlen(s), &why));
        EXPECT_TRUE(why != NULL);
        /* ...and nothing older caught them. */
        EXPECT_TRUE(hl_gpu_wgsl_complexity(s, strlen(s)) < 16);
        EXPECT_TRUE(hl_gpu_wgsl_type_depth(s, strlen(s)) < 4);
        free(s);
    }
    /* Below the bounds they pass. */
    char *d = a39_shape(A39_LOCAL_CONST, HL_GPU_WGSL_INFERRED_DEPTH_MAX);
    char *d1 = a39_shape(A39_LOCAL_CONST, HL_GPU_WGSL_INFERRED_DEPTH_MAX + 1);
    char *e = a39_shape(A39_EXP_CONST, 8);
    ASSERT_TRUE(d && d1 && e);
    EXPECT_EQ(0, a37_check(d));
    EXPECT_EQ(-1, a37_check(d1));
    EXPECT_EQ(0, a37_check(e));
    free(d); free(d1); free(e);
}

static pthread_t a39_caller, a39_seen;
static int a39_compile_thread(HlGpuDevice *dev, const char *name,
                              const char *wgsl, size_t wgsl_len,
                              HlGpuPipeline *out)
{
    (void)dev; (void)name; (void)wgsl; (void)wgsl_len; (void)out;
    a39_seen = pthread_self();
    mock_compiles++;
    return HL_GPU_OK;
}

UTEST(hull_cap_gpu_audit39, compile_runs_off_the_calling_thread)
{
    HlGpuBackend be = MOCK_BACKEND;
    be.compile = a39_compile_thread;
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    ctx->backend = &be;
    mock_compiles = 0;
    a39_caller = pthread_self();
    a39_seen = a39_caller;
    const char *ok = "@compute @workgroup_size(1) fn main() {}";
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "ok", ok, strlen(ok)));
    EXPECT_EQ(1, mock_compiles);
    EXPECT_FALSE(pthread_equal(a39_caller, a39_seen));
    /* The round-39 shapes never reach it. */
    for (int w = 0; w < 5; w++) {
        char *s = a39_round39(w);
        ASSERT_TRUE(s != NULL);
        EXPECT_EQ(HL_GPU_ERR_SHADER, hl_cap_gpu_compile(ctx, -1, "bad", s, strlen(s)));
        free(s);
    }
    EXPECT_EQ(1, mock_compiles);
    mock_free(ctx);
}

#ifdef HL_ENABLE_GPU
UTEST(hull_cap_gpu_audit39, real_device_refuses_round39)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }
    for (int w = 0; w < 5; w++) {
        char *s = a39_round39(w);
        ASSERT_TRUE(s != NULL);
        EXPECT_EQ(HL_GPU_ERR_SHADER, hl_cap_gpu_compile(&ctx, -1, "r39", s, strlen(s)));
        free(s);
    }
    /* At the bounds they compile. */
    char *d = a39_shape(A39_LOCAL_CONST, HL_GPU_WGSL_INFERRED_DEPTH_MAX);
    char *e = a39_shape(A39_EXP_CONST, 8);
    ASSERT_TRUE(d && e);
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(&ctx, -1, "depth", d, strlen(d)));
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(&ctx, -1, "exp", e, strlen(e)));
    free(d); free(e);
    hl_cap_gpu_destroy(&ctx);
}

/* Compile, first waiting out a device left busy by an abandoned compile
 * (audit 40). */
static int a40_compile_settled(HlGpuCtx *ctx, const char *name, const char *src)
{
    int rc = HL_GPU_ERR_BUSY;
    for (int tries = 0; tries < 6000 && rc == HL_GPU_ERR_BUSY; tries++) {
        rc = hl_cap_gpu_compile(ctx, -1, name, src, strlen(src));
        if (rc == HL_GPU_ERR_BUSY) {
            struct timespec ts = { 0, 50 * 1000000L };
            nanosleep(&ts, NULL);
        }
    }
    return rc;
}

/* The structural fix: with every estimator bypassed, the shapes that
 * overflowed the event loop's stack in rounds 36-39 fail cleanly (or
 * compile) on the compile thread; the process survives. */
UTEST(hull_cap_gpu_audit39, compile_thread_survives_without_estimators)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }
    char *shapes[6];
    shapes[0] = a36_chain(A36_ADD_HEAD, "+y", 9000, "; }");     /* round 36 */
    shapes[1] = a38_chain(A38_OVERRIDE, 40000, 1);               /* round 38 */
    shapes[2] = a38_chain(A38_CONST, 30000, 1);
    shapes[3] = a39_shape(A39_LOCAL_CONST, 10000);               /* round 39 */
    shapes[4] = a39_shape(A39_LOCAL_LET, 10000);
    shapes[5] = a37_elseif(5500);                                /* round 37 */
    hl_gpu_test_set_precheck_bypass(1);
    for (int k = 0; k < 6; k++) {
        ASSERT_TRUE(shapes[k] != NULL);
        char name[16];
        snprintf(name, sizeof(name), "bypass%d", k);
        /* A compile past the deadline is abandoned (audit 40); the device
         * is busy until it finishes. */
        int rc = a40_compile_settled(&ctx, name, shapes[k]);
        EXPECT_TRUE(rc == HL_GPU_OK || rc == HL_GPU_ERR_SHADER ||
                    rc == HL_GPU_ERR_TIMEOUT);
        free(shapes[k]);
    }
    hl_gpu_test_set_precheck_bypass(0);
    /* A shader still compiles after them. */
    const char *ok = "@compute @workgroup_size(1) fn main() {}";
    EXPECT_EQ(HL_GPU_OK, a40_compile_settled(&ctx, "after", ok));
    hl_cap_gpu_destroy(&ctx);
}
#endif /* HL_ENABLE_GPU */

/* ── Audit 40: shader size bounds, compile charge, compile deadline ────
 *
 * A shader that passed every depth check could hold the event loop for
 * minutes: Metal's compiler time grows faster than linearly with the code
 * in one function (30000 live values: 343 s), and the loop waited for the
 * compile thread with a plain join. Now the size of each function and of
 * the module is bounded, the compile is charged by its size, and the wait
 * has a deadline that also polls the run. */

#include "hull/cap/db_budget.h"
#include <time.h>
#include <unistd.h>

/* A value each run's shapes store, so a device's shader cache (Metal
 * keeps one across processes) never answers for a compile being timed. */
static unsigned a40_nonce(void)
{
    static unsigned n;
    if (n == 0) n = ((unsigned)time(NULL) ^ (unsigned)getpid() * 2654435761u) | 1u;
    return n;
}

/* N live values: `let vK = o[K] * 3u;` x N, then `s ^= vK;` x N reversed. */
static char *a40_live(size_t n)
{
    size_t cap = 96 * (n + 8);
    char *s = malloc(cap);
    if (!s) return NULL;
    size_t off = (size_t)snprintf(s, cap,
        "@group(0) @binding(0) var<storage, read_write> o: array<u32>;\n"
        "@compute @workgroup_size(1) fn main() {\nvar s = 0u;\n");
    for (size_t k = 0; k < n; k++)
        off += (size_t)snprintf(s + off, cap - off, "let v%zu = o[%zu] * 3u;\n", k, k);
    for (size_t k = n; k-- > 0;)
        off += (size_t)snprintf(s + off, cap - off, "s ^= v%zu;\n", k);
    snprintf(s + off, cap - off, "o[0] = s + %uu;\n}\n", a40_nonce());
    return s;
}

/* @p fns functions of @p per independent stores each, called from main. */
static char *a40_stores(size_t fns, size_t per)
{
    size_t cap = 64 * (fns * (per + 4) + 8);
    char *s = malloc(cap);
    if (!s) return NULL;
    size_t off = (size_t)snprintf(s, cap,
        "@group(0) @binding(0) var<storage, read_write> o: array<u32>;\n");
    for (size_t f = 0; f < fns; f++) {
        off += (size_t)snprintf(s + off, cap - off, "fn f%zu() {\n", f);
        for (size_t k = 0; k < per; k++)
            off += (size_t)snprintf(s + off, cap - off,
                                    "o[%zu] = o[%zu] * %zuu;\n", k, k, f + 3);
        off += (size_t)snprintf(s + off, cap - off, "}\n");
    }
    off += (size_t)snprintf(s + off, cap - off,
                            "@compute @workgroup_size(1) fn main() {\n");
    for (size_t f = 0; f < fns; f++)
        off += (size_t)snprintf(s + off, cap - off, "f%zu();\n", f);
    snprintf(s + off, cap - off, "o[1] = %uu;\n}\n", a40_nonce());
    return s;
}

static HlGpuWgslSize a40_sz;
static int a40_size(const char *s)
{
    return hl_gpu_wgsl_size(s, strlen(s), &a40_sz);
}

UTEST(hull_cap_gpu_audit40, size_counted)
{
    EXPECT_EQ(0, hl_gpu_wgsl_size(NULL, 0, &a40_sz));
    EXPECT_EQ((size_t)0, a40_sz.module_words);
    EXPECT_EQ(-1, hl_gpu_wgsl_size("x", 1, NULL));
    /* fn, f, let, a, 1u, return, a: header words count. */
    ASSERT_EQ(0, a40_size("fn f() -> u32 { let a = 1u; return a; }"));
    EXPECT_EQ((size_t)8, a40_sz.fn_words);   /* fn f u32 let a 1u return a */
    EXPECT_EQ((size_t)1, a40_sz.fn_decls);
    EXPECT_EQ((size_t)3, a40_sz.fn_stmts);   /* { ; ; */
    EXPECT_EQ((size_t)8, a40_sz.module_words);
    EXPECT_EQ((uint64_t)1, a40_sz.decls_sq);
    /* A for header's semicolons are not statements; its var is a declaration;
     * attributes and comments are not words outside functions. */
    ASSERT_EQ(0, a40_size("// c c c\n@compute @workgroup_size(64) fn m() {"
                          " for (var i = 0u; i < 4u; i++) { } }"));
    EXPECT_EQ((size_t)1, a40_sz.fn_decls);
    EXPECT_EQ((size_t)2, a40_sz.fn_stmts);   /* the two blocks */
    EXPECT_EQ((size_t)9, a40_sz.fn_words);   /* fn m for var i 0u i 4u i */
    /* Module-scope const / override / alias / struct are not counted; a
     * module-scope var is. */
    ASSERT_EQ(0, a40_size("const T = array<u32, 4>(1u, 2u, 3u, 4u);"
                          "override W = 1u; alias A = u32; struct S { a: u32 }"));
    EXPECT_EQ((size_t)0, a40_sz.module_words);
    ASSERT_EQ(0, a40_size("var<private> t: array<u32, 2> = array<u32, 2>(1u, 2u);"));
    EXPECT_EQ((size_t)11, a40_sz.module_words);
    /* The largest function, and the module's totals. */
    ASSERT_EQ(0, a40_size("fn a() { let x = 1; let y = 2; } fn b() { let z = 3; }"));
    EXPECT_EQ((size_t)2, a40_sz.fn_decls);
    EXPECT_EQ((uint64_t)5, a40_sz.decls_sq);
    EXPECT_EQ((size_t)5, a40_sz.module_stmts);
    /* The lexer's refusals carry over. */
    EXPECT_EQ(-1, a40_size("fn a() { let s = \"q\"; }"));
}

UTEST(hull_cap_gpu_audit40, each_bound_refused)
{
    const char *why = NULL;
    /* Words in one function: `x = x + 1;` is 3 words, the head 5. */
    size_t per = HL_GPU_WGSL_FN_WORDS_MAX / 3;
    char *w_ok = a36_chain("fn main() { var x = 0;", " x = x + 1;", per - 2, " }");
    char *w_bad = a36_chain("fn main() { var x = 0;", " x = x + 1;", per + 1, " }");
    /* Declarations: `let aK = 0;` x N (3 words each), in two halves so no
     * other bound comes first. */
    char *d_bad = a36_chain("fn main() {", " let a = 0;",
                            HL_GPU_WGSL_FN_DECLS_MAX + 1, " }");
    /* Statements: `;` alone. */
    char *s_bad = a36_chain("fn main() {", ";", HL_GPU_WGSL_FN_STMTS_MAX + 1, " }");
    /* Module words: three functions of ~1800 words. */
    char *mw_bad = a36_chain("fn a() { var x = 0;", " x = x + 1;", 600,
        " } fn b() { var x = 0;");
    char *mw_bad2 = NULL;
    ASSERT_TRUE(w_ok && w_bad && d_bad && s_bad && mw_bad);
    {
        size_t l = strlen(mw_bad);
        char *more = a36_chain(" ", " x = x + 1;", 600, " } fn c() { var x = 0;");
        char *last = a36_chain(" ", " x = x + 1;", 600, " }");
        ASSERT_TRUE(more && last);
        mw_bad2 = malloc(l + strlen(more) + strlen(last) + 1);
        ASSERT_TRUE(mw_bad2 != NULL);
        strcpy(mw_bad2, mw_bad);
        strcat(mw_bad2, more);
        strcat(mw_bad2, last);
        free(more); free(last);
    }
    /* Module statements: functions of `;` alone. */
    char *ms_bad = a36_chain("fn a() {", ";", HL_GPU_WGSL_FN_STMTS_MAX - 1,
                             "} fn b() {");
    char *ms_bad2 = NULL;
    ASSERT_TRUE(ms_bad != NULL);
    {
        char *rest = a36_chain("", ";", HL_GPU_WGSL_FN_STMTS_MAX - 1,
                               "} fn c() { ; ; }");
        ASSERT_TRUE(rest != NULL);
        ms_bad2 = malloc(strlen(ms_bad) + strlen(rest) + 1);
        ASSERT_TRUE(ms_bad2 != NULL);
        strcpy(ms_bad2, ms_bad);
        strcat(ms_bad2, rest);
        free(rest);
    }
    EXPECT_EQ(0, a37_check(w_ok));
    EXPECT_EQ(-1, hl_gpu_wgsl_precheck(w_bad, strlen(w_bad), &why));
    EXPECT_TRUE(why && strstr(why, "function is too large"));
    EXPECT_EQ(-1, hl_gpu_wgsl_precheck(d_bad, strlen(d_bad), &why));
    EXPECT_TRUE(why && strstr(why, "declares too many"));
    EXPECT_EQ(-1, hl_gpu_wgsl_precheck(s_bad, strlen(s_bad), &why));
    EXPECT_TRUE(why && strstr(why, "function has too many statements"));
    EXPECT_EQ(-1, hl_gpu_wgsl_precheck(mw_bad2, strlen(mw_bad2), &why));
    EXPECT_TRUE(why && strstr(why, "shader is too large"));
    EXPECT_EQ(-1, hl_gpu_wgsl_precheck(ms_bad2, strlen(ms_bad2), &why));
    EXPECT_TRUE(why && strstr(why, "shader has too many statements"));
    /* The reported shapes: every depth shallow, refused by size. */
    char *live = a40_live(24000);
    char *chain = a36_chain("@group(0) @binding(0) var<storage, read_write> o: array<u32>;\n"
                            "@compute @workgroup_size(1) fn main() { var x = o[0];",
                            " x = x * 7u + o[3];", 25000, " o[0] = x; }");
    ASSERT_TRUE(live && chain);
    EXPECT_TRUE(strlen(live) <= HL_GPU_WGSL_MAX_BYTES);
    EXPECT_TRUE(hl_gpu_wgsl_complexity(live, strlen(live)) < 16);
    EXPECT_EQ(-1, a37_check(live));
    EXPECT_EQ(-1, a37_check(chain));

    /* Through hl_cap_gpu_compile: refused before the backend. */
    HlGpuBackend be = MOCK_BACKEND;
    be.compile = mock_compile;
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    ctx->backend = &be;
    mock_compiles = 0;
    char *bad[] = { w_bad, d_bad, s_bad, mw_bad2, ms_bad2, live, chain };
    for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
        EXPECT_EQ(HL_GPU_ERR_SHADER,
                  hl_cap_gpu_compile(ctx, -1, "bad", bad[k], strlen(bad[k])));
    EXPECT_EQ(0, mock_compiles);
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "ok", w_ok, strlen(w_ok)));
    EXPECT_EQ(1, mock_compiles);
    mock_free(ctx);
    free(w_ok); free(w_bad); free(d_bad); free(s_bad); free(mw_bad);
    free(mw_bad2); free(ms_bad); free(ms_bad2); free(live); free(chain);
}

/* The compile charge: a shader at the bounds costs a large share of the
 * default budget, refused ones are charged too, and the examples stay
 * cheap. */
UTEST(hull_cap_gpu_audit40, compile_charge)
{
    const uint64_t budget = 100000000u;            /* the default */
    char *big = a40_stores(2, (HL_GPU_WGSL_FN_WORDS_MAX - 16) / 5);
    ASSERT_TRUE(big != NULL);
    EXPECT_EQ(0, a37_check(big));
    uint64_t u = hl_gpu_wgsl_compile_units(big, strlen(big));
    EXPECT_TRUE(u >= budget / 10);
    /* A loop of them trips the default budget within a dozen. */
    uint64_t total = 0;
    int n = 0;
    while (total <= budget && n < 100) { total += u; n++; }
    EXPECT_TRUE(n <= 12);
    /* Refused is charged like compiled. */
    char *live = a40_live(24000);
    ASSERT_TRUE(live != NULL);
    EXPECT_TRUE(hl_gpu_wgsl_compile_units(live, strlen(live)) > budget);
    free(live);
    free(big);
    /* The shipped example shaders cost under 1M units. */
    size_t len = 0;
    char *ex = a36_read_file("examples/gpu_search/shaders/cosine_similarity.wgsl", &len);
    if (ex) {
        uint64_t eu = hl_gpu_wgsl_compile_units(ex, len);
        printf("  audit40 example shader: %llu units\n", (unsigned long long)eu);
        EXPECT_TRUE(eu < 1000000u);
        free(ex);
    }
}

/* A backend compile that blocks until the test releases it. */
static pthread_mutex_t a40_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t a40_cv = PTHREAD_COND_INITIALIZER;
static int a40_release, a40_started, a40_finished, a40_destroyed;

static int a40_slow_compile(HlGpuDevice *dev, const char *name,
                            const char *wgsl, size_t wgsl_len,
                            HlGpuPipeline *out)
{
    (void)dev; (void)name; (void)wgsl; (void)wgsl_len;
    pthread_mutex_lock(&a40_mu);
    a40_started = 1;
    while (!a40_release)
        pthread_cond_wait(&a40_cv, &a40_mu);
    pthread_mutex_unlock(&a40_mu);
    out->handle = (void *)&a40_destroyed;      /* something to release */
    pthread_mutex_lock(&a40_mu);
    a40_finished++;
    pthread_mutex_unlock(&a40_mu);
    return HL_GPU_OK;
}

static void a40_pipeline_destroy(HlGpuDevice *dev, HlGpuPipeline *p)
{
    (void)dev;
    if (p && p->handle) {
        pthread_mutex_lock(&a40_mu);
        a40_destroyed++;
        pthread_mutex_unlock(&a40_mu);
        p->handle = NULL;
    }
}

static void a40_reset(void)
{
    pthread_mutex_lock(&a40_mu);
    a40_release = a40_started = a40_finished = a40_destroyed = 0;
    pthread_mutex_unlock(&a40_mu);
}

static void a40_release_now(void)
{
    pthread_mutex_lock(&a40_mu);
    a40_release = 1;
    pthread_cond_broadcast(&a40_cv);
    pthread_mutex_unlock(&a40_mu);
}

static double a40_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void a40_sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* Wait (bounded) until the device's abandoned compile is gone. */
static int a40_wait_settled(HlGpuCtx *ctx)
{
    (void)ctx;
    for (int k = 0; k < 500; k++) {
        int f;
        pthread_mutex_lock(&a40_mu);
        f = a40_finished;
        pthread_mutex_unlock(&a40_mu);
        if (f) return 1;
        a40_sleep_ms(10);
    }
    return 0;
}

UTEST(hull_cap_gpu_audit40, deadline_abandons_and_device_recovers)
{
    HlGpuBackend be = MOCK_BACKEND;
    be.compile = a40_slow_compile;
    be.pipeline_destroy = a40_pipeline_destroy;
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    ctx->backend = &be;
    a40_reset();
    hl_gpu_test_set_compile_timeout_ms(100);
    const char *src = "@compute @workgroup_size(1) fn main() {}";
    double t0 = a40_now();
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, hl_cap_gpu_compile(ctx, -1, "slow", src, strlen(src)));
    double dt = a40_now() - t0;
    EXPECT_TRUE(dt < 2.0);
    EXPECT_EQ(0, ctx->devices[0].pipeline_count);
    EXPECT_TRUE(ctx->devices[0].compile_job != NULL);
    /* Busy: compiles, dispatches, writes and reads are refused. */
    EXPECT_EQ(HL_GPU_ERR_BUSY, hl_cap_gpu_compile(ctx, -1, "next", src, strlen(src)));
    mock_add_shader(ctx, "sh");
    mock_dispatches = 0;
    HlGpuDispatchOpts opts = {
        .output_buffer = -1, .output_texture = -1, .device = -1,
    };
    const char *err = NULL;
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, hl_cap_gpu_dispatch(ctx, "sh", &opts, NULL, NULL, &err));
    EXPECT_TRUE(err && strcmp(err, "gpu_busy") == 0);
    EXPECT_EQ(0, mock_dispatches);
    ASSERT_EQ(HL_GPU_OK, hl_cap_gpu_buffer_create(ctx, -1, "b", 16, HL_GPU_USAGE_READWRITE));
    unsigned char data[16] = { 0 };
    EXPECT_EQ(HL_GPU_ERR_BUSY, hl_cap_gpu_buffer_write(ctx, -1, "b", data, 16, 0));
    /* The compile finishes: its pipeline is released, the device is
     * free again. */
    a40_release_now();
    EXPECT_TRUE(a40_wait_settled(ctx));
    for (int k = 0; k < 200 && ctx->devices[0].compile_job; k++) {
        a40_sleep_ms(10);
        int rc = hl_cap_gpu_buffer_write(ctx, -1, "b", data, 16, 0);
        if (rc == HL_GPU_OK) break;
    }
    EXPECT_TRUE(ctx->devices[0].compile_job == NULL);
    pthread_mutex_lock(&a40_mu);
    EXPECT_EQ(1, a40_destroyed);
    pthread_mutex_unlock(&a40_mu);
    /* A compile now completes normally. */
    hl_gpu_test_set_compile_timeout_ms(0);
    int before = ctx->devices[0].pipeline_count;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "again", src, strlen(src)));
    EXPECT_EQ(before + 1, ctx->devices[0].pipeline_count);
    hl_cap_gpu_buffer_destroy(ctx, -1, "b");
    mock_free(ctx);
}

#ifdef HL_ENABLE_DB
/* The run stopping (deadline or budget: a bound charge that interrupts)
 * abandons the wait at once, whatever the compile deadline. */
static int a40_tripped(void *ud, int64_t units)
{
    (void)ud; (void)units;
    return 1;
}

UTEST(hull_cap_gpu_audit40, stopped_run_abandons)
{
    HlGpuBackend be = MOCK_BACKEND;
    be.compile = a40_slow_compile;
    be.pipeline_destroy = a40_pipeline_destroy;
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    ctx->backend = &be;
    a40_reset();
    hl_gpu_test_set_compile_timeout_ms(0);       /* the default */
    HlDbBudgetBinding prev = hl_db_budget_swap(a40_tripped, NULL);
    const char *src = "@compute @workgroup_size(1) fn main() {}";
    double t0 = a40_now();
    int rc = hl_cap_gpu_compile(ctx, -1, "slow", src, strlen(src));
    double dt = a40_now() - t0;
    hl_db_budget_restore(prev);
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, rc);
    EXPECT_TRUE(dt < 1.0);
    a40_release_now();
    EXPECT_TRUE(a40_wait_settled(ctx));
    for (int k = 0; k < 200; k++) {
        if (hl_cap_gpu_compile(ctx, -1, "after", src, strlen(src)) != HL_GPU_ERR_BUSY)
            break;
        a40_sleep_ms(10);
    }
    EXPECT_TRUE(ctx->devices[0].compile_job == NULL);
    mock_free(ctx);
}
#endif

/* Teardown with an abandoned compile still running waits for it (bounded)
 * before the backend goes. */
static void *a40_release_later(void *arg)
{
    (void)arg;
    a40_sleep_ms(200);
    a40_release_now();
    return NULL;
}

UTEST(hull_cap_gpu_audit40, teardown_waits_for_compile)
{
    static HlGpuBackend be;                       /* outlives the thread */
    be = MOCK_BACKEND;
    be.compile = a40_slow_compile;
    be.pipeline_destroy = a40_pipeline_destroy;
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    ctx->backend = &be;
    a40_reset();
    hl_gpu_test_set_compile_timeout_ms(50);
    const char *src = "@compute @workgroup_size(1) fn main() {}";
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, hl_cap_gpu_compile(ctx, -1, "slow", src, strlen(src)));
    hl_gpu_test_set_compile_timeout_ms(0);
    pthread_t th;
    ASSERT_EQ(0, pthread_create(&th, NULL, a40_release_later, NULL));
    double t0 = a40_now();
    hl_cap_gpu_destroy(ctx);                      /* frees the mutex too */
    double dt = a40_now() - t0;
    pthread_join(th, NULL);
    EXPECT_TRUE(dt >= 0.1 && dt < 3.0);
    pthread_mutex_lock(&a40_mu);
    EXPECT_EQ(1, a40_finished);
    EXPECT_EQ(1, a40_destroyed);
    pthread_mutex_unlock(&a40_mu);
    free(ctx);
}

#ifdef HL_ENABLE_GPU
/* On a real device (skipped without an adapter): shaders at the bounds
 * compile within a bounded time, and the reported shapes are refused. */
UTEST(hull_cap_gpu_audit40, real_device_bounded)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }
    /* As close to the bounds as each shape gets. */
    char *shapes[3];
    shapes[0] = a40_live((HL_GPU_WGSL_FN_WORDS_MAX - 16) / 7);
    shapes[1] = a40_stores(1, (HL_GPU_WGSL_FN_WORDS_MAX - 16) / 5);
    shapes[2] = a40_stores(2, (HL_GPU_WGSL_FN_WORDS_MAX - 16) / 5);
    for (int k = 0; k < 3; k++) {
        ASSERT_TRUE(shapes[k] != NULL);
        EXPECT_EQ(0, a37_check(shapes[k]));
        char name[16];
        snprintf(name, sizeof(name), "a40_%d", k);
        double t0 = a40_now();
        EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(&ctx, -1, name, shapes[k],
                                                strlen(shapes[k])));
        double dt = a40_now() - t0;
        EXPECT_TRUE(dt < 5.0);   /* ~1 s measured on Apple M1 Max */
        printf("  audit40 shape %d: %zu bytes, %.2f s\n", k, strlen(shapes[k]), dt);
        free(shapes[k]);
    }
    char *live = a40_live(4000);
    ASSERT_TRUE(live != NULL);
    EXPECT_EQ(HL_GPU_ERR_SHADER, hl_cap_gpu_compile(&ctx, -1, "live", live, strlen(live)));
    free(live);
    hl_cap_gpu_destroy(&ctx);
}
#endif /* HL_ENABLE_GPU */

/* ── Audit 41: calls are inlined ──────────────────────────────────── */

/* A chain of @p depth functions, each calling the one before @p fan times
 * in one nested expression; main calls the last. Small in source, but
 * inlined it is fan^depth copies of the first. */
static char *a41_fan(size_t fan, size_t depth, unsigned salt)
{
    size_t cap = (depth + 4) * (fan * 24 + 96) + 512;
    char *s = malloc(cap);
    if (!s) return NULL;
    size_t off = (size_t)snprintf(s, cap,
        "@group(0) @binding(0) var<storage, read_write> data: array<u32>;\n"
        "fn g0(x: u32) -> u32 { return (x ^ (x >> 3u)) * 2654435761u + (x << 5u) + %uu; }\n",
        salt);
    for (size_t k = 1; k <= depth; k++) {
        off += (size_t)snprintf(s + off, cap - off,
                                "fn g%zu(x: u32) -> u32 { return ", k);
        for (size_t j = 0; j < fan; j++)
            off += (size_t)snprintf(s + off, cap - off, "g%zu(", k - 1);
        off += (size_t)snprintf(s + off, cap - off, "x");
        for (size_t j = 0; j < fan; j++)
            off += (size_t)snprintf(s + off, cap - off, " ^ %zuu)", j + 1);
        off += (size_t)snprintf(s + off, cap - off, "; }\n");
    }
    snprintf(s + off, cap - off,
             "@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id: vec3<u32>) "
             "{ data[id.x] = g%zu(data[id.x]); }\n", depth);
    return s;
}


/* A helper of @p n live values (`let vK = d[K] * 3u + k;` x n, then
 * `s ^= vK;` x n reversed), or (@p arr) one array constructor of n
 * `d[K] * 3u + k` elements, called @p calls times from main. */
static char *a41_helper(size_t n, size_t calls, int arr, unsigned salt)
{
    size_t cap = 48 * (2 * n + calls + 16) + 512;
    char *s = malloc(cap);
    if (!s) return NULL;
    size_t off = (size_t)snprintf(s, cap,
        "@group(0) @binding(0) var<storage, read_write> d: array<u32>;\n"
        "fn h(k: u32) -> u32 {\n");
    if (arr) {
        off += (size_t)snprintf(s + off, cap - off, "let a = array<u32, %zu>(", n);
        for (size_t k = 0; k < n; k++)
            off += (size_t)snprintf(s + off, cap - off, "%sd[%zu] * 3u + k",
                                    k ? ", " : "", k);
        off += (size_t)snprintf(s + off, cap - off, ");\nreturn a[k & 7u] + %uu;\n}\n", salt);
    } else {
        off += (size_t)snprintf(s + off, cap - off, "var s = %uu;\n", salt);
        for (size_t k = 0; k < n; k++)
            off += (size_t)snprintf(s + off, cap - off, "let v%zu = d[%zu] * 3u + k;\n", k, k);
        for (size_t k = n; k-- > 0;)
            off += (size_t)snprintf(s + off, cap - off, "s ^= v%zu;\n", k);
        off += (size_t)snprintf(s + off, cap - off, "return s;\n}\n");
    }
    off += (size_t)snprintf(s + off, cap - off,
                            "@compute @workgroup_size(1) fn main() {\n");
    for (size_t c = 0; c < calls; c++)
        off += (size_t)snprintf(s + off, cap - off, "d[%zu] = h(%zuu);\n", c, c + 1);
    snprintf(s + off, cap - off, "}\n");
    return s;
}


static HlGpuWgslCalls a41_c;
static int a41_calls(const char *s)
{
    return hl_gpu_wgsl_calls(s, strlen(s), &a41_c);
}

UTEST(hull_cap_gpu_audit41, calls_counted)
{
    EXPECT_EQ(-1, hl_gpu_wgsl_calls("x", 1, NULL));
    EXPECT_EQ(0, hl_gpu_wgsl_calls(NULL, 0, &a41_c));
    EXPECT_EQ((uint64_t)0, a41_c.module_words);
    /* h: fn h x u32 u32 return x (7 words, 2 statements); main: fn main
     * let a h 1u h 2u (8 words, 1 declaration, 2 statements) plus h twice. */
    ASSERT_EQ(0, a41_calls("fn h(x: u32) -> u32 { return x; }\n"
                           "fn main() { let a = h(1u) + h(2u); }"));
    EXPECT_EQ((uint64_t)22, a41_c.fn_words);
    EXPECT_EQ((uint64_t)1, a41_c.fn_decls);
    EXPECT_EQ((uint64_t)6, a41_c.fn_stmts);
    EXPECT_EQ((uint64_t)22, a41_c.module_words);   /* h is called: not a root */
    EXPECT_EQ((uint64_t)1, a41_c.decls_sq);
    /* Without calls the counts are hl_gpu_wgsl_size's. */
    const char *flat = "fn f() -> u32 { let a = 1u; return a; }";
    HlGpuWgslSize z;
    ASSERT_EQ(0, hl_gpu_wgsl_size(flat, strlen(flat), &z));
    ASSERT_EQ(0, a41_calls(flat));
    EXPECT_EQ((uint64_t)z.fn_words, a41_c.fn_words);
    EXPECT_EQ((uint64_t)z.fn_decls, a41_c.fn_decls);
    EXPECT_EQ((uint64_t)z.fn_stmts, a41_c.fn_stmts);
    /* An unused helper is a root of its own; a local that shadows a
     * function's name is not a call of it. */
    ASSERT_EQ(0, a41_calls("fn h() -> u32 { return 1u; }\n"
                           "fn u() -> u32 { return 2u; }\n"
                           "fn main() { let h = 3u; let b = h + h; }"));
    EXPECT_EQ((uint64_t)(5 + 5 + 9), a41_c.module_words);
    /* Calls in a chain multiply: a 5 words, b 7 + 3a, c 7 + 3b. */
    ASSERT_EQ(0, a41_calls("fn a() -> u32 { return 1u; }\n"
                           "fn b() -> u32 { return a() + a() + a(); }\n"
                           "fn c() -> u32 { return b() + b() + b(); }\n"));
    EXPECT_EQ((uint64_t)(7 + 3 * (7 + 3 * 5)), a41_c.module_words);
    /* WGSL forbids recursion: a cycle is refused. */
    EXPECT_EQ(-1, a41_calls("fn a() { b(); } fn b() { a(); }"));
    EXPECT_EQ(UINT64_MAX, a41_c.module_words);
    EXPECT_EQ(-1, a41_calls("fn a() { a(); }"));
    /* Saturating: a long fan-out chain counts as UINT64_MAX, not a wrap. */
    char *deep = a41_fan(16, 40, 1);
    ASSERT_TRUE(deep != NULL);
    ASSERT_EQ(0, a41_calls(deep));
    EXPECT_EQ(UINT64_MAX, a41_c.fn_words);
    EXPECT_EQ(UINT64_MAX, a41_c.module_words);
    free(deep);
}

/* The reported shapes are within every audit-40 bound and refused by the
 * inlined counts, before the backend. */
UTEST(hull_cap_gpu_audit41, fan_out_chains_refused_before_backend)
{
    const char *why = NULL;
    char *shapes[] = {
        a41_fan(16, 12, 1),           /* ~2.5 KB: 4.3 s on Metal */
        a41_fan(8, 90, 2),            /* ~3800 words: ~36 s */
        a41_fan(2, 20, 3),            /* 1.4 s */
        a41_fan(2, 68, 4),            /* 6.3 s */
        a41_fan(3, 140, 5),           /* 2.4 s */
        a41_fan(4, 8, 6),
    };
    size_t n = sizeof(shapes) / sizeof(shapes[0]);
    HlGpuBackend be = MOCK_BACKEND;
    be.compile = mock_compile;
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    ctx->backend = &be;
    mock_compiles = 0;
    for (size_t k = 0; k < n; k++) {
        ASSERT_TRUE(shapes[k] != NULL);
        HlGpuWgslSize z;
        ASSERT_EQ(0, hl_gpu_wgsl_size(shapes[k], strlen(shapes[k]), &z));
        EXPECT_TRUE(z.fn_words <= HL_GPU_WGSL_FN_WORDS_MAX);
        EXPECT_TRUE(z.module_words <= HL_GPU_WGSL_MODULE_WORDS_MAX);
        EXPECT_TRUE(z.module_stmts <= HL_GPU_WGSL_MODULE_STMTS_MAX);
        EXPECT_EQ(-1, hl_gpu_wgsl_precheck(shapes[k], strlen(shapes[k]), &why));
        EXPECT_TRUE(why && strstr(why, "calls expand"));
        EXPECT_EQ(HL_GPU_ERR_SHADER,
                  hl_cap_gpu_compile(ctx, -1, "fan", shapes[k], strlen(shapes[k])));
        free(shapes[k]);
    }
    EXPECT_EQ(0, mock_compiles);
    /* A cycle never reaches the backend. */
    const char *cyc = "fn a() { b(); } fn b() { a(); }\n"
                      "@compute @workgroup_size(1) fn main() { a(); }";
    EXPECT_EQ(HL_GPU_ERR_SHADER, hl_cap_gpu_compile(ctx, -1, "cyc", cyc, strlen(cyc)));
    EXPECT_EQ(0, mock_compiles);
    /* At the bound: a helper of 32 live values (~270 words) called as
     * often as the inlined bounds allow passes; one call more is refused. */
    size_t per = 32;
    size_t calls_ok = 1, calls_bad = 1;
    for (;;) {
        char *t = a41_helper(per, calls_bad, 0, 1);
        ASSERT_TRUE(t != NULL);
        ASSERT_EQ(0, a41_calls(t));
        int over = a41_c.module_words > HL_GPU_WGSL_INLINED_WORDS_MAX ||
                   a41_c.module_decls > HL_GPU_WGSL_INLINED_DECLS_MAX ||
                   a41_c.module_stmts > HL_GPU_WGSL_INLINED_STMTS_MAX;
        free(t);
        if (over) break;
        calls_ok = calls_bad++;
    }
    char *ok = a41_helper(per, calls_ok, 0, 1);
    char *bad = a41_helper(per, calls_bad, 0, 1);
    ASSERT_TRUE(ok && bad);
    EXPECT_EQ(0, a37_check(ok));
    EXPECT_EQ(-1, hl_gpu_wgsl_precheck(bad, strlen(bad), &why));
    EXPECT_TRUE(why && strstr(why, "calls expand"));
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "ok", ok, strlen(ok)));
    EXPECT_EQ(1, mock_compiles);
    free(ok); free(bad);
    mock_free(ctx);
}

/* Shaders with helpers called a few times, and the shipped examples, pass. */
UTEST(hull_cap_gpu_audit41, helpers_called_a_few_times_pass)
{
    const char *helpers =
        "@group(0) @binding(0) var<storage, read_write> o: array<f32>;\n"
        "fn hash(x: u32) -> u32 { var h = x * 747796405u + 2891336453u;\n"
        "  h = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u; return (h >> 22u) ^ h; }\n"
        "fn rnd(x: u32) -> f32 { return f32(hash(x)) / 4294967295.0; }\n"
        "fn noise(p: vec2<f32>) -> f32 {\n"
        "  let i = vec2<u32>(floor(p)); let f = fract(p);\n"
        "  let a = rnd(i.x + i.y * 57u); let b = rnd(i.x + 1u + i.y * 57u);\n"
        "  let c = rnd(i.x + (i.y + 1u) * 57u); let d = rnd(i.x + 1u + (i.y + 1u) * 57u);\n"
        "  let u = f * f * (3.0 - 2.0 * f);\n"
        "  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y); }\n"
        "fn fbm(p: vec2<f32>) -> f32 { var v = 0.0; var q = p;\n"
        "  for (var k = 0u; k < 6u; k++) { v += noise(q) * 0.5; q = q * 2.0; }\n"
        "  return v + noise(p * 3.0) + noise(p * 5.0); }\n"
        "@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
        "  let p = vec2<f32>(f32(id.x % 256u), f32(id.x / 256u));\n"
        "  o[id.x] = fbm(p) + fbm(p * 0.5) + rnd(id.x); }\n";
    EXPECT_EQ(0, a41_calls(helpers));
    EXPECT_TRUE(a41_c.module_words < 2048);
    EXPECT_EQ(0, a37_check(helpers));
    char *few = a41_helper(200, 4, 0, 1);         /* ~1600 words, 4 calls */
    ASSERT_TRUE(few != NULL);
    EXPECT_EQ(0, a37_check(few));
    free(few);
    size_t len = 0;
    char *ex = a36_read_file("examples/gpu_search/shaders/cosine_similarity.wgsl", &len);
    if (ex) {
        EXPECT_EQ(0, hl_gpu_wgsl_precheck(ex, len, NULL));
        free(ex);
    }
}

/* The compile charge counts the code as inlined: a chain is charged for
 * what it expands to, and a refused fan-out bomb costs a whole budget. */
UTEST(hull_cap_gpu_audit41, compile_charge_reflects_expansion)
{
    char *t = a41_fan(4, 4, 1);                   /* within the bounds */
    ASSERT_TRUE(t != NULL);
    ASSERT_EQ(0, a41_calls(t));
    EXPECT_EQ(0, a37_check(t));
    HlGpuWgslSize z;
    ASSERT_EQ(0, hl_gpu_wgsl_size(t, strlen(t), &z));
    EXPECT_TRUE(a41_c.module_words > 4 * (uint64_t)z.module_words);
    uint64_t u = hl_gpu_wgsl_compile_units(t, strlen(t));
    /* Written code at the written rates, what inlining adds at the
     * inlined rates (audit 42). */
    EXPECT_TRUE(u >= (uint64_t)z.module_words * HL_GPU_WGSL_WORD_UNITS +
                     (uint64_t)z.module_stmts * HL_GPU_WGSL_STMT_UNITS +
                     (a41_c.module_words - z.module_words) * HL_GPU_WGSL_INLINED_WORD_UNITS +
                     (a41_c.module_stmts - z.module_stmts) * HL_GPU_WGSL_INLINED_STMT_UNITS);
    EXPECT_TRUE(u > (uint64_t)z.module_words * HL_GPU_WGSL_WORD_UNITS +
                    (uint64_t)z.module_stmts * HL_GPU_WGSL_STMT_UNITS +
                    hl_work_bytes_units(strlen(t)));
    free(t);
    char *bomb = a41_fan(8, 90, 1);
    ASSERT_TRUE(bomb != NULL);
    EXPECT_TRUE(hl_gpu_wgsl_compile_units(bomb, strlen(bomb)) > 100000000u);
    free(bomb);
    /* A shader whose functions are each called once is charged as before:
     * inlined, it is no larger than written. */
    char *flat = a40_stores(1, 64);
    ASSERT_TRUE(flat != NULL);
    ASSERT_EQ(0, hl_gpu_wgsl_size(flat, strlen(flat), &z));
    ASSERT_EQ(0, a41_calls(flat));
    EXPECT_TRUE(a41_c.module_words <= (uint64_t)z.module_words);
    EXPECT_TRUE(a41_c.module_stmts <= (uint64_t)z.module_stmts);
    free(flat);
}

#ifdef HL_ENABLE_GPU
/* On a real device (skipped without an adapter): shaders at the inlined
 * bounds compile within ~1 s, well inside the compile deadline, and the
 * reported fan-out chains are refused. */
UTEST(hull_cap_gpu_audit41, real_device_bounded)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) { ASSERT_TRUE(1); return; }
    unsigned salt = a40_nonce();
    char *shapes[4];
    /* A helper of 64 live values called as often as the bounds allow (the
     * slowest inlined shape measured), and the deepest fan-out chains
     * within them. */
    size_t calls = 1, d2 = 1, d16 = 1;
    for (;;) {
        char *t = a41_helper(64, calls + 1, 0, salt);
        int fits = t && hl_gpu_wgsl_precheck(t, strlen(t), NULL) == 0;
        free(t);
        if (!fits) break;
        calls++;
    }
    for (;;) {
        char *t = a41_fan(2, d2 + 1, salt);
        int fits = t && hl_gpu_wgsl_precheck(t, strlen(t), NULL) == 0;
        free(t);
        if (!fits) break;
        d2++;
    }
    for (;;) {
        char *t = a41_fan(16, d16 + 1, salt);
        int fits = t && hl_gpu_wgsl_precheck(t, strlen(t), NULL) == 0;
        free(t);
        if (!fits) break;
        d16++;
    }
    printf("  audit41 bounds: %zu calls, depth %zu (fan 2), depth %zu (fan 16)\n",
           calls, d2, d16);
    shapes[0] = a41_helper(64, calls, 0, salt);
    shapes[1] = a41_fan(2, d2, salt);
    shapes[2] = a41_fan(16, d16, salt);
    shapes[3] = a41_helper(400, 8, 1, salt);      /* big helper, kept a call */
    for (int k = 0; k < 4; k++) {
        ASSERT_TRUE(shapes[k] != NULL);
        char name[16];
        snprintf(name, sizeof(name), "a41_%d", k);
        double t0 = a40_now();
        int rc = hl_cap_gpu_compile(&ctx, -1, name, shapes[k], strlen(shapes[k]));
        double dt = a40_now() - t0;
        printf("  audit41 shape %d: %zu bytes, rc %d, %.2f s\n",
               k, strlen(shapes[k]), rc, dt);
        if (k < 3) EXPECT_EQ(HL_GPU_OK, rc);
        EXPECT_TRUE(dt < 1.5);   /* ~0.9 s at most measured on Apple M1 Max */
        free(shapes[k]);
    }
    char *bomb = a41_fan(8, 90, salt);
    ASSERT_TRUE(bomb != NULL);
    double t0 = a40_now();
    EXPECT_EQ(HL_GPU_ERR_SHADER, hl_cap_gpu_compile(&ctx, -1, "bomb", bomb, strlen(bomb)));
    EXPECT_TRUE(a40_now() - t0 < 0.5);
    free(bomb);
    hl_cap_gpu_destroy(&ctx);
}
#endif /* HL_ENABLE_GPU */

/* ── Audit convergence §7.3: the shader policy ─────────────────────────
 * A built binary compiles only the shaders embedded from shaders/ unless
 * the manifest sets gpu.dynamic_shaders; development compiles any source.
 * The policy is enforced in hl_cap_gpu_compile_ex, before the pre-checks
 * and the backend. */
#include "hull/vfs.h"

static const char AC_EMBEDDED[] =
    "@compute @workgroup_size(1) fn main() { let x = 1 + 2; }";
static const char AC_OTHER[] =
    "@compute @workgroup_size(1) fn main() { let y = 3 + 4; }";
/* Same length as AC_EMBEDDED, one byte different. */
static const char AC_NEAR[] =
    "@compute @workgroup_size(1) fn main() { let x = 1 + 3; }";

static const HlEntry AC_ENTRIES[] = {
    { "./app.lua", (const unsigned char *)"-- app", 6 },
    { "shaders/dbl.wgsl", (const unsigned char *)AC_EMBEDDED,
      (unsigned int)(sizeof AC_EMBEDDED - 1) },
    /* Not a shader: same bytes, but under another prefix. */
    { "templates/x.wgsl", (const unsigned char *)AC_OTHER,
      (unsigned int)(sizeof AC_OTHER - 1) },
    { NULL, NULL, 0 },
};

/* Restart the compile helper (audit convergence §7.3) with the test hook
 * @p hook (HULL_TEST_GPU_HELPER, NULL = none) and an empty verdict cache.
 * The helper is this test binary (main below routes HL_GPU_HELPER_ARGV1). */
static int ach_start(const char *hook)
{
    hl_gpu_helper_stop();
    if (hook) setenv("HULL_TEST_GPU_HELPER", hook, 1);
    else unsetenv("HULL_TEST_GPU_HELPER");
    hl_gpu_helper_test_reset_cache(1);
    hl_gpu_helper_test_set_timeouts(0, 0);
    int rc = hl_gpu_helper_start(NULL);
    unsetenv("HULL_TEST_GPU_HELPER");
    return rc;
}

/* No compile helper where GPU never runs (cap/gpu_helper.c: cosmo and
 * Windows have no GPU backend, and an APE cannot spawn itself as the
 * helper): hl_gpu_helper_start fails and opted-in runtime source is
 * refused (dynamic_source_with_opt_in checks that); the supervision
 * tests skip. */
#if defined(__COSMOPOLITAN__) || defined(_WIN32)
#define AGH_NO_HELPER 1
#define AGH_NEED_HELPER() \
    UTEST_SKIP("no GPU compile helper on this platform (GPU is native-only)")
#else
#define AGH_NEED_HELPER() do { } while (0)
#endif

static HlGpuCtx *ac_ctx(HlGpuBackend *be, HlVfs *vfs)
{
    *be = MOCK_BACKEND;
    be->compile = mock_compile;
    HlGpuCtx *ctx = mock_ctx();
    if (!ctx) return NULL;
    ctx->backend = be;
    mock_compiles = 0;
    hl_vfs_init(vfs, AC_ENTRIES, NULL);
    return ctx;
}

UTEST(hull_cap_gpu_audit_convergence, embedded_shader_accepted)
{
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = ac_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_EMBEDDED, 0, &vfs);
    EXPECT_EQ(1, hl_gpu_shader_is_embedded(&vfs, AC_EMBEDDED, strlen(AC_EMBEDDED)));
    EXPECT_EQ(0, hl_gpu_shader_is_embedded(&vfs, AC_NEAR, strlen(AC_NEAR)));
    EXPECT_EQ(0, hl_gpu_shader_is_embedded(&vfs, AC_OTHER, strlen(AC_OTHER)));
    /* gpu.load: the embedded entry's own bytes. */
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile_ex(ctx, -1, "dbl",
              (const char *)AC_ENTRIES[1].data, AC_ENTRIES[1].len,
              HL_GPU_COMPILE_FROM_SHADERS));
    /* gpu.compile of a byte-identical copy is the same shader. */
    char copy[sizeof AC_EMBEDDED];
    memcpy(copy, AC_EMBEDDED, sizeof copy);
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "dbl2", copy, strlen(copy)));
    EXPECT_EQ(2, mock_compiles);
    mock_free(ctx);
}

UTEST(hull_cap_gpu_audit_convergence, dynamic_source_refused_without_opt_in)
{
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = ac_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_EMBEDDED, 0, &vfs);
    EXPECT_EQ(HL_GPU_ERR_POLICY, hl_cap_gpu_compile(ctx, -1, "other",
              AC_OTHER, strlen(AC_OTHER)));
    EXPECT_EQ(HL_GPU_ERR_POLICY, hl_cap_gpu_compile(ctx, -1, "near",
              AC_NEAR, strlen(AC_NEAR)));
    /* The source-origin flag never widens the policy. */
    EXPECT_EQ(HL_GPU_ERR_POLICY, hl_cap_gpu_compile_ex(ctx, -1, "other",
              AC_OTHER, strlen(AC_OTHER), HL_GPU_COMPILE_FROM_SHADERS));
    /* Refused before the backend, nothing cached. */
    EXPECT_EQ(0, mock_compiles);
    EXPECT_EQ(0, ctx->devices[0].pipeline_count);
    EXPECT_STREQ("dynamic_shader_refused", hl_gpu_compile_errstr(HL_GPU_ERR_POLICY));
    /* A built binary with no shaders at all refuses every source. */
    static const HlEntry none[] = {
        { "./app.lua", (const unsigned char *)"-- app", 6 }, { NULL, NULL, 0 },
    };
    HlVfs empty;
    hl_vfs_init(&empty, none, NULL);
    hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_EMBEDDED, 0, &empty);
    EXPECT_EQ(HL_GPU_ERR_POLICY, hl_cap_gpu_compile(ctx, -1, "dbl",
              AC_EMBEDDED, strlen(AC_EMBEDDED)));
    EXPECT_EQ(0, mock_compiles);
    mock_free(ctx);
}

UTEST(hull_cap_gpu_audit_convergence, dynamic_source_with_opt_in)
{
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = ac_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_EMBEDDED, 1, &vfs);
    /* Runtime source goes through the compile helper first; with none
     * running it fails closed, before the backend. */
    hl_gpu_helper_stop();
    EXPECT_EQ(HL_GPU_ERR_HELPER, hl_cap_gpu_compile(ctx, -1, "other",
              AC_OTHER, strlen(AC_OTHER)));
    EXPECT_STREQ("shader_helper_failed", hl_gpu_compile_errstr(HL_GPU_ERR_HELPER));
    EXPECT_EQ(0, mock_compiles);
#ifdef AGH_NO_HELPER
    /* No helper on this platform: it never starts, and opted-in runtime
     * source is refused with shader_helper_failed, never hung on. */
    EXPECT_EQ(-1, ach_start("accept"));
    EXPECT_EQ(0, hl_gpu_helper_running());
    EXPECT_EQ(HL_GPU_ERR_HELPER, hl_cap_gpu_compile(ctx, -1, "other",
              AC_OTHER, strlen(AC_OTHER)));
    EXPECT_EQ(0, mock_compiles);
    mock_free(ctx);
    return;
#endif
    ASSERT_EQ(0, ach_start("accept"));
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "other",
              AC_OTHER, strlen(AC_OTHER)));
    EXPECT_EQ(1, mock_compiles);
    /* An embedded shader never goes to the helper. */
    HlGpuHelperStats st0, st1;
    hl_gpu_helper_stats(&st0);
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "emb",
              AC_EMBEDDED, strlen(AC_EMBEDDED)));
    hl_gpu_helper_stats(&st1);
    EXPECT_EQ(st0.round_trips, st1.round_trips);
    EXPECT_EQ(st0.cache_hits, st1.cache_hits);
    EXPECT_EQ(2, mock_compiles);
    /* The pre-checks still run on dynamic source. */
    char *add = a36_chain(A36_ADD_HEAD, "+y", 20000, "; }");
    ASSERT_TRUE(add != NULL);
    EXPECT_EQ(HL_GPU_ERR_SHADER, hl_cap_gpu_compile(ctx, -1, "deep", add, strlen(add)));
    free(add);
    EXPECT_EQ(2, mock_compiles);
    hl_gpu_helper_stop();
    mock_free(ctx);
}

UTEST(hull_cap_gpu_audit_convergence, dev_mode_compiles_inline_source)
{
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = ac_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    /* Development: no embedded files (the VFS is empty, root_dir set). */
    static const HlEntry none[] = { { NULL, NULL, 0 } };
    HlVfs dev;
    hl_vfs_init(&dev, none, "/nonexistent-app");
    hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_DEV, 0, &dev);
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "inline",
              AC_OTHER, strlen(AC_OTHER)));
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile_ex(ctx, -1, "file",
              AC_EMBEDDED, strlen(AC_EMBEDDED), HL_GPU_COMPILE_FROM_SHADERS));
    EXPECT_EQ(2, mock_compiles);
    /* No policy set (unit tests, embedders) compiles anything too. */
    hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_ANY, 0, NULL);
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "any",
              AC_NEAR, strlen(AC_NEAR)));
    EXPECT_EQ(3, mock_compiles);
    mock_free(ctx);
}

/* Once the compile helper has accepted runtime source, the in-process
 * compile keeps its own deadline: a compile that outlives it is abandoned
 * and the call returns. Uses the round-40 slow-compile hook. */
UTEST(hull_cap_gpu_audit_convergence, opt_in_compile_abandoned_at_deadline)
{
    AGH_NEED_HELPER();
    HlGpuBackend be = MOCK_BACKEND;
    be.compile = a40_slow_compile;
    be.pipeline_destroy = a40_pipeline_destroy;
    HlGpuCtx *ctx = mock_ctx();
    ASSERT_TRUE(ctx != NULL);
    ctx->backend = &be;
    HlVfs vfs;
    hl_vfs_init(&vfs, AC_ENTRIES, NULL);
    hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_EMBEDDED, 1, &vfs);
    ASSERT_EQ(0, ach_start("accept"));
    a40_reset();
    hl_gpu_test_set_compile_timeout_ms(100);
    double t0 = a40_now();
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, hl_cap_gpu_compile(ctx, -1, "slow",
              AC_OTHER, strlen(AC_OTHER)));
    EXPECT_TRUE(a40_now() - t0 < 2.0);
    EXPECT_STREQ("compile_timeout", hl_gpu_compile_errstr(HL_GPU_ERR_TIMEOUT));
    a40_release_now();
    EXPECT_TRUE(a40_wait_settled(ctx));
    for (int k = 0; k < 200 && ctx->devices[0].compile_job; k++) {
        a40_sleep_ms(10);
        (void)hl_cap_gpu_compile(ctx, -1, "poke", AC_EMBEDDED, strlen(AC_EMBEDDED));
    }
    hl_gpu_test_set_compile_timeout_ms(0);
    hl_gpu_helper_stop();
    mock_free(ctx);
}

UTEST(hull_cap_gpu_audit_convergence, build_check_runs_the_precheck)
{
    const char *srcs[2] = { AC_EMBEDDED, AC_OTHER };
    size_t lens[2] = { strlen(AC_EMBEDDED), strlen(AC_OTHER) };
    size_t bad = 99;
    const char *why = NULL;
    int compiled = -1;
    EXPECT_EQ(0, hl_gpu_shaders_build_check(NULL, srcs, lens, 2, &bad, &why,
                                            &compiled));
    EXPECT_EQ(0, compiled);
    /* A source the pre-check refuses fails the build, naming its index. */
    const char *badsrc = "@compute @workgroup_size(1) fn main() { $ }";
    srcs[1] = badsrc;
    lens[1] = strlen(badsrc);
    EXPECT_EQ(-1, hl_gpu_shaders_build_check(NULL, srcs, lens, 2, &bad, &why,
                                             &compiled));
    EXPECT_EQ((size_t)1, bad);
    EXPECT_TRUE(why != NULL);
    /* Larger than the size cap. */
    lens[1] = (size_t)HL_GPU_WGSL_MAX_BYTES + 1;
    srcs[1] = AC_OTHER;   /* never read past the cap check */
    EXPECT_EQ(-1, hl_gpu_shaders_build_check(NULL, srcs, lens, 2, &bad, &why,
                                             &compiled));
    EXPECT_EQ((size_t)1, bad);
}

#ifdef HL_ENABLE_GPU
/* On a real device (skipped without an adapter): the build check test-
 * compiles, and a source the pre-check passes but naga rejects fails. */
UTEST(hull_cap_gpu_audit_convergence, build_check_test_compiles_on_device)
{
    HlGpuCtx probe;
    if (!gpu_test_init(&probe)) { ASSERT_TRUE(1); return; }
    hl_cap_gpu_destroy(&probe);
    const char *good = "@group(0) @binding(0) var<storage, read_write> d: array<f32>;\n"
                       "@compute @workgroup_size(1) fn main(@builtin(global_invocation_id) id: vec3<u32>) {"
                       " d[id.x] = d[id.x] * 2.0; }";
    const char *srcs[1] = { good };
    size_t lens[1] = { strlen(good) };
    size_t bad = 99;
    const char *why = NULL;
    int compiled = 0;
    EXPECT_EQ(0, hl_gpu_shaders_build_check(&hl_gpu_backend_wgpu, srcs, lens, 1,
                                            &bad, &why, &compiled));
    EXPECT_EQ(1, compiled);
    const char *broken = "@compute @workgroup_size(1) fn main() { let x: u32 = undefined_name; }";
    srcs[0] = broken;
    lens[0] = strlen(broken);
    EXPECT_EQ(-1, hl_gpu_shaders_build_check(&hl_gpu_backend_wgpu, srcs, lens, 1,
                                             &bad, &why, &compiled));
    EXPECT_EQ((size_t)0, bad);
}

/* Measurement for the helper-process follow-up: a source one device
 * compiled, compiled again on a fresh device. The printed times say how
 * much a driver cache saves (Apple M1 Max: ~0.64 s, then ~0.01 s; ~6 ms
 * in a new process); the expectation only bounds it. */
UTEST(hull_cap_gpu_audit_convergence, recompile_on_a_fresh_device_measured)
{
    HlGpuCtx a;
    if (!gpu_test_init(&a)) { ASSERT_TRUE(1); return; }
    /* One statement short of audit 40's shape, so no earlier test in this
     * process compiled this exact source. */
    char *src = a40_stores(2, (HL_GPU_WGSL_FN_WORDS_MAX - 16) / 5 - 1);
    ASSERT_TRUE(src != NULL);
    double t0 = a40_now();
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(&a, -1, "m", src, strlen(src)));
    double first = a40_now() - t0;
    hl_cap_gpu_destroy(&a);
    HlGpuCtx b;
    if (gpu_test_init(&b)) {
        t0 = a40_now();
        EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(&b, -1, "m", src, strlen(src)));
        double again = a40_now() - t0;
        printf("  convergence: %zu bytes, first %.2f s, fresh-device again %.2f s\n",
               strlen(src), first, again);
        EXPECT_TRUE(again < 5.0);
        hl_cap_gpu_destroy(&b);
    }
    free(src);
}
#endif /* HL_ENABLE_GPU */


/* ── Audit 42: helper-based shaders fit the budget ──────────────────
 * The audit-41 compile charge read call-inlined words at the written-word
 * rate (HL_GPU_WGSL_WORD_UNITS), so an ordinary domain-warp shader - main
 * calls pattern() 6 times, pattern calls fbm() 5 times, fbm loops over a
 * simplex noise - was charged 50-70M of the 100M default budget for a
 * compile Metal finishes in ~0.1 s, and an app compiling three or four
 * such shaders at startup could not load. Inlined words past the written
 * ones are now charged at HL_GPU_WGSL_INLINED_WORD_UNITS. */
static const char A42_WARP[] =
    "@group(0) @binding(0) var<storage, read_write> out_px: array<vec4<f32>>;\n"
    "struct Params { width: u32, height: u32, time: f32, scale: f32 }\n"
    "@group(0) @binding(1) var<uniform> params: Params;\n"
    "fn mod289_3(x: vec3<f32>) -> vec3<f32> { return x - floor(x * (1.0 / 289.0)) * 289.0; }\n"
    "fn mod289_4(x: vec4<f32>) -> vec4<f32> { return x - floor(x * (1.0 / 289.0)) * 289.0; }\n"
    "fn permute4(x: vec4<f32>) -> vec4<f32> { return mod289_4(((x * 34.0) + 10.0) * x); }\n"
    "fn taylor_inv_sqrt4(r: vec4<f32>) -> vec4<f32> { return 1.79284291400159 - 0.85373472095314 * r; }\n"
    "fn snoise(v: vec3<f32>) -> f32 {\n"
    "  let C = vec2<f32>(1.0 / 6.0, 1.0 / 3.0);\n"
    "  let D = vec4<f32>(0.0, 0.5, 1.0, 2.0);\n"
    "  var i = floor(v + dot(v, C.yyy));\n"
    "  let x0 = v - i + dot(i, C.xxx);\n"
    "  let g = step(x0.yzx, x0.xyz);\n"
    "  let l = 1.0 - g;\n"
    "  let i1 = min(g.xyz, l.zxy);\n"
    "  let i2 = max(g.xyz, l.zxy);\n"
    "  let x1 = x0 - i1 + C.xxx;\n"
    "  let x2 = x0 - i2 + C.yyy;\n"
    "  let x3 = x0 - D.yyy;\n"
    "  i = mod289_3(i);\n"
    "  let p = permute4(permute4(permute4(i.z + vec4<f32>(0.0, i1.z, i2.z, 1.0))\n"
    "        + i.y + vec4<f32>(0.0, i1.y, i2.y, 1.0)) + i.x + vec4<f32>(0.0, i1.x, i2.x, 1.0));\n"
    "  let n_ = 0.142857142857;\n"
    "  let ns = n_ * D.wyz - D.xzx;\n"
    "  let j = p - 49.0 * floor(p * ns.z * ns.z);\n"
    "  let x_ = floor(j * ns.z);\n"
    "  let y_ = floor(j - 7.0 * x_);\n"
    "  let x = x_ * ns.x + ns.yyyy;\n"
    "  let y = y_ * ns.x + ns.yyyy;\n"
    "  let h = 1.0 - abs(x) - abs(y);\n"
    "  let b0 = vec4<f32>(x.xy, y.xy);\n"
    "  let b1 = vec4<f32>(x.zw, y.zw);\n"
    "  let s0 = floor(b0) * 2.0 + 1.0;\n"
    "  let s1 = floor(b1) * 2.0 + 1.0;\n"
    "  let sh = -step(h, vec4<f32>(0.0));\n"
    "  let a0 = b0.xzyw + s0.xzyw * sh.xxyy;\n"
    "  let a1 = b1.xzyw + s1.xzyw * sh.zzww;\n"
    "  var p0 = vec3<f32>(a0.xy, h.x);\n"
    "  var p1 = vec3<f32>(a0.zw, h.y);\n"
    "  var p2 = vec3<f32>(a1.xy, h.z);\n"
    "  var p3 = vec3<f32>(a1.zw, h.w);\n"
    "  let norm = taylor_inv_sqrt4(vec4<f32>(dot(p0, p0), dot(p1, p1), dot(p2, p2), dot(p3, p3)));\n"
    "  p0 = p0 * norm.x;\n"
    "  p1 = p1 * norm.y;\n"
    "  p2 = p2 * norm.z;\n"
    "  p3 = p3 * norm.w;\n"
    "  var m = max(0.5 - vec4<f32>(dot(x0, x0), dot(x1, x1), dot(x2, x2), dot(x3, x3)), vec4<f32>(0.0));\n"
    "  m = m * m;\n"
    "  return 105.0 * dot(m * m, vec4<f32>(dot(p0, x0), dot(p1, x1), dot(p2, x2), dot(p3, x3)));\n"
    "}\n"
    "fn fbm(p: vec3<f32>) -> f32 {\n"
    "  var sum = 0.0;\n"
    "  var amp = 0.5;\n"
    "  var q = p;\n"
    "  for (var o = 0; o < 6; o = o + 1) {\n"
    "    sum = sum + amp * snoise(q);\n"
    "    q = q * 2.02 + vec3<f32>(1.7, 9.2, 3.1);\n"
    "    amp = amp * 0.5;\n"
    "  }\n"
    "  return sum;\n"
    "}\n"
    "fn pattern(p: vec2<f32>, t: f32) -> f32 {\n"
    "  let q = vec2<f32>(fbm(vec3<f32>(p, t)), fbm(vec3<f32>(p + vec2<f32>(5.2, 1.3), t)));\n"
    "  let r = vec2<f32>(fbm(vec3<f32>(p + 4.0 * q + vec2<f32>(1.7, 9.2), t)),\n"
    "                    fbm(vec3<f32>(p + 4.0 * q + vec2<f32>(8.3, 2.8), t)));\n"
    "  return fbm(vec3<f32>(p + 4.0 * r, t));\n"
    "}\n"
    "@compute @workgroup_size(8, 8)\n"
    "fn main(@builtin(global_invocation_id) gid: vec3<u32>) {\n"
    "  if (gid.x >= params.width || gid.y >= params.height) { return; }\n"
    "  let uv = vec2<f32>(f32(gid.x), f32(gid.y)) / f32(params.width) * params.scale;\n"
    "  let t = params.time * 0.1;\n"
    "  let a = pattern(uv, t);\n"
    "  let b = pattern(uv + vec2<f32>(0.01, 0.0), t);\n"
    "  let c = pattern(uv + vec2<f32>(0.0, 0.01), t);\n"
    "  let d = pattern(uv * 0.5, t + 3.0);\n"
    "  let e = pattern(uv * 2.0, t - 1.0);\n"
    "  let f = pattern(uv.yx, t);\n"
    "  let n = normalize(vec3<f32>(a - b, a - c, 0.02));\n"
    "  let col = vec3<f32>(a, d * 0.5 + e * 0.5, f) * (0.6 + 0.4 * n.z);\n"
    "  out_px[gid.y * params.width + gid.x] = vec4<f32>(col, 1.0);\n"
    "}\n";

UTEST(gpu_audit42, helper_shaders_fit_budget)
{
    const size_t len = strlen(A42_WARP);
    /* Ordinary code: within every pre-check bound. */
    EXPECT_EQ(0, hl_gpu_wgsl_precheck(A42_WARP, len, NULL));
    HlGpuWgslSize z;
    ASSERT_EQ(0, hl_gpu_wgsl_size(A42_WARP, len, &z));
    HlGpuWgslCalls c;
    ASSERT_EQ(0, hl_gpu_wgsl_calls(A42_WARP, len, &c));
    /* The call fan-out is real: inlined, it is many times its written size. */
    EXPECT_TRUE(c.module_words > 10 * (uint64_t)z.module_words);
    uint64_t u = hl_gpu_wgsl_compile_units(A42_WARP, len);
    printf("  audit42 warp shader: %llu written words, %llu inlined, %llu units\n",
           (unsigned long long)z.module_words, (unsigned long long)c.module_words,
           (unsigned long long)u);
    /* Well under the 100M default: several such shaders compile at startup. */
    EXPECT_TRUE(u < 10000000u);
    /* Still charged for its expansion, not as written alone. */
    EXPECT_TRUE(u > (uint64_t)z.module_words * HL_GPU_WGSL_WORD_UNITS);
}

#ifdef HL_ENABLE_GPU
/* On a real device (skipped without an adapter): the warp shader compiles
 * well inside the deadline (~0.11 s on Apple M1 Max / Metal). */
UTEST(gpu_audit42, helper_shader_real_device)
{
    HlGpuCtx ctx;
    if (!gpu_test_init(&ctx)) return;
    double t0 = a40_now();
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(&ctx, -1, "warp", A42_WARP, strlen(A42_WARP)));
    double dt = a40_now() - t0;
    printf("  audit42 warp shader compile: %.3f s\n", dt);
    EXPECT_TRUE(dt < 1.5);
    hl_cap_gpu_destroy(&ctx);
}
#endif /* HL_ENABLE_GPU */

/* ── Audit convergence §7.3: the compile helper process ──────────────
 * A built binary's opted-in runtime source is compiled first in a helper
 * process (cap/gpu_helper.c). These drive the supervision with the
 * HULL_TEST_GPU_HELPER hooks (crash / slow / oversize / silent / accept),
 * which act in the worker after it has its sandbox and before it compiles,
 * so they run on hosts with no GPU too; the real compile is tested when a
 * device is present. */

static HlGpuCtx *agh_ctx(HlGpuBackend *be, HlVfs *vfs)
{
    HlGpuCtx *ctx = ac_ctx(be, vfs);
    if (ctx) hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_EMBEDDED, 1, vfs);
    return ctx;
}

UTEST(hull_cap_gpu_helper, accepted_source_compiles_and_is_cached)
{
    AGH_NEED_HELPER();
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = agh_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ(0, ach_start("accept"));
    HlGpuHelperStats a, b, c;
    hl_gpu_helper_stats(&a);
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "one", AC_OTHER, strlen(AC_OTHER)));
    hl_gpu_helper_stats(&b);
    EXPECT_EQ(a.round_trips + 1, b.round_trips);
    /* The same source under another name: the verdict is cached. */
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "two", AC_OTHER, strlen(AC_OTHER)));
    hl_gpu_helper_stats(&c);
    EXPECT_EQ(b.round_trips, c.round_trips);
    EXPECT_EQ(b.cache_hits + 1, c.cache_hits);
    EXPECT_EQ(2, mock_compiles);
    /* An already-compiled name is not sent again. */
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "one", AC_NEAR, strlen(AC_NEAR)));
    EXPECT_EQ(2, mock_compiles);
    hl_gpu_helper_stop();
    mock_free(ctx);
}

UTEST(hull_cap_gpu_helper, crash_is_an_error_not_a_crash)
{
    AGH_NEED_HELPER();
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = agh_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ(0, ach_start("crash"));
    EXPECT_EQ(HL_GPU_ERR_HELPER, hl_cap_gpu_compile(ctx, -1, "c", AC_OTHER, strlen(AC_OTHER)));
    EXPECT_EQ(0, mock_compiles);
    EXPECT_EQ(0, ctx->devices[0].pipeline_count);
    /* The supervisor survives its worker's crash and takes the next one. */
    EXPECT_EQ(1, hl_gpu_helper_running());
    EXPECT_EQ(HL_GPU_ERR_HELPER, hl_cap_gpu_compile(ctx, -1, "d", AC_NEAR, strlen(AC_NEAR)));
    EXPECT_EQ(1, hl_gpu_helper_running());
    /* A worker that exits without a reply counts the same. */
    ASSERT_EQ(0, ach_start("silent"));
    EXPECT_EQ(HL_GPU_ERR_HELPER, hl_cap_gpu_compile(ctx, -1, "e", AC_OTHER, strlen(AC_OTHER)));
    EXPECT_EQ(0, mock_compiles);
    hl_gpu_helper_stop();
    mock_free(ctx);
}

UTEST(hull_cap_gpu_helper, killed_at_its_deadline)
{
    AGH_NEED_HELPER();
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = agh_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ(0, ach_start("slow"));
    hl_gpu_helper_test_set_timeouts(0, 200);
    double t0 = a40_now();
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, hl_cap_gpu_compile(ctx, -1, "s", AC_OTHER, strlen(AC_OTHER)));
    double dt = a40_now() - t0;
    printf("  helper: killed at a 200 ms compile deadline after %.2f s\n", dt);
    EXPECT_TRUE(dt < 2.5);
    EXPECT_EQ(0, mock_compiles);
    EXPECT_STREQ("compile_timeout", hl_gpu_compile_errstr(HL_GPU_ERR_TIMEOUT));
    /* The supervisor is still there, and the timed-out verdict is cached:
     * the same source is refused at once, without another 200 ms. */
    EXPECT_EQ(1, hl_gpu_helper_running());
    t0 = a40_now();
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, hl_cap_gpu_compile(ctx, -1, "s2", AC_OTHER, strlen(AC_OTHER)));
    EXPECT_TRUE(a40_now() - t0 < 0.1);
    hl_gpu_helper_test_set_timeouts(0, 0);
    hl_gpu_helper_stop();
    mock_free(ctx);
}

UTEST(hull_cap_gpu_helper, oversized_reply_refused)
{
    AGH_NEED_HELPER();
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = agh_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ(0, ach_start("oversize"));
    EXPECT_EQ(HL_GPU_ERR_HELPER, hl_cap_gpu_compile(ctx, -1, "o", AC_OTHER, strlen(AC_OTHER)));
    EXPECT_EQ(0, mock_compiles);
    EXPECT_EQ(1, hl_gpu_helper_running());
    hl_gpu_helper_stop();
    mock_free(ctx);
}

/* The worker's OS sandbox: once applied, it can neither read the user's
 * home nor reach the network (the "probe" hook tries both). Where the
 * host could not apply it (stats.unsandboxed), the server warned instead. */
UTEST(hull_cap_gpu_helper, worker_sandbox_denies_home_and_network)
{
    AGH_NEED_HELPER();
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = agh_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ(0, ach_start("probe"));
    HlGpuHelperStats a, b;
    hl_gpu_helper_stats(&a);
    int rc = hl_cap_gpu_compile(ctx, -1, "p", AC_OTHER, strlen(AC_OTHER));
    hl_gpu_helper_stats(&b);
    if (b.unsandboxed == a.unsandboxed) {
        EXPECT_EQ(HL_GPU_OK, rc);
    } else {
        printf("  helper: no OS sandbox on this host (warned)\n");
    }
#ifdef __APPLE__
    EXPECT_EQ(a.unsandboxed, b.unsandboxed);
#endif
    hl_gpu_helper_stop();
    mock_free(ctx);
}

UTEST(hull_cap_gpu_helper, lost_supervisor_fails_closed)
{
    AGH_NEED_HELPER();
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = agh_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ(0, ach_start("accept"));
    hl_gpu_helper_stop();
    EXPECT_EQ(0, hl_gpu_helper_running());
    EXPECT_EQ(HL_GPU_ERR_HELPER, hl_cap_gpu_compile(ctx, -1, "l", AC_OTHER, strlen(AC_OTHER)));
    EXPECT_EQ(0, mock_compiles);
    mock_free(ctx);
}

#ifdef HL_ENABLE_DB
/* The run stopping while the helper works sends a cancel: the worker is
 * killed and the call returns at once, whatever the helper's deadline. */
static int agh_polls;
static int agh_stop_after_polls(void *ud, int64_t units)
{
    (void)ud;
    if (units > 0) return 0;          /* the floor is paid */
    return ++agh_polls > 20;          /* ~0.2 s of 10 ms polls */
}

UTEST(hull_cap_gpu_helper, stopped_run_cancels)
{
    AGH_NEED_HELPER();
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = agh_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ(0, ach_start("slow"));
    hl_gpu_helper_test_set_timeouts(0, 10000);
    agh_polls = 0;
    HlDbBudgetBinding prev = hl_db_budget_swap(agh_stop_after_polls, NULL);
    double t0 = a40_now();
    int rc = hl_cap_gpu_compile(ctx, -1, "x", AC_OTHER, strlen(AC_OTHER));
    double dt = a40_now() - t0;
    hl_db_budget_restore(prev);
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, rc);
    EXPECT_TRUE(dt < 2.0);
    /* The supervisor answered the cancel and takes the next request. */
    EXPECT_EQ(1, hl_gpu_helper_running());
    hl_gpu_helper_test_set_timeouts(0, 0);
    hl_gpu_helper_stop();
    mock_free(ctx);
}

/* An exhausted budget refuses the round trip before it starts (the floor,
 * HL_GPU_HELPER_UNITS). */
static int agh_exhausted(void *ud, int64_t units)
{
    (void)ud;
    return units > 0;
}

UTEST(hull_cap_gpu_helper, floor_charged_before_round_trip)
{
    AGH_NEED_HELPER();
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = agh_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ(0, ach_start("accept"));
    HlGpuHelperStats a, b;
    hl_gpu_helper_stats(&a);
    HlDbBudgetBinding prev = hl_db_budget_swap(agh_exhausted, NULL);
    EXPECT_EQ(HL_GPU_ERR_TIMEOUT, hl_cap_gpu_compile(ctx, -1, "f", AC_OTHER, strlen(AC_OTHER)));
    hl_db_budget_restore(prev);
    hl_gpu_helper_stats(&b);
    EXPECT_EQ(a.round_trips, b.round_trips);
    EXPECT_EQ(0, mock_compiles);
    hl_gpu_helper_stop();
    mock_free(ctx);
}
#endif


/* ── Audit convergence §7.3 confirming audit ─────────────────────────
 * M1: the shader policy holds from GPU init, before the app's top level
 * runs (serve.c wired the context into the runtime with no policy - the
 * implicit HL_GPU_SHADERS_ANY - and set it only after the manifest was
 * applied, so a top-level gpu.compile of any source compiled in-process
 * with no helper). M2: a helper worker's memory is capped. L1: the
 * worker's stack is never above the server's compile thread's. */

/* A context over a built binary's files that never got a policy refuses
 * source it did not embed (fail closed), and still takes its own
 * shaders. */
UTEST(gpu_convergence_policy, any_mode_refuses_non_embedded_in_a_built_binary)
{
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = ac_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_ANY, 0, &vfs);
    int rc = hl_cap_gpu_compile(ctx, -1, "top", AC_OTHER, strlen(AC_OTHER));
    EXPECT_EQ(HL_GPU_ERR_POLICY, rc);
    EXPECT_STREQ("dynamic_shader_refused", hl_gpu_compile_errstr(rc));
    EXPECT_EQ(0, mock_compiles);
    EXPECT_EQ(HL_GPU_OK,
              hl_cap_gpu_compile(ctx, -1, "own", AC_EMBEDDED, strlen(AC_EMBEDDED)));
    mock_free(ctx);
}

/* The context the way serve.c sets it up: policy at GPU init over the
 * embedded app VFS, the top level runs (a compile of stored, non-embedded
 * source is refused even though the manifest will opt in: the opt-in is
 * raised only once the manifest is applied), then the manifest's
 * gpu.dynamic_shaders sends runtime source to the helper. */
UTEST(gpu_convergence_policy, top_level_compile_refused_before_manifest)
{
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = ac_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    hl_cap_gpu_init_shader_policy(ctx, &vfs);
    EXPECT_EQ(HL_GPU_SHADERS_EMBEDDED, ctx->shader_mode);
    EXPECT_EQ(0, ctx->dynamic_shaders);
    /* The app's top level. */
    int rc = hl_cap_gpu_compile(ctx, -1, "stored1", AC_OTHER, strlen(AC_OTHER));
    EXPECT_EQ(HL_GPU_ERR_POLICY, rc);
    EXPECT_STREQ("dynamic_shader_refused", hl_gpu_compile_errstr(rc));
    EXPECT_EQ(0, mock_compiles);
    EXPECT_EQ(HL_GPU_OK,
              hl_cap_gpu_compile(ctx, -1, "own", AC_EMBEDDED, strlen(AC_EMBEDDED)));
    /* The manifest applied with the opt-in: runtime source now goes to
     * the helper (none runs here, so it fails closed - never compiled
     * in-process unchecked). */
    hl_gpu_helper_stop();
    hl_gpu_helper_test_reset_cache(1);
    hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_EMBEDDED, 1, &vfs);
    EXPECT_EQ(HL_GPU_ERR_HELPER,
              hl_cap_gpu_compile(ctx, -1, "stored2", AC_OTHER, strlen(AC_OTHER)));
    EXPECT_EQ(1, mock_compiles);
    mock_free(ctx);

    /* Development (no embedded files): any source, from the start. */
    static const HlEntry dev_entries[] = { { NULL, NULL, 0 } };
    HlVfs dev;
    hl_vfs_init(&dev, dev_entries, NULL);
    ctx = ac_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    hl_cap_gpu_init_shader_policy(ctx, &dev);
    EXPECT_EQ(HL_GPU_SHADERS_DEV, ctx->shader_mode);
    EXPECT_EQ(HL_GPU_OK,
              hl_cap_gpu_compile(ctx, -1, "inline", AC_OTHER, strlen(AC_OTHER)));
    mock_free(ctx);
}

/* An unknown mode is refused, not admitted. */
UTEST(gpu_convergence_policy, unknown_mode_refused)
{
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = ac_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    hl_cap_gpu_set_shader_policy(ctx, 77, 0, &vfs);
    EXPECT_EQ(HL_GPU_ERR_POLICY,
              hl_cap_gpu_compile(ctx, -1, "x", AC_EMBEDDED, strlen(AC_EMBEDDED)));
    mock_free(ctx);
}

#ifndef AGH_NO_HELPER
/* M2: a worker that allocates past its memory cap (the "alloc" hook: no
 * shader involved) is stopped - killed by the supervisor on macOS, refused
 * its allocation by RLIMIT_DATA on Linux - and the source is refused with
 * shader_helper_failed; the server and the supervisor carry on. */
UTEST(gpu_convergence_policy, helper_memory_cap_refuses)
{
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = agh_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ(0, ach_start("alloc"));
    hl_gpu_helper_test_set_mem_cap((uint64_t)256 << 20);
    double t0 = a40_now();
    int rc = hl_cap_gpu_compile(ctx, -1, "m", AC_OTHER, strlen(AC_OTHER));
    double dt = a40_now() - t0;
    EXPECT_EQ(HL_GPU_ERR_HELPER, rc);
    EXPECT_STREQ("shader_helper_failed", hl_gpu_compile_errstr(rc));
    EXPECT_EQ(0, mock_compiles);
    EXPECT_EQ(0, ctx->devices[0].pipeline_count);
    printf("  helper: worker past a 256 MiB memory cap stopped after %.2f s\n", dt);
    EXPECT_TRUE(dt < 5.0);
    /* The supervisor takes the next request. */
    EXPECT_EQ(1, hl_gpu_helper_running());
    hl_gpu_helper_test_set_mem_cap(0);
    hl_gpu_helper_stop();
    mock_free(ctx);
}

/* The supervisor reports the worker's peak memory (the measurement the cap
 * is sized from: docs/design/gpu_compile_helper.md section 5). */
UTEST(gpu_convergence_policy, helper_reports_worker_peak)
{
    HlGpuBackend be;
    HlVfs vfs;
    HlGpuCtx *ctx = agh_ctx(&be, &vfs);
    ASSERT_TRUE(ctx != NULL);
    ASSERT_EQ(0, ach_start("accept"));
    HlGpuHelperStats st;
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "p", AC_OTHER, strlen(AC_OTHER)));
    hl_gpu_helper_stats(&st);
    EXPECT_TRUE(st.peak_bytes > 0);
    EXPECT_TRUE(st.peak_bytes < HL_GPU_HELPER_MEM_BYTES);
    hl_gpu_helper_stop();
    mock_free(ctx);
}

/* L1: the worker's stack is part of the verdict: one checked on a smaller
 * stack is a different verdict (its own round trip), and the server asks
 * for no more than its compile thread can get. */
UTEST(gpu_convergence_policy, helper_stack_follows_the_server)
{
    size_t avail = hl_gpu_compile_stack_available();
    EXPECT_TRUE(avail >= ((size_t)64 << 20));
    ASSERT_EQ(0, ach_start("accept"));
    HlGpuHelperStats a, b, c;
    hl_gpu_helper_stats(&a);
    EXPECT_EQ(HL_GPU_HELPER_OK,
              hl_gpu_helper_check(AC_OTHER, strlen(AC_OTHER), 0, "dev", 0));
    hl_gpu_helper_stats(&b);
    EXPECT_EQ(a.round_trips + 1, b.round_trips);
    EXPECT_EQ(HL_GPU_HELPER_OK,
              hl_gpu_helper_check(AC_OTHER, strlen(AC_OTHER), 0, "dev",
                                  (size_t)64 << 20));
    hl_gpu_helper_stats(&c);
    EXPECT_EQ(b.round_trips + 1, c.round_trips);
    /* A stack above the helper's own cap is the cap: the same verdict. */
    EXPECT_EQ(HL_GPU_HELPER_OK,
              hl_gpu_helper_check(AC_OTHER, strlen(AC_OTHER), 0, "dev",
                                  (size_t)4 << 30));
    hl_gpu_helper_stats(&a);
    EXPECT_EQ(c.round_trips, a.round_trips);
    hl_gpu_helper_stop();
}
#endif /* !AGH_NO_HELPER */

#ifdef HL_ENABLE_GPU
/* A real compile through the helper, on the device: a valid shader is
 * compiled by the helper and then in-process; an invalid one is refused by
 * the helper, never compiled here. Prints the measurements the design doc
 * quotes. */
UTEST(hull_cap_gpu_helper, real_compile_on_device)
{
    HlGpuCtx *ctx = calloc(1, sizeof *ctx);
    ASSERT_TRUE(ctx != NULL);
    if (!gpu_test_init(ctx)) { free(ctx); ASSERT_TRUE(1); return; }
    HlVfs vfs;
    hl_vfs_init(&vfs, AC_ENTRIES, NULL);
    hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_EMBEDDED, 1, &vfs);
    ASSERT_EQ(0, ach_start(NULL));
    double t0 = a40_now();
    int hs = hl_gpu_helper_check(A42_WARP, strlen(A42_WARP), 0, ctx->devices[0].name, 0);
    double helper_t = a40_now() - t0;
    EXPECT_EQ(HL_GPU_HELPER_OK, hs);
    /* The in-process compile of the source the helper compiled. */
    t0 = a40_now();
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "warp", A42_WARP, strlen(A42_WARP)));
    double server_t = a40_now() - t0;
    printf("  helper: domain-warp shader (%zu bytes): helper round trip %.3f s, "
           "server compile after it %.3f s\n", strlen(A42_WARP), helper_t, server_t);
    /* A small shader: the round trip is mostly the worker's start-up. */
    static const char small[] =
        "@group(0) @binding(0) var<storage,read_write> d: array<f32>;\n"
        "@compute @workgroup_size(1) fn main(@builtin(global_invocation_id) id: vec3<u32>)"
        "{ d[id.x] = d[id.x] * 5.0; }\n";
    hl_gpu_helper_test_reset_cache(1);
    t0 = a40_now();
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "small", small, strlen(small)));
    double small_t = a40_now() - t0;
    t0 = a40_now();
    EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, "small2", small, strlen(small)));
    double cached_t = a40_now() - t0;
    printf("  helper: small shader: helper + server %.3f s; cached verdict + "
           "server %.4f s\n", small_t, cached_t);
    /* An invalid shader: refused by the helper, no pipeline here. */
    int before = ctx->devices[0].pipeline_count;
    static const char bad[] =
        "@compute @workgroup_size(1) fn main() { let x: u32 = undefined_name; }\n";
    EXPECT_EQ(HL_GPU_ERR_SHADER, hl_cap_gpu_compile(ctx, -1, "bad", bad, strlen(bad)));
    EXPECT_EQ(before, ctx->devices[0].pipeline_count);
#ifdef __APPLE__
    /* The worker compiled under its Seatbelt profile. */
    HlGpuHelperStats st;
    hl_gpu_helper_stats(&st);
    EXPECT_EQ(0u, st.unsandboxed);
#endif
    hl_gpu_helper_stop();
    hl_cap_gpu_destroy(ctx);
    free(ctx);
}

/* What the server's own compile costs once the helper has compiled the
 * same source, against a cold in-process compile of a source no process
 * has compiled: each run makes its sources unique (a constant derived from
 * the clock), so no shader cache has seen them. Prints, asserts nothing
 * but success: the design doc quotes it. */
static char *agh_unique_warp(unsigned tag)
{
    /* The tag goes into a constant the shader uses, so the generated
     * platform code differs too (naga drops an unused declaration). */
    static const char pat[] = "1.79284291400159";
    const char *at = strstr(A42_WARP, pat);
    if (!at) return NULL;
    size_t n = sizeof A42_WARP + 64;
    char *s = malloc(n);
    if (!s) return NULL;
    snprintf(s, n, "%.*s1.79284%u%s", (int)(at - A42_WARP), A42_WARP, tag,
             at + sizeof pat - 1);
    return s;
}

UTEST(hull_cap_gpu_helper, server_residual_measured)
{
    HlGpuCtx *ctx = calloc(1, sizeof *ctx);
    ASSERT_TRUE(ctx != NULL);
    if (!gpu_test_init(ctx)) { free(ctx); ASSERT_TRUE(1); return; }
    unsigned base = (unsigned)(a40_now() * 1000.0);
    double cold = 0, helper = 0, after = 0;
    const int rounds = 3;
    HlVfs vfs;
    hl_vfs_init(&vfs, AC_ENTRIES, NULL);
    ASSERT_EQ(0, ach_start(NULL));
    for (int i = 0; i < rounds; i++) {
        char name[32];
        /* Cold, in-process, no helper. */
        char *a = agh_unique_warp(base + 2u * (unsigned)i);
        ASSERT_TRUE(a != NULL);
        hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_ANY, 0, NULL);
        snprintf(name, sizeof name, "cold%d", i);
        double t0 = a40_now();
        EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, name, a, strlen(a)));
        cold += a40_now() - t0;
        free(a);
        /* Through the helper, then in-process. */
        char *b = agh_unique_warp(base + 2u * (unsigned)i + 1u);
        ASSERT_TRUE(b != NULL);
        hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_EMBEDDED, 1, &vfs);
        t0 = a40_now();
        EXPECT_EQ(HL_GPU_HELPER_OK,
                  hl_gpu_helper_check(b, strlen(b), 0, ctx->devices[0].name, 0));
        helper += a40_now() - t0;
        snprintf(name, sizeof name, "warm%d", i);
        t0 = a40_now();
        EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, name, b, strlen(b)));
        after += a40_now() - t0;
        free(b);
    }
    printf("  helper: domain-warp, mean of %d unique sources: cold in-process "
           "compile %.3f s; helper round trip %.3f s; server compile after the "
           "helper %.3f s\n", rounds, cold / rounds, helper / rounds,
           after / rounds);
    /* A large shader near the audit-40 function-size bound (a40_stores
     * makes each source unique). */
    cold = helper = after = 0;
    size_t per = (HL_GPU_WGSL_FN_WORDS_MAX - 16) / 5 - 3;
    for (int i = 0; i < 2; i++) {
        char name[32];
        char *a = a40_stores(2, per);
        ASSERT_TRUE(a != NULL);
        hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_ANY, 0, NULL);
        snprintf(name, sizeof name, "bigcold%d", i);
        double t0 = a40_now();
        EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, name, a, strlen(a)));
        cold += a40_now() - t0;
        free(a);
        char *b = a40_stores(2, per);
        ASSERT_TRUE(b != NULL);
        hl_cap_gpu_set_shader_policy(ctx, HL_GPU_SHADERS_EMBEDDED, 1, &vfs);
        t0 = a40_now();
        EXPECT_EQ(HL_GPU_HELPER_OK,
                  hl_gpu_helper_check(b, strlen(b), 0, ctx->devices[0].name, 0));
        helper += a40_now() - t0;
        snprintf(name, sizeof name, "bigwarm%d", i);
        t0 = a40_now();
        EXPECT_EQ(HL_GPU_OK, hl_cap_gpu_compile(ctx, -1, name, b, strlen(b)));
        after += a40_now() - t0;
        free(b);
    }
    printf("  helper: large shader (2 x %zu stores), mean of 2: cold in-process "
           "compile %.3f s; helper round trip %.3f s; server compile after the "
           "helper %.3f s\n", per, cold / 2, helper / 2, after / 2);
    hl_gpu_helper_stop();
    hl_cap_gpu_destroy(ctx);
    free(ctx);
}

/* M2: the worker's peak memory compiling the largest real shaders - the
 * test suite's (the audit-40 function-size shader, the audit-42
 * domain-warp shader) and the examples' shaders/ (read from the source
 * tree when the test runs from the repository root) - which
 * HL_GPU_HELPER_MEM_BYTES is sized from. Prints the measurement the design
 * doc quotes; asserts every compile stayed under the cap. */
static char *agh_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(65536);
    size_t n = buf ? fread(buf, 1, 65535, f) : 0;
    fclose(f);
    if (!buf) return NULL;
    buf[n] = '\0';
    return buf;
}

UTEST(gpu_convergence_policy, worker_peak_measured)
{
    HlGpuCtx *ctx = calloc(1, sizeof *ctx);
    ASSERT_TRUE(ctx != NULL);
    if (!gpu_test_init(ctx)) { free(ctx); ASSERT_TRUE(1); return; }
    ASSERT_EQ(0, ach_start(NULL));
    static const char *const examples[] = {
        "examples/gpu_search/shaders/cosine_similarity.wgsl",
        "examples/gpu_pipeline/shaders/normalize.wgsl",
        "examples/gpu_texture/shaders/invert.wgsl",
        "examples/gpu_pipeline/shaders/reduce.wgsl",
        "examples/gpu_pipeline/shaders/weight.wgsl",
        "examples/compute_gpu_chain/shaders/double.wgsl",
    };
    uint64_t worst = 0;
    HlGpuHelperStats st;
    size_t per = (HL_GPU_WGSL_FN_WORDS_MAX - 16) / 5 - 3;
    char *big = a40_stores(2, per);
    ASSERT_TRUE(big != NULL);
    struct { const char *what; const char *src; } cases[] = {
        { "audit-40 large (2 x stores)", big },
        { "audit-42 domain warp", A42_WARP },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        EXPECT_EQ(HL_GPU_HELPER_OK,
                  hl_gpu_helper_check(cases[i].src, strlen(cases[i].src), 0,
                                      ctx->devices[0].name, 0));
        hl_gpu_helper_stats(&st);
        printf("  helper: worker peak %.1f MiB: %s (%zu bytes)\n",
               (double)st.last_peak_bytes / (1 << 20), cases[i].what,
               strlen(cases[i].src));
        if (st.last_peak_bytes > worst) worst = st.last_peak_bytes;
    }
    free(big);
    for (size_t i = 0; i < sizeof examples / sizeof examples[0]; i++) {
        char *src = agh_read_file(examples[i]);
        if (!src) continue;
        EXPECT_EQ(HL_GPU_HELPER_OK,
                  hl_gpu_helper_check(src, strlen(src), 0,
                                      ctx->devices[0].name, 0));
        hl_gpu_helper_stats(&st);
        printf("  helper: worker peak %.1f MiB: %s\n",
               (double)st.last_peak_bytes / (1 << 20), examples[i]);
        if (st.last_peak_bytes > worst) worst = st.last_peak_bytes;
        free(src);
    }
    printf("  helper: worst worker peak %.1f MiB (cap %.0f MiB)\n",
           (double)worst / (1 << 20), (double)HL_GPU_HELPER_MEM_BYTES / (1 << 20));
    EXPECT_TRUE(worst < HL_GPU_HELPER_MEM_BYTES);
    hl_gpu_helper_stop();
    hl_cap_gpu_destroy(ctx);
    free(ctx);
}
#endif /* HL_ENABLE_GPU */

/* The test binary is also the compile helper: hl_gpu_helper_start(NULL)
 * spawns this executable with HL_GPU_HELPER_ARGV1, as a built binary
 * spawns itself. */
UTEST_STATE();
int main(int argc, const char *const argv[])
{
    if (argc >= 2 && strcmp(argv[1], HL_GPU_HELPER_ARGV1) == 0)
        return hl_gpu_helper_main(argc >= 3 ? argv[2] : NULL);
    return utest_main(argc, argv);
}
