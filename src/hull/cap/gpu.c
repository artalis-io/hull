/*
 * gpu.c - GPU compute dispatch layer
 *
 * Resolves device index, looks up cached pipelines/buffers,
 * and calls through the backend vtable. The backend (wgpu-native,
 * Dawn, etc.) never sees Hull types - only its own handles.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* Base-resident: the generic dispatch layer is always compiled (it drives any
 * HlGpuBackend vtable and never touches a concrete backend). A base build with
 * no backend simply has ctx->backend == NULL and reports GPU unavailable; a
 * `hull build --with=gpu` feature (or an HL_ENABLE_GPU monolithic build)
 * supplies the wgpu backend via hl_gpu_feature_backends() / serve.c. */

#include "hull/cap/gpu.h"
#include "hull/cap/audit.h"
#include "hull/limits/gpu.h"
#include "hull/shared/thread_affinity.h"
#include "log.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

/* Resolve device index: -1 means default device.
 * Returns -1 if the device is out of range or restricted by allowlist. */
static int resolve_device(HlGpuCtx *ctx, int device)
{
    int idx = (device < 0) ? ctx->default_device : device;
    if (idx < 0 || idx >= ctx->device_count)
        return -1;
    if (ctx->device_restriction && !ctx->allowed_devices[idx])
        return -1;
    return idx;
}

/* Find pipeline by name in a device (linear scan - small N) */
static HlGpuPipeline *find_pipeline(HlGpuDevice *dev, const char *name)
{
    for (int i = 0; i < dev->pipeline_count; i++) {
        if (strcmp(dev->pipelines[i].name, name) == 0)
            return &dev->pipelines[i];
    }
    return NULL;
}

/* Find persistent buffer by name in a device */
static HlGpuBuffer *find_buffer(HlGpuDevice *dev, const char *name)
{
    for (int i = 0; i < dev->buffer_count; i++) {
        if (strcmp(dev->buffers[i].name, name) == 0)
            return &dev->buffers[i];
    }
    return NULL;
}

/* Find persistent texture by name in a device */
static HlGpuTexture *find_texture(HlGpuDevice *dev, const char *name)
{
    for (int i = 0; i < dev->texture_count; i++) {
        if (strcmp(dev->textures[i].name, name) == 0)
            return &dev->textures[i];
    }
    return NULL;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

int hl_cap_gpu_init(HlGpuCtx *ctx, const HlGpuBackend *backend)
{
    if (!ctx || !backend)
        return HL_GPU_ERR_INTERNAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->backend = backend;

    /* Initialize backend instance */
    int rc = backend->init(&ctx->backend_ctx);
    if (rc != HL_GPU_OK) {
        log_warn("[hull:gpu] %s backend init failed: %d", backend->name, rc);
        return rc;
    }

    /* Enumerate GPU devices */
    ctx->device_count = backend->enumerate_devices(
        ctx, ctx->devices, HL_GPU_MAX_DEVICES);

    if (ctx->device_count < 0) {
        log_warn("[hull:gpu] device enumeration failed");
        backend->destroy(ctx);
        ctx->backend_ctx = NULL;
        ctx->device_count = 0;
        return HL_GPU_ERR_NOT_AVAILABLE;
    }

    /* Initialize per-device mutexes */
    for (int i = 0; i < ctx->device_count; i++) {
        if (pthread_mutex_init(&ctx->devices[i].mutex, NULL) != 0) {
            for (int j = 0; j < i; j++)
                pthread_mutex_destroy(&ctx->devices[j].mutex);
            backend->destroy(ctx);
            ctx->backend_ctx = NULL;
            ctx->device_count = 0;
            return HL_GPU_ERR_NOT_AVAILABLE;
        }
    }

    if (ctx->device_count > 0)
        log_info("[hull:gpu] %s: %d device(s), default=%s",
                 backend->name, ctx->device_count, ctx->devices[0].name);
    else
        log_info("[hull:gpu] %s: no GPU devices available", backend->name);

    return HL_GPU_OK;
}

void hl_cap_gpu_destroy(HlGpuCtx *ctx)
{
    if (!ctx || !ctx->backend)
        return;

    /* Destroy all pipelines, buffers, and textures per device */
    for (int d = 0; d < ctx->device_count; d++) {
        HlGpuDevice *dev = &ctx->devices[d];

        for (int i = 0; i < dev->pipeline_count; i++)
            ctx->backend->pipeline_destroy(dev,
                                           &dev->pipelines[i]);

        for (int i = 0; i < dev->buffer_count; i++)
            ctx->backend->buffer_destroy(dev,
                                         &dev->buffers[i]);

        if (ctx->backend->texture_destroy) {
            for (int i = 0; i < dev->texture_count; i++)
                ctx->backend->texture_destroy(dev,
                                               &dev->textures[i]);
        }

        pthread_mutex_destroy(&dev->mutex);
    }

    /* Destroy backend instance */
    if (ctx->backend_ctx)
        ctx->backend->destroy(ctx);

    memset(ctx, 0, sizeof(*ctx));
}

int hl_cap_gpu_available(HlGpuCtx *ctx)
{
    return ctx && ctx->device_count > 0;
}

/* ── Device query ──────────────────────────────────────────────────── */

int hl_cap_gpu_device_count(HlGpuCtx *ctx)
{
    return ctx ? ctx->device_count : 0;
}

const char *hl_cap_gpu_device_name(HlGpuCtx *ctx, int device)
{
    if (!ctx || device < 0 || device >= ctx->device_count)
        return NULL;
    return ctx->devices[device].name;
}

/* ── Pipeline ──────────────────────────────────────────────────────── */

int hl_cap_gpu_compile(HlGpuCtx *ctx, int device, const char *name,
                       const char *wgsl, size_t wgsl_len)
{
    /* Shader compile is event-loop-only (gpu.compile / gpu.load bindings);
     * workers only look up already-compiled pipelines under the device lock. */
    hl_assert_on_event_loop("hl_cap_gpu_compile (gpu.compile/load)");
    if (!ctx || !name || !wgsl)
        return HL_GPU_ERR_INTERNAL;
    if (ctx->device_count == 0)
        return HL_GPU_ERR_NOT_AVAILABLE;

    int dev_idx = resolve_device(ctx, device);
    if (dev_idx < 0 || dev_idx >= ctx->device_count)
        return HL_GPU_ERR_DEVICE;

    HlGpuDevice *dev = &ctx->devices[dev_idx];
    pthread_mutex_lock(&dev->mutex);

    /* Check if already compiled */
    if (find_pipeline(dev, name)) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_OK;
    }

    /* Check capacity */
    if (dev->pipeline_count >= HL_GPU_PIPELINE_MAX) {
        pthread_mutex_unlock(&dev->mutex);
        log_error("[hull:gpu] pipeline cache full (%d)", HL_GPU_PIPELINE_MAX);
        return HL_GPU_ERR_INTERNAL;
    }

    /* Compile via backend */
    HlGpuPipeline *slot = &dev->pipelines[dev->pipeline_count];
    int rc = ctx->backend->compile(dev, name,
                                   wgsl, wgsl_len, slot);
    if (rc == HL_GPU_OK) {
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        dev->pipeline_count++;
    }

    pthread_mutex_unlock(&dev->mutex);

    if (hl_audit_enabled) {
        ShJsonWriter w = hl_audit_begin("gpu.compile");
        sh_json_write_kv_string(&w, "shader", name);
        sh_json_write_kv_int(&w, "device", dev_idx);
        sh_json_write_kv_int(&w, "rc", rc);
        hl_audit_end(&w);
    }

    return rc;
}

/* ── Dispatch ──────────────────────────────────────────────────────── */

int hl_cap_gpu_dispatch(HlGpuCtx *ctx, const char *shader_name,
                        const HlGpuDispatchOpts *opts,
                        void **output, size_t *output_len,
                        const char **err_msg)
{
    static const char *err_no_gpu = "gpu_not_available";
    static const char *err_no_dev = "invalid_device";
    static const char *err_no_shader = "shader_not_found";

    if (!ctx || !shader_name || !opts) {
        if (err_msg) *err_msg = "internal_error";
        return HL_GPU_ERR_INTERNAL;
    }
    /* output/output_len may be NULL for fire-and-forget (output_buffer == -1) */

    if (ctx->device_count == 0) {
        if (err_msg) *err_msg = err_no_gpu;
        return HL_GPU_ERR_NOT_AVAILABLE;
    }

    int dev_idx = resolve_device(ctx, opts->device);
    if (dev_idx < 0 || dev_idx >= ctx->device_count) {
        if (err_msg) *err_msg = err_no_dev;
        return HL_GPU_ERR_DEVICE;
    }

    HlGpuDevice *dev = &ctx->devices[dev_idx];
    pthread_mutex_lock(&dev->mutex);

    /* Find compiled pipeline */
    HlGpuPipeline *pipeline = find_pipeline(dev, shader_name);
    if (!pipeline) {
        pthread_mutex_unlock(&dev->mutex);
        if (err_msg) *err_msg = err_no_shader;
        return HL_GPU_ERR_NOT_FOUND;
    }

    /* Dispatch through backend */
    int rc = ctx->backend->dispatch(dev, pipeline, opts,
                                    dev->buffers, dev->buffer_count,
                                    dev->textures, dev->texture_count,
                                    output, output_len, err_msg);

    pthread_mutex_unlock(&dev->mutex);

    if (hl_audit_enabled) {
        ShJsonWriter w = hl_audit_begin("gpu.dispatch");
        sh_json_write_kv_string(&w, "shader", shader_name);
        sh_json_write_kv_int(&w, "device", dev_idx);
        sh_json_write_kv_int(&w, "wg_x", (int)opts->workgroups.x);
        sh_json_write_kv_int(&w, "wg_y", (int)opts->workgroups.y);
        sh_json_write_kv_int(&w, "wg_z", (int)opts->workgroups.z);
        sh_json_write_kv_int(&w, "rc", rc);
        hl_audit_end(&w);
    }

    return rc;
}

/* ── Pipeline (multi-stage dispatch) ────────────────────────────────── */

int hl_cap_gpu_pipeline(HlGpuCtx *ctx, const HlGpuPipelineOpts *opts,
                         HlGpuPipelineResult *result, const char **err_msg)
{
    static const char *err_internal = "internal_error";

    if (!ctx || !opts || !result || !opts->stages) {
        if (err_msg) *err_msg = err_internal;
        return HL_GPU_ERR_INTERNAL;
    }
    if (opts->stage_count <= 0 || opts->stage_count > HL_GPU_MAX_PIPELINE_STAGES) {
        if (err_msg) *err_msg = "invalid_stage_count";
        return HL_GPU_ERR_DISPATCH;
    }
    if (ctx->device_count == 0) {
        if (err_msg) *err_msg = "gpu_not_available";
        return HL_GPU_ERR_NOT_AVAILABLE;
    }

    int dev_idx = resolve_device(ctx, opts->device);
    if (dev_idx < 0 || dev_idx >= ctx->device_count) {
        if (err_msg) *err_msg = "invalid_device";
        return HL_GPU_ERR_DEVICE;
    }

    HlGpuDevice *dev = &ctx->devices[dev_idx];
    pthread_mutex_lock(&dev->mutex);

    /* Look up all shader pipelines (fail fast) */
    HlGpuPipeline *pipelines[HL_GPU_MAX_PIPELINE_STAGES];
    for (int i = 0; i < opts->stage_count; i++) {
        if (!opts->stages[i].shader) {
            pthread_mutex_unlock(&dev->mutex);
            if (err_msg) *err_msg = "stage_missing_shader";
            return HL_GPU_ERR_INTERNAL;
        }
        pipelines[i] = find_pipeline(dev, opts->stages[i].shader);
        if (!pipelines[i]) {
            pthread_mutex_unlock(&dev->mutex);
            if (err_msg) *err_msg = "shader_not_found";
            return HL_GPU_ERR_NOT_FOUND;
        }
    }

    if (!ctx->backend->dispatch_pipeline) {
        pthread_mutex_unlock(&dev->mutex);
        if (err_msg) *err_msg = "pipeline_not_supported";
        return HL_GPU_ERR_INTERNAL;
    }

    memset(result, 0, sizeof(*result));

    int rc = ctx->backend->dispatch_pipeline(
        dev, pipelines, opts->stage_count, opts,
        dev->buffers, dev->buffer_count,
        dev->textures, dev->texture_count, result, err_msg);

    pthread_mutex_unlock(&dev->mutex);

    if (hl_audit_enabled) {
        ShJsonWriter w = hl_audit_begin("gpu.pipeline");
        sh_json_write_kv_int(&w, "stages", opts->stage_count);
        sh_json_write_kv_int(&w, "outputs", result->count);
        sh_json_write_kv_int(&w, "device", dev_idx);
        sh_json_write_kv_int(&w, "rc", rc);
        hl_audit_end(&w);
    }

    return rc;
}

void hl_cap_gpu_pipeline_result_free(HlGpuPipelineResult *result)
{
    if (!result) return;
    for (int i = 0; i < result->count; i++)
        free(result->data[i]);
    memset(result, 0, sizeof(*result));
}

/* ── Persistent buffers ────────────────────────────────────────────── */

int hl_cap_gpu_buffer_create(HlGpuCtx *ctx, int device, const char *name,
                             size_t size, int usage)
{
    if (!ctx || !name || size == 0)
        return HL_GPU_ERR_INTERNAL;
    if (ctx->device_count == 0)
        return HL_GPU_ERR_NOT_AVAILABLE;

    int dev_idx = resolve_device(ctx, device);
    if (dev_idx < 0 || dev_idx >= ctx->device_count)
        return HL_GPU_ERR_DEVICE;

    HlGpuDevice *dev = &ctx->devices[dev_idx];
    pthread_mutex_lock(&dev->mutex);

    /* Check if already exists */
    if (find_buffer(dev, name)) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_OK; /* idempotent */
    }

    /* Check capacity */
    if (dev->buffer_count >= HL_GPU_BUFFER_MAX) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_INTERNAL;
    }

    HlGpuBuffer *slot = &dev->buffers[dev->buffer_count];
    int rc = ctx->backend->buffer_create(dev, name,
                                         size, usage, slot);
    if (rc == HL_GPU_OK) {
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        slot->size = size;
        slot->usage = usage;
        dev->buffer_count++;
    }

    pthread_mutex_unlock(&dev->mutex);
    return rc;
}

int hl_cap_gpu_buffer_write(HlGpuCtx *ctx, int device, const char *name,
                            const void *data, size_t len, size_t offset)
{
    if (!ctx || !name || !data)
        return HL_GPU_ERR_INTERNAL;

    int dev_idx = resolve_device(ctx, device);
    if (dev_idx < 0 || dev_idx >= ctx->device_count)
        return HL_GPU_ERR_DEVICE;

    HlGpuDevice *dev = &ctx->devices[dev_idx];
    pthread_mutex_lock(&dev->mutex);

    HlGpuBuffer *buf = find_buffer(dev, name);
    if (!buf) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_NOT_FOUND;
    }

    /* Bounds check */
    if (offset > buf->size || len > buf->size - offset) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_BUFFER;
    }

    int rc = ctx->backend->buffer_write(dev, buf,
                                        data, len, offset);
    pthread_mutex_unlock(&dev->mutex);
    return rc;
}

int hl_cap_gpu_buffer_read(HlGpuCtx *ctx, int device, const char *name,
                           void **data, size_t *len)
{
    if (!ctx || !name || !data || !len)
        return HL_GPU_ERR_INTERNAL;

    int dev_idx = resolve_device(ctx, device);
    if (dev_idx < 0 || dev_idx >= ctx->device_count)
        return HL_GPU_ERR_DEVICE;

    HlGpuDevice *dev = &ctx->devices[dev_idx];
    pthread_mutex_lock(&dev->mutex);

    HlGpuBuffer *buf = find_buffer(dev, name);
    if (!buf) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_NOT_FOUND;
    }

    int rc = ctx->backend->buffer_read(dev, buf, data, len);
    pthread_mutex_unlock(&dev->mutex);
    return rc;
}

void hl_cap_gpu_buffer_destroy(HlGpuCtx *ctx, int device, const char *name)
{
    if (!ctx || !name)
        return;

    int dev_idx = resolve_device(ctx, device);
    if (dev_idx < 0 || dev_idx >= ctx->device_count)
        return;

    HlGpuDevice *dev = &ctx->devices[dev_idx];
    pthread_mutex_lock(&dev->mutex);

    for (int i = 0; i < dev->buffer_count; i++) {
        if (strcmp(dev->buffers[i].name, name) == 0) {
            ctx->backend->buffer_destroy(dev,
                                         &dev->buffers[i]);
            /* Compact: move last element into gap */
            if (i < dev->buffer_count - 1)
                dev->buffers[i] = dev->buffers[dev->buffer_count - 1];
            dev->buffer_count--;
            break;
        }
    }

    pthread_mutex_unlock(&dev->mutex);
}

/* ── Persistent textures ───────────────────────────────────────────── */

int hl_cap_gpu_texture_create(HlGpuCtx *ctx, int device, const char *name,
                               uint32_t width, uint32_t height,
                               HlGpuTexFormat format, int storage,
                               int filter, int address_u, int address_v)
{
    if (!ctx || !name || width == 0 || height == 0)
        return HL_GPU_ERR_INTERNAL;
    if (width > HL_GPU_MAX_TEXTURE_DIM || height > HL_GPU_MAX_TEXTURE_DIM)
        return HL_GPU_ERR_BUFFER;
    if (ctx->device_count == 0)
        return HL_GPU_ERR_NOT_AVAILABLE;
    if (!ctx->backend->texture_create)
        return HL_GPU_ERR_INTERNAL;

    int dev_idx = resolve_device(ctx, device);
    if (dev_idx < 0 || dev_idx >= ctx->device_count)
        return HL_GPU_ERR_DEVICE;

    HlGpuDevice *dev = &ctx->devices[dev_idx];
    pthread_mutex_lock(&dev->mutex);

    /* Check if already exists */
    if (find_texture(dev, name)) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_OK; /* idempotent */
    }

    if (dev->texture_count >= HL_GPU_TEXTURE_MAX) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_INTERNAL;
    }

    HlGpuTexture *slot = &dev->textures[dev->texture_count];
    int rc = ctx->backend->texture_create(dev, width, height,
                                           format, storage, filter,
                                           address_u, address_v, slot);
    if (rc == HL_GPU_OK) {
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        dev->texture_count++;
    }

    pthread_mutex_unlock(&dev->mutex);
    return rc;
}

int hl_cap_gpu_texture_write(HlGpuCtx *ctx, int device, const char *name,
                              const void *data, size_t len)
{
    if (!ctx || !name || !data)
        return HL_GPU_ERR_INTERNAL;
    if (!ctx->backend->texture_write)
        return HL_GPU_ERR_INTERNAL;

    int dev_idx = resolve_device(ctx, device);
    if (dev_idx < 0 || dev_idx >= ctx->device_count)
        return HL_GPU_ERR_DEVICE;

    HlGpuDevice *dev = &ctx->devices[dev_idx];
    pthread_mutex_lock(&dev->mutex);

    HlGpuTexture *tex = find_texture(dev, name);
    if (!tex) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_NOT_FOUND;
    }

    int rc = ctx->backend->texture_write(dev, tex, data, len);
    pthread_mutex_unlock(&dev->mutex);
    return rc;
}

int hl_cap_gpu_texture_read(HlGpuCtx *ctx, int device, const char *name,
                             void **data, size_t *len,
                             uint32_t *out_width, uint32_t *out_height,
                             HlGpuTexFormat *out_format)
{
    if (!ctx || !name || !data || !len)
        return HL_GPU_ERR_INTERNAL;
    if (!ctx->backend->texture_read)
        return HL_GPU_ERR_INTERNAL;

    int dev_idx = resolve_device(ctx, device);
    if (dev_idx < 0 || dev_idx >= ctx->device_count)
        return HL_GPU_ERR_DEVICE;

    HlGpuDevice *dev = &ctx->devices[dev_idx];
    pthread_mutex_lock(&dev->mutex);

    HlGpuTexture *tex = find_texture(dev, name);
    if (!tex) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_NOT_FOUND;
    }

    int rc = ctx->backend->texture_read(dev, tex, data, len);
    if (rc == HL_GPU_OK) {
        if (out_width)  *out_width  = tex->width;
        if (out_height) *out_height = tex->height;
        if (out_format) *out_format = tex->format;
    }

    pthread_mutex_unlock(&dev->mutex);
    return rc;
}

void hl_cap_gpu_texture_destroy(HlGpuCtx *ctx, int device, const char *name)
{
    if (!ctx || !name)
        return;
    if (!ctx->backend->texture_destroy)
        return;

    int dev_idx = resolve_device(ctx, device);
    if (dev_idx < 0 || dev_idx >= ctx->device_count)
        return;

    HlGpuDevice *dev = &ctx->devices[dev_idx];
    pthread_mutex_lock(&dev->mutex);

    for (int i = 0; i < dev->texture_count; i++) {
        if (strcmp(dev->textures[i].name, name) == 0) {
            ctx->backend->texture_destroy(dev,
                                           &dev->textures[i]);
            /* Compact: move last element into gap */
            if (i < dev->texture_count - 1)
                dev->textures[i] = dev->textures[dev->texture_count - 1];
            dev->texture_count--;
            break;
        }
    }

    pthread_mutex_unlock(&dev->mutex);
}

/* ── GPU-side buffer copy ──────────────────────────────────────────── */

int hl_cap_gpu_buffer_copy(HlGpuCtx *ctx, int device,
                            const char *src_name, const char *dst_name,
                            size_t src_offset, size_t dst_offset, size_t size)
{
    if (!ctx || !src_name || !dst_name)
        return HL_GPU_ERR_INTERNAL;
    if (ctx->device_count == 0)
        return HL_GPU_ERR_NOT_AVAILABLE;

    int dev_idx = resolve_device(ctx, device);
    if (dev_idx < 0 || dev_idx >= ctx->device_count)
        return HL_GPU_ERR_DEVICE;

    HlGpuDevice *dev = &ctx->devices[dev_idx];
    pthread_mutex_lock(&dev->mutex);

    HlGpuBuffer *src = find_buffer(dev, src_name);
    HlGpuBuffer *dst = find_buffer(dev, dst_name);
    if (!src || !dst) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_NOT_FOUND;
    }

    /* size=0 means full copy from src_offset */
    size_t copy_size = size;
    if (copy_size == 0) {
        if (src_offset >= src->size) {
            pthread_mutex_unlock(&dev->mutex);
            return HL_GPU_ERR_BUFFER;
        }
        copy_size = src->size - src_offset;
    }

    /* Bounds checks */
    if (src_offset > src->size || copy_size > src->size - src_offset
        || dst_offset > dst->size || copy_size > dst->size - dst_offset) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_BUFFER;
    }

    /* Align to 4 bytes (WebGPU requirement for copy operations) */
    copy_size = (copy_size + 3) & ~(size_t)3;

    if (!ctx->backend->buffer_copy) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_INTERNAL;
    }

    int rc = ctx->backend->buffer_copy(dev,
                                        src, dst,
                                        src_offset, dst_offset, copy_size);
    pthread_mutex_unlock(&dev->mutex);

    if (hl_audit_enabled) {
        ShJsonWriter w = hl_audit_begin("gpu.buffer_copy");
        sh_json_write_kv_string(&w, "src", src_name);
        sh_json_write_kv_string(&w, "dst", dst_name);
        sh_json_write_kv_int(&w, "size", (int)copy_size);
        sh_json_write_kv_int(&w, "rc", rc);
        hl_audit_end(&w);
    }

    return rc;
}
