/*
 * gpu.h — GPU compute capability (wgpu-native backend)
 *
 * Provides GPU compute dispatch for WGSL compute shaders.
 * Backend-agnostic vtable design: wgpu-native is the default,
 * Dawn is a future option.
 *
 * Disabled by default (HL_ENABLE_GPU=0). When disabled, all
 * GPU files compile to empty translation units.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_GPU_H
#define HL_CAP_GPU_H

#ifdef HL_ENABLE_GPU

#include "hull/limits.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

/* ── Error codes ───────────────────────────────────────────────────── */

#define HL_GPU_OK                0
#define HL_GPU_ERR_NOT_AVAILABLE -1
#define HL_GPU_ERR_SHADER        -2
#define HL_GPU_ERR_DISPATCH      -3
#define HL_GPU_ERR_BUFFER        -4
#define HL_GPU_ERR_INTERNAL      -5
#define HL_GPU_ERR_READBACK      -6
#define HL_GPU_ERR_NOT_FOUND     -7
#define HL_GPU_ERR_DEVICE        -8

/* ── Buffer usage flags ────────────────────────────────────────────── */

#define HL_GPU_USAGE_READ       0x01
#define HL_GPU_USAGE_WRITE      0x02
#define HL_GPU_USAGE_READWRITE  0x03

/* ── Types ─────────────────────────────────────────────────────────── */

typedef struct HlGpuBufferDesc {
    const char *name;       /* persistent buffer name, or NULL for inline */
    const void *data;       /* initial data (NULL for output/existing) */
    size_t      size;
    int         usage;
    int         binding;    /* -1 = auto-assign */
} HlGpuBufferDesc;

typedef struct {
    uint32_t x, y, z;
} HlGpuWorkgroups;

typedef struct HlGpuDispatchOpts {
    HlGpuBufferDesc *buffers;
    int              buffer_count;
    HlGpuWorkgroups  workgroups;
    const void      *uniforms;
    size_t           uniforms_len;
    int              output_buffer;   /* index to read back */
    int              device;          /* -1 = default device */
} HlGpuDispatchOpts;

/* Cached compiled pipeline */
typedef struct HlGpuPipeline {
    char  name[HL_GPU_NAME_MAX];
    void *handle;           /* backend-specific pipeline handle */
    void *bind_group_layout;
} HlGpuPipeline;

/* Persistent named buffer */
typedef struct HlGpuBuffer {
    char    name[HL_GPU_NAME_MAX];
    void   *handle;         /* backend-specific buffer handle */
    size_t  size;
    int     usage;
} HlGpuBuffer;

/* Per-device state (pipelines + buffers + backend handle) */
typedef struct HlGpuDevice {
    char  name[HL_GPU_NAME_MAX];     /* adapter name for gpu.devices() */
    void *backend_device;             /* opaque, owned by backend vtable */
    HlGpuPipeline pipelines[HL_GPU_PIPELINE_MAX];
    int            pipeline_count;
    HlGpuBuffer   buffers[HL_GPU_BUFFER_MAX];
    int            buffer_count;
    pthread_mutex_t mutex;
} HlGpuDevice;

/* ── Backend vtable ──────────────────────────────────────────────── */

typedef struct HlGpuBackend {
    const char *name;   /* "wgpu", "dawn", etc. */

    /* Lifecycle */
    int   (*init)(void **backend_ctx);
    void  (*destroy)(void *backend_ctx);

    /* Device enumeration — populates devices[] array */
    int   (*enumerate_devices)(void *backend_ctx,
                               HlGpuDevice *devices, int max_devices);

    /* Per-device ops */
    int   (*compile)(void *backend_device, const char *name,
                     const char *wgsl, size_t wgsl_len,
                     HlGpuPipeline *out);
    void  (*pipeline_destroy)(void *backend_device, HlGpuPipeline *pipeline);

    int   (*dispatch)(void *backend_device, HlGpuPipeline *pipeline,
                      const HlGpuDispatchOpts *opts,
                      const HlGpuBuffer *persistent_buffers, int persistent_count,
                      void **output, size_t *output_len,
                      const char **err_msg);

    int   (*buffer_create)(void *backend_device, const char *name,
                           size_t size, int usage, HlGpuBuffer *out);
    int   (*buffer_write)(void *backend_device, HlGpuBuffer *buf,
                          const void *data, size_t len, size_t offset);
    int   (*buffer_read)(void *backend_device, HlGpuBuffer *buf,
                         void **data, size_t *len);
    void  (*buffer_destroy)(void *backend_device, HlGpuBuffer *buf);
} HlGpuBackend;

/* Top-level GPU context */
typedef struct HlGpuCtx {
    const HlGpuBackend *backend;
    void               *backend_ctx;
    HlGpuDevice         devices[HL_GPU_MAX_DEVICES];
    int                  device_count;
    int                  default_device;
} HlGpuCtx;

/* ── Public API ────────────────────────────────────────────────────── */

/* Lifecycle */
int  hl_cap_gpu_init(HlGpuCtx *ctx, const HlGpuBackend *backend);
void hl_cap_gpu_destroy(HlGpuCtx *ctx);
int  hl_cap_gpu_available(HlGpuCtx *ctx);

/* Device query */
int  hl_cap_gpu_device_count(HlGpuCtx *ctx);
const char *hl_cap_gpu_device_name(HlGpuCtx *ctx, int device);

/* Pipeline */
int hl_cap_gpu_compile(HlGpuCtx *ctx, int device, const char *name,
                       const char *wgsl, size_t wgsl_len);

/* Dispatch */
int hl_cap_gpu_dispatch(HlGpuCtx *ctx, const char *shader_name,
                        const HlGpuDispatchOpts *opts,
                        void **output, size_t *output_len,
                        const char **err_msg);

/* Persistent buffers (per-device) */
int  hl_cap_gpu_buffer_create(HlGpuCtx *ctx, int device, const char *name,
                              size_t size, int usage);
int  hl_cap_gpu_buffer_write(HlGpuCtx *ctx, int device, const char *name,
                             const void *data, size_t len, size_t offset);
int  hl_cap_gpu_buffer_read(HlGpuCtx *ctx, int device, const char *name,
                            void **data, size_t *len);
void hl_cap_gpu_buffer_destroy(HlGpuCtx *ctx, int device, const char *name);

/* Backend selection (defined in gpu_wgpu.c) */
extern const HlGpuBackend hl_gpu_backend_wgpu;

#endif /* HL_ENABLE_GPU */
#endif /* HL_CAP_GPU_H */
