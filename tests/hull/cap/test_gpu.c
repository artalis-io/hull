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

static const HlGpuBackend MOCK_BACKEND = {
    .name              = "mock",
    .buffer_create     = mock_buffer_create,
    .buffer_read       = mock_buffer_read,
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

UTEST_MAIN();
