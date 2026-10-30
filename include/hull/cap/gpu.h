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
#include "hull/shared/offheap.h"  /* HlOffheapShared (audit 29) */

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
    /* The device is still running work a timed-out call left (audit 33):
     * refused before anything is queued ("gpu_busy"). Returned by the
     * buffer / texture write, read and copy entry points (audit 34);
     * dispatch / pipeline keep HL_GPU_ERR_TIMEOUT with "gpu_busy". */
    HL_GPU_ERR_BUSY          = -10,
} HlGpuError;

/* ── Buffer usage flags ────────────────────────────────────────────── */

/* Fire-and-forget sentinel: skip readback when output_buffer == this */
#define HL_GPU_OUTPUT_NONE      -1

/* Ceiling on an app-supplied dispatch / pipeline `timeout` (ms). A sync
 * dispatch waits on the event loop, outside the instruction budget.
 * Enforced in hl_cap_gpu_dispatch / hl_cap_gpu_pipeline (hl_gpu_timeout_clamp)
 * for every caller, sync and async: the JS bindings passed `timeout: -1`
 * through as 2^32-1 ms, ~49.7 days (audit 33). */
#define HL_GPU_TIMEOUT_MAX_MS   60000

/* The timeout a dispatch / pipeline runs with: @p ms, at most
 * HL_GPU_TIMEOUT_MAX_MS (0 stays 0, the backend's default). */
static inline uint32_t hl_gpu_timeout_clamp(uint32_t ms)
{
    return ms > HL_GPU_TIMEOUT_MAX_MS ? HL_GPU_TIMEOUT_MAX_MS : ms;
}

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
    size_t   bytes;      /* reserved on the device's live total (audit 17) */
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

/* Bind-group slots a texture takes: storage = 1 (view), sampled = 2 (view +
 * sampler). A dispatch / pipeline sizes its entry array from the DESCRIPTORS'
 * storage flags, before any texture is resolved. */
int hl_gpu_tex_binding_slots(int storage);

/* Check one resolved texture before its bind-group entries are written at
 * [cursor, cursor + slots): the texture must be the kind (storage / sampled)
 * its descriptor declared - the count the entry array was sized from - and
 * its slots must fit below `end`. Returns the slot count, or -1 with *err set
 * ("texture_storage_mismatch" / "too_many_bindings"). Audit 10 H1: a named
 * persistent texture created as sampled but passed as `storage = true` (or the
 * reverse) was counted as 1 slot and written as 2, past the heap array. */
int hl_gpu_tex_binding_check(int desc_storage, int tex_storage,
                             int cursor, int end, const char **err);

/* ── Types ─────────────────────────────────────────────────────────── */

typedef struct HlGpuBufferDesc {
    const char *name;       /* persistent buffer name, or NULL for inline */
    const void *data;       /* initial data (NULL for output/existing) */
    size_t      data_len;   /* bytes at data: the most an upload reads */
    size_t      size;       /* buffer size; may exceed data_len (rest zero) */
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
    size_t           max_output;     /* readback refused past this (the VM heap
                                      * limit); 0 = none (audit 17) */
    HlOffheapShared *result_hold;    /* an async op's result share: each
                                      * readback is taken on it before the
                                      * copy (audit 29); NULL = sync call */
} HlGpuDispatchOpts;

/* ── Shader binding layout (audit 35) ─────────────────────────────── */

/* The kind of resource a WGSL `@group(0) @binding(N)` declaration takes. */
typedef enum {
    HL_GPU_BIND_UNKNOWN = 0,      /* a handle type named by an alias: any
                                   * texture / sampler slot is let through */
    HL_GPU_BIND_UNIFORM,          /* var<uniform> */
    HL_GPU_BIND_STORAGE,          /* var<storage, ...> */
    HL_GPU_BIND_TEXTURE,          /* texture_2d<...> */
    HL_GPU_BIND_STORAGE_TEXTURE,  /* texture_storage_2d<fmt, access> */
    HL_GPU_BIND_SAMPLER,          /* sampler */
    HL_GPU_BIND_OTHER,            /* any other texture / sampler type:
                                   * nothing Hull binds can fill it */
} HlGpuBindKind;

#ifndef HL_GPU_SHADER_BINDINGS_MAX
#define HL_GPU_SHADER_BINDINGS_MAX 64
#endif

typedef struct HlGpuBindingDecl {
    uint16_t binding;
    uint8_t  kind;      /* HlGpuBindKind */
    int8_t   format;    /* a storage texture's HlGpuTexFormat, -1 = any */
} HlGpuBindingDecl;

/* Cached compiled pipeline */
typedef struct HlGpuPipeline {
    char  name[HL_GPU_NAME_MAX];
    void *handle;           /* backend-specific pipeline handle */
    void *bind_group_layout;
    /* Group 0's declared bindings, read from the WGSL source at compile
     * (hl_gpu_wgsl_layout). layout_known = 0 when the source used a form
     * the reader does not follow (a const-expression index, ...): the
     * backend's own validation is then the only check. */
    int              layout_known;
    int              decl_count;
    HlGpuBindingDecl decls[HL_GPU_SHADER_BINDINGS_MAX];
} HlGpuPipeline;

/* Read the `@group(0) @binding(N) var ...` declarations of @p wgsl into
 * @p out (layout_known, decl_count, decls). Returns 0 when the layout is
 * known, -1 when it is not (out->layout_known = 0). Never fails a compile. */
int hl_gpu_wgsl_layout(const char *wgsl, size_t len, HlGpuPipeline *out);

/* The deepest expression @p wgsl builds, estimated without parsing
 * (comments and numeric literals skipped): per statement, the operators
 * and accessors chained in one operand plus the brackets open around it.
 * hl_cap_gpu_compile refuses a source past HL_GPU_WGSL_EXPR_DEPTH_MAX, or
 * larger than HL_GPU_WGSL_MAX_BYTES, before the backend sees it: naga
 * recurses over such a tree until the stack overflows (audit 36). Returns
 * 0 for NULL, HL_GPU_WGSL_UNREADABLE for a source the shared lexer cannot
 * split as naga does (malformed UTF-8, a byte outside every WGSL token). */
size_t hl_gpu_wgsl_complexity(const char *wgsl, size_t len);

/* What the depth checks return for a source they refuse to read (audit
 * 37): larger than every limit, so a comparison refuses it. */
#define HL_GPU_WGSL_UNREADABLE ((size_t)-1)

/* The deepest block nesting @p wgsl builds, braces and `else if` chains
 * together (an `else if` nests without a brace; naga validates the If tree
 * recursively). 0 for NULL, HL_GPU_WGSL_UNREADABLE as above (audit 37). */
size_t hl_gpu_wgsl_block_depth(const char *wgsl, size_t len);

/* The deepest type the module-scope `struct` / `alias` declarations of
 * @p wgsl build, across declarations in any order (a cycle counts as past
 * HL_GPU_WGSL_TYPE_DEPTH_MAX). 0 for NULL or none, HL_GPU_WGSL_UNREADABLE
 * as above or when its tables cannot be allocated (audit 37). */
size_t hl_gpu_wgsl_type_depth(const char *wgsl, size_t len);

/* The longest chain of module-scope declarations of @p wgsl naming one
 * another (const, override, var, let, alias, struct, fn - a function names
 * what its body calls and reads - and const_assert), counted in
 * declarations, across declarations in any order; names a function
 * declares itself shadow module-scope ones. naga orders declarations with
 * a recursive walk, one frame per link (audit 38). A cycle counts as past
 * HL_GPU_WGSL_DECL_CHAIN_MAX. 0 for NULL or none, HL_GPU_WGSL_UNREADABLE as
 * above or when its tables cannot be allocated. */
size_t hl_gpu_wgsl_decl_chain(const char *wgsl, size_t len);

/* Every pre-compile check hl_cap_gpu_compile runs: size, lexing, the
 * expression / block / type depths and the declaration chains. 0 when the source passes; -1 with a
 * short reason in *why otherwise. */
int hl_gpu_wgsl_precheck(const char *wgsl, size_t len, const char **why);

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
    /* Bytes the persistent buffers and textures hold on the device, capped
     * at HL_GPU_MAX_LIVE_BYTES: the slot counts alone let an app hold
     * 64 x 16384^2 x 8 bytes of textures (audit 17). */
    size_t         live_bytes;
    /* A dispatch / pipeline that timed out is still running on the device,
     * and the transient buffers / textures it made are only freed once it
     * finishes (WebGPU destroy semantics). Their bytes stay counted in
     * live_bytes as `stalled_bytes`, and the device refuses new dispatches
     * and pipelines ("gpu_busy") until the backend's queue_idle reports the
     * queue drained, which gives them back (audit 33: they were released
     * on the timeout, so repeated timed-out calls held device memory past
     * HL_GPU_MAX_LIVE_BYTES). */
    size_t         stalled_bytes;
    int            stalled;
    /* Bytes buffer / texture writes queued since the last submission the
     * device waited out: wgpu holds each write's staging copy until a
     * submission completes, and a write submits nothing (audit 34: a loop
     * of uploads with no dispatch held 4.8 GB). Past
     * HL_GPU_MAX_QUEUED_WRITE_BYTES the cap layer has the backend flush
     * them (flush_writes); a flush, read, copy or dispatch that timed out
     * moves them into stalled_bytes. A timed-out read also counts its
     * staging buffer there. */
    size_t         queued_write_bytes;
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
    size_t               max_output;  /* all readbacks together refused past
                                       * this; 0 = none (audit 17) */
    HlOffheapShared     *result_hold; /* as HlGpuDispatchOpts (audit 29) */
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

    /* Device enumeration - populates devices[] array */
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

    /* 1 when the device has no submitted work still running, 0 otherwise;
     * never blocks. Asked before a dispatch / pipeline on a device a timed-
     * out call stalled (audit 33). NULL = the backend runs nothing
     * asynchronously, so a stalled device is taken as drained. */
    int   (*queue_idle)(HlGpuDevice *dev);

    /* Submit the writes buffer_write / texture_write queued and wait for
     * them, at most @p timeout_ms (0 = the backend default), asking the
     * run like every GPU wait. HL_GPU_OK, or HL_GPU_ERR_TIMEOUT (the work
     * is still running). NULL = a write holds nothing past its call
     * (audit 34). */
    int   (*flush_writes)(HlGpuDevice *dev, uint32_t timeout_ms);
} HlGpuBackend;

/* Top-level GPU context */
struct HlGpuCtx {
    /* The manifest's device allowlist: a leading policy span (see
     * cap/policy_seal.h). serve.c allocates the context page-aligned and
     * seals this span once it is set, so a write cannot widen the
     * allowlist; the device and pipeline state after it stays writable. */
    union {
        struct {
            int allowed_devices[HL_GPU_MAX_DEVICES]; /* 1 = allowed */
            int device_restriction;                  /* 1 = allowlist active */
        };
        unsigned char policy_span[65536];            /* == HL_POLICY_SPAN */
    };
    const HlGpuBackend *backend;
    void               *backend_ctx;
    HlGpuDevice         devices[HL_GPU_MAX_DEVICES];
    int                  device_count;
    int                  default_device;
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

/* Check one dispatch's (or one pipeline stage's) bindings against the
 * shader's declared layout before anything reaches the backend (audit 35):
 * wgpu-native aborts the process on a submit whose bind group failed
 * validation. Hull binds the uniforms (when given) at binding 0, the
 * buffers after them in order, then each texture (storage: 1 slot;
 * sampled: view + sampler). Every one of those bindings must be declared
 * in group 0 with that kind (a storage texture also with the texture's
 * format). Returns 0, or -1 with *err = "binding_mismatch". A pipeline
 * whose layout is not known passes (the backend validates). @p dev
 * resolves named persistent textures' kind and format; NULL = use the
 * descriptor's. A declared binding Hull does not fill is left to the
 * backend: WGSL drops a declaration the entry point never uses from the
 * layout, which the source alone cannot tell. */
int hl_gpu_bindings_check(const HlGpuPipeline *pipeline,
                          const HlGpuDevice *dev, int has_uniforms,
                          const HlGpuBufferDesc *buffers, int buffer_count,
                          const HlGpuTextureDesc *textures, int texture_count,
                          const char **err);

/* Free all data pointers in a pipeline result */
void hl_cap_gpu_pipeline_result_free(HlGpuPipelineResult *result);

/* Persistent buffers (per-device) */
int  hl_cap_gpu_buffer_create(HlGpuCtx *ctx, int device, const char *name,
                              size_t size, int usage);
int  hl_cap_gpu_buffer_write(HlGpuCtx *ctx, int device, const char *name,
                             const void *data, size_t len, size_t offset);
/* A read of a buffer larger than @p max_len (the caller's VM heap limit;
 * 0 = none) is refused before the backend allocates its copy. */
int  hl_cap_gpu_buffer_read(HlGpuCtx *ctx, int device, const char *name,
                            size_t max_len, void **data, size_t *len);
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
                              size_t max_len, void **data, size_t *len,
                              uint32_t *out_width, uint32_t *out_height,
                              HlGpuTexFormat *out_format);
void hl_cap_gpu_texture_destroy(HlGpuCtx *ctx, int device, const char *name);

/* Bytes of a width x height texture of @p fmt (unpadded rows): what a
 * create reserves, an upload of the whole texture writes and a read-back
 * returns. */
size_t hl_gpu_tex_bytes(uint32_t width, uint32_t height, HlGpuTexFormat fmt);

/* Device bytes a dispatch / pipeline creates for the call itself (inline
 * storage buffers, uniform buffers, a dispatch's inline textures; a
 * pipeline's named buffer once, at its largest declared size). A name that
 * is a persistent buffer / texture of @p dev adds nothing; @p dev NULL
 * counts every one. hl_cap_gpu_dispatch / _pipeline reserve this on the
 * device's live total for the call and refuse it past HL_GPU_MAX_LIVE_BYTES
 * ("device_memory_limit") before the backend runs (audit 17). */
size_t hl_gpu_dispatch_transient_bytes(const HlGpuDevice *dev,
                                       const HlGpuDispatchOpts *opts);
size_t hl_gpu_pipeline_transient_bytes(const HlGpuDevice *dev,
                                       const HlGpuPipelineOpts *opts);
/* What a binding charges (1 unit per 64 bytes) before a dispatch / pipeline:
 * the dev-NULL count, at most HL_GPU_MAX_LIVE_BYTES (more is refused before
 * any work). */
size_t hl_gpu_dispatch_charge_bytes(const HlGpuDispatchOpts *opts);
size_t hl_gpu_pipeline_charge_bytes(const HlGpuPipelineOpts *opts);

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
