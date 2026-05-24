/*
 * gpu_wgpu.c — wgpu-native backend for GPU compute
 *
 * Implements the HlGpuBackend vtable using wgpu-native v27.
 * Uses auto-layout pipelines, staging buffers for readback,
 * and sync device polling.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_GPU

#include "hull/cap/gpu.h"
#include "webgpu.h"
#include "wgpu.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

/* ── GPU dispatch timeout (milliseconds, 0 = no timeout) ───────────── */

#ifndef HL_GPU_TIMEOUT_MS
#define HL_GPU_TIMEOUT_MS 5000  /* 5 seconds default */
#endif

#include <time.h>

/* Forward declarations for texture functions (defined at bottom, used in dispatch) */
static int wgpu_texture_create(void *backend_device, uint32_t width,
                                uint32_t height, HlGpuTexFormat format,
                                int storage, int filter, int address_u,
                                int address_v, HlGpuTexture *out);
static int wgpu_texture_write(void *backend_device, HlGpuTexture *tex,
                               const void *data, size_t len);
static int wgpu_texture_read(void *backend_device, HlGpuTexture *tex,
                              void **data, size_t *len);
static void wgpu_texture_destroy(void *backend_device, HlGpuTexture *tex);

static uint64_t gpu_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* Poll device until work completes or timeout expires.
 * Returns 0 on success, -1 on timeout. */
static int gpu_poll_with_timeout(WGPUDevice device, uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        wgpuDevicePoll(device, 1, NULL);
        return 0;
    }
    uint64_t deadline = gpu_now_ms() + timeout_ms;
    for (;;) {
        WGPUBool done = wgpuDevicePoll(device, 0, NULL);
        if (done) return 0;
        if (gpu_now_ms() >= deadline) return -1;
        /* Brief yield to avoid busy-spin */
        struct timespec ts = { 0, 100000 }; /* 100 µs */
        nanosleep(&ts, NULL);
    }
}

/* ── Helper: C string → WGPUStringView ─────────────────────────────── */

static WGPUStringView sv(const char *s)
{
    return (WGPUStringView){ .data = s, .length = s ? strlen(s) : 0 };
}

/* ── Internal types ────────────────────────────────────────────────── */

typedef struct WgpuDeviceCtx {
    WGPUAdapter adapter;
    WGPUDevice  device;
    WGPUQueue   queue;
} WgpuDeviceCtx;

typedef struct WgpuBackendCtx {
    WGPUInstance   instance;
    WgpuDeviceCtx *device_ctxs[HL_GPU_MAX_DEVICES];
    int            device_count;
} WgpuBackendCtx;

/* ── Error handlers (suppress wgpu-native's default panic handler) ── */

/* Track last uncaptured error per-device for shader compilation.
 * wgpu-native surfaces errors via callback, not NULL returns. */
static _Thread_local int wgpu_last_error;

static void on_uncaptured_error(WGPUDevice const *device,
                                 WGPUErrorType type,
                                 WGPUStringView message,
                                 void *userdata1, void *userdata2)
{
    (void)device; (void)type; (void)userdata1; (void)userdata2;
    wgpu_last_error = 1;
    if (message.data && message.length > 0)
        log_error("[hull:gpu:wgpu] uncaptured error: %.*s",
                  (int)message.length, message.data);
}

static void on_device_lost(WGPUDevice const *device,
                            WGPUDeviceLostReason reason,
                            WGPUStringView message,
                            void *userdata1, void *userdata2)
{
    (void)device; (void)reason; (void)userdata1; (void)userdata2;
    if (message.data && message.length > 0)
        log_warn("[hull:gpu:wgpu] device lost: %.*s",
                 (int)message.length, message.data);
}

/* ── Sync callback helpers (v27 API: two userdata pointers) ────────── */

typedef struct {
    WGPUDevice device;
    int ok;
} DeviceReq;

static void on_device_request(WGPURequestDeviceStatus status,
                               WGPUDevice device,
                               WGPUStringView message,
                               void *userdata1, void *userdata2)
{
    (void)userdata2;
    DeviceReq *r = (DeviceReq *)userdata1;
    r->device = device;
    r->ok = (status == WGPURequestDeviceStatus_Success);
    if (!r->ok && message.data && message.length > 0)
        log_warn("[hull:gpu:wgpu] device request failed: %.*s",
                 (int)message.length, message.data);
}

typedef struct {
    int done;
    WGPUMapAsyncStatus status;
} MapReq;

static void on_buffer_map(WGPUMapAsyncStatus status,
                            WGPUStringView message,
                            void *userdata1, void *userdata2)
{
    (void)message;
    (void)userdata2;
    MapReq *r = (MapReq *)userdata1;
    r->status = status;
    r->done = 1;
}

/* ── Readback helper ───────────────────────────────────────────────── */

/*
 * Copy GPU buffer contents to a malloc'd host buffer.
 * Creates a staging buffer, copies, maps, memcpy, cleans up.
 * Caller must free(*out_data).
 */
static int readback_buffer(WgpuDeviceCtx *dctx, WGPUBuffer src,
                            size_t size, void **out_data, size_t *out_len)
{
    /* Create staging buffer for MapRead */
    WGPUBufferDescriptor staging_desc = {
        .label = sv("hull_staging"),
        .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
        .size  = size,
    };
    WGPUBuffer staging = wgpuDeviceCreateBuffer(dctx->device, &staging_desc);
    if (!staging)
        return HL_GPU_ERR_READBACK;

    /* Encode copy command */
    WGPUCommandEncoderDescriptor enc_desc = { .label = sv("hull_readback_enc") };
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(
        dctx->device, &enc_desc);
    if (!encoder) {
        wgpuBufferDestroy(staging);
        wgpuBufferRelease(staging);
        return HL_GPU_ERR_READBACK;
    }
    wgpuCommandEncoderCopyBufferToBuffer(encoder, src, 0, staging, 0, size);

    WGPUCommandBufferDescriptor cmd_desc = { .label = sv("hull_readback_cmd") };
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
    if (!cmd) {
        wgpuCommandEncoderRelease(encoder);
        wgpuBufferDestroy(staging);
        wgpuBufferRelease(staging);
        return HL_GPU_ERR_READBACK;
    }

    /* Submit and poll to completion */
    wgpuQueueSubmit(dctx->queue, 1, &cmd);
    wgpuDevicePoll(dctx->device, 1, NULL);

    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(encoder);

    /* Map staging buffer (v27: uses WGPUBufferMapCallbackInfo) */
    MapReq map_req = {0};
    WGPUBufferMapCallbackInfo map_cb = {
        .mode = WGPUCallbackMode_AllowSpontaneous,
        .callback = on_buffer_map,
        .userdata1 = &map_req,
    };
    wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, size, map_cb);
    wgpuDevicePoll(dctx->device, 1, NULL);

    if (!map_req.done || map_req.status != WGPUMapAsyncStatus_Success) {
        wgpuBufferDestroy(staging);
        wgpuBufferRelease(staging);
        return HL_GPU_ERR_READBACK;
    }

    /* Copy mapped data to output */
    const void *mapped = wgpuBufferGetConstMappedRange(staging, 0, size);
    if (!mapped) {
        wgpuBufferUnmap(staging);
        wgpuBufferDestroy(staging);
        wgpuBufferRelease(staging);
        return HL_GPU_ERR_READBACK;
    }

    void *result = malloc(size);
    if (!result) {
        wgpuBufferUnmap(staging);
        wgpuBufferDestroy(staging);
        wgpuBufferRelease(staging);
        return HL_GPU_ERR_INTERNAL;
    }
    memcpy(result, mapped, size);

    wgpuBufferUnmap(staging);
    wgpuBufferDestroy(staging);
    wgpuBufferRelease(staging);

    *out_data = result;
    *out_len = size;
    return HL_GPU_OK;
}

/* ── wgpu_init ─────────────────────────────────────────────────────── */

static int wgpu_init(void **backend_ctx)
{
    WgpuBackendCtx *bctx = calloc(1, sizeof(WgpuBackendCtx));
    if (!bctx)
        return HL_GPU_ERR_INTERNAL;

    /* Create instance with primary backends (Vulkan + Metal + DX12) */
    WGPUInstanceExtras extras = {
        .chain = {
            .sType = (WGPUSType)WGPUSType_InstanceExtras,
        },
        .backends = WGPUInstanceBackend_Primary,
    };
    WGPUInstanceDescriptor inst_desc = {
        .nextInChain = &extras.chain,
    };

    bctx->instance = wgpuCreateInstance(&inst_desc);
    if (!bctx->instance) {
        free(bctx);
        return HL_GPU_ERR_NOT_AVAILABLE;
    }

    *backend_ctx = bctx;
    return HL_GPU_OK;
}

/* ── wgpu_destroy ──────────────────────────────────────────────────── */

static void wgpu_destroy(void *backend_ctx)
{
    WgpuBackendCtx *bctx = (WgpuBackendCtx *)backend_ctx;
    if (!bctx)
        return;

    for (int i = 0; i < bctx->device_count; i++) {
        WgpuDeviceCtx *dctx = bctx->device_ctxs[i];
        if (!dctx) continue;
        if (dctx->queue) wgpuQueueRelease(dctx->queue);
        if (dctx->device) wgpuDeviceRelease(dctx->device);
        if (dctx->adapter) wgpuAdapterRelease(dctx->adapter);
        free(dctx);
    }

    if (bctx->instance)
        wgpuInstanceRelease(bctx->instance);

    free(bctx);
}

/* ── wgpu_enumerate_devices ────────────────────────────────────────── */

static int wgpu_enumerate_devices(void *backend_ctx,
                                   HlGpuDevice *devices, int max_devices)
{
    WgpuBackendCtx *bctx = (WgpuBackendCtx *)backend_ctx;
    if (!bctx || !bctx->instance)
        return 0;

    /* Count available adapters */
    size_t adapter_count = wgpuInstanceEnumerateAdapters(
        bctx->instance, NULL, NULL);
    if (adapter_count == 0)
        return 0;

    /* Cap to available slots */
    if (adapter_count > (size_t)max_devices)
        adapter_count = (size_t)max_devices;
    if (adapter_count > (size_t)HL_GPU_MAX_DEVICES)
        adapter_count = (size_t)HL_GPU_MAX_DEVICES;

    /* Stack-allocate adapter array */
    WGPUAdapter adapters[HL_GPU_MAX_DEVICES];
    wgpuInstanceEnumerateAdapters(bctx->instance, NULL, adapters);

    int count = 0;
    for (size_t i = 0; i < adapter_count; i++) {
        /* Get adapter info for device name */
        WGPUAdapterInfo info = {0};
        wgpuAdapterGetInfo(adapters[i], &info);

        /* Request device (v27: uses WGPURequestDeviceCallbackInfo) */
        DeviceReq req = {0};
        WGPUDeviceDescriptor dev_desc = {
            .label = sv("hull_device"),
            .uncapturedErrorCallbackInfo = {
                .callback = on_uncaptured_error,
            },
            .deviceLostCallbackInfo = {
                .callback = on_device_lost,
            },
        };
        WGPURequestDeviceCallbackInfo cb_info = {
            .mode = WGPUCallbackMode_AllowSpontaneous,
            .callback = on_device_request,
            .userdata1 = &req,
        };
        wgpuAdapterRequestDevice(adapters[i], &dev_desc, cb_info);

        if (!req.ok) {
            wgpuAdapterInfoFreeMembers(info);
            wgpuAdapterRelease(adapters[i]);
            continue;
        }

        WGPUQueue queue = wgpuDeviceGetQueue(req.device);
        if (!queue) {
            wgpuAdapterInfoFreeMembers(info);
            wgpuDeviceRelease(req.device);
            wgpuAdapterRelease(adapters[i]);
            continue;
        }

        /* Bounds check before storing */
        if (count >= max_devices || count >= HL_GPU_MAX_DEVICES) {
            wgpuAdapterInfoFreeMembers(info);
            wgpuQueueRelease(queue);
            wgpuDeviceRelease(req.device);
            wgpuAdapterRelease(adapters[i]);
            continue;
        }

        /* Allocate per-device context */
        WgpuDeviceCtx *dctx = calloc(1, sizeof(WgpuDeviceCtx));
        if (!dctx) {
            wgpuAdapterInfoFreeMembers(info);
            wgpuQueueRelease(queue);
            wgpuDeviceRelease(req.device);
            wgpuAdapterRelease(adapters[i]);
            continue;
        }

        dctx->adapter = adapters[i];
        dctx->device = req.device;
        dctx->queue = queue;

        bctx->device_ctxs[count] = dctx;

        /* Fill Hull device entry (v27: info fields are WGPUStringView) */
        const char *dev_name = "GPU";
        if (info.device.data && info.device.length > 0)
            dev_name = info.device.data;
        else if (info.description.data && info.description.length > 0)
            dev_name = info.description.data;
        snprintf(devices[count].name, sizeof(devices[count].name),
                 "%.*s", (int)(info.device.length > 0 ? info.device.length :
                               info.description.length), dev_name);
        devices[count].backend_device = dctx;

        wgpuAdapterInfoFreeMembers(info);
        count++;
    }

    /* Release adapters that weren't claimed */
    for (size_t i = (size_t)count; i < adapter_count; i++)
        wgpuAdapterRelease(adapters[i]);

    bctx->device_count = count;
    return count;
}

/* ── wgpu_compile ──────────────────────────────────────────────────── */

static int wgpu_compile(void *backend_device, const char *name,
                         const char *wgsl, size_t wgsl_len,
                         HlGpuPipeline *out)
{
    WgpuDeviceCtx *dctx = (WgpuDeviceCtx *)backend_device;
    if (!dctx || !dctx->device)
        return HL_GPU_ERR_DEVICE;

    (void)name;

    /* Clear error flag — wgpu surfaces errors via callback, not NULL */
    wgpu_last_error = 0;

    /* Create shader module from WGSL source (v27: WGPUShaderSourceWGSL) */
    WGPUShaderSourceWGSL wgsl_desc = {
        .chain = {
            .sType = WGPUSType_ShaderSourceWGSL,
        },
        .code = { .data = wgsl, .length = wgsl_len },
    };
    WGPUShaderModuleDescriptor sm_desc = {
        .nextInChain = &wgsl_desc.chain,
        .label = sv("hull_shader"),
    };

    WGPUShaderModule shader = wgpuDeviceCreateShaderModule(
        dctx->device, &sm_desc);
    if (!shader || wgpu_last_error) {
        if (shader) wgpuShaderModuleRelease(shader);
        return HL_GPU_ERR_SHADER;
    }

    /* Create compute pipeline with auto-layout (layout = NULL) */
    WGPUComputePipelineDescriptor pipe_desc = {
        .label = sv("hull_pipeline"),
        .layout = NULL, /* auto-layout */
        .compute = {
            .module = shader,
            .entryPoint = sv("main"),
        },
    };

    WGPUComputePipeline pipeline = wgpuDeviceCreateComputePipeline(
        dctx->device, &pipe_desc);
    wgpuShaderModuleRelease(shader);

    if (!pipeline || wgpu_last_error) {
        if (pipeline) wgpuComputePipelineRelease(pipeline);
        return HL_GPU_ERR_SHADER;
    }

    out->handle = pipeline;
    out->bind_group_layout = NULL; /* set lazily at first dispatch */

    return HL_GPU_OK;
}

/* ── wgpu_pipeline_destroy ─────────────────────────────────────────── */

static void wgpu_pipeline_destroy(void *backend_device,
                                    HlGpuPipeline *pipeline)
{
    (void)backend_device;
    if (!pipeline)
        return;

    if (pipeline->bind_group_layout)
        wgpuBindGroupLayoutRelease(
            (WGPUBindGroupLayout)pipeline->bind_group_layout);
    if (pipeline->handle)
        wgpuComputePipelineRelease(
            (WGPUComputePipeline)pipeline->handle);

    pipeline->handle = NULL;
    pipeline->bind_group_layout = NULL;
}

/* ── wgpu_dispatch ─────────────────────────────────────────────────── */

static int wgpu_dispatch(void *backend_device, HlGpuPipeline *pipeline,
                          const HlGpuDispatchOpts *opts,
                          const HlGpuBuffer *persistent_buffers,
                          int persistent_count,
                          const HlGpuTexture *persistent_textures,
                          int persistent_tex_count,
                          void **output, size_t *output_len,
                          const char **err_msg)
{
    WgpuDeviceCtx *dctx = (WgpuDeviceCtx *)backend_device;
    if (!dctx || !pipeline || !pipeline->handle || !opts) {
        if (err_msg) *err_msg = "invalid_device";
        return HL_GPU_ERR_DEVICE;
    }

    int rc = HL_GPU_ERR_DISPATCH;
    WGPUBuffer uniform_buf = NULL;
    WGPUBuffer *temp_bufs = NULL;
    int temp_count = 0;
    WGPUBindGroup bind_group = NULL;
    WGPUCommandEncoder encoder = NULL;
    WGPUComputePassEncoder pass = NULL;
    WGPUCommandBuffer cmd = NULL;

    /* Inline textures created during dispatch (destroyed in cleanup) */
    HlGpuTexture inline_textures[16];
    int inline_tex_count = 0;

    /* Count texture bindings: sampled = 2 (view + sampler), storage = 1 */
    int tex_bindings = 0;
    for (int i = 0; i < opts->texture_count; i++)
        tex_bindings += opts->textures[i].storage ? 1 : 2;

    int has_uniforms = (opts->uniforms && opts->uniforms_len > 0);
    int binding_offset = has_uniforms ? 1 : 0;
    int total_bindings = opts->buffer_count + binding_offset + tex_bindings;

    if (total_bindings == 0) {
        if (err_msg) *err_msg = "no_buffers";
        return HL_GPU_ERR_DISPATCH;
    }
    if (total_bindings > 256) {
        if (err_msg) *err_msg = "too_many_bindings";
        return HL_GPU_ERR_DISPATCH;
    }

    /* Allocate entry arrays */
    WGPUBindGroupEntry *entries = calloc((size_t)total_bindings,
                                          sizeof(WGPUBindGroupEntry));
    if (!entries) {
        if (err_msg) *err_msg = "out_of_memory";
        return HL_GPU_ERR_INTERNAL;
    }

    temp_bufs = calloc((size_t)(opts->buffer_count > 0 ? opts->buffer_count : 1),
                        sizeof(WGPUBuffer));
    if (!temp_bufs) {
        free(entries);
        if (err_msg) *err_msg = "out_of_memory";
        return HL_GPU_ERR_INTERNAL;
    }

    /* ── Uniform buffer (binding 0) ────────────────────────── */

    if (has_uniforms) {
        /* Align uniform size to 16 bytes (WebGPU requirement) */
        size_t usize = (opts->uniforms_len + 15) & ~(size_t)15;
        WGPUBufferDescriptor ub_desc = {
            .label = sv("hull_uniform"),
            .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
            .size  = usize,
        };
        uniform_buf = wgpuDeviceCreateBuffer(dctx->device, &ub_desc);
        if (!uniform_buf) {
            if (err_msg) *err_msg = "uniform_buffer_create_failed";
            goto cleanup;
        }
        wgpuQueueWriteBuffer(dctx->queue, uniform_buf, 0,
                              opts->uniforms, opts->uniforms_len);

        entries[0] = (WGPUBindGroupEntry){
            .binding = 0,
            .buffer  = uniform_buf,
            .offset  = 0,
            .size    = usize,
        };
    }

    /* ── Storage buffers ───────────────────────────────────── */

    if (opts->buffer_count > 0 && !opts->buffers) {
        if (err_msg) *err_msg = "buffers_pointer_null";
        goto cleanup;
    }

    for (int i = 0; i < opts->buffer_count; i++) {
        const HlGpuBufferDesc *desc = &opts->buffers[i];
        WGPUBuffer gpu_buf = NULL;

        /* Check if this refers to a persistent buffer */
        if (desc->name && persistent_buffers) {
            for (int p = 0; p < persistent_count; p++) {
                if (persistent_buffers[p].name[0] != '\0' &&
                    strcmp(persistent_buffers[p].name, desc->name) == 0) {
                    gpu_buf = (WGPUBuffer)persistent_buffers[p].handle;
                    break;
                }
            }
        }

        if (!gpu_buf) {
            /* Create temporary buffer */
            size_t buf_size = desc->size;
            if (buf_size == 0 && desc->data)
                buf_size = 1;

            /* Align to 4 bytes */
            buf_size = (buf_size + 3) & ~(size_t)3;

            WGPUBufferDescriptor buf_desc = {
                .label = sv("hull_storage"),
                .usage = WGPUBufferUsage_Storage
                       | WGPUBufferUsage_CopyDst
                       | WGPUBufferUsage_CopySrc,
                .size  = buf_size,
            };
            gpu_buf = wgpuDeviceCreateBuffer(dctx->device, &buf_desc);
            if (!gpu_buf) {
                if (err_msg) *err_msg = "buffer_create_failed";
                goto cleanup;
            }
            temp_bufs[temp_count++] = gpu_buf;

            /* Upload initial data if present */
            if (desc->data && desc->size > 0)
                wgpuQueueWriteBuffer(dctx->queue, gpu_buf, 0,
                                      desc->data, desc->size);
        }

        uint32_t binding = (uint32_t)(i + binding_offset);
        /* Use persistent buffer's size when desc->size is 0 (name-only ref) */
        size_t effective_size = desc->size;
        if (effective_size == 0 && desc->name && persistent_buffers) {
            for (int p = 0; p < persistent_count; p++) {
                if (persistent_buffers[p].name[0] != '\0' &&
                    strcmp(persistent_buffers[p].name, desc->name) == 0) {
                    effective_size = persistent_buffers[p].size;
                    break;
                }
            }
        }
        uint64_t buf_size_aligned = effective_size <= SIZE_MAX - 3
            ? (uint64_t)((effective_size + 3) & ~(size_t)3) : (uint64_t)effective_size;
        if (buf_size_aligned == 0)
            buf_size_aligned = 4;

        entries[i + binding_offset] = (WGPUBindGroupEntry){
            .binding = binding,
            .buffer  = gpu_buf,
            .offset  = 0,
            .size    = buf_size_aligned,
        };
    }

    /* ── Texture entries ─────────────────────────────────────── */

    {
        int tex_binding = opts->buffer_count + binding_offset;
        for (int i = 0; i < opts->texture_count; i++) {
            const HlGpuTextureDesc *td = &opts->textures[i];
            const HlGpuTexture *tex = NULL;

            /* Look up persistent texture by name */
            if (td->name && persistent_textures) {
                for (int p = 0; p < persistent_tex_count; p++) {
                    if (persistent_textures[p].name[0] != '\0' &&
                        strcmp(persistent_textures[p].name, td->name) == 0) {
                        tex = &persistent_textures[p];
                        break;
                    }
                }
            }

            /* Create inline texture if not persistent */
            if (!tex && td->width > 0 && td->height > 0 &&
                inline_tex_count < 16) {
                HlGpuTexture *itex = &inline_textures[inline_tex_count];
                memset(itex, 0, sizeof(*itex));
                int trc = wgpu_texture_create(backend_device, td->width,
                    td->height, td->format, td->storage,
                    HL_GPU_FILTER_NEAREST, HL_GPU_ADDRESS_CLAMP,
                    HL_GPU_ADDRESS_CLAMP, itex);
                if (trc != HL_GPU_OK) {
                    if (err_msg) *err_msg = "texture_create_failed";
                    goto cleanup;
                }
                itex->storage = td->storage;
                if (td->data && td->data_len > 0) {
                    trc = wgpu_texture_write(backend_device, itex,
                                              td->data, td->data_len);
                    if (trc != HL_GPU_OK) {
                        wgpu_texture_destroy(backend_device, itex);
                        if (err_msg) *err_msg = "texture_write_failed";
                        goto cleanup;
                    }
                }
                inline_tex_count++;
                tex = itex;
            }

            if (!tex) {
                if (err_msg) *err_msg = "texture_not_found";
                goto cleanup;
            }

            if (tex->storage) {
                /* Storage texture: single binding (view only) */
                entries[tex_binding] = (WGPUBindGroupEntry){
                    .binding = (uint32_t)tex_binding,
                    .textureView = (WGPUTextureView)tex->view,
                };
                tex_binding++;
            } else {
                /* Sampled texture: view + sampler */
                entries[tex_binding] = (WGPUBindGroupEntry){
                    .binding = (uint32_t)tex_binding,
                    .textureView = (WGPUTextureView)tex->view,
                };
                tex_binding++;
                entries[tex_binding] = (WGPUBindGroupEntry){
                    .binding = (uint32_t)tex_binding,
                    .sampler = (WGPUSampler)tex->sampler,
                };
                tex_binding++;
            }
        }
    }

    /* ── Bind group ────────────────────────────────────────── */

    {
        /* Get bind group layout lazily (deferred from compile to handle
         * shaders with zero bindings — GetBindGroupLayout(0) panics
         * when there are no bind groups). */
        WGPUBindGroupLayout layout = (WGPUBindGroupLayout)pipeline->bind_group_layout;
        if (!layout) {
            layout = wgpuComputePipelineGetBindGroupLayout(
                (WGPUComputePipeline)pipeline->handle, 0);
            if (!layout) {
                if (err_msg) *err_msg = "bind_group_layout_failed";
                goto cleanup;
            }
            pipeline->bind_group_layout = layout;
        }

        WGPUBindGroupDescriptor bg_desc = {
            .label = sv("hull_bind_group"),
            .layout = layout,
            .entryCount = (size_t)total_bindings,
            .entries = entries,
        };
        bind_group = wgpuDeviceCreateBindGroup(dctx->device, &bg_desc);
        if (!bind_group) {
            if (err_msg) *err_msg = "bind_group_create_failed";
            goto cleanup;
        }
    }

    /* ── Compute pass ──────────────────────────────────────── */

    {
        WGPUCommandEncoderDescriptor enc_desc = {
            .label = sv("hull_dispatch_enc"),
        };
        encoder = wgpuDeviceCreateCommandEncoder(dctx->device, &enc_desc);
        if (!encoder) {
            if (err_msg) *err_msg = "encoder_create_failed";
            goto cleanup;
        }

        WGPUComputePassDescriptor pass_desc = {
            .label = sv("hull_compute_pass"),
        };
        pass = wgpuCommandEncoderBeginComputePass(encoder, &pass_desc);
        if (!pass) {
            if (err_msg) *err_msg = "compute_pass_create_failed";
            goto cleanup;
        }
        wgpuComputePassEncoderSetPipeline(
            pass, (WGPUComputePipeline)pipeline->handle);
        wgpuComputePassEncoderSetBindGroup(pass, 0, bind_group, 0, NULL);
        wgpuComputePassEncoderDispatchWorkgroups(
            pass,
            opts->workgroups.x > 0 ? opts->workgroups.x : 1,
            opts->workgroups.y > 0 ? opts->workgroups.y : 1,
            opts->workgroups.z > 0 ? opts->workgroups.z : 1);
        wgpuComputePassEncoderEnd(pass);

        WGPUCommandBufferDescriptor cmd_desc = {
            .label = sv("hull_dispatch_cmd"),
        };
        cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
        if (!cmd) {
            if (err_msg) *err_msg = "command_buffer_create_failed";
            goto cleanup;
        }
    }

    /* ── Submit + poll (with timeout) ─────────────────────── */

    {
        uint32_t timeout = (opts->timeout_ms > 0) ? opts->timeout_ms : HL_GPU_TIMEOUT_MS;
        if (timeout < 10) timeout = 10;  /* floor: 10ms */
        wgpuQueueSubmit(dctx->queue, 1, &cmd);
        if (gpu_poll_with_timeout(dctx->device, timeout) != 0) {
            if (err_msg) *err_msg = "gpu_timeout";
            rc = HL_GPU_ERR_TIMEOUT;
            goto cleanup;
        }
    }

    /* ── Output readback (skip for fire-and-forget) ──────── */

    if (output && output_len && opts->output_buffer >= 0) {
        int out_idx = opts->output_buffer;
        if (out_idx >= opts->buffer_count ||
            out_idx + binding_offset >= total_bindings) {
            if (err_msg) *err_msg = "output_buffer_index_out_of_range";
            goto cleanup;
        }

        WGPUBuffer out_buf = entries[out_idx + binding_offset].buffer;
        size_t out_size = (size_t)entries[out_idx + binding_offset].size;

        rc = readback_buffer(dctx, out_buf, out_size, output, output_len);
        if (rc != HL_GPU_OK && err_msg)
            *err_msg = "readback_failed";
    } else if (output && output_len && opts->output_texture >= 0) {
        /* Texture readback */
        int ti = opts->output_texture;
        const HlGpuTexture *tex = NULL;
        if (ti < inline_tex_count) {
            tex = &inline_textures[ti];
        } else if (ti < opts->texture_count && opts->textures[ti].name &&
                   persistent_textures) {
            for (int p = 0; p < persistent_tex_count; p++) {
                if (persistent_textures[p].name[0] != '\0' &&
                    strcmp(persistent_textures[p].name, opts->textures[ti].name) == 0) {
                    tex = &persistent_textures[p];
                    break;
                }
            }
        }
        if (!tex) {
            if (err_msg) *err_msg = "output_texture_not_found";
            goto cleanup;
        }
        rc = wgpu_texture_read(backend_device, (HlGpuTexture *)tex,
                                output, output_len);
        if (rc != HL_GPU_OK && err_msg)
            *err_msg = "texture_readback_failed";
    } else {
        rc = HL_GPU_OK; /* fire-and-forget: compute done, no readback */
    }

cleanup:
    if (cmd) wgpuCommandBufferRelease(cmd);
    if (pass) wgpuComputePassEncoderRelease(pass);
    if (encoder) wgpuCommandEncoderRelease(encoder);
    if (bind_group) wgpuBindGroupRelease(bind_group);
    if (uniform_buf) {
        wgpuBufferDestroy(uniform_buf);
        wgpuBufferRelease(uniform_buf);
    }
    for (int i = 0; i < temp_count; i++) {
        if (temp_bufs[i]) {
            wgpuBufferDestroy(temp_bufs[i]);
            wgpuBufferRelease(temp_bufs[i]);
        }
    }
    for (int i = 0; i < inline_tex_count; i++)
        wgpu_texture_destroy(backend_device, &inline_textures[i]);
    free(temp_bufs);
    free(entries);
    return rc;
}

/* ── wgpu_buffer_create ────────────────────────────────────────────── */

static int wgpu_buffer_create(void *backend_device, const char *name,
                               size_t size, int usage, HlGpuBuffer *out)
{
    WgpuDeviceCtx *dctx = (WgpuDeviceCtx *)backend_device;
    if (!dctx || !dctx->device)
        return HL_GPU_ERR_DEVICE;

    (void)name;
    (void)usage;

    /* Align to 4 bytes */
    size = (size + 3) & ~(size_t)3;

    WGPUBufferDescriptor desc = {
        .label = sv("hull_persistent"),
        .usage = WGPUBufferUsage_Storage
               | WGPUBufferUsage_CopyDst
               | WGPUBufferUsage_CopySrc,
        .size  = size,
    };

    WGPUBuffer buf = wgpuDeviceCreateBuffer(dctx->device, &desc);
    if (!buf)
        return HL_GPU_ERR_BUFFER;

    out->handle = buf;
    return HL_GPU_OK;
}

/* ── wgpu_buffer_write ─────────────────────────────────────────────── */

static int wgpu_buffer_write(void *backend_device, HlGpuBuffer *buf,
                              const void *data, size_t len, size_t offset)
{
    WgpuDeviceCtx *dctx = (WgpuDeviceCtx *)backend_device;
    if (!dctx || !buf || !buf->handle)
        return HL_GPU_ERR_BUFFER;

    wgpuQueueWriteBuffer(dctx->queue, (WGPUBuffer)buf->handle,
                          offset, data, len);
    return HL_GPU_OK;
}

/* ── wgpu_buffer_read ──────────────────────────────────────────────── */

static int wgpu_buffer_read(void *backend_device, HlGpuBuffer *buf,
                             void **data, size_t *len)
{
    WgpuDeviceCtx *dctx = (WgpuDeviceCtx *)backend_device;
    if (!dctx || !buf || !buf->handle)
        return HL_GPU_ERR_BUFFER;

    return readback_buffer(dctx, (WGPUBuffer)buf->handle,
                            buf->size, data, len);
}

/* ── wgpu_buffer_destroy ───────────────────────────────────────────── */

static void wgpu_buffer_destroy(void *backend_device, HlGpuBuffer *buf)
{
    (void)backend_device;
    if (!buf || !buf->handle)
        return;

    wgpuBufferDestroy((WGPUBuffer)buf->handle);
    wgpuBufferRelease((WGPUBuffer)buf->handle);
    buf->handle = NULL;
}

/* ── wgpu_dispatch_pipeline ─────────────────────────────────────────
 *
 * Multi-stage dispatch: one command encoder, N compute passes, one
 * submit+poll, then readback the requested output buffers.
 *
 * Named temporary buffers are shared across stages. When multiple
 * stages reference the same buffer name, the maximum declared size
 * is used for allocation.
 */

/* Temp buffer name map entry */
typedef struct {
    const char *name;
    WGPUBuffer  buf;
    size_t      size;  /* allocated (aligned) size */
} TempBufEntry;

static WGPUBuffer find_or_create_temp(WgpuDeviceCtx *dctx,
                                        TempBufEntry *map, int *map_count,
                                        const char *name, size_t size,
                                        const HlGpuBuffer *persistent,
                                        int persistent_count)
{
    /* Check persistent buffers first */
    if (name && persistent) {
        for (int p = 0; p < persistent_count; p++) {
            if (persistent[p].name[0] != '\0' &&
                strcmp(persistent[p].name, name) == 0)
                return (WGPUBuffer)persistent[p].handle;
        }
    }

    /* Check existing temp map */
    if (name) {
        for (int i = 0; i < *map_count; i++) {
            if (map[i].name && strcmp(map[i].name, name) == 0)
                return map[i].buf;
        }
    }

    /* Create new temp buffer */
    size_t aligned = size <= SIZE_MAX - 3 ? (size + 3) & ~(size_t)3 : size;
    if (aligned == 0) aligned = 4;

    WGPUBufferDescriptor desc = {
        .label = sv("hull_pipe_buf"),
        .usage = WGPUBufferUsage_Storage
               | WGPUBufferUsage_CopyDst
               | WGPUBufferUsage_CopySrc,
        .size  = aligned,
    };
    WGPUBuffer buf = wgpuDeviceCreateBuffer(dctx->device, &desc);
    if (!buf) return NULL;

    if (*map_count < HL_GPU_MAX_PIPELINE_BUFFERS) {
        map[*map_count] = (TempBufEntry){ name, buf, aligned };
        (*map_count)++;
    }
    return buf;
}

static int wgpu_dispatch_pipeline(void *backend_device,
                                    HlGpuPipeline **pipelines, int stage_count,
                                    const HlGpuPipelineOpts *opts,
                                    const HlGpuBuffer *persistent_buffers,
                                    int persistent_count,
                                    const HlGpuTexture *persistent_textures,
                                    int persistent_tex_count,
                                    HlGpuPipelineResult *result,
                                    const char **err_msg)
{
    WgpuDeviceCtx *dctx = (WgpuDeviceCtx *)backend_device;
    if (!dctx || !opts || !pipelines) {
        if (err_msg) *err_msg = "invalid_device";
        return HL_GPU_ERR_DEVICE;
    }

    int rc = HL_GPU_ERR_DISPATCH;
    WGPUCommandEncoder encoder = NULL;
    WGPUCommandBuffer cmd = NULL;

    /* Temp buffer name map */
    TempBufEntry temp_map[HL_GPU_MAX_PIPELINE_BUFFERS];
    int temp_count = 0;

    /* Per-stage uniform buffers */
    WGPUBuffer uniform_bufs[HL_GPU_MAX_PIPELINE_STAGES];
    memset(uniform_bufs, 0, sizeof(uniform_bufs));

    /* Per-stage bind groups */
    WGPUBindGroup bind_groups[HL_GPU_MAX_PIPELINE_STAGES];
    memset(bind_groups, 0, sizeof(bind_groups));

    /* Per-stage bind group entries (flat array, indexed by stage offset) */
    WGPUBindGroupEntry all_entries[HL_GPU_MAX_PIPELINE_BUFFERS + HL_GPU_MAX_PIPELINE_STAGES];
    int entry_count = 0;

    /* Track which WGPUBuffer corresponds to each (stage, buffer_idx) for readback.
     * Stored as flat array: stage_buf_map[stage * 16 + buf_idx] */
    WGPUBuffer stage_buf_map[HL_GPU_MAX_PIPELINE_STAGES * 16];
    size_t     stage_buf_size[HL_GPU_MAX_PIPELINE_STAGES * 16];
    memset(stage_buf_map, 0, sizeof(stage_buf_map));
    memset(stage_buf_size, 0, sizeof(stage_buf_size));

    /* ── Pass 1: compute max size per named buffer across all stages ── */
    /* (Simple approach: first-create wins; names with larger sizes
     *  declared later don't resize — users should declare max size first.
     *  Document: declare the largest size in the earliest stage.) */
    /* Actually, per user request: use max size. Pre-scan all stages. */
    typedef struct { const char *name; size_t max_size; } SizeEntry;
    SizeEntry size_map[HL_GPU_MAX_PIPELINE_BUFFERS];
    int size_count = 0;

    for (int s = 0; s < stage_count; s++) {
        const HlGpuPipelineStage *stage = &opts->stages[s];
        for (int b = 0; b < stage->buffer_count; b++) {
            const char *bname = stage->buffers[b].name;
            size_t bsize = stage->buffers[b].size;
            if (stage->buffers[b].data && bsize == 0)
                bsize = 4; /* guard */
            if (bname) {
                int found = 0;
                for (int k = 0; k < size_count; k++) {
                    if (size_map[k].name && strcmp(size_map[k].name, bname) == 0) {
                        if (bsize > size_map[k].max_size)
                            size_map[k].max_size = bsize;
                        found = 1;
                        break;
                    }
                }
                if (!found && size_count < HL_GPU_MAX_PIPELINE_BUFFERS) {
                    size_map[size_count++] = (SizeEntry){ bname, bsize };
                }
            }
        }
    }

    /* ── Pass 2: create buffers, upload data, build bind groups ──── */

    for (int s = 0; s < stage_count; s++) {
        const HlGpuPipelineStage *stage = &opts->stages[s];
        int has_uniforms = (stage->uniforms && stage->uniforms_len > 0);
        int binding_offset = has_uniforms ? 1 : 0;

        /* Count texture bindings: sampled = 2 (view + sampler), storage = 1 */
        int stage_tex_bindings = 0;
        for (int t = 0; t < stage->texture_count; t++)
            stage_tex_bindings += stage->textures[t].storage ? 1 : 2;

        int total_bindings = stage->buffer_count + binding_offset + stage_tex_bindings;

        if (entry_count + total_bindings > (int)(sizeof(all_entries) / sizeof(all_entries[0]))) {
            if (err_msg) *err_msg = "too_many_bindings";
            goto cleanup;
        }

        WGPUBindGroupEntry *entries = &all_entries[entry_count];

        /* Uniform buffer */
        if (has_uniforms) {
            size_t usize = (stage->uniforms_len + 15) & ~(size_t)15;
            WGPUBufferDescriptor ub_desc = {
                .label = sv("hull_pipe_uni"),
                .usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst,
                .size  = usize,
            };
            uniform_bufs[s] = wgpuDeviceCreateBuffer(dctx->device, &ub_desc);
            if (!uniform_bufs[s]) {
                if (err_msg) *err_msg = "uniform_buffer_create_failed";
                goto cleanup;
            }
            wgpuQueueWriteBuffer(dctx->queue, uniform_bufs[s], 0,
                                  stage->uniforms, stage->uniforms_len);
            entries[0] = (WGPUBindGroupEntry){
                .binding = 0, .buffer = uniform_bufs[s],
                .offset = 0, .size = usize,
            };
        }

        /* Storage buffers */
        if (stage->buffer_count > 0 && !stage->buffers) {
            if (err_msg) *err_msg = "buffers_pointer_null";
            goto cleanup;
        }

        for (int b = 0; b < stage->buffer_count; b++) {
            const HlGpuBufferDesc *desc = &stage->buffers[b];

            /* Determine allocation size (max across stages for named buffers,
             * falling back to persistent buffer's actual size) */
            size_t alloc_size = desc->size;
            if (desc->name) {
                for (int k = 0; k < size_count; k++) {
                    if (size_map[k].name && strcmp(size_map[k].name, desc->name) == 0) {
                        alloc_size = size_map[k].max_size;
                        break;
                    }
                }
                /* Fall back to persistent buffer's actual size */
                if (alloc_size == 0 && persistent_buffers) {
                    for (int p = 0; p < persistent_count; p++) {
                        if (persistent_buffers[p].name[0] != '\0' &&
                            strcmp(persistent_buffers[p].name, desc->name) == 0) {
                            alloc_size = persistent_buffers[p].size;
                            break;
                        }
                    }
                }
            }
            if (alloc_size == 0 && desc->data) alloc_size = 4;

            WGPUBuffer gpu_buf = find_or_create_temp(
                dctx, temp_map, &temp_count,
                desc->name, alloc_size,
                persistent_buffers, persistent_count);
            if (!gpu_buf) {
                if (err_msg) *err_msg = "buffer_create_failed";
                goto cleanup;
            }

            /* Upload data if this is the first declaration with data */
            if (desc->data && desc->size > 0)
                wgpuQueueWriteBuffer(dctx->queue, gpu_buf, 0,
                                      desc->data, desc->size);

            size_t buf_aligned = alloc_size <= SIZE_MAX - 3
                ? (alloc_size + 3) & ~(size_t)3 : alloc_size;
            if (buf_aligned == 0) buf_aligned = 4;

            /* Track for readback */
            if (s < HL_GPU_MAX_PIPELINE_STAGES && b < 16) {
                stage_buf_map[s * 16 + b] = gpu_buf;
                stage_buf_size[s * 16 + b] = buf_aligned;
            }

            entries[b + binding_offset] = (WGPUBindGroupEntry){
                .binding = (uint32_t)(b + binding_offset),
                .buffer = gpu_buf, .offset = 0,
                .size = (uint64_t)buf_aligned,
            };
        }

        /* Texture entries for this stage */
        {
            int tex_bind = stage->buffer_count + binding_offset;
            for (int t = 0; t < stage->texture_count; t++) {
                const HlGpuTextureDesc *td = &stage->textures[t];
                const HlGpuTexture *tex = NULL;

                /* Look up persistent texture by name */
                if (td->name && persistent_textures) {
                    for (int p = 0; p < persistent_tex_count; p++) {
                        if (persistent_textures[p].name[0] != '\0' &&
                            strcmp(persistent_textures[p].name, td->name) == 0) {
                            tex = &persistent_textures[p];
                            break;
                        }
                    }
                }

                if (!tex) {
                    if (err_msg) *err_msg = "texture_not_found_in_pipeline";
                    goto cleanup;
                }

                if (tex->storage) {
                    entries[tex_bind] = (WGPUBindGroupEntry){
                        .binding = (uint32_t)tex_bind,
                        .textureView = (WGPUTextureView)tex->view,
                    };
                    tex_bind++;
                } else {
                    entries[tex_bind] = (WGPUBindGroupEntry){
                        .binding = (uint32_t)tex_bind,
                        .textureView = (WGPUTextureView)tex->view,
                    };
                    tex_bind++;
                    entries[tex_bind] = (WGPUBindGroupEntry){
                        .binding = (uint32_t)tex_bind,
                        .sampler = (WGPUSampler)tex->sampler,
                    };
                    tex_bind++;
                }
            }
        }

        /* Get bind group layout (lazy) */
        WGPUBindGroupLayout layout = (WGPUBindGroupLayout)pipelines[s]->bind_group_layout;
        if (!layout && total_bindings > 0) {
            layout = wgpuComputePipelineGetBindGroupLayout(
                (WGPUComputePipeline)pipelines[s]->handle, 0);
            if (!layout) {
                if (err_msg) *err_msg = "bind_group_layout_failed";
                goto cleanup;
            }
            pipelines[s]->bind_group_layout = layout;
        }

        if (total_bindings > 0 && layout) {
            WGPUBindGroupDescriptor bg_desc = {
                .label = sv("hull_pipe_bg"),
                .layout = layout,
                .entryCount = (size_t)total_bindings,
                .entries = entries,
            };
            bind_groups[s] = wgpuDeviceCreateBindGroup(dctx->device, &bg_desc);
            if (!bind_groups[s]) {
                if (err_msg) *err_msg = "bind_group_create_failed";
                goto cleanup;
            }
        }

        entry_count += total_bindings;
    }

    /* ── Encode all compute passes in one command encoder ──────── */

    {
        WGPUCommandEncoderDescriptor enc_desc = {
            .label = sv("hull_pipeline_enc"),
        };
        encoder = wgpuDeviceCreateCommandEncoder(dctx->device, &enc_desc);
        if (!encoder) {
            if (err_msg) *err_msg = "encoder_create_failed";
            goto cleanup;
        }

        for (int s = 0; s < stage_count; s++) {
            const HlGpuPipelineStage *stage = &opts->stages[s];

            WGPUComputePassDescriptor pass_desc = {
                .label = sv("hull_pipe_pass"),
            };
            WGPUComputePassEncoder pass =
                wgpuCommandEncoderBeginComputePass(encoder, &pass_desc);
            if (!pass) {
                if (err_msg) *err_msg = "compute_pass_create_failed";
                goto cleanup;
            }
            wgpuComputePassEncoderSetPipeline(
                pass, (WGPUComputePipeline)pipelines[s]->handle);
            if (bind_groups[s])
                wgpuComputePassEncoderSetBindGroup(pass, 0, bind_groups[s], 0, NULL);
            wgpuComputePassEncoderDispatchWorkgroups(
                pass,
                stage->workgroups.x > 0 ? stage->workgroups.x : 1,
                stage->workgroups.y > 0 ? stage->workgroups.y : 1,
                stage->workgroups.z > 0 ? stage->workgroups.z : 1);
            wgpuComputePassEncoderEnd(pass);
            wgpuComputePassEncoderRelease(pass);
        }

        WGPUCommandBufferDescriptor cmd_desc = {
            .label = sv("hull_pipeline_cmd"),
        };
        cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
        if (!cmd) {
            if (err_msg) *err_msg = "command_buffer_create_failed";
            goto cleanup;
        }
    }

    /* ── Single submit + poll (with timeout) ─────────────────── */

    {
        uint32_t timeout = (opts->timeout_ms > 0) ? opts->timeout_ms : HL_GPU_TIMEOUT_MS;
        if (timeout < 10) timeout = 10;  /* floor: 10ms */
        wgpuQueueSubmit(dctx->queue, 1, &cmd);
        if (gpu_poll_with_timeout(dctx->device, timeout) != 0) {
            if (err_msg) *err_msg = "gpu_timeout";
            rc = HL_GPU_ERR_TIMEOUT;
            goto cleanup;
        }
    }

    /* ── Readback requested outputs (skip for fire-and-forget) ── */

    if (opts->output_count == HL_GPU_OUTPUT_NONE) {
        rc = HL_GPU_OK;  /* fire-and-forget: compute done */
    } else {
        /* Default: last stage's first buffer */
        HlGpuPipelineOutput default_out = {
            .stage = stage_count - 1, .buffer = 0,
        };
        const HlGpuPipelineOutput *outs = opts->outputs;
        int out_count = opts->output_count;
        if (out_count <= 0 || !outs) {
            outs = &default_out;
            out_count = 1;
        }
        if (out_count > HL_GPU_MAX_PIPELINE_OUTPUTS)
            out_count = HL_GPU_MAX_PIPELINE_OUTPUTS;

        result->count = out_count;
        for (int i = 0; i < out_count; i++) {
            int os = outs[i].stage;
            int ob = outs[i].buffer;
            if (os < 0 || os >= stage_count || ob < 0 || ob >= 16) {
                if (err_msg) *err_msg = "output_index_out_of_range";
                goto cleanup;
            }
            WGPUBuffer out_buf = stage_buf_map[os * 16 + ob];
            size_t out_size = stage_buf_size[os * 16 + ob];
            if (!out_buf || out_size == 0) {
                if (err_msg) *err_msg = "output_buffer_not_found";
                goto cleanup;
            }

            rc = readback_buffer(dctx, out_buf, out_size,
                                  &result->data[i], &result->len[i]);
            if (rc != HL_GPU_OK) {
                if (err_msg) *err_msg = "readback_failed";
                goto cleanup;
            }
        }
        rc = HL_GPU_OK;
    } /* end of readback block */

cleanup:
    if (cmd) wgpuCommandBufferRelease(cmd);
    if (encoder) wgpuCommandEncoderRelease(encoder);
    for (int s = 0; s < stage_count; s++) {
        if (bind_groups[s]) wgpuBindGroupRelease(bind_groups[s]);
        if (uniform_bufs[s]) {
            wgpuBufferDestroy(uniform_bufs[s]);
            wgpuBufferRelease(uniform_bufs[s]);
        }
    }
    /* Destroy temp buffers (but not persistent ones) */
    for (int i = 0; i < temp_count; i++) {
        if (temp_map[i].buf) {
            /* Check it's not a persistent buffer before destroying */
            int is_persistent = 0;
            if (temp_map[i].name && persistent_buffers) {
                for (int p = 0; p < persistent_count; p++) {
                    if (persistent_buffers[p].name[0] != '\0' &&
                        strcmp(persistent_buffers[p].name, temp_map[i].name) == 0) {
                        is_persistent = 1;
                        break;
                    }
                }
            }
            if (!is_persistent) {
                wgpuBufferDestroy(temp_map[i].buf);
                wgpuBufferRelease(temp_map[i].buf);
            }
        }
    }
    if (rc != HL_GPU_OK) {
        /* Free any partial readback results */
        for (int i = 0; i < result->count; i++)
            free(result->data[i]);
        memset(result, 0, sizeof(*result));
    }
    return rc;
}

/* ── Texture format helpers ─────────────────────────────────────────── */

static WGPUTextureFormat tex_format_to_wgpu(HlGpuTexFormat fmt)
{
    switch (fmt) {
    case HL_GPU_TEX_RGBA8:   return WGPUTextureFormat_RGBA8Unorm;
    case HL_GPU_TEX_R8:      return WGPUTextureFormat_R8Unorm;
    case HL_GPU_TEX_RGBA16F: return WGPUTextureFormat_RGBA16Float;
    case HL_GPU_TEX_R32F:    return WGPUTextureFormat_R32Float;
    default:                 return WGPUTextureFormat_RGBA8Unorm;
    }
}

static int tex_format_bpp(HlGpuTexFormat fmt)
{
    switch (fmt) {
    case HL_GPU_TEX_RGBA8:   return 4;
    case HL_GPU_TEX_R8:      return 1;
    case HL_GPU_TEX_RGBA16F: return 8;
    case HL_GPU_TEX_R32F:    return 4;
    default:                 return 4;
    }
}

static WGPUFilterMode filter_to_wgpu(int filter)
{
    return (filter == HL_GPU_FILTER_LINEAR) ? WGPUFilterMode_Linear
                                            : WGPUFilterMode_Nearest;
}

static WGPUAddressMode address_to_wgpu(int addr)
{
    switch (addr) {
    case HL_GPU_ADDRESS_REPEAT: return WGPUAddressMode_Repeat;
    case HL_GPU_ADDRESS_MIRROR: return WGPUAddressMode_MirrorRepeat;
    default:                    return WGPUAddressMode_ClampToEdge;
    }
}

/* ── wgpu_texture_create ───────────────────────────────────────────── */

static int wgpu_texture_create(void *backend_device, uint32_t width,
                                uint32_t height, HlGpuTexFormat format,
                                int storage, int filter,
                                int address_u, int address_v,
                                HlGpuTexture *out)
{
    WgpuDeviceCtx *dctx = (WgpuDeviceCtx *)backend_device;
    if (!dctx || !dctx->device)
        return HL_GPU_ERR_DEVICE;

    WGPUTextureUsage usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;
    if (storage)
        usage |= WGPUTextureUsage_StorageBinding;
    else
        usage |= WGPUTextureUsage_TextureBinding;

    WGPUTextureDescriptor tex_desc = {
        .label = sv("hull_texture"),
        .usage = usage,
        .dimension = WGPUTextureDimension_2D,
        .size = { width, height, 1 },
        .format = tex_format_to_wgpu(format),
        .mipLevelCount = 1,
        .sampleCount = 1,
    };

    WGPUTexture texture = wgpuDeviceCreateTexture(dctx->device, &tex_desc);
    if (!texture)
        return HL_GPU_ERR_BUFFER;

    WGPUTextureView view = wgpuTextureCreateView(texture, NULL);
    if (!view) {
        wgpuTextureDestroy(texture);
        wgpuTextureRelease(texture);
        return HL_GPU_ERR_BUFFER;
    }

    WGPUSamplerDescriptor sampler_desc = {
        .label = sv("hull_sampler"),
        .addressModeU = address_to_wgpu(address_u),
        .addressModeV = address_to_wgpu(address_v),
        .addressModeW = WGPUAddressMode_ClampToEdge,
        .magFilter = filter_to_wgpu(filter),
        .minFilter = filter_to_wgpu(filter),
        .mipmapFilter = WGPUMipmapFilterMode_Nearest,
        .lodMinClamp = 0.0f,
        .lodMaxClamp = 1.0f,
        .maxAnisotropy = 1,
    };
    WGPUSampler sampler = wgpuDeviceCreateSampler(dctx->device, &sampler_desc);
    if (!sampler) {
        wgpuTextureViewRelease(view);
        wgpuTextureDestroy(texture);
        wgpuTextureRelease(texture);
        return HL_GPU_ERR_BUFFER;
    }

    out->handle  = texture;
    out->view    = view;
    out->sampler = sampler;
    out->width   = width;
    out->height  = height;
    out->format  = format;
    out->storage = storage;
    return HL_GPU_OK;
}

/* ── wgpu_texture_write ────────────────────────────────────────────── */

static int wgpu_texture_write(void *backend_device, HlGpuTexture *tex,
                               const void *data, size_t len)
{
    WgpuDeviceCtx *dctx = (WgpuDeviceCtx *)backend_device;
    if (!dctx || !tex || !tex->handle || !data)
        return HL_GPU_ERR_BUFFER;

    int bpp = tex_format_bpp(tex->format);
    uint64_t row64 = (uint64_t)tex->width * (uint64_t)bpp;
    if (row64 > UINT32_MAX)
        return HL_GPU_ERR_BUFFER;
    uint32_t bytes_per_row = (uint32_t)row64;
    size_t expected = (size_t)bytes_per_row * tex->height;
    if (len < expected)
        return HL_GPU_ERR_BUFFER;

    WGPUTexelCopyTextureInfo dst = {
        .texture  = (WGPUTexture)tex->handle,
        .mipLevel = 0,
        .origin   = { 0, 0, 0 },
        .aspect   = WGPUTextureAspect_All,
    };
    WGPUTexelCopyBufferLayout layout = {
        .offset       = 0,
        .bytesPerRow  = bytes_per_row,
        .rowsPerImage = tex->height,
    };
    WGPUExtent3D extent = { tex->width, tex->height, 1 };

    wgpuQueueWriteTexture(dctx->queue, &dst, data, len, &layout, &extent);
    return HL_GPU_OK;
}

/* ── wgpu_texture_read ─────────────────────────────────────────────── */

static int wgpu_texture_read(void *backend_device, HlGpuTexture *tex,
                              void **data, size_t *len)
{
    WgpuDeviceCtx *dctx = (WgpuDeviceCtx *)backend_device;
    if (!dctx || !tex || !tex->handle)
        return HL_GPU_ERR_BUFFER;

    int bpp = tex_format_bpp(tex->format);
    uint64_t row64 = (uint64_t)tex->width * (uint64_t)bpp;
    if (row64 > UINT32_MAX)
        return HL_GPU_ERR_BUFFER;
    uint32_t row_bytes = (uint32_t)row64;
    /* 256-byte alignment required by WebGPU for buffer-to-texture copies */
    uint32_t aligned_row = (row_bytes + 255) & ~(uint32_t)255;
    size_t staging_size = (size_t)aligned_row * tex->height;

    /* Create staging buffer */
    WGPUBufferDescriptor staging_desc = {
        .label = sv("hull_tex_staging"),
        .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
        .size  = staging_size,
    };
    WGPUBuffer staging = wgpuDeviceCreateBuffer(dctx->device, &staging_desc);
    if (!staging)
        return HL_GPU_ERR_READBACK;

    /* Encode CopyTextureToBuffer */
    WGPUCommandEncoderDescriptor enc_desc = { .label = sv("hull_tex_read_enc") };
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(
        dctx->device, &enc_desc);
    if (!encoder) {
        wgpuBufferDestroy(staging);
        wgpuBufferRelease(staging);
        return HL_GPU_ERR_READBACK;
    }

    WGPUTexelCopyTextureInfo src = {
        .texture  = (WGPUTexture)tex->handle,
        .mipLevel = 0,
        .origin   = { 0, 0, 0 },
        .aspect   = WGPUTextureAspect_All,
    };
    WGPUTexelCopyBufferInfo dst = {
        .buffer = staging,
        .layout = {
            .offset       = 0,
            .bytesPerRow  = aligned_row,
            .rowsPerImage = tex->height,
        },
    };
    WGPUExtent3D extent = { tex->width, tex->height, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &src, &dst, &extent);

    WGPUCommandBufferDescriptor cmd_desc = { .label = sv("hull_tex_read_cmd") };
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
    if (!cmd) {
        wgpuCommandEncoderRelease(encoder);
        wgpuBufferDestroy(staging);
        wgpuBufferRelease(staging);
        return HL_GPU_ERR_READBACK;
    }

    wgpuQueueSubmit(dctx->queue, 1, &cmd);
    wgpuDevicePoll(dctx->device, 1, NULL);

    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(encoder);

    /* Map staging buffer */
    MapReq map_req = {0};
    WGPUBufferMapCallbackInfo map_cb = {
        .mode = WGPUCallbackMode_AllowSpontaneous,
        .callback = on_buffer_map,
        .userdata1 = &map_req,
    };
    wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, staging_size, map_cb);
    wgpuDevicePoll(dctx->device, 1, NULL);

    if (!map_req.done || map_req.status != WGPUMapAsyncStatus_Success) {
        wgpuBufferDestroy(staging);
        wgpuBufferRelease(staging);
        return HL_GPU_ERR_READBACK;
    }

    const void *mapped = wgpuBufferGetConstMappedRange(staging, 0, staging_size);
    if (!mapped) {
        wgpuBufferUnmap(staging);
        wgpuBufferDestroy(staging);
        wgpuBufferRelease(staging);
        return HL_GPU_ERR_READBACK;
    }

    /* Copy with row-stripping: remove padding */
    size_t out_size = (size_t)row_bytes * tex->height;
    void *result = malloc(out_size);
    if (!result) {
        wgpuBufferUnmap(staging);
        wgpuBufferDestroy(staging);
        wgpuBufferRelease(staging);
        return HL_GPU_ERR_INTERNAL;
    }

    const uint8_t *src_ptr = (const uint8_t *)mapped;
    uint8_t *dst_ptr = (uint8_t *)result;
    for (uint32_t row = 0; row < tex->height; row++) {
        memcpy(dst_ptr + (size_t)row * row_bytes,
               src_ptr + (size_t)row * aligned_row,
               row_bytes);
    }

    wgpuBufferUnmap(staging);
    wgpuBufferDestroy(staging);
    wgpuBufferRelease(staging);

    *data = result;
    *len = out_size;
    return HL_GPU_OK;
}

/* ── wgpu_texture_destroy ──────────────────────────────────────────── */

static void wgpu_texture_destroy(void *backend_device, HlGpuTexture *tex)
{
    (void)backend_device;
    if (!tex)
        return;
    if (tex->sampler)
        wgpuSamplerRelease((WGPUSampler)tex->sampler);
    if (tex->view)
        wgpuTextureViewRelease((WGPUTextureView)tex->view);
    if (tex->handle) {
        wgpuTextureDestroy((WGPUTexture)tex->handle);
        wgpuTextureRelease((WGPUTexture)tex->handle);
    }
    tex->handle = NULL;
    tex->view = NULL;
    tex->sampler = NULL;
}

/* ── wgpu_buffer_copy ──────────────────────────────────────────────── */

static int wgpu_buffer_copy(void *backend_device,
                             HlGpuBuffer *src, HlGpuBuffer *dst,
                             size_t src_offset, size_t dst_offset, size_t size)
{
    WgpuDeviceCtx *dctx = (WgpuDeviceCtx *)backend_device;
    if (!dctx || !src || !dst || !src->handle || !dst->handle)
        return HL_GPU_ERR_BUFFER;

    WGPUCommandEncoderDescriptor enc_desc = { .label = sv("hull_copy_enc") };
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(
        dctx->device, &enc_desc);
    if (!encoder)
        return HL_GPU_ERR_INTERNAL;

    wgpuCommandEncoderCopyBufferToBuffer(encoder,
        (WGPUBuffer)src->handle, src_offset,
        (WGPUBuffer)dst->handle, dst_offset, size);

    WGPUCommandBufferDescriptor cmd_desc = { .label = sv("hull_copy_cmd") };
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
    if (!cmd) {
        wgpuCommandEncoderRelease(encoder);
        return HL_GPU_ERR_INTERNAL;
    }

    wgpuQueueSubmit(dctx->queue, 1, &cmd);
    int poll_rc = gpu_poll_with_timeout(dctx->device, HL_GPU_TIMEOUT_MS);

    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(encoder);
    return poll_rc == 0 ? HL_GPU_OK : HL_GPU_ERR_TIMEOUT;
}

/* ── Backend vtable ────────────────────────────────────────────────── */

const HlGpuBackend hl_gpu_backend_wgpu = {
    .name              = "wgpu",
    .init              = wgpu_init,
    .destroy           = wgpu_destroy,
    .enumerate_devices = wgpu_enumerate_devices,
    .compile           = wgpu_compile,
    .pipeline_destroy  = wgpu_pipeline_destroy,
    .dispatch          = wgpu_dispatch,
    .dispatch_pipeline = wgpu_dispatch_pipeline,
    .buffer_copy       = wgpu_buffer_copy,
    .buffer_create     = wgpu_buffer_create,
    .buffer_write      = wgpu_buffer_write,
    .buffer_read       = wgpu_buffer_read,
    .buffer_destroy    = wgpu_buffer_destroy,
    .texture_create    = wgpu_texture_create,
    .texture_write     = wgpu_texture_write,
    .texture_read      = wgpu_texture_read,
    .texture_destroy   = wgpu_texture_destroy,
};

#else /* !HL_ENABLE_GPU */
/* ISO C requires a translation unit to contain at least one declaration. */
typedef int hl_cap_gpu_wgpu_disabled;
#endif /* HL_ENABLE_GPU */
