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

    int has_uniforms = (opts->uniforms && opts->uniforms_len > 0);
    int binding_offset = has_uniforms ? 1 : 0;
    int total_bindings = opts->buffer_count + binding_offset;

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

    temp_bufs = calloc((size_t)opts->buffer_count, sizeof(WGPUBuffer));
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
        uint64_t buf_size_aligned = (uint64_t)((desc->size + 3) & ~(size_t)3);
        if (buf_size_aligned == 0)
            buf_size_aligned = 4;

        entries[i + binding_offset] = (WGPUBindGroupEntry){
            .binding = binding,
            .buffer  = gpu_buf,
            .offset  = 0,
            .size    = buf_size_aligned,
        };
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

    /* ── Submit + poll ─────────────────────────────────────── */

    wgpuQueueSubmit(dctx->queue, 1, &cmd);
    wgpuDevicePoll(dctx->device, 1, NULL);

    /* ── Output readback ───────────────────────────────────── */

    {
        int out_idx = opts->output_buffer;
        if (out_idx < 0) out_idx = 0;
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

/* ── Backend vtable ────────────────────────────────────────────────── */

const HlGpuBackend hl_gpu_backend_wgpu = {
    .name              = "wgpu",
    .init              = wgpu_init,
    .destroy           = wgpu_destroy,
    .enumerate_devices = wgpu_enumerate_devices,
    .compile           = wgpu_compile,
    .pipeline_destroy  = wgpu_pipeline_destroy,
    .dispatch          = wgpu_dispatch,
    .buffer_create     = wgpu_buffer_create,
    .buffer_write      = wgpu_buffer_write,
    .buffer_read       = wgpu_buffer_read,
    .buffer_destroy    = wgpu_buffer_destroy,
};

#endif /* HL_ENABLE_GPU */
