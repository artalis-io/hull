/*
 * gpu_mock.h - an in-memory GPU backend for the runtime tests.
 *
 * The base build carries the generic GPU layer (cap/gpu.c) and both runtimes'
 * gpu bindings; only the wgpu backend is a feature. With this backend behind
 * an HlGpuCtx the bindings run on every CI runner, adapter or not: device
 * selection, create / destroy, read-back, the shape of dispatch and pipeline
 * results, and the dispatch layer's live-bytes accounting.
 *
 * Buffers and textures keep their bytes, so what is written reads back. A
 * dispatch is an identity shader: its output is the bytes of the buffer it
 * names (inline data, a same-named buffer of an earlier stage, or a persistent
 * buffer), padded with zeros to the buffer's size. The read-back limits wgpu
 * enforces (max_output, result_hold; gpu_wgpu.c) are mirrored, so the
 * bindings' handling of them is exercised too - the backend's own copy of
 * that check is covered only where wgpu runs.
 *
 * Header-only, for one test TU at a time. A store bigger than
 * GPU_MOCK_STORE_MAX is not backed (reads as zeros): the dispatch-layer caps
 * are tested with sizes no test should allocate.
 */
#ifndef HULL_TESTS_GPU_MOCK_H
#define HULL_TESTS_GPU_MOCK_H

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hull/cap/gpu.h"
#include "hull/shared/offheap.h"

#define GPU_MOCK_STORE_MAX ((size_t)16 << 20)

/* Calls the backend saw, for tests that assert one was or was not made. */
static int gpu_mock_buffer_creates, gpu_mock_texture_creates;
static int gpu_mock_dispatches, gpu_mock_compiles;

typedef struct {
    unsigned char *bytes;   /* NULL past GPU_MOCK_STORE_MAX */
    size_t         len;
} GpuMockStore;

static GpuMockStore *gpu_mock_store_new(size_t len)
{
    GpuMockStore *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->len = len;
    if (len && len <= GPU_MOCK_STORE_MAX) {
        s->bytes = calloc(1, len);
        if (!s->bytes) { free(s); return NULL; }
    }
    return s;
}

static void gpu_mock_store_free(GpuMockStore *s)
{
    if (!s) return;
    free(s->bytes);
    free(s);
}

static int gpu_mock_store_copy(const GpuMockStore *s, void **data, size_t *len)
{
    *data = calloc(1, s->len ? s->len : 1);
    if (!*data) return HL_GPU_ERR_INTERNAL;
    if (s->bytes) memcpy(*data, s->bytes, s->len);
    *len = s->len;
    return HL_GPU_OK;
}

static int gpu_mock_compile(HlGpuDevice *dev, const char *name,
                            const char *wgsl, size_t wgsl_len, HlGpuPipeline *out)
{
    (void)dev; (void)name; (void)wgsl;
    gpu_mock_compiles++;
    if (wgsl_len == 0) return HL_GPU_ERR_SHADER;
    out->handle = (void *)1;
    return HL_GPU_OK;
}

static void gpu_mock_pipeline_destroy(HlGpuDevice *dev, HlGpuPipeline *p)
{
    (void)dev;
    p->handle = NULL;
}

static int gpu_mock_buffer_create(HlGpuDevice *dev, const char *name,
                                  size_t size, int usage, HlGpuBuffer *out)
{
    (void)dev; (void)name; (void)usage;
    gpu_mock_buffer_creates++;
    out->handle = gpu_mock_store_new(size);
    return out->handle ? HL_GPU_OK : HL_GPU_ERR_BUFFER;
}

static int gpu_mock_buffer_write(HlGpuDevice *dev, HlGpuBuffer *buf,
                                 const void *data, size_t len, size_t offset)
{
    (void)dev;
    GpuMockStore *s = buf->handle;
    if (!s || offset > s->len || len > s->len - offset) return HL_GPU_ERR_BUFFER;
    if (s->bytes && len) memcpy(s->bytes + offset, data, len);
    return HL_GPU_OK;
}

static int gpu_mock_buffer_read(HlGpuDevice *dev, HlGpuBuffer *buf,
                                void **data, size_t *len)
{
    (void)dev;
    return gpu_mock_store_copy(buf->handle, data, len);
}

static void gpu_mock_buffer_destroy(HlGpuDevice *dev, HlGpuBuffer *buf)
{
    (void)dev;
    gpu_mock_store_free(buf->handle);
    buf->handle = NULL;
}

static int gpu_mock_buffer_copy(HlGpuDevice *dev, HlGpuBuffer *src,
                                HlGpuBuffer *dst, size_t src_offset,
                                size_t dst_offset, size_t size)
{
    (void)dev;
    GpuMockStore *a = src->handle, *b = dst->handle;
    if (!a || !b || src_offset > a->len || size > a->len - src_offset ||
        dst_offset > b->len || size > b->len - dst_offset)
        return HL_GPU_ERR_BUFFER;
    if (a->bytes && b->bytes && size)
        memmove(b->bytes + dst_offset, a->bytes + src_offset, size);
    return HL_GPU_OK;
}

static int gpu_mock_texture_create(HlGpuDevice *dev, uint32_t w, uint32_t h,
                                   HlGpuTexFormat fmt, int storage, int filter,
                                   int au, int av, HlGpuTexture *out)
{
    (void)dev; (void)filter; (void)au; (void)av;
    gpu_mock_texture_creates++;
    out->handle = gpu_mock_store_new(hl_gpu_tex_bytes(w, h, fmt));
    if (!out->handle) return HL_GPU_ERR_BUFFER;
    out->width = w;
    out->height = h;
    out->format = fmt;
    out->storage = storage;
    return HL_GPU_OK;
}

static int gpu_mock_texture_write(HlGpuDevice *dev, HlGpuTexture *tex,
                                  const void *data, size_t len)
{
    (void)dev;
    GpuMockStore *s = tex->handle;
    if (!s || len > s->len) return HL_GPU_ERR_BUFFER;
    if (s->bytes && len) memcpy(s->bytes, data, len);
    return HL_GPU_OK;
}

static int gpu_mock_texture_read(HlGpuDevice *dev, HlGpuTexture *tex,
                                 void **data, size_t *len)
{
    (void)dev;
    return gpu_mock_store_copy(tex->handle, data, len);
}

static void gpu_mock_texture_destroy(HlGpuDevice *dev, HlGpuTexture *tex)
{
    (void)dev;
    gpu_mock_store_free(tex->handle);
    tex->handle = NULL;
}

/* The bytes a buffer descriptor names, as a fresh zero-padded copy of
 * `desc->size` (or the persistent buffer's size). `stages` lets a pipeline
 * buffer with no data of its own take the data an earlier stage gave a
 * buffer of the same name. */
static int gpu_mock_desc_bytes(const HlGpuBufferDesc *desc,
                               const HlGpuPipelineStage *stages, int stage_count,
                               const HlGpuBuffer *pb, int pc,
                               void **out, size_t *out_len)
{
    const void *src = desc->data;
    size_t src_len = desc->data_len, size = desc->size;
    if (!src && desc->name) {
        for (int s = 0; s < stage_count && !src; s++)
            for (int b = 0; b < stages[s].buffer_count && !src; b++) {
                const HlGpuBufferDesc *o = &stages[s].buffers[b];
                if (o->name && o->data && strcmp(o->name, desc->name) == 0) {
                    src = o->data;
                    src_len = o->data_len;
                    if (o->size > size) size = o->size;
                }
            }
        for (int i = 0; i < pc && !src; i++)
            if (strcmp(pb[i].name, desc->name) == 0) {
                const GpuMockStore *st = pb[i].handle;
                src = st ? st->bytes : NULL;
                src_len = st ? st->len : 0;
                size = pb[i].size;
            }
    }
    if (size < src_len) size = src_len;
    *out = calloc(1, size ? size : 1);
    if (!*out) return HL_GPU_ERR_INTERNAL;
    if (src && src_len) memcpy(*out, src, src_len);
    *out_len = size;
    return HL_GPU_OK;
}

static size_t gpu_mock_desc_size(const HlGpuBufferDesc *d,
                                 const HlGpuBuffer *pb, int pc)
{
    if (!d->data && d->name)
        for (int i = 0; i < pc; i++)
            if (strcmp(pb[i].name, d->name) == 0) return pb[i].size;
    return d->size > d->data_len ? d->size : d->data_len;
}

static int gpu_mock_dispatch(HlGpuDevice *dev, HlGpuPipeline *pipeline,
                             const HlGpuDispatchOpts *opts,
                             const HlGpuBuffer *pb, int pc,
                             const HlGpuTexture *pt, int ptc,
                             void **output, size_t *output_len,
                             const char **err_msg)
{
    (void)dev; (void)pipeline; (void)pt; (void)ptc;
    gpu_mock_dispatches++;
    if (!output || opts->output_buffer == HL_GPU_OUTPUT_NONE) return HL_GPU_OK;
    int oi = opts->output_buffer;
    if (oi < 0 || oi >= opts->buffer_count) {
        if (err_msg) *err_msg = "output_buffer_index_out_of_range";
        return HL_GPU_ERR_DISPATCH;
    }
    size_t n = gpu_mock_desc_size(&opts->buffers[oi], pb, pc);
    if (opts->max_output && n > opts->max_output) {
        if (err_msg) *err_msg = "output_too_large";
        return HL_GPU_ERR_READBACK;
    }
    if (hl_offheap_shared_take(opts->result_hold, n) != 0) {
        if (err_msg) *err_msg = "result too large";
        return HL_GPU_ERR_READBACK;
    }
    return gpu_mock_desc_bytes(&opts->buffers[oi], NULL, 0, pb, pc,
                               output, output_len);
}

static int gpu_mock_dispatch_pipeline(HlGpuDevice *dev, HlGpuPipeline **pipelines,
                                      int stage_count, const HlGpuPipelineOpts *opts,
                                      const HlGpuBuffer *pb, int pc,
                                      const HlGpuTexture *pt, int ptc,
                                      HlGpuPipelineResult *result,
                                      const char **err_msg)
{
    (void)dev; (void)pipelines; (void)pt; (void)ptc;
    gpu_mock_dispatches++;
    HlGpuPipelineOutput last = { stage_count - 1, 0 };
    const HlGpuPipelineOutput *outs = opts->output_count ? opts->outputs : &last;
    int n = opts->output_count ? opts->output_count : 1;
    size_t total = 0;
    result->count = 0;
    for (int i = 0; i < n && i < HL_GPU_MAX_PIPELINE_OUTPUTS; i++) {
        int s = outs[i].stage, b = outs[i].buffer;
        if (s < 0 || s >= stage_count || b < 0 || b >= opts->stages[s].buffer_count) {
            if (err_msg) *err_msg = "output_buffer_not_found";
            return HL_GPU_ERR_DISPATCH;
        }
        const HlGpuBufferDesc *d = &opts->stages[s].buffers[b];
        size_t sz = gpu_mock_desc_size(d, pb, pc);
        if (opts->max_output &&
            (sz > opts->max_output || total > opts->max_output - sz)) {
            if (err_msg) *err_msg = "output_too_large";
            return HL_GPU_ERR_READBACK;
        }
        total += sz;
        if (hl_offheap_shared_take(opts->result_hold, sz) != 0) {
            if (err_msg) *err_msg = "result too large";
            return HL_GPU_ERR_READBACK;
        }
        int rc = gpu_mock_desc_bytes(d, opts->stages, stage_count, pb, pc,
                                     &result->data[i], &result->len[i]);
        if (rc != HL_GPU_OK) return rc;
        result->count = i + 1;
    }
    return HL_GPU_OK;
}

static const HlGpuBackend GPU_MOCK_BACKEND = {
    .name              = "mock",
    .compile           = gpu_mock_compile,
    .pipeline_destroy  = gpu_mock_pipeline_destroy,
    .dispatch          = gpu_mock_dispatch,
    .dispatch_pipeline = gpu_mock_dispatch_pipeline,
    .buffer_copy       = gpu_mock_buffer_copy,
    .buffer_create     = gpu_mock_buffer_create,
    .buffer_write      = gpu_mock_buffer_write,
    .buffer_read       = gpu_mock_buffer_read,
    .buffer_destroy    = gpu_mock_buffer_destroy,
    .texture_create    = gpu_mock_texture_create,
    .texture_write     = gpu_mock_texture_write,
    .texture_read      = gpu_mock_texture_read,
    .texture_destroy   = gpu_mock_texture_destroy,
};

/* A context with `devices` devices named mock0, mock1, ... and no allowlist. */
static inline HlGpuCtx *gpu_mock_ctx(int devices)
{
    if (devices < 1 || devices > HL_GPU_MAX_DEVICES) return NULL;
    HlGpuCtx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return NULL;
    ctx->backend = &GPU_MOCK_BACKEND;
    ctx->device_count = devices;
    for (int i = 0; i < devices; i++) {
        snprintf(ctx->devices[i].name, sizeof ctx->devices[i].name, "mock%d", i);
        pthread_mutex_init(&ctx->devices[i].mutex, NULL);
    }
    gpu_mock_buffer_creates = gpu_mock_texture_creates = 0;
    gpu_mock_dispatches = gpu_mock_compiles = 0;
    return ctx;
}

/* Frees what the app left behind (the dispatch layer's own destroy expects a
 * backend `destroy`, which this one has no use for). */
static inline void gpu_mock_free(HlGpuCtx *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < ctx->device_count; i++) {
        HlGpuDevice *d = &ctx->devices[i];
        for (int b = 0; b < d->buffer_count; b++)
            gpu_mock_store_free(d->buffers[b].handle);
        for (int t = 0; t < d->texture_count; t++)
            gpu_mock_store_free(d->textures[t].handle);
        pthread_mutex_destroy(&d->mutex);
    }
    free(ctx);
}

#endif /* HULL_TESTS_GPU_MOCK_H */
