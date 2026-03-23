/*
 * gpu.c — GPU compute dispatch layer
 *
 * Resolves device index, looks up cached pipelines/buffers,
 * and calls through the backend vtable. The backend (wgpu-native,
 * Dawn, etc.) never sees Hull types — only its own handles.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_GPU

#include "hull/cap/gpu.h"
#include "hull/cap/audit.h"
#include "hull/limits.h"
#include "log.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

/* Resolve device index: -1 means default device */
static int resolve_device(HlGpuCtx *ctx, int device)
{
    if (device < 0)
        return ctx->default_device;
    return device;
}

/* Find pipeline by name in a device (linear scan — small N) */
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
        ctx->backend_ctx, ctx->devices, HL_GPU_MAX_DEVICES);

    if (ctx->device_count < 0) {
        log_warn("[hull:gpu] device enumeration failed");
        backend->destroy(ctx->backend_ctx);
        ctx->backend_ctx = NULL;
        ctx->device_count = 0;
        return HL_GPU_ERR_NOT_AVAILABLE;
    }

    /* Initialize per-device mutexes */
    for (int i = 0; i < ctx->device_count; i++)
        pthread_mutex_init(&ctx->devices[i].mutex, NULL);

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

    /* Destroy all pipelines and buffers per device */
    for (int d = 0; d < ctx->device_count; d++) {
        HlGpuDevice *dev = &ctx->devices[d];

        for (int i = 0; i < dev->pipeline_count; i++)
            ctx->backend->pipeline_destroy(dev->backend_device,
                                           &dev->pipelines[i]);

        for (int i = 0; i < dev->buffer_count; i++)
            ctx->backend->buffer_destroy(dev->backend_device,
                                         &dev->buffers[i]);

        pthread_mutex_destroy(&dev->mutex);
    }

    /* Destroy backend instance */
    if (ctx->backend_ctx)
        ctx->backend->destroy(ctx->backend_ctx);

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
    int rc = ctx->backend->compile(dev->backend_device, name,
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

    if (!ctx || !shader_name || !opts || !output || !output_len) {
        if (err_msg) *err_msg = "internal_error";
        return HL_GPU_ERR_INTERNAL;
    }

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
    int rc = ctx->backend->dispatch(dev->backend_device, pipeline, opts,
                                    dev->buffers, dev->buffer_count,
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
    int rc = ctx->backend->buffer_create(dev->backend_device, name,
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

    int rc = ctx->backend->buffer_write(dev->backend_device, buf,
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

    int rc = ctx->backend->buffer_read(dev->backend_device, buf, data, len);
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
            ctx->backend->buffer_destroy(dev->backend_device,
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

#endif /* HL_ENABLE_GPU */
