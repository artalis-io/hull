/**
 * @file cap/gpu.h
 * @brief GPU compute capability (wgpu-native backend).
 *
 * GPU compute dispatch for WGSL shaders. Backend-agnostic vtable design:
 * wgpu-native is the default (Vulkan/Metal/DX12); Dawn is a future option.
 *
 * @par Lifecycle:
 *   1. `hl_cap_gpu_init` creates the context (one per process).
 *   2. `hl_cap_gpu_compile(name, wgsl)` builds and caches a pipeline.
 *      `hl_cap_gpu_load(name)` reads `shaders/<name>.wgsl` from VFS.
 *   3. `hl_cap_gpu_dispatch(name, opts)` runs the shader. Persistent
 *      buffers (`hl_cap_gpu_buffer(name, data)`) survive across dispatches.
 *   4. `hl_cap_gpu_pipeline(stages, opts)` chains stages in one submit.
 *
 * @par Compile-time:
 *   Disabled by default. `make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu`
 *   enables. When disabled the TU is empty (one `@empty-translation-unit`).
 *
 * @par Performance:
 *   ~2.5 ms submit floor on Apple Silicon, independent of payload size.
 *   Crossover vs. WASM AOT at ~16K elements for typical workloads. Use
 *   `gpu.pipeline()` to amortise multiple dispatches into one submit.
 *
 * @par Manifest:
 *   Apps must declare `gpu: true` in the manifest to access these
 *   functions from Lua/JS.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_GPU_H
#define HL_CAP_GPU_H

/* The GPU types + the generic cap API are ALWAYS declared: the generic dispatch
 * layer (cap/gpu.c) and the composable-feature hook (cap/gpu_feature.c) are
 * base-resident. Only the concrete wgpu backend symbol (at the bottom) is gated
 * on a compiled-in HL_ENABLE_GPU build; a `hull build --with=gpu` feature
 * supplies that backend instead. */

#include "hull/limits/gpu.h"

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

/* ── Texture format ───────────────────────────────────────────────── */

typedef enum {
    HL_GPU_TEX_RGBA8   = 0,
    HL_GPU_TEX_R8      = 1,
    HL_GPU_TEX_RGBA16F = 2,
    HL_GPU_TEX_R32F    = 3,
} HlGpuTexFormat;

/* ── Sampler configuration ────────────────────────────────────────── */

typedef enum {
    HL_GPU_FILTER_NEAREST = 0,
    HL_GPU_FILTER_LINEAR  = 1,
} HlGpuFilterMode;

typedef enum {
    HL_GPU_ADDRESS_CLAMP  = 0,
    HL_GPU_ADDRESS_REPEAT = 1,
    HL_GPU_ADDRESS_MIRROR = 2,
} HlGpuAddressMode;

/* ── Persistent texture ───────────────────────────────────────────── */

typedef struct HlGpuTexture {
    char     name[HL_GPU_NAME_MAX];
    void    *handle;     /* WGPUTexture */
    void    *view;       /* WGPUTextureView */
    void    *sampler;    /* WGPUSampler */
    uint32_t width;
    uint32_t height;
    HlGpuTexFormat format;
    int      storage;    /* 1 = storage texture, 0 = sampled */
} HlGpuTexture;

/* ── Texture descriptor (for dispatch/pipeline) ───────────────────── */

typedef struct HlGpuTextureDesc {
    const char    *name;       /* persistent texture name, or NULL for inline */
    const void    *data;       /* pixel data to upload (NULL = use existing) */
    size_t         data_len;
    uint32_t       width;
    uint32_t       height;
    HlGpuTexFormat format;
    int            storage;    /* 1 = storage, 0 = sampled (default) */
    int            binding;    /* -1 = auto-assign */
} HlGpuTextureDesc;

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
    HlGpuTextureDesc *textures;
    int              texture_count;
    HlGpuWorkgroups  workgroups;
    const void      *uniforms;
    size_t           uniforms_len;
    int              output_buffer;   /* index to read back */
    int              output_texture;  /* texture index to read back as pixels, -1 = none */
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

/* Per-device state (pipelines + buffers + textures + backend handle) */
typedef struct HlGpuDevice {
    char  name[HL_GPU_NAME_MAX];     /* adapter name for gpu.devices() */
    void *backend_device;             /* opaque, owned by backend vtable */
    HlGpuPipeline pipelines[HL_GPU_PIPELINE_MAX];
    int            pipeline_count;
    HlGpuBuffer   buffers[HL_GPU_BUFFER_MAX];
    int            buffer_count;
    HlGpuTexture  textures[HL_GPU_TEXTURE_MAX];
    int            texture_count;
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
    HlGpuTextureDesc *textures;
    int              texture_count;
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

typedef struct HlGpuCtx HlGpuCtx;

/* Vtable methods take typed handles (HlGpuCtx * for backend-level
 * ops, HlGpuDevice * for per-device ops) instead of the historical
 * void *backend_ctx / void *backend_device.  The concrete backend
 * context lives in ctx->backend_ctx / dev->backend_device; each
 * method casts it to its own concrete type at the top of the body.
 * The cast is a normal pointer access inside the body, invisible
 * to CFI's call-site type check.
 *
 * Compared to the historical void * shape, the typed handles give
 * clang -fsanitize=cfi-icall a matching signature at every call
 * site so CFI no longer flags the polymorphic vtable dispatch as
 * a type mismatch.  See docs/security.md § 4c. */
typedef struct HlGpuBackend {
    const char *name;   /* "wgpu", "dawn", etc. */

    /* Lifecycle.  `init` is special: writes its concrete backend
     * context pointer to *out_ctx for the caller to wire into a
     * HlGpuCtx.  All subsequent methods receive the HlGpuCtx /
     * HlGpuDevice handle so CFI sees a typed parameter. */
    int   (*init)(void **out_ctx);
    void  (*destroy)(HlGpuCtx *ctx);

    /* Device enumeration — populates devices[] array */
    int   (*enumerate_devices)(HlGpuCtx *ctx,
                               HlGpuDevice *devices, int max_devices);

    /* Per-device ops */
    int   (*compile)(HlGpuDevice *dev, const char *name,
                     const char *wgsl, size_t wgsl_len,
                     HlGpuPipeline *out);
    void  (*pipeline_destroy)(HlGpuDevice *dev, HlGpuPipeline *pipeline);

    int   (*dispatch)(HlGpuDevice *dev, HlGpuPipeline *pipeline,
                      const HlGpuDispatchOpts *opts,
                      const HlGpuBuffer *persistent_buffers, int persistent_count,
                      const HlGpuTexture *persistent_textures, int persistent_tex_count,
                      void **output, size_t *output_len,
                      const char **err_msg);

    /* Multi-stage pipeline dispatch (NULL = not supported) */
    int   (*dispatch_pipeline)(HlGpuDevice *dev,
                                HlGpuPipeline **pipelines, int stage_count,
                                const HlGpuPipelineOpts *opts,
                                const HlGpuBuffer *persistent_buffers,
                                int persistent_count,
                                const HlGpuTexture *persistent_textures,
                                int persistent_tex_count,
                                HlGpuPipelineResult *result,
                                const char **err_msg);

    /* GPU-side buffer copy (NULL = not supported) */
    int   (*buffer_copy)(HlGpuDevice *dev,
                          HlGpuBuffer *src, HlGpuBuffer *dst,
                          size_t src_offset, size_t dst_offset, size_t size);

    int   (*buffer_create)(HlGpuDevice *dev, const char *name,
                           size_t size, int usage, HlGpuBuffer *out);
    int   (*buffer_write)(HlGpuDevice *dev, HlGpuBuffer *buf,
                          const void *data, size_t len, size_t offset);
    int   (*buffer_read)(HlGpuDevice *dev, HlGpuBuffer *buf,
                         void **data, size_t *len);
    void  (*buffer_destroy)(HlGpuDevice *dev, HlGpuBuffer *buf);

    /* Texture ops */
    int   (*texture_create)(HlGpuDevice *dev, uint32_t width, uint32_t height,
                            HlGpuTexFormat format, int storage,
                            int filter, int address_u, int address_v,
                            HlGpuTexture *out);
    int   (*texture_write)(HlGpuDevice *dev, HlGpuTexture *tex,
                           const void *data, size_t len);
    int   (*texture_read)(HlGpuDevice *dev, HlGpuTexture *tex,
                          void **data, size_t *len);
    void  (*texture_destroy)(HlGpuDevice *dev, HlGpuTexture *tex);
} HlGpuBackend;

/* Top-level GPU context */
struct HlGpuCtx {
    const HlGpuBackend *backend;
    void               *backend_ctx;
    HlGpuDevice         devices[HL_GPU_MAX_DEVICES];
    int                  device_count;
    int                  default_device;
    int                  allowed_devices[HL_GPU_MAX_DEVICES]; /* 1 = allowed */
    int                  device_restriction;  /* 1 = allowlist active */
};

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

/* Persistent textures (per-device) */
int  hl_cap_gpu_texture_create(HlGpuCtx *ctx, int device, const char *name,
                                uint32_t width, uint32_t height,
                                HlGpuTexFormat format, int storage,
                                int filter, int address_u, int address_v);
int  hl_cap_gpu_texture_write(HlGpuCtx *ctx, int device, const char *name,
                               const void *data, size_t len);
int  hl_cap_gpu_texture_read(HlGpuCtx *ctx, int device, const char *name,
                              void **data, size_t *len,
                              uint32_t *out_width, uint32_t *out_height,
                              HlGpuTexFormat *out_format);
void hl_cap_gpu_texture_destroy(HlGpuCtx *ctx, int device, const char *name);

/* Composable-feature hook: the base returns NO backend (weak default in
 * cap/gpu_feature.c); a `hull build --with=gpu` build links a STRONG override
 * returning the wgpu backend. Mirrors hl_db_feature_backends. */
const HlGpuBackend *const *hl_gpu_feature_backends(size_t *count);

#ifdef HL_ENABLE_GPU
/* The wgpu backend vtable (defined in gpu_wgpu.c). Present only in a monolithic
 * HL_ENABLE_GPU build or the gpu feature archive. */
extern const HlGpuBackend hl_gpu_backend_wgpu;
#endif /* HL_ENABLE_GPU */

#endif /* HL_CAP_GPU_H */
