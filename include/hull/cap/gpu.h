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

typedef enum {
    HL_GPU_OK                =  0,
    HL_GPU_ERR_NOT_AVAILABLE = -1,
    HL_GPU_ERR_SHADER        = -2,
    HL_GPU_ERR_DISPATCH      = -3,
    HL_GPU_ERR_BUFFER        = -4,
    HL_GPU_ERR_INTERNAL      = -5,
    HL_GPU_ERR_READBACK      = -6,
    HL_GPU_ERR_NOT_FOUND     = -7,
    HL_GPU_ERR_DEVICE        = -8,
    HL_GPU_ERR_TIMEOUT       = -9,
} HlGpuError;

/* ── Buffer usage flags ────────────────────────────────────────────── */

/* Fire-and-forget sentinel: skip readback when output_buffer == this */
#define HL_GPU_OUTPUT_NONE      -1

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
    uint32_t         timeout_ms;     /* 0 = default (HL_GPU_TIMEOUT_MS), min 10ms */
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

/* ── Pipeline (multi-stage dispatch) ───────────────────────────────── */

/*
 * A pipeline stage: one compute shader dispatch with its own buffers,
 * uniforms, and workgroup dimensions.
 *
 * Buffers are shared across stages by name: the first stage to declare
 * a named buffer creates it (using the maximum size declared across all
 * stages). Subsequent stages referencing the same name reuse the buffer.
 */
typedef struct HlGpuPipelineStage {
    const char      *shader;          /* compiled shader name (required) */
    HlGpuBufferDesc *buffers;
    int              buffer_count;
    HlGpuWorkgroups  workgroups;
    const void      *uniforms;
    size_t           uniforms_len;
} HlGpuPipelineStage;

/* Output descriptor: which buffer to read back after pipeline execution */
typedef struct HlGpuPipelineOutput {
    int stage;     /* stage index (0-based) */
    int buffer;    /* buffer index within that stage (0-based) */
} HlGpuPipelineOutput;

typedef struct HlGpuPipelineOpts {
    HlGpuPipelineStage *stages;
    int                  stage_count;
    HlGpuPipelineOutput *outputs;     /* buffers to read back */
    int                  output_count; /* 0 = read last stage's first buffer */
    int                  device;       /* -1 = default */
    uint32_t             timeout_ms;  /* 0 = default (HL_GPU_TIMEOUT_MS), min 10ms */
} HlGpuPipelineOpts;

/* Pipeline result: array of readback buffers */
typedef struct HlGpuPipelineResult {
    void   *data[HL_GPU_MAX_PIPELINE_OUTPUTS];
    size_t  len[HL_GPU_MAX_PIPELINE_OUTPUTS];
    int     count;
} HlGpuPipelineResult;

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

    /* Multi-stage pipeline dispatch (NULL = not supported) */
    int   (*dispatch_pipeline)(void *backend_device,
                                HlGpuPipeline **pipelines, int stage_count,
                                const HlGpuPipelineOpts *opts,
                                const HlGpuBuffer *persistent_buffers,
                                int persistent_count,
                                HlGpuPipelineResult *result,
                                const char **err_msg);

    /* GPU-side buffer copy (NULL = not supported) */
    int   (*buffer_copy)(void *backend_device,
                          HlGpuBuffer *src, HlGpuBuffer *dst,
                          size_t src_offset, size_t dst_offset, size_t size);

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
    int                  allowed_devices[HL_GPU_MAX_DEVICES]; /* 1 = allowed */
    int                  device_restriction;  /* 1 = allowlist active */
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

/* Pipeline (multi-stage dispatch) */
int hl_cap_gpu_pipeline(HlGpuCtx *ctx, const HlGpuPipelineOpts *opts,
                         HlGpuPipelineResult *result, const char **err_msg);

/* Free all data pointers in a pipeline result */
void hl_cap_gpu_pipeline_result_free(HlGpuPipelineResult *result);

/* Persistent buffers (per-device) */
int  hl_cap_gpu_buffer_create(HlGpuCtx *ctx, int device, const char *name,
                              size_t size, int usage);
int  hl_cap_gpu_buffer_write(HlGpuCtx *ctx, int device, const char *name,
                             const void *data, size_t len, size_t offset);
int  hl_cap_gpu_buffer_read(HlGpuCtx *ctx, int device, const char *name,
                            void **data, size_t *len);
void hl_cap_gpu_buffer_destroy(HlGpuCtx *ctx, int device, const char *name);
int  hl_cap_gpu_buffer_copy(HlGpuCtx *ctx, int device,
                             const char *src_name, const char *dst_name,
                             size_t src_offset, size_t dst_offset, size_t size);

/* Backend selection (defined in gpu_wgpu.c) */
extern const HlGpuBackend hl_gpu_backend_wgpu;

#endif /* HL_ENABLE_GPU */
#endif /* HL_CAP_GPU_H */
