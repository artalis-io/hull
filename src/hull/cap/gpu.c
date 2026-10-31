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
#ifdef HL_ENABLE_DB
#include "hull/cap/db_budget.h"
#endif
#include "hull/limits/gpu.h"
#include "hull/shared/thread_affinity.h"
#include "hull/shared/work_charge.h"
#include "log.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* The device's live-bytes reservation (audit 17): persistent buffers and
 * textures held memory on the device bounded only by their slot counts, and
 * a texture's create allocated it with no upload at all. Called under the
 * device mutex. 0, or -1 when @p n does not fit under the ceiling. */
static int live_reserve(HlGpuDevice *dev, size_t n)
{
    if (n > HL_GPU_MAX_LIVE_BYTES || dev->live_bytes > HL_GPU_MAX_LIVE_BYTES - n)
        return -1;
    dev->live_bytes += n;
    return 0;
}

static void live_release(HlGpuDevice *dev, size_t n)
{
    dev->live_bytes = n > dev->live_bytes ? 0 : dev->live_bytes - n;
}

/* A device a timed-out dispatch / pipeline left running work on (audit 33).
 * Called under the device mutex before a new submission: gives the stalled
 * transient bytes back once the backend reports the queue drained, and
 * otherwise refuses (1). Refusing, rather than queueing behind the hung
 * submission, keeps each later call from waiting out its own timeout on the
 * same device - holding the device mutex, and on the sync path the event
 * loop, for every one of them. */
static int compile_busy(HlGpuDevice *dev);
static int compile_drain(HlGpuDevice *dev);

static int stalled_refuse(const HlGpuCtx *ctx, HlGpuDevice *dev)
{
    /* An abandoned shader compile still running (audit 40). */
    if (compile_busy(dev)) return 1;
    if (!dev->stalled) return 0;
    if (ctx->backend->queue_idle && !ctx->backend->queue_idle(dev))
        return 1;
    live_release(dev, dev->stalled_bytes);
    dev->stalled_bytes = 0;
    dev->stalled = 0;
    return 0;
}

/* After a submission: a timeout keeps the call's transient bytes reserved
 * and marks the device stalled; anything else gives them back. */
static void transient_settle(HlGpuDevice *dev, size_t transient, int rc)
{
    if (rc == HL_GPU_ERR_TIMEOUT) {
        dev->stalled = 1;
        dev->stalled_bytes += transient;
    } else {
        live_release(dev, transient);
    }
}

static const char GPU_BUSY_MSG[] = "gpu_busy";

static size_t sat_add(size_t a, size_t b)
{
    return a > SIZE_MAX - b ? SIZE_MAX : a + b;
}

/* Work a timed-out submission left running holds @p n more device bytes
 * the backend made for it (a read's staging buffer, queued writes' staging
 * copies): counted on live_bytes as stalled bytes, even past the ceiling -
 * they ARE held - until queue_idle gives them back (audit 34). */
static void stall_add(HlGpuDevice *dev, size_t n)
{
    dev->stalled = 1;
    dev->live_bytes = sat_add(dev->live_bytes, n);
    dev->stalled_bytes = sat_add(dev->stalled_bytes, n);
}

/* After a backend call that submits and waits (dispatch, pipeline, read,
 * copy, flush): every queued write went with that submission. Success
 * means wgpu freed their staging copies; a timeout leaves them, and the
 * call's own @p staging bytes, held by the running work (audit 34). */
static void submit_settle(HlGpuDevice *dev, size_t staging, int rc)
{
    if (rc == HL_GPU_ERR_TIMEOUT) {
        stall_add(dev, sat_add(staging, dev->queued_write_bytes));
        dev->queued_write_bytes = 0;
    } else if (rc == HL_GPU_OK) {
        dev->queued_write_bytes = 0;
    }
}

/* After a buffer / texture write the backend accepted: count its staging
 * copy, and once the device holds HL_GPU_MAX_QUEUED_WRITE_BYTES of them,
 * have the backend submit and wait for them (audit 34: wgpu frees a
 * write's staging only when a later submission completes, so uploads with
 * no dispatch grew device memory without bound). Called under the device
 * mutex. HL_GPU_OK, or HL_GPU_ERR_TIMEOUT when the flush timed out (the
 * device is then stalled). */
static int writes_settle(const HlGpuCtx *ctx, HlGpuDevice *dev, size_t n)
{
    dev->queued_write_bytes = sat_add(dev->queued_write_bytes, n);
    if (dev->queued_write_bytes < HL_GPU_MAX_QUEUED_WRITE_BYTES)
        return HL_GPU_OK;
    if (!ctx->backend->flush_writes) {
        dev->queued_write_bytes = 0;
        return HL_GPU_OK;
    }
    int rc = ctx->backend->flush_writes(dev, 0);
    submit_settle(dev, 0, rc);
    return rc == HL_GPU_ERR_TIMEOUT ? HL_GPU_ERR_TIMEOUT : HL_GPU_OK;
}

/* After a dispatch / pipeline that failed (neither OK nor a timeout): it
 * may have queued its inline uploads and failed before submitting them,
 * leaving their staging copies in the backend's pending writes. The wgpu
 * backend submits them itself on that path; here they are counted too -
 * the call's transient bytes, an upper bound - so the threshold flush comes
 * for a backend that does not (audit 35: the failure path counted nothing,
 * and a loop of failing dispatches grew device memory without bound). */
static void failed_call_settle(const HlGpuCtx *ctx, HlGpuDevice *dev,
                               size_t transient, int rc)
{
    if (rc == HL_GPU_OK || rc == HL_GPU_ERR_TIMEOUT || transient == 0)
        return;
    (void)writes_settle(ctx, dev, transient);
}

/* Bytes of a 4-aligned copy of @p n bytes, at least a word: a storage
 * buffer's allocation, and what wgpu stages for a buffer write or
 * read-back. */
static size_t word_bytes(size_t n)
{
    if (n == 0) return 4;
    return n > SIZE_MAX - 3 ? SIZE_MAX : (n + 3) & ~(size_t)3;
}

/* Bytes of a texture's staging copy: rows padded to 256 bytes, as WebGPU
 * lays out a texture <-> buffer copy. */
static size_t tex_staging_bytes(const HlGpuTexture *tex)
{
    size_t row = hl_gpu_tex_bytes(tex->width, 1, tex->format);
    if (row > SIZE_MAX - 255) return SIZE_MAX;
    row = (row + 255) & ~(size_t)255;
    if (tex->height && row > SIZE_MAX / tex->height) return SIZE_MAX;
    return row * tex->height;
}

size_t hl_gpu_tex_bytes(uint32_t width, uint32_t height, HlGpuTexFormat fmt)
{
    size_t bpp = 4;
    switch (fmt) {
    case HL_GPU_TEX_R8:      bpp = 1; break;
    case HL_GPU_TEX_RGBA16F: bpp = 8; break;
    case HL_GPU_TEX_R32F:    bpp = 4; break;
    case HL_GPU_TEX_RGBA8:   bpp = 4; break;
    }
    uint64_t n = (uint64_t)width * (uint64_t)height * (uint64_t)bpp;
    return n > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)n;
}

/* ── Transient (call-scoped) device memory ─────────────────────────────
 *
 * gpu.dispatch / gpu.pipeline create inline storage buffers, uniform
 * buffers and (dispatch) inline textures for the call itself: they were
 * neither charged to the run nor counted on the device's live total
 * (audit 17). The sizes follow the wgpu backend's own rounding (a word at
 * least, a uniform to 16 bytes); a pipeline's named, non-persistent buffer
 * is made once, at the largest size any stage declares. Saturates at
 * SIZE_MAX. */

/* (sat_add and word_bytes, the storage-buffer rounding, are above.) */

static size_t uniform_bytes(const void *data, size_t n)
{
    if (!data || n == 0) return 0;
    return n > SIZE_MAX - 15 ? SIZE_MAX : (n + 15) & ~(size_t)15;
}

static int persistent_buffer(const HlGpuDevice *dev, const char *name)
{
    if (!dev || !name) return 0;
    for (int i = 0; i < dev->buffer_count; i++)
        if (strcmp(dev->buffers[i].name, name) == 0)
            return 1;
    return 0;
}

static int persistent_texture(const HlGpuDevice *dev, const char *name)
{
    if (!dev || !name) return 0;
    for (int i = 0; i < dev->texture_count; i++)
        if (strcmp(dev->textures[i].name, name) == 0)
            return 1;
    return 0;
}

size_t hl_gpu_dispatch_transient_bytes(const HlGpuDevice *dev,
                                       const HlGpuDispatchOpts *opts)
{
    if (!opts) return 0;
    size_t n = uniform_bytes(opts->uniforms, opts->uniforms_len);
    for (int i = 0; opts->buffers && i < opts->buffer_count; i++) {
        const HlGpuBufferDesc *d = &opts->buffers[i];
        if (!persistent_buffer(dev, d->name))
            n = sat_add(n, word_bytes(d->size));
    }
    for (int i = 0; opts->textures && i < opts->texture_count; i++) {
        const HlGpuTextureDesc *td = &opts->textures[i];
        if (td->width > 0 && td->height > 0 && !persistent_texture(dev, td->name))
            n = sat_add(n, hl_gpu_tex_bytes(td->width, td->height, td->format));
    }
    return n;
}

size_t hl_gpu_pipeline_transient_bytes(const HlGpuDevice *dev,
                                       const HlGpuPipelineOpts *opts)
{
    if (!opts || !opts->stages) return 0;
    size_t n = 0;
    for (int s = 0; s < opts->stage_count; s++) {
        const HlGpuPipelineStage *st = &opts->stages[s];
        n = sat_add(n, uniform_bytes(st->uniforms, st->uniforms_len));
        for (int b = 0; st->buffers && b < st->buffer_count; b++) {
            const HlGpuBufferDesc *d = &st->buffers[b];
            if (!d->name) {
                n = sat_add(n, word_bytes(d->size));
                continue;
            }
            if (persistent_buffer(dev, d->name))
                continue;
            /* A named buffer counts once, at its first declaration, sized
             * by the largest declaration of that name in any stage. */
            int first = 1;
            size_t max = d->size;
            for (int s2 = 0; s2 < opts->stage_count && first; s2++) {
                const HlGpuPipelineStage *o = &opts->stages[s2];
                for (int b2 = 0; o->buffers && b2 < o->buffer_count; b2++) {
                    const HlGpuBufferDesc *e = &o->buffers[b2];
                    if (!e->name || strcmp(e->name, d->name) != 0)
                        continue;
                    if (s2 < s || (s2 == s && b2 < b)) { first = 0; break; }
                    if (e->size > max) max = e->size;
                }
            }
            if (first)
                n = sat_add(n, word_bytes(max));
        }
    }
    return n;
}

static size_t charge_bytes(size_t n)
{
    return n > HL_GPU_MAX_LIVE_BYTES ? HL_GPU_MAX_LIVE_BYTES : n;
}

size_t hl_gpu_dispatch_charge_bytes(const HlGpuDispatchOpts *opts)
{
    return charge_bytes(hl_gpu_dispatch_transient_bytes(NULL, opts));
}

size_t hl_gpu_pipeline_charge_bytes(const HlGpuPipelineOpts *opts)
{
    return charge_bytes(hl_gpu_pipeline_transient_bytes(NULL, opts));
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

    /* An abandoned compile still running uses its device's backend handle
     * (audit 40): waited for, bounded; past that the backend's devices and
     * instance are left unfreed rather than freed under it. */
    int backend_in_use = 0;
    for (int d = 0; d < ctx->device_count; d++)
        if (!compile_drain(&ctx->devices[d]))
            backend_in_use = 1;
    if (backend_in_use)
        log_warn("[hull:gpu] a shader compile is still running at "
                 "teardown: the GPU backend is left unfreed");

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
    if (ctx->backend_ctx && !backend_in_use)
        ctx->backend->destroy(ctx);

    memset(ctx, 0, sizeof(*ctx));
}

int hl_gpu_tex_binding_slots(int storage)
{
    return storage ? 1 : 2;
}

int hl_gpu_tex_binding_check(int desc_storage, int tex_storage,
                             int cursor, int end, const char **err)
{
    if ((desc_storage != 0) != (tex_storage != 0)) {
        if (err) *err = "texture_storage_mismatch";
        return -1;
    }
    int slots = hl_gpu_tex_binding_slots(tex_storage);
    if (cursor < 0 || end < 0 || cursor > end || end - cursor < slots) {
        if (err) *err = "too_many_bindings";
        return -1;
    }
    return slots;
}

/* ── Shader binding layout (audit 35) ──────────────────────────────── */

/* wgpu-native validates a bind group against the pipeline's layout, logs
 * the error and returns an invalid group; the submit that uses it is then
 * a Rust panic, which cannot unwind out of the C API and aborts the whole
 * process. So the bindings a call makes are checked against the shader's
 * declarations first. WGSL has no reflection in webgpu.h, so the
 * declarations are read from the source at compile: only the module-scope
 * `@group(G) @binding(B) var<...> name : type` form, with comments
 * skipped. Anything the reader does not follow leaves the layout unknown
 * (never a failed compile); the backend then refuses such a call itself
 * before it submits. */

/* The WGSL lexer these readers share (audit 37). It must split the source
 * exactly where naga does: a reader that skipped text naga parses can be
 * steered past the checks. So blankspace and comments follow the WGSL
 * spec to the code point, identifiers take every non-ASCII code point
 * (WGSL takes XID characters; anything else is a token naga refuses, so
 * reading it as identifier text hides nothing naga accepts), and a byte
 * outside every WGSL token - an ASCII control, `"`, `'`, `#`, `$`, `?`,
 * `\`, a backtick, malformed UTF-8 - is reported (WL_BAD) so the checks
 * refuse the source instead of stepping over it. */

/* The length of the well-formed UTF-8 sequence at s[i] (1-4), or 0. */
static size_t wl_utf8(const char *s, size_t n, size_t i)
{
    const unsigned char *u = (const unsigned char *)s;
    unsigned char c = u[i];
    if (c < 0x80) return 1;
    size_t len;
    unsigned char lo = 0x80, hi = 0xBF;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        if (c == 0xE0) lo = 0xA0;           /* no overlong form */
        if (c == 0xED) hi = 0x9F;           /* no surrogate */
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        if (c == 0xF0) lo = 0x90;
        if (c == 0xF4) hi = 0x8F;           /* nothing past U+10FFFF */
    } else {
        return 0;
    }
    if (len > n - i) return 0;
    if (u[i + 1] < lo || u[i + 1] > hi) return 0;
    for (size_t k = 2; k < len; k++)
        if (u[i + k] < 0x80 || u[i + k] > 0xBF) return 0;
    return len;
}

/* WGSL blankspace: U+0009-000D, U+0020, U+0085, U+200E, U+200F, U+2028,
 * U+2029. Returns its length at s[i], or 0. (These lead bytes are never
 * continuation bytes, so a match cannot start inside another character.) */
static size_t wl_blank(const char *s, size_t n, size_t i)
{
    const unsigned char *u = (const unsigned char *)s;
    unsigned char c = u[i];
    if ((c >= 0x09 && c <= 0x0D) || c == 0x20) return 1;
    if (c == 0xC2 && i + 1 < n && u[i + 1] == 0x85) return 2;
    if (c == 0xE2 && i + 2 < n && u[i + 1] == 0x80 &&
        (u[i + 2] == 0x8E || u[i + 2] == 0x8F ||
         u[i + 2] == 0xA8 || u[i + 2] == 0xA9))
        return 3;
    return 0;
}

/* What ends a WGSL line comment: U+000A-000D, U+0085, U+2028, U+2029. */
static int wl_line_break(const char *s, size_t n, size_t i)
{
    const unsigned char *u = (const unsigned char *)s;
    unsigned char c = u[i];
    if (c >= 0x0A && c <= 0x0D) return 1;
    if (c == 0xC2 && i + 1 < n && u[i + 1] == 0x85) return 1;
    return c == 0xE2 && i + 2 < n && u[i + 1] == 0x80 &&
           (u[i + 2] == 0xA8 || u[i + 2] == 0xA9);
}

static size_t wl_skip_ws(const char *s, size_t n, size_t i)
{
    while (i < n) {
        size_t b = wl_blank(s, n, i);
        if (b) {
            i += b;
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && !wl_line_break(s, n, i)) i++;
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            /* WGSL block comments nest; pairs are taken left to right,
             * as naga takes them. */
            int depth = 1;
            i += 2;
            while (i < n && depth > 0) {
                if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
                    depth++; i += 2;
                } else if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') {
                    depth--; i += 2;
                } else {
                    i++;
                }
            }
        } else {
            break;
        }
    }
    return i;
}

static int wl_digit(char c) { return c >= '0' && c <= '9'; }

/* The length of the identifier character at s[i]: an ASCII letter, digit
 * or '_', or a well-formed non-ASCII code point that is not blankspace.
 * 0 for anything else. */
static size_t wl_ident_unit(const char *s, size_t n, size_t i)
{
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
        return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                c == '_' || wl_digit((char)c)) ? 1 : 0;
    }
    if (wl_blank(s, n, i)) return 0;
    return wl_utf8(s, n, i);
}

static int wl_ident_start_at(const char *s, size_t n, size_t i)
{
    return i < n && !wl_digit(s[i]) && wl_ident_unit(s, n, i) > 0;
}

static int wl_ident_char_at(const char *s, size_t n, size_t i)
{
    return i < n && wl_ident_unit(s, n, i) > 0;
}

/* The identifier at s[i], as [*start, *start + *len); returns the index
 * past it (== i when there is none). */
static size_t wl_ident(const char *s, size_t n, size_t i,
                       const char **start, size_t *len)
{
    size_t j = i;
    if (wl_ident_start_at(s, n, j)) {
        size_t u;
        while (j < n && (u = wl_ident_unit(s, n, j)) > 0) j += u;
    }
    *start = s + i;
    *len = j - i;
    return j;
}

/* A numeric literal at s[i]: digits, a '.', a hex prefix, an exponent
 * whose sign is taken only before a digit, a suffix. Returns the index
 * past it. */
static size_t wl_number(const char *s, size_t n, size_t i)
{
    int hex = s[i] == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X');
    if (hex) i += 2;
    while (i < n) {
        char d = s[i];
        if ((!hex && (d == 'e' || d == 'E')) ||
            (hex && (d == 'p' || d == 'P'))) {
            i++;
            if (i + 1 < n && (s[i] == '+' || s[i] == '-') && wl_digit(s[i + 1]))
                i++;
        } else if ((d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') ||
                   wl_digit(d) || d == '_' || d == '.') {
            i++;
        } else {
            break;
        }
    }
    return i;
}

/* The ASCII punctuation WGSL tokens are made of. */
static int wl_punct(char c)
{
    return c != '\0' && strchr("!%&()*+,-./:;<=>@[]^{|}~", c) != NULL;
}

enum { WL_END, WL_IDENT, WL_NUM, WL_PUNCT, WL_BAD };

typedef struct {
    int kind;
    size_t start;      /* first byte */
    size_t end;        /* past the token (a punctuation token is one byte) */
} WlTok;

/* The token at or after s[i] (blankspace and comments skipped). */
static int wl_next(const char *s, size_t n, size_t i, WlTok *t)
{
    i = wl_skip_ws(s, n, i);
    t->start = t->end = i;
    if (i >= n) return t->kind = WL_END;
    char c = s[i];
    if (wl_digit(c) || (c == '.' && i + 1 < n && wl_digit(s[i + 1]))) {
        t->end = wl_number(s, n, i);
        return t->kind = WL_NUM;
    }
    if (wl_ident_start_at(s, n, i)) {
        const char *id;
        size_t len;
        t->end = wl_ident(s, n, i, &id, &len);
        return t->kind = WL_IDENT;
    }
    if (wl_punct(c)) {
        t->end = i + 1;
        return t->kind = WL_PUNCT;
    }
    return t->kind = WL_BAD;
}

/* Whether the whole source is well-formed UTF-8 (naga takes a Rust str). */
static int wl_utf8_valid(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if ((unsigned char)s[i] < 0x80) { i++; continue; }
        size_t u = wl_utf8(s, n, i);
        if (u == 0) return 0;
        i += u;
    }
    return 1;
}

static int wl_is(const char *id, size_t len, const char *word)
{
    size_t wl = strlen(word);
    return len == wl && memcmp(id, word, wl) == 0;
}

/* `( <int> [,] )` at s[i]: a decimal or 0x literal with an optional i / u
 * suffix. Returns the index past ')', or 0 when it is not that form. */
static size_t wl_int_arg(const char *s, size_t n, size_t i, unsigned long *out)
{
    i = wl_skip_ws(s, n, i);
    if (i >= n || s[i] != '(') return 0;
    i = wl_skip_ws(s, n, i + 1);
    unsigned long v = 0;
    size_t digits = 0;
    if (i + 1 < n && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
        i += 2;
        while (i < n) {
            char c = s[i];
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) break;
            if (v > 0xFFFFFul) return 0;
            v = v * 16 + (unsigned long)d;
            digits++; i++;
        }
    } else {
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            if (v > 0xFFFFFul) return 0;
            v = v * 10 + (unsigned long)(s[i] - '0');
            digits++; i++;
        }
    }
    if (digits == 0) return 0;
    if (i < n && (s[i] == 'i' || s[i] == 'u')) i++;
    if (i < n && wl_ident_char_at(s, n, i)) return 0;
    i = wl_skip_ws(s, n, i);
    if (i < n && s[i] == ',') i = wl_skip_ws(s, n, i + 1);
    if (i >= n || s[i] != ')') return 0;
    *out = v;
    return i + 1;
}

/* Skip a balanced (...) or <...> group starting at s[i] (== open). Returns
 * the index past the close, or 0 when unbalanced. */
static size_t wl_skip_group(const char *s, size_t n, size_t i,
                            char open, char close)
{
    int depth = 0;
    while (i < n) {
        size_t j = wl_skip_ws(s, n, i);
        if (j != i) { i = j; continue; }
        if (s[i] == open) depth++;
        else if (s[i] == close) {
            if (--depth == 0) return i + 1;
        }
        i++;
    }
    return 0;
}

static int wl_storage_format(const char *id, size_t len)
{
    if (wl_is(id, len, "rgba8unorm"))  return HL_GPU_TEX_RGBA8;
    if (wl_is(id, len, "r8unorm"))     return HL_GPU_TEX_R8;
    if (wl_is(id, len, "rgba16float")) return HL_GPU_TEX_RGBA16F;
    if (wl_is(id, len, "r32float"))    return HL_GPU_TEX_R32F;
    return 100;   /* a format no Hull texture has */
}

/* After `var` with group/binding attributes: fill *kind / *format. Returns
 * the index past the type's first identifier, or 0 when not followed. */
static size_t wl_var_decl(const char *s, size_t n, size_t i,
                          int *kind, int *format)
{
    const char *id;
    size_t len;
    *format = -1;
    int space = -1;
    i = wl_skip_ws(s, n, i);
    if (i < n && s[i] == '<') {
        size_t j = wl_skip_ws(s, n, i + 1);
        (void)wl_ident(s, n, j, &id, &len);
        if (wl_is(id, len, "uniform"))      space = HL_GPU_BIND_UNIFORM;
        else if (wl_is(id, len, "storage")) space = HL_GPU_BIND_STORAGE;
        else return 0;   /* private / workgroup / function take no binding */
        i = wl_skip_group(s, n, i, '<', '>');
        if (i == 0) return 0;
    }
    i = wl_skip_ws(s, n, i);
    i = wl_ident(s, n, i, &id, &len);           /* the variable's name */
    if (len == 0) return 0;
    i = wl_skip_ws(s, n, i);
    if (i >= n || s[i] != ':') return 0;
    i = wl_skip_ws(s, n, i + 1);
    i = wl_ident(s, n, i, &id, &len);           /* the type */
    if (len == 0) return 0;
    if (space >= 0) {
        *kind = space;
        return i;
    }
    if (wl_is(id, len, "sampler")) {
        *kind = HL_GPU_BIND_SAMPLER;
    } else if (wl_is(id, len, "texture_2d")) {
        *kind = HL_GPU_BIND_TEXTURE;
    } else if (wl_is(id, len, "texture_storage_2d")) {
        *kind = HL_GPU_BIND_STORAGE_TEXTURE;
        size_t j = wl_skip_ws(s, n, i);
        if (j < n && s[j] == '<') {
            j = wl_skip_ws(s, n, j + 1);
            (void)wl_ident(s, n, j, &id, &len);
            if (len == 0) return 0;
            *format = wl_storage_format(id, len);
        }
    } else if (len > 8 && memcmp(id, "texture_", 8) == 0) {
        *kind = HL_GPU_BIND_OTHER;      /* 1d / 3d / cube / array / depth / ... */
    } else if (wl_is(id, len, "sampler_comparison")) {
        *kind = HL_GPU_BIND_OTHER;
    } else {
        *kind = HL_GPU_BIND_UNKNOWN;    /* an alias */
    }
    return i;
}

int hl_gpu_wgsl_layout(const char *wgsl, size_t len, HlGpuPipeline *out)
{
    if (!out) return -1;
    out->layout_known = 0;
    out->decl_count = 0;
    if (!wgsl) return -1;

    const char *s = wgsl;
    size_t n = len;
    size_t i = 0;
    int have_group = 0, have_binding = 0;
    unsigned long group = 0, binding = 0;
    int count = 0;

    for (;;) {
        i = wl_skip_ws(s, n, i);
        if (i >= n) break;
        char c = s[i];
        const char *id;
        size_t idlen;

        if (c == '@') {
            i = wl_skip_ws(s, n, i + 1);
            i = wl_ident(s, n, i, &id, &idlen);
            if (idlen == 0) return -1;
            if (wl_is(id, idlen, "group") || wl_is(id, idlen, "binding")) {
                unsigned long v = 0;
                size_t j = wl_int_arg(s, n, i, &v);
                if (j == 0) return -1;   /* a const-expression index */
                if (id[0] == 'g') {
                    if (have_group) return -1;
                    have_group = 1; group = v;
                } else {
                    if (have_binding) return -1;
                    have_binding = 1; binding = v;
                }
                i = j;
            } else {
                size_t j = wl_skip_ws(s, n, i);
                if (j < n && s[j] == '(') {
                    j = wl_skip_group(s, n, j, '(', ')');
                    if (j == 0) return -1;
                    i = j;
                }
            }
            continue;
        }

        if (wl_ident_start_at(s, n, i)) {
            i = wl_ident(s, n, i, &id, &idlen);
            if (have_group || have_binding) {
                if (!wl_is(id, idlen, "var") || !have_group || !have_binding)
                    return -1;
                int kind = HL_GPU_BIND_UNKNOWN, format = -1;
                size_t j = wl_var_decl(s, n, i, &kind, &format);
                if (j == 0) return -1;
                i = j;
                if (group == 0) {
                    if (binding > 0xFFFF) return -1;
                    for (int k = 0; k < count; k++)
                        if (out->decls[k].binding == (uint16_t)binding)
                            return -1;          /* WGSL refuses it anyway */
                    if (count >= HL_GPU_SHADER_BINDINGS_MAX) return -1;
                    out->decls[count].binding = (uint16_t)binding;
                    out->decls[count].kind = (uint8_t)kind;
                    out->decls[count].format = (int8_t)format;
                    count++;
                }
                have_group = have_binding = 0;
            }
            continue;
        }

        if (have_group || have_binding) return -1;
        i++;
    }
    if (have_group || have_binding) return -1;

    out->decl_count = count;
    out->layout_known = 1;
    return 0;
}

/* ── Shader expression depth (audit 36) ────────────────────────────
 *
 * naga parses a chain of left-associative operators or accessors into a
 * tree as deep as the chain and walks it recursively (validation, constant
 * evaluation, the backend writers); its parser has a recursion limit,
 * those passes do not. A few thousand `+ y` in one expression overflowed
 * the event loop's stack and killed the process. So the depth a shader can
 * reach is bounded before the backend sees it.
 *
 * The estimate is an upper bound on the tree's depth, read token by token
 * (comments skipped, identifiers and numeric literals as single operands),
 * per statement (`;`, `{`, `}` end one). Each bracket level is a frame:
 * its binary operators (each may be one level of the tree), plus the
 * longest run of unary / postfix operators applied to one operand (`-`,
 * `!`, `.x`, `[i]`), plus the deepest bracketed sub-expression closed in
 * it (+1). A comma separates sibling arguments, so it starts a new count
 * and the frame keeps the largest. A long flat list - a constant array of
 * thousands of `-1.0` elements - stays shallow, while nesting
 * (`((a + a ...) + a ...) + a ...`) adds up. */

typedef struct {
    size_t bin;        /* binary operators in the current argument */
    size_t run;        /* unary / postfix operators on the current operand */
    size_t run_max;
    size_t child;      /* deepest closed sub-expression, +1 */
    size_t arg_max;    /* deepest earlier argument */
} WlFrame;

static size_t wl_frame_depth(const WlFrame *f)
{
    size_t cur = f->bin + f->run_max + f->child;
    return cur > f->arg_max ? cur : f->arg_max;
}

static void wl_frame_run(WlFrame *f)
{
    f->run++;
    if (f->run > f->run_max) f->run_max = f->run;
}

/* The operator at s[i]: its length (0 when s[i] starts none), and whether
 * it can be binary (*bin). */
static size_t wl_operator(const char *s, size_t n, size_t i, int *bin)
{
    static const char *const two[] = {
        "&&", "||", "==", "!=", "<=", ">=", "<<", ">>", "->",
        "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    };
    if (i + 3 <= n && (memcmp(s + i, "<<=", 3) == 0 ||
                       memcmp(s + i, ">>=", 3) == 0)) {
        *bin = 1;
        return 3;
    }
    if (i + 2 <= n && (memcmp(s + i, "++", 2) == 0 ||
                       memcmp(s + i, "--", 2) == 0)) {
        *bin = 0;
        return 2;
    }
    for (size_t k = 0; k < sizeof(two) / sizeof(two[0]); k++) {
        if (i + 2 <= n && memcmp(s + i, two[k], 2) == 0) {
            *bin = 1;
            return 2;
        }
    }
    switch (s[i]) {
    case '!': case '~':
        *bin = 0;
        return 1;
    case '-': case '&': case '*':           /* binary, or a prefix */
    case '+': case '/': case '%': case '|': case '^':
    case '<': case '>': case '=':
        *bin = 1;
        return 1;
    default:
        return 0;
    }
}

size_t hl_gpu_wgsl_complexity(const char *wgsl, size_t len)
{
    if (!wgsl) return 0;
    if (!wl_utf8_valid(wgsl, len)) return HL_GPU_WGSL_UNREADABLE;
    enum { WL_FRAMES = HL_GPU_WGSL_EXPR_DEPTH_MAX + 2 };
    WlFrame fr[WL_FRAMES];
    const char *s = wgsl;
    size_t n = len, i = 0, top = 0, peak = 0;
    int operand = 0;    /* the last token ended an operand */
    memset(&fr[0], 0, sizeof(fr[0]));

    /* Note depth d; past the limit the answer is known, stop reading. */
#define WL_NOTE(d) do {                                                   \
        size_t d_ = (d);                                                  \
        if (d_ > peak) peak = d_;                                         \
        if (peak > HL_GPU_WGSL_EXPR_DEPTH_MAX) return peak;               \
    } while (0)
    /* Close the innermost frame into its parent. */
#define WL_POP() do {                                                     \
        size_t c_ = wl_frame_depth(&fr[top]) + 1;                         \
        top--;                                                            \
        if (c_ > fr[top].child) fr[top].child = c_;                       \
        WL_NOTE(wl_frame_depth(&fr[top]));                                \
    } while (0)

    for (;;) {
        WlTok t;
        int kind = wl_next(s, n, i, &t);
        if (kind == WL_END) break;
        if (kind == WL_BAD) return HL_GPU_WGSL_UNREADABLE;
        i = t.start;
        char c = s[i];
        WlFrame *f = &fr[top];

        if (kind == WL_IDENT || kind == WL_NUM) {
            i = t.end;
            operand = 1;
            continue;
        }

        switch (c) {
        case ';': case '{': case '}':
            /* A statement ends: close what it left open, start over. */
            while (top > 0) WL_POP();
            WL_NOTE(wl_frame_depth(&fr[0]));
            memset(&fr[0], 0, sizeof(fr[0]));
            operand = 0;
            i++;
            continue;
        case '(': case '[':
            if (c == '[' && operand) {          /* an index accessor */
                wl_frame_run(f);
                WL_NOTE(wl_frame_depth(f));
            }
            if (top + 1 >= WL_FRAMES) return top + 1;
            top++;
            memset(&fr[top], 0, sizeof(fr[top]));
            operand = 0;
            i++;
            continue;
        case ')': case ']':
            if (top > 0) WL_POP();
            operand = 1;
            i++;
            continue;
        case ',':
            f->arg_max = wl_frame_depth(f);
            f->bin = f->run = f->run_max = f->child = 0;
            operand = 0;
            i++;
            continue;
        case '.':                               /* member / swizzle */
            wl_frame_run(f);
            WL_NOTE(wl_frame_depth(f));
            operand = 0;
            i++;
            continue;
        case '@': case ':':
            operand = 0;
            i++;
            continue;
        default:
            break;
        }

        int bin = 0;
        size_t olen = wl_operator(s, n, i, &bin);
        if (olen == 0) return HL_GPU_WGSL_UNREADABLE;   /* not reached */
        if (bin && operand) {
            f->bin++;
            f->run = 0;
        } else {
            /* A prefix operator, a postfix ++ / --, or a binary one with
             * no left operand: one more level on this operand. */
            wl_frame_run(f);
        }
        operand = 0;
        WL_NOTE(wl_frame_depth(f));
        i += olen;
    }
    while (top > 0) WL_POP();
    WL_NOTE(wl_frame_depth(&fr[0]));
#undef WL_POP
#undef WL_NOTE
    return peak;
}

/* ── Statement nesting (audit 37) ──────────────────────────────────
 *
 * naga's parser stops at 127 nested braces, but `else if` nests without
 * one: `if (c) {} else if (c) {} else if ...` is parsed into an If whose
 * reject block holds the next If, and validation walks that tree
 * recursively (validate_block_impl). ~5400 links overflowed the event
 * loop's stack while the expression estimate, which starts over at every
 * brace, stayed at 4. So the nesting of blocks is counted on its own: a
 * brace opens one level below the enclosing block, an `else if` body one
 * below the block of the `if` it continues, an `else` body at that
 * block's level. */

static int wl_tok_is(const char *s, const WlTok *t, const char *word)
{
    return t->kind == WL_IDENT && wl_is(s + t->start, t->end - t->start, word);
}

size_t hl_gpu_wgsl_block_depth(const char *wgsl, size_t len)
{
    if (!wgsl) return 0;
    if (!wl_utf8_valid(wgsl, len)) return HL_GPU_WGSL_UNREADABLE;
    enum { WL_BLOCKS = HL_GPU_WGSL_BLOCK_DEPTH_MAX + 2 };
    size_t level[WL_BLOCKS];   /* the depth of each open block */
    size_t top = 0, peak = 0, pending = 0, i = 0;
    const char *s = wgsl;
    size_t n = len;

    for (;;) {
        WlTok t;
        int kind = wl_next(s, n, i, &t);
        if (kind == WL_END) break;
        if (kind == WL_BAD) return HL_GPU_WGSL_UNREADABLE;
        i = t.end;
        if (kind != WL_PUNCT) continue;
        char c = s[t.start];
        if (c == '{') {
            size_t d = (top ? level[top - 1] : 0) + 1 + pending;
            pending = 0;
            if (d > peak) peak = d;
            if (peak > HL_GPU_WGSL_BLOCK_DEPTH_MAX) return peak;
            /* d >= top + 1, so the stack never fills before the limit. */
            if (top >= WL_BLOCKS) return HL_GPU_WGSL_UNREADABLE;
            level[top++] = d;
        } else if (c == '}' && top > 0) {
            size_t closed = level[--top];
            size_t parent = top ? level[top - 1] : 0;
            size_t extra = closed - parent - 1;   /* the else-if chain so far */
            WlTok e, f;
            if (wl_next(s, n, i, &e) == WL_IDENT && wl_tok_is(s, &e, "else")) {
                (void)wl_next(s, n, e.end, &f);
                pending = wl_tok_is(s, &f, "if") ? extra + 1 : extra;
            }
        }
    }
    return peak;
}

/* ── Type nesting (audit 37) ───────────────────────────────────────
 *
 * Each `alias A<k> = array<A<k-1>, 1>;` nests a type one level deeper than
 * the last; naga and the Metal / SPIR-V writers expand the whole chain,
 * work that grows with its square (10000 aliases: 24 s on the event loop,
 * and the Metal compiler service crashed on a few hundred). Module-scope
 * declarations may come in any order, so every `struct` and `alias` is
 * read first - its name, and each identifier its body names with the
 * template brackets open around it - and the depths are resolved after:
 * a type is as deep as the deepest type it names plus the template
 * brackets around that name (a predeclared type is 1), and a struct one
 * level more than its deepest member. Member and attribute names are not type
 * references. A cycle (WGSL refuses recursive types) counts as too deep. */

typedef struct {
    size_t start, len;        /* the name */
    size_t ref_first, ref_count;
    size_t depth;
    size_t next_ref;          /* resolution cursor */
    int state;                /* 0 unvisited, 1 on the stack, 2 done */
    int is_struct;
} WlTypeDecl;

typedef struct {
    size_t start, len;
    size_t level;             /* template brackets open around it */
    long target;              /* declaration index, or -1 */
} WlTypeRef;

static int wl_grow(void **p, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) return 0;
    size_t nc = *cap ? *cap * 2 : 64;
    while (nc < need) nc *= 2;
    if (nc > SIZE_MAX / elem) return -1;
    void *q = realloc(*p, nc * elem);
    if (!q) return -1;
    *p = q;
    *cap = nc;
    return 0;
}

/* Names resolved by sorting (audit 38): the declarations' names are merge
 * sorted (bottom-up, no recursion) and each reference binary-searched, so
 * resolution is O(n log n) whatever the names are - an unseeded hash table
 * could be filled with names that all collide. */
typedef struct {
    size_t start, len;
    long idx;                 /* the declaration's index */
} WlName;

static int wl_name_cmp(const char *s, size_t as, size_t al,
                       size_t bs, size_t bl)
{
    size_t m = al < bl ? al : bl;
    int c = memcmp(s + as, s + bs, m);
    if (c != 0) return c;
    return al < bl ? -1 : (al > bl ? 1 : 0);
}

/* Sort @p v (@p n entries) by name; @p tmp has room for @p n. */
static void wl_names_sort(const char *s, WlName *v, WlName *tmp, size_t n)
{
    WlName *src = v, *dst = tmp;
    for (size_t w = 1; w < n; w *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n;
            size_t hi = lo + 2 * w < n ? lo + 2 * w : n;
            size_t a = lo, b = mid, o = lo;
            while (a < mid && b < hi)
                dst[o++] = wl_name_cmp(s, src[b].start, src[b].len,
                                       src[a].start, src[a].len) < 0
                           ? src[b++] : src[a++];
            while (a < mid) dst[o++] = src[a++];
            while (b < hi) dst[o++] = src[b++];
        }
        WlName *sw = src; src = dst; dst = sw;
        if (w > SIZE_MAX / 4) break;
    }
    if (src != v) memcpy(v, src, n * sizeof(*v));
}

/* The declaration named s[start, start + len) in sorted @p v, or -1. */
static long wl_names_find(const char *s, const WlName *v, size_t n,
                          size_t start, size_t len)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = wl_name_cmp(s, v[mid].start, v[mid].len, start, len);
        if (c == 0) return v[mid].idx;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

size_t hl_gpu_wgsl_type_depth(const char *wgsl, size_t len)
{
    if (!wgsl) return 0;
    if (!wl_utf8_valid(wgsl, len)) return HL_GPU_WGSL_UNREADABLE;
    const char *s = wgsl;
    size_t n = len, i = 0, braces = 0, result = 0;
    WlTypeDecl *decls = NULL;
    WlTypeRef *refs = NULL;
    WlName *names = NULL;
    size_t *stack = NULL;
    size_t ndecl = 0, capdecl = 0, nref = 0, capref = 0;

    /* Pass 1: the declarations and what their bodies name. */
    for (;;) {
        WlTok t;
        int kind = wl_next(s, n, i, &t);
        if (kind == WL_END) break;
        if (kind == WL_BAD) { result = HL_GPU_WGSL_UNREADABLE; goto out; }
        i = t.end;
        if (kind == WL_PUNCT) {
            if (s[t.start] == '{') braces++;
            else if (s[t.start] == '}' && braces > 0) braces--;
            continue;
        }
        if (kind != WL_IDENT || braces != 0) continue;
        int is_struct = wl_tok_is(s, &t, "struct");
        if (!is_struct && !wl_tok_is(s, &t, "alias")) continue;

        WlTok name, open;
        if (wl_next(s, n, i, &name) != WL_IDENT) continue;
        if (wl_next(s, n, name.end, &open) != WL_PUNCT ||
            s[open.start] != (is_struct ? '{' : '='))
            continue;
        i = open.end;

        if (wl_grow((void **)&decls, &capdecl, ndecl + 1, sizeof(*decls)) != 0) {
            result = HL_GPU_WGSL_UNREADABLE;
            goto out;
        }
        WlTypeDecl *d = &decls[ndecl++];
        memset(d, 0, sizeof(*d));
        d->start = name.start;
        d->len = name.end - name.start;
        d->ref_first = nref;
        d->is_struct = is_struct;

        /* The body: to the struct's '}' or the alias's ';'. A template
         * '<' is closed only by a '>' (not '>=') at its own bracket depth,
         * as WGSL's template-list discovery pairs them. */
        enum { WL_TMPL_MAX = 256 };
        size_t tmpl[WL_TMPL_MAX];   /* bracket depth of each open '<' */
        size_t ntmpl = 0, pd = 0, extra_level = 0;
        int after_at = 0;
        for (;;) {
            WlTok b;
            int bk = wl_next(s, n, i, &b);
            if (bk == WL_END) break;
            if (bk == WL_BAD) { result = HL_GPU_WGSL_UNREADABLE; goto out; }
            i = b.end;
            if (bk == WL_IDENT) {
                WlTok nx;
                int member = wl_next(s, n, i, &nx) == WL_PUNCT &&
                             s[nx.start] == ':';
                if (!after_at && !member) {
                    if (wl_grow((void **)&refs, &capref, nref + 1,
                                sizeof(*refs)) != 0) {
                        result = HL_GPU_WGSL_UNREADABLE;
                        goto out;
                    }
                    refs[nref].start = b.start;
                    refs[nref].len = b.end - b.start;
                    refs[nref].level = ntmpl + extra_level;
                    refs[nref].target = -1;
                    nref++;
                }
                after_at = 0;
                continue;
            }
            after_at = 0;
            if (bk != WL_PUNCT) continue;
            char c = s[b.start];
            char nc = b.end < n ? s[b.end] : '\0';
            if (c == '@') {
                after_at = 1;
            } else if ((is_struct && c == '}') || (!is_struct && c == ';')) {
                break;
            } else if (c == '{' || c == '}' || c == ';') {
                break;                          /* malformed: stop here */
            } else if (c == '(' || c == '[') {
                pd++;
            } else if (c == ')' || c == ']') {
                if (pd > 0) pd--;
                while (ntmpl > 0 && tmpl[ntmpl - 1] > pd) ntmpl--;
            } else if (c == '<' && nc != '=' && nc != '<') {
                if (ntmpl < WL_TMPL_MAX) tmpl[ntmpl++] = pd;
                else extra_level++;             /* counted, never closed */
            } else if (c == '<') {
                i = b.end + 1;                  /* '<=' / '<<' */
            } else if (c == '>' && nc != '=') {
                if (ntmpl > 0 && tmpl[ntmpl - 1] == pd) ntmpl--;
            }
        }
        d->ref_count = nref - d->ref_first;
    }
    if (ndecl == 0) goto out;

    /* Resolve each reference to the declaration of that name. */
    names = malloc(ndecl * 2 * sizeof(*names));
    stack = malloc(ndecl * sizeof(*stack));
    if (!names || !stack) { result = HL_GPU_WGSL_UNREADABLE; goto out; }
    for (size_t k = 0; k < ndecl; k++) {
        names[k].start = decls[k].start;
        names[k].len = decls[k].len;
        names[k].idx = (long)k;
    }
    wl_names_sort(s, names, names + ndecl, ndecl);
    for (size_t r = 0; r < nref; r++)
        refs[r].target = wl_names_find(s, names, ndecl, refs[r].start,
                                       refs[r].len);

    /* Depths, by an explicit depth-first walk (no recursion here either). */
    for (size_t root = 0; root < ndecl; root++) {
        if (decls[root].state == 2) continue;
        size_t sp = 0;
        stack[sp++] = root;
        decls[root].state = 1;
        decls[root].next_ref = 0;
        decls[root].depth = 0;
        while (sp > 0) {
            WlTypeDecl *d = &decls[stack[sp - 1]];
            int descended = 0;
            /* refs is non-NULL whenever a declaration has references
             * (ref_count > 0 implies nref > 0); said explicitly for scan-build. */
            while (refs && d->next_ref < d->ref_count) {
                const WlTypeRef *r = &refs[d->ref_first + d->next_ref];
                size_t v = r->level + 1;    /* a predeclared type: 1 */
                if (r->target >= 0) {
                    WlTypeDecl *t = &decls[r->target];
                    if (t->state == 1) {            /* a cycle */
                        result = HL_GPU_WGSL_TYPE_DEPTH_MAX + 1;
                        goto out;
                    }
                    if (t->state == 0) {
                        t->state = 1;
                        t->next_ref = 0;
                        t->depth = 0;
                        stack[sp++] = (size_t)r->target;
                        descended = 1;
                        break;
                    }
                    v = r->level + t->depth;
                }
                if (v > d->depth) d->depth = v;
                d->next_ref++;
            }
            if (descended) continue;
            /* An alias is the type it names; a struct is one level more. */
            if (d->is_struct) d->depth += 1;
            if (d->depth == 0) d->depth = 1;
            d->state = 2;
            if (d->depth > result) result = d->depth;
            if (result > HL_GPU_WGSL_TYPE_DEPTH_MAX) goto out;
            sp--;
        }
    }
out:
    free(decls);
    free(refs);
    free(names);
    free(stack);
    return result;
}

/* ── Declaration reference chains (audit 38) ───────────────────────
 *
 * naga orders a module's declarations before it lowers them with a
 * recursive depth-first walk (front::wgsl::index::DependencySolver::dfs),
 * one stack frame per edge from a declaration to one it names. So a chain
 * `const c2 = c1; const c1 = c0; ...` - or overrides, or functions each
 * calling the next - each naming one declared after it (counted here in
 * any order) overflowed the event loop's stack (SIGSEGV) long
 * before any other check saw anything: every expression, block and type in
 * it is shallow. Here every module-scope declaration (const, override,
 * var, let, alias, struct, fn and the anonymous const_assert) is read with
 * every identifier it names - an initializer's, a type's, a function's
 * parameter and return types, the identifiers its body names (callees and
 * the globals it reads), and the arguments of the attributes before it -
 * and the longest chain of declarations naming one another is counted with
 * an explicit walk. Names a function declares itself (parameters, and
 * let / var / const in a block, from the end of their declaration to the
 * end of their block; a `for` header's to the end of the loop) shadow a
 * module-scope name, as in WGSL; member names (after '.', or before ':' in
 * a struct) and attribute names are not references. Anything else that
 * reads as a reference counts as one, so a misread can only lengthen a
 * chain. A cycle (WGSL refuses recursive declarations) counts as too
 * long. */

enum { WE_REF, WE_LOCAL, WE_OPEN, WE_FOROPEN, WE_CLOSE };

typedef struct {
    size_t start, len;
    long target;              /* declaration index, or -1 */
    int kind;
} WlEvt;

/* A function's own size, counted as hl_gpu_wgsl_size counts it (words,
 * let / var / const declarations, statements), and the same with every
 * call inlined (audit 41). */
typedef struct {
    uint64_t words, decls, stmts;
} WlCallSize;

typedef struct {
    size_t ev_first, ev_count;
    size_t depth, next;       /* chain length; walk cursor */
    int state;                /* 0 unvisited, 1 on the walk, 2 done */
    int is_fn, called;        /* a function; called by one */
    WlCallSize own, expanded;
} WlChainDecl;

typedef struct {
    const char *s;
    size_t n;
    WlEvt *ev;
    size_t nev, capev;
    int oom;
} WlChainScan;

static void wl_evt(WlChainScan *c, int kind, size_t start, size_t len)
{
    if (c->oom) return;
    if (wl_grow((void **)&c->ev, &c->capev, c->nev + 1, sizeof(*c->ev)) != 0) {
        c->oom = 1;
        return;
    }
    WlEvt *e = &c->ev[c->nev++];
    e->start = start;
    e->len = len;
    e->target = -1;
    e->kind = kind;
}

/* Past a template list `<...>` at s[i] (when one starts there), its
 * identifiers not recorded: `var<storage, read_write>`. */
static size_t wl_skip_template(const char *s, size_t n, size_t i)
{
    WlTok t;
    if (wl_next(s, n, i, &t) != WL_PUNCT || s[t.start] != '<') return i;
    size_t depth = 0;
    for (;;) {
        int k = wl_next(s, n, i, &t);
        if (k == WL_END || k == WL_BAD) return i;
        if (k == WL_PUNCT) {
            char c = s[t.start];
            if (c == ';' || c == '{' || c == '}') return t.start;
            if (c == '<') depth++;
            else if (c == '>' && depth > 0 && --depth == 0) return t.end;
        }
        i = t.end;
    }
}

/* Scan from s[i] to the end of a declaration: ';' (consumed) for a
 * simple one, the closing '}' for a struct; a brace or ';' it does not
 * expect ends it unconsumed. Records each identifier as a reference but
 * member and attribute names. Returns the index to continue at. */
static size_t wl_chain_body(WlChainScan *c, size_t i, int is_struct)
{
    const char *s = c->s;
    size_t n = c->n;
    int skip_next = 0;          /* after '.' or '@' */
    int opened = 0;             /* the struct's '{' was read */
    for (;;) {
        WlTok t;
        int k = wl_next(s, n, i, &t);
        if (k == WL_END || k == WL_BAD) return t.start;
        if (k == WL_IDENT) {
            i = t.end;
            if (skip_next) { skip_next = 0; continue; }
            WlTok nx;
            if (is_struct && wl_next(s, n, i, &nx) == WL_PUNCT &&
                s[nx.start] == ':')
                continue;                       /* a member name */
            wl_evt(c, WE_REF, t.start, t.end - t.start);
            continue;
        }
        skip_next = 0;
        i = t.end;
        if (k != WL_PUNCT) continue;
        char ch = s[t.start];
        if (ch == '.' || ch == '@') {
            skip_next = 1;
        } else if (is_struct && ch == '{' && !opened) {
            opened = 1;
        } else if (is_struct && ch == '}' && opened) {
            return t.end;
        } else if (!is_struct && ch == ';') {
            return t.end;
        } else if (ch == '{' || ch == '}' || ch == ';') {
            return t.start;                     /* malformed: stop here */
        }
    }
}

/* A function from s[i] (past its name): parameters, return type, body.
 * Its own size (audit 41) goes to @p sz: every word (identifier, keyword,
 * numeric literal; `fn` and the name count too), each let / var / const
 * in the body, and each statement (a block's `{`, or a `;` in the body
 * outside parentheses). */
static size_t wl_chain_fn(WlChainScan *c, size_t i, WlCallSize *sz)
{
    const char *s = c->s;
    size_t n = c->n;
    WlEvt *params = NULL;       /* parameter names, declared at the body */
    size_t np = 0, capp = 0, depth = 0, pd = 0, bp = 0;
    int skip_next = 0, done_params = 0, for_pending = 0;
    size_t local_start = 0, local_len = 0;   /* a let / var / const name */
    int local_pending = 0;

    sz->words = 2;              /* fn, its name */
    sz->decls = sz->stmts = 0;
    for (;;) {
        WlTok t;
        int k = wl_next(s, n, i, &t);
        if (k == WL_END || k == WL_BAD) { i = t.start; break; }
        i = t.end;
        if (k == WL_IDENT || k == WL_NUM) sz->words++;
        if (k == WL_IDENT) {
            if (skip_next) { skip_next = 0; continue; }
            if (depth == 0) {
                WlTok nx;
                if (pd == 1 && wl_next(s, n, i, &nx) == WL_PUNCT &&
                    s[nx.start] == ':') {
                    if (wl_grow((void **)&params, &capp, np + 1,
                                sizeof(*params)) != 0) {
                        c->oom = 1;
                        break;
                    }
                    params[np].start = t.start;
                    params[np].len = t.end - t.start;
                    np++;
                    continue;
                }
                wl_evt(c, WE_REF, t.start, t.end - t.start);
                continue;
            }
            int is_var = wl_tok_is(s, &t, "var");
            if (is_var || wl_tok_is(s, &t, "let") || wl_tok_is(s, &t, "const")) {
                sz->decls++;
                if (is_var) i = wl_skip_template(s, n, i);
                WlTok nm;
                if (wl_next(s, n, i, &nm) == WL_IDENT) {
                    sz->words++;
                    i = nm.end;
                    if (local_pending)
                        wl_evt(c, WE_LOCAL, local_start, local_len);
                    local_start = nm.start;
                    local_len = nm.end - nm.start;
                    local_pending = 1;
                }
                continue;
            }
            if (wl_tok_is(s, &t, "for")) {
                wl_evt(c, WE_OPEN, 0, 0);       /* the header's scope */
                for_pending = 1;
                continue;
            }
            wl_evt(c, WE_REF, t.start, t.end - t.start);
            continue;
        }
        skip_next = 0;
        if (k != WL_PUNCT) continue;
        char ch = s[t.start];
        if (ch == '.' || ch == '@') {
            skip_next = 1;
        } else if (depth == 0 && !done_params && (ch == '(' || ch == ')')) {
            /* The parameter list, and the parentheses of attributes in it:
             * a parameter name is an identifier before ':' at its level. */
            if (ch == '(') pd++;
            else if (pd > 0 && --pd == 0) done_params = 1;
        } else if (depth > 0 && ch == '(') {
            bp++;
        } else if (depth > 0 && ch == ')') {
            if (bp > 0) bp--;
        } else if (ch == '{') {
            sz->stmts++;
            if (depth == 0) {
                wl_evt(c, WE_OPEN, 0, 0);
                for (size_t p = 0; p < np; p++)
                    wl_evt(c, WE_LOCAL, params[p].start, params[p].len);
            } else {
                local_pending = 0;              /* malformed */
                wl_evt(c, for_pending ? WE_FOROPEN : WE_OPEN, 0, 0);
                for_pending = 0;
            }
            depth++;
        } else if (ch == '}') {
            if (depth == 0) { i = t.start; break; }
            local_pending = 0;
            wl_evt(c, WE_CLOSE, 0, 0);
            if (--depth == 0) break;
        } else if (ch == ';') {
            if (depth == 0) { i = t.start; break; }   /* malformed */
            if (bp == 0) sz->stmts++;
            if (local_pending) {
                wl_evt(c, WE_LOCAL, local_start, local_len);
                local_pending = 0;
            }
        }
    }
    free(params);
    return i;
}

static uint64_t wl_sat(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b ? UINT64_MAX : a + b;
}

static void wl_call_add(WlCallSize *acc, const WlCallSize *v)
{
    acc->words = wl_sat(acc->words, v->words);
    acc->decls = wl_sat(acc->decls, v->decls);
    acc->stmts = wl_sat(acc->stmts, v->stmts);
}

static void wl_call_max(WlCallSize *acc, const WlCallSize *v)
{
    if (v->words > acc->words) acc->words = v->words;
    if (v->decls > acc->decls) acc->decls = v->decls;
    if (v->stmts > acc->stmts) acc->stmts = v->stmts;
}

static void wl_calls_saturate(HlGpuWgslCalls *calls)
{
    calls->fn_words = calls->fn_decls = calls->fn_stmts = UINT64_MAX;
    calls->module_words = calls->module_decls = calls->module_stmts = UINT64_MAX;
    calls->decls_sq = UINT64_MAX;
}

/* The declaration graph: the longest chain (the result), and - when
 * @p calls is not NULL - each function's size with its calls inlined
 * (audit 41). A source the walk cannot finish (unreadable, a cycle, a
 * chain past HL_GPU_WGSL_DECL_CHAIN_MAX, no memory) saturates @p calls. */
static size_t wl_decl_graph(const char *wgsl, size_t len, HlGpuWgslCalls *calls)
{
    if (calls) memset(calls, 0, sizeof(*calls));
    if (!wgsl) return 0;
    if (!wl_utf8_valid(wgsl, len)) {
        if (calls) wl_calls_saturate(calls);
        return HL_GPU_WGSL_UNREADABLE;
    }
    const char *s = wgsl;
    size_t n = len, i = 0, result = 0;
    WlChainScan c = { s, n, NULL, 0, 0, 0 };
    WlChainDecl *decls = NULL;
    WlName *names = NULL;
    size_t *shadow = NULL, *stack = NULL;
    long *scope = NULL;
    size_t ndecl = 0, capdecl = 0, nnames = 0, capnames = 0;
    size_t top_first = 0;   /* events since the last declaration ended */

    /* Pass 1: the declarations and the events of each. */
    for (;;) {
        WlTok t;
        int kind = wl_next(s, n, i, &t);
        if (kind == WL_END) break;
        if (kind == WL_BAD) { result = HL_GPU_WGSL_UNREADABLE; goto out; }
        i = t.end;
        if (kind == WL_PUNCT && s[t.start] == '@') {
            /* An attribute: its name is not a reference, its arguments
             * are references of the declaration it precedes. */
            WlTok nm, op;
            if (wl_next(s, n, i, &nm) != WL_IDENT) continue;
            i = nm.end;
            if (wl_next(s, n, i, &op) != WL_PUNCT || s[op.start] != '(')
                continue;
            i = op.end;
            size_t pd = 1;
            int after_dot = 0;
            while (pd > 0) {
                WlTok a;
                int ak = wl_next(s, n, i, &a);
                if (ak == WL_END || ak == WL_BAD) break;
                if (ak == WL_PUNCT) {
                    char ch = s[a.start];
                    if (ch == ';' || ch == '{' || ch == '}') break;
                    if (ch == '(') pd++;
                    else if (ch == ')') pd--;
                    after_dot = ch == '.';
                } else {
                    if (ak == WL_IDENT && !after_dot)
                        wl_evt(&c, WE_REF, a.start, a.end - a.start);
                    after_dot = 0;
                }
                i = a.end;
            }
            continue;
        }
        if (kind != WL_IDENT) continue;
        int is_fn = wl_tok_is(s, &t, "fn");
        int is_struct = wl_tok_is(s, &t, "struct");
        int is_var = wl_tok_is(s, &t, "var");
        int is_assert = wl_tok_is(s, &t, "const_assert");
        if (!is_fn && !is_struct && !is_var && !is_assert &&
            !wl_tok_is(s, &t, "const") && !wl_tok_is(s, &t, "override") &&
            !wl_tok_is(s, &t, "let") && !wl_tok_is(s, &t, "alias"))
            continue;

        if (wl_grow((void **)&decls, &capdecl, ndecl + 1, sizeof(*decls)) != 0) {
            result = HL_GPU_WGSL_UNREADABLE;
            goto out;
        }
        WlChainDecl *d = &decls[ndecl];
        memset(d, 0, sizeof(*d));
        d->ev_first = top_first;
        if (!is_assert) {
            if (is_var) i = wl_skip_template(s, n, i);
            WlTok nm;
            if (wl_next(s, n, i, &nm) == WL_IDENT) {
                i = nm.end;
                if (wl_grow((void **)&names, &capnames, nnames + 1,
                            sizeof(*names)) != 0) {
                    result = HL_GPU_WGSL_UNREADABLE;
                    goto out;
                }
                names[nnames].start = nm.start;
                names[nnames].len = nm.end - nm.start;
                names[nnames].idx = (long)ndecl;
                nnames++;
            }
        }
        d->is_fn = is_fn;
        i = is_fn ? wl_chain_fn(&c, i, &d->own)
                  : wl_chain_body(&c, i, is_struct);
        if (c.oom) { result = HL_GPU_WGSL_UNREADABLE; goto out; }
        d->ev_count = c.nev - d->ev_first;
        top_first = c.nev;
        ndecl++;
    }
    if (c.oom) { result = HL_GPU_WGSL_UNREADABLE; goto out; }
    if (ndecl == 0) { calls = NULL; goto out; }

    /* Resolve names, a declaration at a time: a local declaration whose
     * name is a module-scope one hides it until its scope closes. */
    if (wl_grow((void **)&names, &capnames, nnames * 2 + 1,
                sizeof(*names)) != 0) {
        result = HL_GPU_WGSL_UNREADABLE;
        goto out;
    }
    wl_names_sort(s, names, names + nnames, nnames);
    shadow = calloc(ndecl, sizeof(*shadow));
    stack = malloc(ndecl * sizeof(*stack));
    scope = malloc((c.nev + 1) * sizeof(*scope));
    if (!shadow || !stack || !scope) { result = HL_GPU_WGSL_UNREADABLE; goto out; }
    for (size_t k = 0; k < ndecl; k++) {
        size_t sp = 0;
        for (size_t e = decls[k].ev_first;
             e < decls[k].ev_first + decls[k].ev_count; e++) {
            WlEvt *ev = &c.ev[e];
            if (ev->kind == WE_REF || ev->kind == WE_LOCAL) {
                long tg = wl_names_find(s, names, nnames, ev->start, ev->len);
                if (ev->kind == WE_REF) {
                    ev->target = (tg >= 0 && shadow[tg] == 0) ? tg : -1;
                } else if (tg >= 0) {
                    shadow[tg]++;
                    scope[sp++] = tg;
                }
            } else if (ev->kind == WE_OPEN) {
                scope[sp++] = -1;
            } else if (ev->kind == WE_FOROPEN) {
                scope[sp++] = -2;
            } else {                            /* WE_CLOSE */
                for (int pass = 0; pass < 2 && sp > 0; pass++) {
                    long m = -1;
                    while (sp > 0 && (m = scope[--sp]) >= 0) shadow[m]--;
                    if (m != -2) break;         /* a loop body: its header too */
                }
            }
        }
        while (sp > 0) {                        /* scopes left open */
            long m = scope[--sp];
            if (m >= 0) shadow[m]--;
        }
    }

    /* The longest chain, by an explicit depth-first walk. */
    for (size_t root = 0; root < ndecl; root++) {
        if (decls[root].state == 2) continue;
        size_t sp = 0;
        stack[sp++] = root;
        decls[root].state = 1;
        while (sp > 0) {
            WlChainDecl *d = &decls[stack[sp - 1]];
            int descended = 0;
            while (d->next < d->ev_count) {
                const WlEvt *ev = &c.ev[d->ev_first + d->next];
                if (ev->kind == WE_REF && ev->target >= 0) {
                    WlChainDecl *t = &decls[ev->target];
                    if (t->state == 1) {            /* a cycle */
                        result = HL_GPU_WGSL_DECL_CHAIN_MAX + 1;
                        goto out;
                    }
                    if (t->state == 0) {
                        t->state = 1;
                        stack[sp++] = (size_t)ev->target;
                        descended = 1;
                        break;
                    }
                    if (t->depth > d->depth) d->depth = t->depth;
                }
                d->next++;
            }
            if (descended) continue;
            d->depth += 1;                      /* itself */
            d->state = 2;
            if (d->is_fn) {
                /* Its size with every call inlined: its own, plus each
                 * call site's callee as inlined (every callee is done:
                 * a cycle was refused above). */
                d->expanded = d->own;
                for (size_t e = d->ev_first; e < d->ev_first + d->ev_count; e++) {
                    const WlEvt *ev = &c.ev[e];
                    if (ev->kind != WE_REF || ev->target < 0) continue;
                    WlChainDecl *t = &decls[ev->target];
                    if (!t->is_fn) continue;
                    t->called = 1;
                    wl_call_add(&d->expanded, &t->expanded);
                }
            }
            if (d->depth > result) result = d->depth;
            if (result > HL_GPU_WGSL_DECL_CHAIN_MAX) goto out;
            sp--;
        }
    }
    if (calls) {
        /* The largest function, and the module: every function no other
         * calls (the entry points, and helpers left unused) as inlined. */
        for (size_t k = 0; k < ndecl; k++) {
            if (!decls[k].is_fn) continue;
            WlCallSize fx = {
                decls[k].expanded.words, decls[k].expanded.decls,
                decls[k].expanded.stmts,
            };
            WlCallSize cur = { calls->fn_words, calls->fn_decls, calls->fn_stmts };
            wl_call_max(&cur, &fx);
            calls->fn_words = cur.words;
            calls->fn_decls = cur.decls;
            calls->fn_stmts = cur.stmts;
            if (decls[k].called) continue;
            calls->module_words = wl_sat(calls->module_words, fx.words);
            calls->module_decls = wl_sat(calls->module_decls, fx.decls);
            calls->module_stmts = wl_sat(calls->module_stmts, fx.stmts);
            uint64_t sq = fx.decls > UINT32_MAX ? UINT64_MAX : fx.decls * fx.decls;
            calls->decls_sq = wl_sat(calls->decls_sq, sq);
        }
        calls = NULL;                           /* complete */
    }
out:
    if (calls) wl_calls_saturate(calls);        /* the walk stopped early */
    free(c.ev);
    free(decls);
    free(names);
    free(shadow);
    free(stack);
    free(scope);
    return result;
}

size_t hl_gpu_wgsl_decl_chain(const char *wgsl, size_t len)
{
    return wl_decl_graph(wgsl, len, NULL);
}

int hl_gpu_wgsl_calls(const char *wgsl, size_t len, HlGpuWgslCalls *out)
{
    if (!out) return -1;
    size_t r = wl_decl_graph(wgsl, len, out);
    if (r == HL_GPU_WGSL_UNREADABLE || r > HL_GPU_WGSL_DECL_CHAIN_MAX)
        return -1;
    return 0;
}

/* ── Inferred nesting and expansion (audit 39) ──────────────────────
 *
 * The type and chain checks above read what a shader WRITES; naga also
 * works on what its initializers BUILD. A declaration that wraps the one
 * before it in a constructor - `const a1 = array(a0); const a2 =
 * array(a1); ...`, module scope or function-local, const or let - nests
 * its inferred type one level deeper each time with no template bracket
 * written, and 10000 function-local links crashed naga's MSL writer on
 * the event loop's stack. And a constant built from two copies of the one
 * before it (`const a2 = array(a1, a1)`) is kept by naga's evaluator as a
 * DAG that the writers expand into a tree: 16-20 such declarations, ~600
 * bytes of source, held the event loop 6-109 s.
 *
 * So every declaration - module scope and function-local const, let, var,
 * override, the parameters, alias, struct and a function's return type -
 * is read with the identifiers its initializer or type names, each with
 * the constructor levels (array / vecN / matCxR / atomic / ptr calls and
 * template lists, and calls of the module's struct / alias names) open
 * around it and the accessors (`.member`, `[index]`) applied after it, and:
 *
 *  - its inferred depth is the deepest constructor level it writes, or the
 *    depth of a declaration it names plus the levels around that name less
 *    the accessors after it (a struct is one more than its deepest member);
 *  - its expanded size is its own value tokens plus, for each const / let
 *    it names as a whole argument (between `(` / `,` / `=` and `)` / `,` /
 *    `;`, accessors allowed), that declaration's expanded size - the copy
 *    naga's evaluator makes. An operand of an operator (arithmetic folds to
 *    a scalar / vector / matrix) is one token; a module constant named from
 *    a function counts once (the writers name it), a function-local one is
 *    expanded at every use;
 *  - its chain is the longest run of function-local declarations naming
 *    one another (module-scope chains are hl_gpu_wgsl_decl_chain's).
 *
 * Names resolve as WGSL scopes them (a local hides a module name from the
 * end of its declaration to the end of its block). Linear, iterative, and
 * fail closed: a source it cannot read, or a cycle, counts as too deep. */

enum { WI_REF, WI_BIND, WI_OPEN, WI_FOROPEN, WI_CLOSE };
#define WI_SIZE_SAT ((uint64_t)1 << 40)   /* sizes saturate here */
enum { WN_CONST, WN_LET, WN_VAR, WN_OVERRIDE, WN_ALIAS, WN_STRUCT,
       WN_FN, WN_PARAM };

typedef struct {
    size_t start, len;        /* WI_REF: the name */
    long target;              /* WI_REF: the node it names, or -1 */
    long node;                /* WI_BIND: the node bound */
    int kind;
    int level;                /* WI_REF: constructor levels around it */
    int sub;                  /* WI_REF: accessors applied after it */
    unsigned char lead;       /* WI_REF: after ( , = or at the start */
    unsigned char whole;      /* WI_REF: a whole argument / initializer */
} WiEvt;

typedef struct {
    size_t name_start, name_len;   /* name_len 0: unnamed */
    size_t ev_first, ev_count;     /* its references, contiguous */
    size_t tokens;                 /* value tokens it writes */
    size_t own_depth;              /* deepest constructor level it writes */
    uint64_t size;
    size_t depth, chain, next;
    long nameid, prev;
    int state, kind, local;
} WiNode;

typedef struct { char kind; unsigned char inc, ctor; long base; } WiOpen;

typedef struct {
    const char *s;
    size_t n;
    WiEvt *ev;
    size_t nev, capev;
    WiNode *nd;
    size_t nnd, capnd;
    WiOpen *st;
    size_t capst;
    WlName *types;            /* module struct / alias names, sorted */
    size_t ntypes;
    int bad;
} WiScan;

static long wi_evt(WiScan *c, int kind)
{
    if (c->bad) return -1;
    if (wl_grow((void **)&c->ev, &c->capev, c->nev + 1, sizeof(*c->ev)) != 0) {
        c->bad = 1;
        return -1;
    }
    WiEvt *e = &c->ev[c->nev];
    memset(e, 0, sizeof(*e));
    e->kind = kind;
    e->target = -1;
    e->node = -1;
    return (long)c->nev++;
}

static long wi_node(WiScan *c, int kind, int local, const WlTok *name)
{
    if (c->bad) return -1;
    if (wl_grow((void **)&c->nd, &c->capnd, c->nnd + 1, sizeof(*c->nd)) != 0) {
        c->bad = 1;
        return -1;
    }
    WiNode *d = &c->nd[c->nnd];
    memset(d, 0, sizeof(*d));
    d->kind = kind;
    d->local = local;
    d->nameid = -1;
    d->prev = -1;
    if (name) {
        d->name_start = name->start;
        d->name_len = name->end - name->start;
    }
    d->ev_first = c->nev;
    return (long)c->nnd++;
}

/* array / atomic / ptr / binding_array / vecN / matCxR, with the f / i / u
 * / h shorthand suffixes when @p suffix: a type that nests what it holds. */
static int wi_builtin_ctor(const char *id, size_t len, int suffix)
{
    if (wl_is(id, len, "array") || wl_is(id, len, "atomic") ||
        wl_is(id, len, "ptr") || wl_is(id, len, "binding_array"))
        return 1;
    if (len >= 4 && memcmp(id, "vec", 3) == 0 && id[3] >= '2' && id[3] <= '4')
        return len == 4 || (suffix && len == 5 && strchr("fiuh", id[4]));
    if (len >= 6 && memcmp(id, "mat", 3) == 0 && id[3] >= '2' && id[3] <= '4' &&
        id[4] == 'x' && id[5] >= '2' && id[5] <= '4')
        return len == 6 || (suffix && len == 7 && strchr("fh", id[6]));
    return 0;
}

static int wi_is_type_name(const WiScan *c, const WlTok *t)
{
    return c->ntypes > 0 &&
           wl_names_find(c->s, c->types, c->ntypes, t->start,
                         t->end - t->start) >= 0;
}

/* Push a bracket; its constructor level counts when @p inc. */
static int wi_push(WiScan *c, size_t *sp, char kind, int inc, int ctor,
                   long base)
{
    if (wl_grow((void **)&c->st, &c->capst, *sp + 1, sizeof(*c->st)) != 0) {
        c->bad = 1;
        return -1;
    }
    c->st[*sp].kind = kind;
    c->st[*sp].inc = (unsigned char)(inc ? 1 : 0);
    c->st[*sp].ctor = (unsigned char)(ctor ? 1 : 0);
    c->st[*sp].base = base;
    (*sp)++;
    return 0;
}

/* Past an attribute's name and argument list at s[i] (just past '@'). */
static size_t wi_skip_attr(const char *s, size_t n, size_t i)
{
    WlTok nm, op;
    if (wl_next(s, n, i, &nm) != WL_IDENT) return i;
    i = nm.end;
    if (wl_next(s, n, i, &op) != WL_PUNCT || s[op.start] != '(') return i;
    size_t j = wl_skip_group(s, n, op.start, '(', ')');
    return j ? j : n;
}

enum { WI_TO_SEMI, WI_TO_PARAM, WI_TO_BRACE, WI_STRUCT };

/* Read an initializer / type from s[i] for @p owner (a node), recording
 * its references; stops as @p mode says. Returns the index to go on at. */
static size_t wi_scan(WiScan *c, size_t i, int mode, long owner)
{
    const char *s = c->s;
    size_t n = c->n, sp = 0;
    int level = 0, lead = 1;
    long last = -1;               /* the reference accessors apply to */
    WiNode *d = &c->nd[owner];

#define WI_WHOLE() do {                                                    \
        if (last >= 0 && c->ev[last].lead) c->ev[last].whole = 1;          \
    } while (0)

    for (;;) {
        if (c->bad) return n;
        WlTok t;
        int k = wl_next(s, n, i, &t);
        if (k == WL_END) { WI_WHOLE(); i = t.start; break; }
        if (k == WL_BAD) { c->bad = 1; return n; }
        if (k == WL_NUM) {
            d->tokens++;
            last = -1;
            lead = 0;
            i = t.end;
            continue;
        }
        if (k == WL_IDENT) {
            i = t.end;
            WlTok nx;
            int nk = wl_next(s, n, i, &nx);
            char nc = nk == WL_PUNCT ? s[nx.start] : '\0';
            if (mode == WI_STRUCT && sp == 0 && nc == ':') {
                lead = 0;                       /* a member name */
                last = -1;
                continue;
            }
            const char *id = s + t.start;
            size_t idl = t.end - t.start;
            d->tokens++;
            long e = wi_evt(c, WI_REF);
            if (e < 0) return n;
            c->ev[e].start = t.start;
            c->ev[e].len = idl;
            c->ev[e].level = level;
            if (nc == '(') {                    /* a call */
                int ctor = wi_builtin_ctor(id, idl, 1) || wi_is_type_name(c, &t);
                if (wi_push(c, &sp, '(', ctor, 0, -1) != 0) return n;
                level += ctor;
                if ((size_t)level > d->own_depth) d->own_depth = (size_t)level;
                i = nx.end;
                lead = 1;
                last = -1;
                continue;
            }
            if (nc == '<' && wi_builtin_ctor(id, idl, 0) &&
                !(nx.end < n && (s[nx.end] == '=' || s[nx.end] == '<'))) {
                if (wi_push(c, &sp, '<', 1, 1, -1) != 0) return n;
                level++;
                if ((size_t)level > d->own_depth) d->own_depth = (size_t)level;
                i = nx.end;
                lead = 0;
                last = -1;
                continue;
            }
            c->ev[e].lead = (unsigned char)lead;
            last = e;
            lead = 0;
            continue;
        }
        /* Punctuation. */
        char ch = s[t.start];
        char pc = t.start > 0 ? s[t.start - 1] : '\0';
        char nc = t.end < n ? s[t.end] : '\0';
        i = t.end;
        if (ch == '.') {
            WlTok m;
            if (wl_next(s, n, i, &m) == WL_IDENT) {
                i = m.end;
                if (last >= 0) c->ev[last].sub++;
            } else {
                last = -1;
            }
            lead = 0;
        } else if (ch == '[') {
            if (last >= 0) c->ev[last].sub++;
            if (wi_push(c, &sp, '[', 0, 0, last) != 0) return n;
            last = -1;
            lead = 0;
        } else if (ch == ']') {
            while (sp > 0 && c->st[sp - 1].kind == '<') level -= c->st[--sp].inc;
            last = -1;
            if (sp > 0 && c->st[sp - 1].kind == '[') last = c->st[--sp].base;
            lead = 0;
        } else if (ch == '(') {
            if (wi_push(c, &sp, '(', 0, 0, -1) != 0) return n;
            last = -1;
            lead = 1;
        } else if (ch == ')') {
            WI_WHOLE();
            last = -1;
            lead = 0;
            while (sp > 0 && c->st[sp - 1].kind != '(') level -= c->st[--sp].inc;
            if (sp == 0) {
                if (mode == WI_TO_PARAM) { i = t.start; break; }
                continue;                       /* malformed */
            }
            level -= c->st[--sp].inc;
        } else if (ch == '>' && nc != '=' && sp > 0 && c->st[sp - 1].kind == '<') {
            level -= c->st[--sp].inc;
            last = -1;
            lead = 0;
            WlTok op;
            if (wl_next(s, n, i, &op) == WL_PUNCT && s[op.start] == '(') {
                /* array<T, N>(...): the constructor's arguments, one level
                 * inside it as its template was. */
                if (wi_push(c, &sp, '(', 1, 0, -1) != 0) return n;
                level++;
                if ((size_t)level > d->own_depth) d->own_depth = (size_t)level;
                i = op.end;
                lead = 1;
            }
        } else if (ch == ',') {
            WI_WHOLE();
            last = -1;
            lead = 1;
            if (sp == 0 && mode == WI_TO_PARAM) { i = t.start; break; }
        } else if (ch == ';') {
            WI_WHOLE();
            if (mode != WI_TO_SEMI) i = t.start;
            break;
        } else if (ch == '=') {
            last = -1;
            /* An initializer starts here; '==', '<=', '>=', '!=' do not. */
            lead = !(pc == '=' || pc == '<' || pc == '>' || pc == '!' ||
                     nc == '=');
        } else if (ch == '{') {
            if (mode == WI_STRUCT) continue;    /* stray */
            i = t.start;
            break;
        } else if (ch == '}') {
            WI_WHOLE();
            if (mode != WI_STRUCT) i = t.start;
            break;
        } else if (ch == '@') {
            i = wi_skip_attr(s, n, i);
            last = -1;
            lead = 0;
        } else {
            last = -1;
            lead = 0;
        }
    }
#undef WI_WHOLE
    d = &c->nd[owner];
    d->ev_count = c->nev - d->ev_first;
    return i;
}

/* A `let` / `var` / `const` name at s[i] (past the keyword): the node,
 * scanned to the end of the declaration. Returns -1 when unnamed. */
static long wi_decl(WiScan *c, size_t *i, int kind, int local)
{
    const char *s = c->s;
    size_t n = c->n;
    if (kind == WN_VAR) *i = wl_skip_template(s, n, *i);
    WlTok nm;
    if (wl_next(s, n, *i, &nm) != WL_IDENT) return -1;
    *i = nm.end;
    long nd = wi_node(c, kind, local, &nm);
    if (nd < 0) return -1;
    *i = wi_scan(c, *i, kind == WN_STRUCT ? WI_STRUCT : WI_TO_SEMI, nd);
    return nd;
}

/* A function from s[i] (past `fn`): parameters, return type, body. */
static size_t wi_fn(WiScan *c, size_t i)
{
    const char *s = c->s;
    size_t n = c->n;
    WlTok nm, t;
    if (wl_next(s, n, i, &nm) != WL_IDENT) return i;
    i = nm.end;
    long fn = wi_node(c, WN_FN, 0, &nm);
    if (fn < 0) return n;
    size_t pfirst = c->nnd;
    if (wl_next(s, n, i, &t) != WL_PUNCT || s[t.start] != '(') return i;
    i = t.end;
    for (;;) {                                  /* the parameters */
        int k = wl_next(s, n, i, &t);
        if (k == WL_END || k == WL_BAD) return t.start;
        if (k == WL_PUNCT && s[t.start] == '@') { i = wi_skip_attr(s, n, t.end); continue; }
        if (k == WL_PUNCT && s[t.start] == ',') { i = t.end; continue; }
        if (k == WL_PUNCT && s[t.start] == ')') { i = t.end; break; }
        if (k != WL_IDENT) return t.start;      /* malformed */
        WlTok colon;
        if (wl_next(s, n, t.end, &colon) != WL_PUNCT || s[colon.start] != ':')
            return t.start;
        long p = wi_node(c, WN_PARAM, 1, &t);
        if (p < 0) return n;
        i = wi_scan(c, colon.end, WI_TO_PARAM, p);
        if (c->bad) return n;
    }
    size_t pend = c->nnd;
    /* The return type, owned by the function. */
    c->nd[fn].ev_first = c->nev;
    WlTok a, b;
    if (wl_next(s, n, i, &a) == WL_PUNCT && s[a.start] == '-' &&
        wl_next(s, n, a.end, &b) == WL_PUNCT && s[b.start] == '>')
        i = wi_scan(c, b.end, WI_TO_BRACE, fn);
    else
        c->nd[fn].ev_count = 0;
    if (c->bad) return n;
    if (wl_next(s, n, i, &t) != WL_PUNCT || s[t.start] != '{') return i;

    size_t depth = 0;
    int for_pending = 0;
    for (;;) {                                  /* the body */
        int k = wl_next(s, n, i, &t);
        if (k == WL_END) { i = t.start; break; }
        if (k == WL_BAD) { c->bad = 1; return n; }
        i = t.end;
        if (k == WL_IDENT) {
            int kind = wl_tok_is(s, &t, "let") ? WN_LET :
                       wl_tok_is(s, &t, "var") ? WN_VAR :
                       wl_tok_is(s, &t, "const") ? WN_CONST : -1;
            if (kind >= 0 && depth > 0) {
                long nd = wi_decl(c, &i, kind, 1);
                if (c->bad) return n;
                if (nd >= 0) {
                    long e = wi_evt(c, WI_BIND);
                    if (e < 0) return n;
                    c->ev[e].node = nd;
                }
            } else if (wl_tok_is(s, &t, "for")) {
                if (wi_evt(c, WI_OPEN) < 0) return n;   /* the header's scope */
                for_pending = 1;
            }
            continue;
        }
        if (k != WL_PUNCT) continue;
        char ch = s[t.start];
        if (ch == '{') {
            if (wi_evt(c, (depth > 0 && for_pending) ? WI_FOROPEN : WI_OPEN) < 0)
                return n;
            if (depth == 0) {
                for (size_t p = pfirst; p < pend; p++) {
                    long e = wi_evt(c, WI_BIND);
                    if (e < 0) return n;
                    c->ev[e].node = (long)p;
                }
            }
            for_pending = 0;
            depth++;
        } else if (ch == '}') {
            if (wi_evt(c, WI_CLOSE) < 0) return n;
            if (--depth == 0) break;
        } else if (ch == '@') {
            i = wi_skip_attr(s, n, i);
        }
    }
    while (depth-- > 0)                         /* left open: malformed */
        if (wi_evt(c, WI_CLOSE) < 0) return n;
    return i;
}

int hl_gpu_wgsl_inferred(const char *wgsl, size_t len, HlGpuWgslInferred *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!wgsl) return 0;
    if (!wl_utf8_valid(wgsl, len)) return -1;
    const char *s = wgsl;
    size_t n = len, i = 0;
    int rc = -1;
    WiScan c;
    memset(&c, 0, sizeof(c));
    c.s = s;
    c.n = n;
    WlName *names = NULL;
    long *cur = NULL, *modof = NULL, *scope = NULL;
    size_t *stack = NULL;

    /* Pass 0: the module's struct / alias names (constructor callees). */
    {
        size_t braces = 0, cap = 0;
        for (;;) {
            WlTok t;
            int k = wl_next(s, n, i, &t);
            if (k == WL_END) break;
            if (k == WL_BAD) goto out;
            i = t.end;
            if (k == WL_PUNCT) {
                if (s[t.start] == '{') braces++;
                else if (s[t.start] == '}' && braces > 0) braces--;
                continue;
            }
            if (k != WL_IDENT || braces != 0) continue;
            if (!wl_tok_is(s, &t, "struct") && !wl_tok_is(s, &t, "alias"))
                continue;
            WlTok nm;
            if (wl_next(s, n, i, &nm) != WL_IDENT) continue;
            if (wl_grow((void **)&c.types, &cap, c.ntypes * 2 + 2,
                        sizeof(*c.types)) != 0)
                goto out;
            c.types[c.ntypes].start = nm.start;
            c.types[c.ntypes].len = nm.end - nm.start;
            c.types[c.ntypes].idx = (long)c.ntypes;
            c.ntypes++;
        }
        if (c.ntypes > 0)
            wl_names_sort(s, c.types, c.types + c.ntypes, c.ntypes);
    }

    /* Pass 1: every declaration and the references it makes. */
    i = 0;
    for (;;) {
        WlTok t;
        int k = wl_next(s, n, i, &t);
        if (k == WL_END) break;
        if (k == WL_BAD) goto out;
        i = t.end;
        if (k == WL_PUNCT && s[t.start] == '@') { i = wi_skip_attr(s, n, i); continue; }
        if (k != WL_IDENT) continue;
        if (wl_tok_is(s, &t, "fn")) {
            i = wi_fn(&c, i);
        } else if (wl_tok_is(s, &t, "const")) {
            wi_decl(&c, &i, WN_CONST, 0);
        } else if (wl_tok_is(s, &t, "let")) {
            wi_decl(&c, &i, WN_LET, 0);
        } else if (wl_tok_is(s, &t, "var")) {
            wi_decl(&c, &i, WN_VAR, 0);
        } else if (wl_tok_is(s, &t, "override")) {
            wi_decl(&c, &i, WN_OVERRIDE, 0);
        } else if (wl_tok_is(s, &t, "alias")) {
            wi_decl(&c, &i, WN_ALIAS, 0);
        } else if (wl_tok_is(s, &t, "struct")) {
            wi_decl(&c, &i, WN_STRUCT, 0);
        }
        if (c.bad) goto out;
    }
    if (c.bad) goto out;
    if (c.nnd == 0) { rc = 0; goto out; }

    /* Name ids: one per distinct name, by sorting. */
    size_t nn = 0;
    names = malloc(c.nnd * 2 * sizeof(*names));
    if (!names) goto out;
    for (size_t k = 0; k < c.nnd; k++) {
        if (c.nd[k].name_len == 0) continue;
        names[nn].start = c.nd[k].name_start;
        names[nn].len = c.nd[k].name_len;
        names[nn].idx = (long)k;
        nn++;
    }
    wl_names_sort(s, names, names + nn, nn);
    long nids = 0;
    for (size_t k = 0; k < nn; k++) {
        if (k > 0 && wl_name_cmp(s, names[k - 1].start, names[k - 1].len,
                                 names[k].start, names[k].len) != 0)
            nids++;
        c.nd[names[k].idx].nameid = nids;
    }
    nids = nn > 0 ? nids + 1 : 0;
    cur = malloc(((size_t)nids + 1) * sizeof(*cur));
    modof = malloc(((size_t)nids + 1) * sizeof(*modof));
    scope = malloc((c.nev + 1) * sizeof(*scope));
    stack = malloc(c.nnd * sizeof(*stack));
    if (!cur || !modof || !scope || !stack) goto out;
    for (long k = 0; k < nids; k++) { cur[k] = -1; modof[k] = -1; }
    for (size_t k = 0; k < c.nnd; k++) {
        const WiNode *d = &c.nd[k];
        if (!d->local && d->nameid >= 0 && modof[d->nameid] < 0)
            modof[d->nameid] = (long)k;
    }

    /* Resolve every reference in order, locals scoped as WGSL scopes them. */
    {
        size_t sp = 0;
        for (size_t e = 0; e < c.nev; e++) {
            WiEvt *ev = &c.ev[e];
            if (ev->kind == WI_REF) {
                long hit = wl_names_find(s, names, nn, ev->start, ev->len);
                if (hit < 0) continue;
                long id = c.nd[hit].nameid;
                ev->target = cur[id] >= 0 ? cur[id] : modof[id];
            } else if (ev->kind == WI_BIND) {
                WiNode *d = &c.nd[ev->node];
                if (d->nameid < 0) continue;
                d->prev = cur[d->nameid];
                cur[d->nameid] = ev->node;
                scope[sp++] = ev->node;
            } else if (ev->kind == WI_OPEN) {
                scope[sp++] = -1;
            } else if (ev->kind == WI_FOROPEN) {
                scope[sp++] = -2;
            } else {                            /* WI_CLOSE */
                for (int pass = 0; pass < 2 && sp > 0; pass++) {
                    long m = -1;
                    while (sp > 0 && (m = scope[--sp]) >= 0)
                        cur[c.nd[m].nameid] = c.nd[m].prev;
                    if (m != -2) break;         /* a loop body: its header too */
                }
            }
        }
    }

    /* Depth, size and chain of every declaration, by an explicit walk. */
    for (size_t root = 0; root < c.nnd; root++) {
        if (c.nd[root].state == 2) continue;
        size_t sp = 0;
        stack[sp++] = root;
        c.nd[root].state = 1;
        while (sp > 0) {
            WiNode *d = &c.nd[stack[sp - 1]];
            int descended = 0;
            while (d->next < d->ev_count) {
                const WiEvt *ev = &c.ev[d->ev_first + d->next];
                if (ev->kind == WI_REF && ev->target >= 0) {
                    WiNode *t = &c.nd[ev->target];
                    if (t->state == 1) {        /* a cycle */
                        out->depth = HL_GPU_WGSL_UNREADABLE;
                        rc = 0;
                        goto out;
                    }
                    if (t->state == 0) {
                        t->state = 1;
                        stack[sp++] = (size_t)ev->target;
                        descended = 1;
                        break;
                    }
                }
                d->next++;
            }
            if (descended) continue;
            /* Every reference is resolved: combine. */
            size_t depth = d->own_depth, chain = 0;
            uint64_t size = d->tokens;
            for (size_t q = 0; q < d->ev_count; q++) {
                const WiEvt *ev = &c.ev[d->ev_first + q];
                if (ev->kind != WI_REF || ev->target < 0) continue;
                const WiNode *t = &c.nd[ev->target];
                size_t sum = t->depth + (size_t)ev->level;
                size_t dd = sum > (size_t)ev->sub ? sum - (size_t)ev->sub : 0;
                if (dd > depth) depth = dd;
                if (d->local && t->local && t->chain > chain) chain = t->chain;
                if (ev->whole && (t->kind == WN_CONST || t->kind == WN_LET) &&
                    (t->local || !d->local) && t->size > 1)
                    size += t->size - 1;        /* the copy it names */
                if (size > WI_SIZE_SAT)
                    size = WI_SIZE_SAT;
            }
            if (d->kind == WN_STRUCT) depth++;
            d->depth = depth;
            d->size = size;
            d->chain = d->local ? chain + 1 : 0;
            d->state = 2;
            if (depth > out->depth) out->depth = depth;
            if (d->chain > out->chain) out->chain = d->chain;
            if (d->kind == WN_CONST || d->kind == WN_LET) {
                out->expanded += size - (size > d->tokens ? d->tokens : size);
                if (out->expanded > WI_SIZE_SAT)
                    out->expanded = WI_SIZE_SAT;
            }
            sp--;
        }
    }
    rc = 0;
out:
    if (rc != 0) {
        out->depth = HL_GPU_WGSL_UNREADABLE;
        out->chain = HL_GPU_WGSL_UNREADABLE;
        out->expanded = WI_SIZE_SAT;
    }
    free(c.ev);
    free(c.nd);
    free(c.st);
    free(c.types);
    free(names);
    free(cur);
    free(modof);
    free(scope);
    free(stack);
    return rc;
}

/* ── Function size (audit 40) ───────────────────────────────────────
 *
 * Every check above bounds a depth; none bounded how much code a shader
 * is. Metal's compiler (MTLCompilerService, out of process, behind
 * wgpuDeviceCreateComputePipeline) takes time that grows faster than
 * linearly with the code in one function: one entry with N
 * `let vK = o[K] * 3u;` followed by N `s ^= vK;` took 4.6 s at N = 4000 and
 * 343 s at N = 30000 (957 KB, every depth shallow), all on the event loop.
 * So the size of each function body and of the module is counted here:
 * words (identifiers, keywords and numeric literals - every operand),
 * let / var / const declarations, and statements (a `;` outside
 * parentheses, or a block's `{`). Module-scope `var` declarations count
 * toward the module's words (a `var<private>` table is initialized in
 * code); module-scope `const` / `override` / `alias` / `struct` do not
 * (naga folds them; a 30000-entry const table compiled in 0.11 s). */

typedef struct {
    size_t words, decls, stmts;
} WsFn;

static void ws_fn_end(HlGpuWgslSize *out, const WsFn *f)
{
    if (f->words > out->fn_words) out->fn_words = f->words;
    if (f->decls > out->fn_decls) out->fn_decls = f->decls;
    if (f->stmts > out->fn_stmts) out->fn_stmts = f->stmts;
    uint64_t sq = (uint64_t)f->decls * (uint64_t)f->decls;
    out->decls_sq = out->decls_sq > UINT64_MAX - sq ? UINT64_MAX
                                                     : out->decls_sq + sq;
}

int hl_gpu_wgsl_size(const char *wgsl, size_t len, HlGpuWgslSize *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!wgsl) return 0;
    if (!wl_utf8_valid(wgsl, len)) return -1;
    enum { MK_NONE, MK_FN, MK_VAR, MK_OTHER };
    const char *s = wgsl;
    size_t n = len, i = 0;
    size_t braces = 0, parens = 0;
    int kind = MK_NONE, in_fn = 0, after_at = 0;
    WsFn f = { 0, 0, 0 };
    for (;;) {
        WlTok t;
        int k = wl_next(s, n, i, &t);
        if (k == WL_END) break;
        if (k == WL_BAD) {
            if (in_fn) ws_fn_end(out, &f);
            return -1;
        }
        i = t.end;
        int at = after_at;
        after_at = 0;
        if (k == WL_IDENT || k == WL_NUM) {
            if (in_fn) {
                f.words++;
                out->module_words++;
                if (k == WL_IDENT && (wl_tok_is(s, &t, "let") ||
                                      wl_tok_is(s, &t, "var") ||
                                      wl_tok_is(s, &t, "const")))
                    f.decls++;
            } else if (braces == 0) {
                if (kind == MK_NONE && parens == 0 && !at && k == WL_IDENT) {
                    if (wl_tok_is(s, &t, "fn")) {
                        kind = MK_FN;
                        in_fn = 1;               /* the header counts too */
                        memset(&f, 0, sizeof(f));
                        f.words = 1;
                        out->module_words++;
                        continue;
                    }
                    kind = wl_tok_is(s, &t, "var") ? MK_VAR : MK_OTHER;
                }
                if (kind == MK_VAR) out->module_words++;
            }
            continue;
        }
        char c = s[t.start];
        if (c == '@') { after_at = 1; continue; }
        if (c == '(') { parens++; continue; }
        if (c == ')') { if (parens > 0) parens--; continue; }
        if (c == '{') {
            braces++;
            if (in_fn) {
                f.stmts++;
                out->module_stmts++;
            }
            continue;
        }
        if (c == '}') {
            if (braces > 0) braces--;
            if (braces == 0) {
                if (in_fn) ws_fn_end(out, &f);
                in_fn = 0;
                kind = MK_NONE;
                parens = 0;
            }
            continue;
        }
        if (c == ';') {
            if (in_fn && parens == 0) {
                f.stmts++;
                out->module_stmts++;
            } else if (braces == 0) {
                kind = MK_NONE;
                parens = 0;
            }
        }
    }
    if (in_fn) ws_fn_end(out, &f);               /* left open: malformed */
    return 0;
}

static uint64_t sat_mul_add(uint64_t acc, uint64_t a, uint64_t b)
{
    if (a != 0 && b > UINT64_MAX / a) return UINT64_MAX;
    uint64_t p = a * b;
    return acc > UINT64_MAX - p ? UINT64_MAX : acc + p;
}

uint64_t hl_gpu_wgsl_compile_units(const char *wgsl, size_t len)
{
    uint64_t units = hl_work_bytes_units((uint64_t)len);
    HlGpuWgslInferred inf;
    if (wgsl && len <= HL_GPU_WGSL_MAX_BYTES &&
        hl_gpu_wgsl_inferred(wgsl, len, &inf) == 0 &&
        inf.expanded <= HL_GPU_WGSL_EXPAND_MAX)
        units += inf.expanded * HL_GPU_WGSL_EXPAND_UNITS;
    /* The size of the code (audit 40), whether or not the source then
     * passes the checks: a refused source is charged like one that
     * compiled, so a loop of refused compiles trips the budget too. */
    if (wgsl && len <= HL_GPU_WGSL_MAX_BYTES) {
        HlGpuWgslSize sz;
        (void)hl_gpu_wgsl_size(wgsl, len, &sz);
        /* Words and statements as compiled, every call inlined (audit 41),
         * when that is more; a source whose calls cannot be counted (a
         * cycle, a chain past its bound) is refused and charged as
         * written. Declarations squared stay per function as written:
         * inlined copies are independent code, and their cost is in the
         * words. */
        uint64_t words = sz.module_words, stmts = sz.module_stmts;
        HlGpuWgslCalls calls;
        if (hl_gpu_wgsl_calls(wgsl, len, &calls) == 0) {
            if (calls.module_words > words) words = calls.module_words;
            if (calls.module_stmts > stmts) stmts = calls.module_stmts;
        }
        units = sat_mul_add(units, words, HL_GPU_WGSL_WORD_UNITS);
        units = sat_mul_add(units, stmts, HL_GPU_WGSL_STMT_UNITS);
        units = sat_mul_add(units, sz.decls_sq, HL_GPU_WGSL_DECL_SQ_UNITS);
    }
    return units;
}

/* The audit-40 size bounds: NULL when the source is within them. */
static const char *wl_size_refusal(const char *wgsl, size_t len)
{
    HlGpuWgslSize sz;
    if (hl_gpu_wgsl_size(wgsl, len, &sz) != 0)
        return "a byte sequence outside WGSL's tokens";
    if (sz.fn_words > HL_GPU_WGSL_FN_WORDS_MAX)
        return "a function is too large (words)";
    if (sz.fn_decls > HL_GPU_WGSL_FN_DECLS_MAX)
        return "a function declares too many values";
    if (sz.fn_stmts > HL_GPU_WGSL_FN_STMTS_MAX)
        return "a function has too many statements";
    if (sz.module_words > HL_GPU_WGSL_MODULE_WORDS_MAX)
        return "the shader is too large (words)";
    if (sz.module_stmts > HL_GPU_WGSL_MODULE_STMTS_MAX)
        return "the shader has too many statements";
    /* The same with every call inlined (audit 41): a chain of small
     * functions each calling the one before a few times is within every
     * bound above and doubles, or more, at each link as compiled. The
     * module's totals bound every function too: each one is inlined into
     * a function no other calls. */
    HlGpuWgslCalls calls;
    if (hl_gpu_wgsl_calls(wgsl, len, &calls) != 0)
        return "functions call one another in a cycle or too deeply";
    if (calls.module_words > HL_GPU_WGSL_INLINED_WORDS_MAX)
        return "calls expand the shader too far (words, inlined)";
    if (calls.module_decls > HL_GPU_WGSL_INLINED_DECLS_MAX)
        return "calls expand the shader too far (declarations, inlined)";
    if (calls.module_stmts > HL_GPU_WGSL_INLINED_STMTS_MAX)
        return "calls expand the shader too far (statements, inlined)";
    return NULL;
}

int hl_gpu_wgsl_precheck(const char *wgsl, size_t len, const char **why)
{
    const char *w = NULL;
    if (!wgsl) {
        w = "no source";
    } else if (len > HL_GPU_WGSL_MAX_BYTES) {
        w = "source too large";
    } else {
        size_t e = hl_gpu_wgsl_complexity(wgsl, len);
        if (e == HL_GPU_WGSL_UNREADABLE) {
            w = "a byte sequence outside WGSL's tokens";
        } else if (e > HL_GPU_WGSL_EXPR_DEPTH_MAX) {
            w = "an expression nests too deeply";
        } else if (hl_gpu_wgsl_block_depth(wgsl, len) >
                   HL_GPU_WGSL_BLOCK_DEPTH_MAX) {
            w = "statements (blocks / else-if chains) nest too deeply";
        } else if (hl_gpu_wgsl_type_depth(wgsl, len) >
                   HL_GPU_WGSL_TYPE_DEPTH_MAX) {
            w = "types nest too deeply";
        } else if (hl_gpu_wgsl_decl_chain(wgsl, len) >
                   HL_GPU_WGSL_DECL_CHAIN_MAX) {
            w = "module-scope declarations refer to one another too deeply";
        } else {
            HlGpuWgslInferred inf;
            if (hl_gpu_wgsl_inferred(wgsl, len, &inf) != 0)
                w = "a byte sequence outside WGSL's tokens";
            else if (inf.depth > HL_GPU_WGSL_INFERRED_DEPTH_MAX)
                w = "values nest too deeply (through their declarations)";
            else if (inf.expanded > HL_GPU_WGSL_EXPAND_MAX)
                w = "constant expressions expand too far";
            else if (inf.chain > HL_GPU_WGSL_DECL_CHAIN_MAX)
                w = "function-local declarations refer to one another too deeply";
            else
                w = wl_size_refusal(wgsl, len);
        }
    }
    if (why) *why = w;
    return w ? -1 : 0;
}

static const HlGpuBindingDecl *wl_find_decl(const HlGpuPipeline *p,
                                            int binding)
{
    if (binding < 0) return NULL;
    for (int k = 0; k < p->decl_count; k++)
        if (p->decls[k].binding == (uint16_t)binding)
            return &p->decls[k];
    return NULL;
}

/* Binding @p b must be declared, as @p want (UNKNOWN declarations accept a
 * texture / sampler slot), and a storage texture with @p format. */
static int wl_slot_ok(const HlGpuPipeline *p, int b, int want, int format)
{
    const HlGpuBindingDecl *d = wl_find_decl(p, b);
    if (!d) return 0;
    if (d->kind == HL_GPU_BIND_UNKNOWN)
        return want == HL_GPU_BIND_TEXTURE ||
               want == HL_GPU_BIND_STORAGE_TEXTURE ||
               want == HL_GPU_BIND_SAMPLER;
    if (d->kind != want) return 0;
    if (want == HL_GPU_BIND_STORAGE_TEXTURE && d->format >= 0 &&
        d->format != format)
        return 0;
    return 1;
}

int hl_gpu_bindings_check(const HlGpuPipeline *pipeline,
                          const HlGpuDevice *dev, int has_uniforms,
                          const HlGpuBufferDesc *buffers, int buffer_count,
                          const HlGpuTextureDesc *textures, int texture_count,
                          const char **err)
{
    static const char *mismatch = "binding_mismatch";
    if (!pipeline || !pipeline->layout_known)
        return 0;
    if (buffer_count < 0 || texture_count < 0 ||
        (buffer_count > 0 && !buffers) || (texture_count > 0 && !textures)) {
        if (err) *err = mismatch;
        return -1;
    }
    int b = 0;
    if (has_uniforms) {
        if (!wl_slot_ok(pipeline, b, HL_GPU_BIND_UNIFORM, -1)) goto bad;
        b++;
    }
    for (int i = 0; i < buffer_count; i++, b++)
        if (!wl_slot_ok(pipeline, b, HL_GPU_BIND_STORAGE, -1)) goto bad;
    for (int i = 0; i < texture_count; i++) {
        const HlGpuTextureDesc *td = &textures[i];
        int format = (int)td->format;
        if (td->name && dev) {
            for (int t = 0; t < dev->texture_count; t++) {
                if (strcmp(dev->textures[t].name, td->name) == 0) {
                    format = (int)dev->textures[t].format;
                    break;
                }
            }
        }
        if (td->storage) {
            if (!wl_slot_ok(pipeline, b, HL_GPU_BIND_STORAGE_TEXTURE, format))
                goto bad;
            b++;
        } else {
            if (!wl_slot_ok(pipeline, b, HL_GPU_BIND_TEXTURE, -1) ||
                !wl_slot_ok(pipeline, b + 1, HL_GPU_BIND_SAMPLER, -1))
                goto bad;
            b += 2;
        }
    }
    return 0;
bad:
    if (err) *err = mismatch;
    return -1;
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

/* ── The compile thread (audit 39) ──────────────────────────────────
 *
 * naga (inside wgpuDeviceCreateShaderModule and the pipeline creation that
 * translates the module for the platform) recurses over the shader's
 * expressions, statements, types and declarations. Rounds 36-39 each found
 * one more shape that recursed past the event loop's 8 MiB stack - an
 * uncatchable SIGSEGV - and each added an estimator to
 * hl_gpu_wgsl_precheck. The estimators stay as the fast first line; the
 * backend compile itself now runs on a thread of its own whose stack is
 * sized so that no source under HL_GPU_WGSL_MAX_BYTES can overflow it
 * (HL_GPU_COMPILE_STACK_BYTES, a virtual reservation: only the pages a
 * compile touches are committed, and the thread exits after each compile,
 * so they are returned). The event loop waits for it - the compile was
 * already synchronous - but no longer without end (audit 40): Metal's
 * compiler service took minutes over a wide shader, and a plain
 * pthread_join held the loop past the run's deadline, a tripped budget and
 * SIGTERM. The wait now polls the run and has a deadline of its own; a
 * compile past either is abandoned to its thread (gpu_compile_wait,
 * compile_busy). wgpu-native's device is thread-safe (gpu.async
 * dispatches use it from pool threads), and the backend's error flag is
 * thread-local, set and read on the compile thread. Signals are blocked on
 * the thread, so they keep going to the threads that handle them. */

static int g_precheck_bypass;      /* test-only: see gpu.h */
static uint32_t g_compile_timeout_ms;   /* test-only: 0 = the default */

void hl_gpu_test_set_precheck_bypass(int on)
{
    g_precheck_bypass = on ? 1 : 0;
}

void hl_gpu_test_set_compile_timeout_ms(uint32_t ms)
{
    g_compile_timeout_ms = ms;
}

/* One backend compile, shared by the waiting event loop and the compile
 * thread (audit 40). The wait has a deadline: a compile past it, or past
 * the run's deadline / budget, is abandoned - the loop returns and the
 * thread runs on. So the job owns everything the thread touches: copies
 * of the name and source, the pipeline it builds, and a device shadow
 * carrying the backend handle (the real HlGpuDevice is the context's,
 * which teardown clears). The last reference frees it. */
struct HlGpuCompileJob {
    const HlGpuBackend *backend;
    HlGpuDevice        *shadow;
    char               *name;
    char               *wgsl;
    size_t              wgsl_len;
    HlGpuPipeline       out;
    int                 rc;
    pthread_mutex_t     mu;
    pthread_cond_t      cv;
    int                 done;       /* the thread is finished with the backend */
    int                 abandoned;  /* nobody takes `out`: the thread frees it */
    int                 refs;
};
typedef struct HlGpuCompileJob HlGpuCompileJob;

static void compile_job_unref(HlGpuCompileJob *job)
{
    pthread_mutex_lock(&job->mu);
    int last = --job->refs == 0;
    pthread_mutex_unlock(&job->mu);
    if (!last) return;
    pthread_cond_destroy(&job->cv);
    pthread_mutex_destroy(&job->mu);
    free(job->shadow);
    free(job->name);
    free(job->wgsl);
    free(job);
}

static HlGpuCompileJob *compile_job_new(const HlGpuCtx *ctx,
                                        const HlGpuDevice *dev,
                                        const char *name,
                                        const char *wgsl, size_t wgsl_len)
{
    HlGpuCompileJob *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    size_t nl = strlen(name);
    job->backend = ctx->backend;
    job->shadow = calloc(1, sizeof(*job->shadow));
    job->name = malloc(nl + 1);
    job->wgsl = malloc(wgsl_len + 1);
    if (!job->shadow || !job->name || !job->wgsl) goto fail;
    if (pthread_mutex_init(&job->mu, NULL) != 0) goto fail;
    if (pthread_cond_init(&job->cv, NULL) != 0) {
        pthread_mutex_destroy(&job->mu);
        goto fail;
    }
    memcpy(job->shadow->name, dev->name, sizeof(job->shadow->name));
    job->shadow->backend_device = dev->backend_device;
    memcpy(job->name, name, nl + 1);
    memcpy(job->wgsl, wgsl, wgsl_len);
    job->wgsl[wgsl_len] = '\0';
    job->wgsl_len = wgsl_len;
    job->rc = HL_GPU_ERR_INTERNAL;
    job->refs = 1;
    return job;
fail:
    free(job->shadow);
    free(job->name);
    free(job->wgsl);
    free(job);
    return NULL;
}

static void *gpu_compile_main(void *arg)
{
    HlGpuCompileJob *job = arg;
    int rc = job->backend->compile(job->shadow, job->name, job->wgsl,
                                   job->wgsl_len, &job->out);
    pthread_mutex_lock(&job->mu);
    job->rc = rc;
    if (job->abandoned) {
        /* Nobody takes the pipeline: release it before `done`, which is
         * what tells the device (and teardown) the backend is free. */
        pthread_mutex_unlock(&job->mu);
        if (rc == HL_GPU_OK && job->backend->pipeline_destroy)
            job->backend->pipeline_destroy(job->shadow, &job->out);
        pthread_mutex_lock(&job->mu);
    }
    job->done = 1;
    pthread_cond_broadcast(&job->cv);
    pthread_mutex_unlock(&job->mu);
    compile_job_unref(job);
    return NULL;
}

/* Start the compile thread, detached, holding its own reference. A host
 * that cannot reserve the full stack gets a smaller one (still far above
 * the event loop's, with the estimators bounding what reaches it). 0, or
 * -1 with no thread at all (the compile is then refused, never run on the
 * loop). */
static int gpu_compile_start(HlGpuCompileJob *job)
{
    static const size_t sizes[] = {
        HL_GPU_COMPILE_STACK_BYTES,
        (size_t)1 << 30,
        (size_t)256 << 20,
        (size_t)64 << 20,
    };
    sigset_t all, old;
    sigfillset(&all);
    int masked = pthread_sigmask(SIG_SETMASK, &all, &old) == 0;
    int started = 0;
    pthread_t th;
    job->refs++;                        /* the thread's */
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]) && !started; k++) {
        if (k > 0 && sizes[k] >= HL_GPU_COMPILE_STACK_BYTES) continue;
        pthread_attr_t attr;
        if (pthread_attr_init(&attr) != 0) continue;
        if (pthread_attr_setstacksize(&attr, sizes[k]) == 0 &&
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0 &&
            pthread_create(&th, &attr, gpu_compile_main, job) == 0) {
            started = 1;
            if (k > 0)
                log_warn("[hull:gpu] shader compile thread got a %zu MiB "
                         "stack (%zu MiB could not be reserved)",
                         sizes[k] >> 20,
                         (size_t)HL_GPU_COMPILE_STACK_BYTES >> 20);
        }
        pthread_attr_destroy(&attr);
    }
    if (masked) pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (!started) {
        job->refs--;
        log_error("[hull:gpu] no thread for the shader compile: refused");
        return -1;
    }
    return 0;
}

static uint64_t gpu_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Whether the calling run was stopped: its deadline passed or its budget
 * tripped (a 0-unit charge asks, as every GPU wait does - audit 33). */
static int gpu_compile_run_stopped(void)
{
#ifdef HL_ENABLE_DB
    return hl_db_budget_charge(0) != 0;
#else
    return 0;
#endif
}

/* Wait for the compile: 1 when it finished, 0 when it was abandoned (the
 * deadline, or the run stopped). Polled every HL_GPU_COMPILE_POLL_MS. */
static int gpu_compile_wait(HlGpuCompileJob *job)
{
    uint32_t limit = g_compile_timeout_ms ? g_compile_timeout_ms
                                          : HL_GPU_COMPILE_TIMEOUT_MS;
    uint64_t deadline = gpu_mono_ms() + limit;
    pthread_mutex_lock(&job->mu);
    for (;;) {
        if (job->done) break;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)HL_GPU_COMPILE_POLL_MS * 1000000L;
        while (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        (void)pthread_cond_timedwait(&job->cv, &job->mu, &ts);
        if (job->done) break;
        pthread_mutex_unlock(&job->mu);
        int stop = gpu_mono_ms() >= deadline || gpu_compile_run_stopped();
        pthread_mutex_lock(&job->mu);
        if (stop && !job->done) {
            job->abandoned = 1;
            break;
        }
    }
    int finished = job->done && !job->abandoned;
    pthread_mutex_unlock(&job->mu);
    return finished;
}

/* Whether the device still has an abandoned compile running (audit 40).
 * Called under the device mutex; drops the device's reference once the
 * compile is finished. */
static int compile_busy(HlGpuDevice *dev)
{
    HlGpuCompileJob *job = dev->compile_job;
    if (!job) return 0;
    pthread_mutex_lock(&job->mu);
    int done = job->done;
    pthread_mutex_unlock(&job->mu);
    if (!done) return 1;
    dev->compile_job = NULL;
    compile_job_unref(job);
    return 0;
}

/* At teardown: wait (bounded) for a device's abandoned compile. 1 when it
 * finished (or there was none), 0 when it still runs - the backend's
 * device must then not be freed under it. */
static int compile_drain(HlGpuDevice *dev)
{
    HlGpuCompileJob *job = dev->compile_job;
    if (!job) return 1;
    uint64_t deadline = gpu_mono_ms() + HL_GPU_COMPILE_TEARDOWN_MS;
    pthread_mutex_lock(&job->mu);
    while (!job->done && gpu_mono_ms() < deadline) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += (long)HL_GPU_COMPILE_POLL_MS * 1000000L;
        while (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        (void)pthread_cond_timedwait(&job->cv, &job->mu, &ts);
    }
    int done = job->done;
    pthread_mutex_unlock(&job->mu);
    dev->compile_job = NULL;
    compile_job_unref(job);   /* a still-running thread frees it at its end */
    return done;
}

const char *hl_gpu_compile_errstr(int rc)
{
    if (rc == HL_GPU_ERR_TIMEOUT) return "compile_timeout";
    if (rc == HL_GPU_ERR_BUSY) return GPU_BUSY_MSG;
    return "shader_error";
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
    /* A source naga would recurse over until the stack overflows, or spend
     * unbounded time expanding, is refused before any wgpu call (audits
     * 36 / 37). Every check is a linear scan, bounded by the size cap. */
    const char *why = NULL;
    if (!g_precheck_bypass &&
        hl_gpu_wgsl_precheck(wgsl, wgsl_len, &why) != 0) {
        log_warn("[hull:gpu] shader '%s' refused before compile: %s "
                 "(%zu bytes of WGSL)", name, why, wgsl_len);
        return HL_GPU_ERR_SHADER;
    }
    /* A name must fit its slot: stored truncated but looked up in full,
     * it never matched, and each call took a new slot until the device's
     * table was full. */
    if (strlen(name) >= HL_GPU_NAME_MAX)
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

    /* An abandoned compile still running holds the device (audit 40). */
    if (compile_busy(dev)) {
        pthread_mutex_unlock(&dev->mutex);
        log_warn("[hull:gpu] shader '%s' refused: %s (an earlier compile "
                 "is still running)", name, GPU_BUSY_MSG);
        return HL_GPU_ERR_BUSY;
    }

    /* Check capacity */
    if (dev->pipeline_count >= HL_GPU_PIPELINE_MAX) {
        pthread_mutex_unlock(&dev->mutex);
        log_error("[hull:gpu] pipeline cache full (%d)", HL_GPU_PIPELINE_MAX);
        return HL_GPU_ERR_INTERNAL;
    }

    /* Compile via backend, on a thread of its own (audit 39), waited for
     * with a deadline (audit 40). */
    HlGpuPipeline *slot = &dev->pipelines[dev->pipeline_count];
    HlGpuCompileJob *job = compile_job_new(ctx, dev, name, wgsl, wgsl_len);
    int rc;
    if (!job) {
        rc = HL_GPU_ERR_INTERNAL;
    } else if (gpu_compile_start(job) != 0) {
        compile_job_unref(job);
        rc = HL_GPU_ERR_INTERNAL;
    } else if (gpu_compile_wait(job)) {
        rc = job->rc;
        if (rc == HL_GPU_OK) {
            slot->handle = job->out.handle;
            slot->bind_group_layout = job->out.bind_group_layout;
        }
        compile_job_unref(job);
    } else {
        /* Abandoned: the device keeps the loop's reference and refuses GPU
         * work until the thread is done (compile_busy). */
        dev->compile_job = job;
        rc = HL_GPU_ERR_TIMEOUT;
        log_warn("[hull:gpu] shader '%s' compile abandoned: past its "
                 "deadline or the run's (%zu bytes of WGSL); the device is "
                 "busy until it finishes", name, wgsl_len);
    }
    if (rc == HL_GPU_OK) {
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        /* Group 0's declared bindings, checked before every dispatch and
         * pipeline stage that uses it (audit 35). */
        (void)hl_gpu_wgsl_layout(wgsl, wgsl_len, slot);
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
                        const HlGpuDispatchOpts *opts_in,
                        void **output, size_t *output_len,
                        const char **err_msg)
{
    /* The timeout is clamped here, for every binding and the async worker
     * (audit 33), on a copy: the caller's opts are const. */
    HlGpuDispatchOpts opts_c;
    const HlGpuDispatchOpts *opts = NULL;
    if (opts_in) {
        opts_c = *opts_in;
        opts_c.timeout_ms = hl_gpu_timeout_clamp(opts_c.timeout_ms);
        opts = &opts_c;
    }
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

    /* Every binding the call makes must be one the shader declares, before
     * wgpu sees it: a submit with a bind group wgpu found invalid aborts
     * the process (audit 35). */
    if (hl_gpu_bindings_check(pipeline, dev,
                              opts->uniforms && opts->uniforms_len > 0,
                              opts->buffers, opts->buffer_count,
                              opts->textures, opts->texture_count,
                              err_msg) != 0) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_DISPATCH;
    }

    if (stalled_refuse(ctx, dev)) {
        pthread_mutex_unlock(&dev->mutex);
        if (err_msg) *err_msg = GPU_BUSY_MSG;
        return HL_GPU_ERR_TIMEOUT;
    }

    /* The call's own buffers, uniforms and inline textures are held on the
     * device while it runs: reserved against the live ceiling before the
     * backend makes them, given back after (audit 17) - or, when it timed
     * out, once the device is idle again (audit 33). */
    size_t transient = hl_gpu_dispatch_transient_bytes(dev, opts);
    if (live_reserve(dev, transient) != 0) {
        pthread_mutex_unlock(&dev->mutex);
        if (err_msg) *err_msg = "device_memory_limit";
        return HL_GPU_ERR_BUFFER;
    }

    /* Dispatch through backend */
    int rc = ctx->backend->dispatch(dev, pipeline, opts,
                                    dev->buffers, dev->buffer_count,
                                    dev->textures, dev->texture_count,
                                    output, output_len, err_msg);
    transient_settle(dev, transient, rc);
    submit_settle(dev, 0, rc);
    failed_call_settle(ctx, dev, transient, rc);

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

int hl_cap_gpu_pipeline(HlGpuCtx *ctx, const HlGpuPipelineOpts *opts_in,
                         HlGpuPipelineResult *result, const char **err_msg)
{
    static const char *err_internal = "internal_error";

    /* As hl_cap_gpu_dispatch: the timeout clamped for every caller. */
    HlGpuPipelineOpts opts_c;
    const HlGpuPipelineOpts *opts = NULL;
    if (opts_in) {
        opts_c = *opts_in;
        opts_c.timeout_ms = hl_gpu_timeout_clamp(opts_c.timeout_ms);
        opts = &opts_c;
    }

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
        /* As hl_cap_gpu_dispatch: each stage's bindings against its
         * shader's declarations (audit 35). */
        const HlGpuPipelineStage *st = &opts->stages[i];
        if (hl_gpu_bindings_check(pipelines[i], dev,
                                  st->uniforms && st->uniforms_len > 0,
                                  st->buffers, st->buffer_count,
                                  st->textures, st->texture_count,
                                  err_msg) != 0) {
            pthread_mutex_unlock(&dev->mutex);
            return HL_GPU_ERR_DISPATCH;
        }
    }

    if (!ctx->backend->dispatch_pipeline) {
        pthread_mutex_unlock(&dev->mutex);
        if (err_msg) *err_msg = "pipeline_not_supported";
        return HL_GPU_ERR_INTERNAL;
    }

    memset(result, 0, sizeof(*result));

    if (stalled_refuse(ctx, dev)) {
        pthread_mutex_unlock(&dev->mutex);
        if (err_msg) *err_msg = GPU_BUSY_MSG;
        return HL_GPU_ERR_TIMEOUT;
    }

    /* As hl_cap_gpu_dispatch: the stages' own buffers are reserved for the
     * call (audit 17), and kept while a timed-out call runs (audit 33). */
    size_t transient = hl_gpu_pipeline_transient_bytes(dev, opts);
    if (live_reserve(dev, transient) != 0) {
        pthread_mutex_unlock(&dev->mutex);
        if (err_msg) *err_msg = "device_memory_limit";
        return HL_GPU_ERR_BUFFER;
    }

    int rc = ctx->backend->dispatch_pipeline(
        dev, pipelines, opts->stage_count, opts,
        dev->buffers, dev->buffer_count,
        dev->textures, dev->texture_count, result, err_msg);
    transient_settle(dev, transient, rc);
    submit_settle(dev, 0, rc);
    failed_call_settle(ctx, dev, transient, rc);

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
    /* A name must fit its slot: stored truncated but looked up in full,
     * it never matched, and each call took a new slot until the device's
     * table was full. */
    if (strlen(name) >= HL_GPU_NAME_MAX)
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
    /* ...and the device's byte ceiling, before the backend allocates. */
    if (live_reserve(dev, size) != 0) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_BUFFER;
    }

    HlGpuBuffer *slot = &dev->buffers[dev->buffer_count];
    int rc = ctx->backend->buffer_create(dev, name,
                                         size, usage, slot);
    if (rc == HL_GPU_OK) {
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        slot->size = size;
        slot->usage = usage;
        dev->buffer_count++;
    } else {
        live_release(dev, size);
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
    /* WebGPU writes whole 4-byte words at 4-aligned offsets. An unaligned
     * write was a validation error that only got logged - the call
     * "succeeded" and the buffer kept its old bytes. A short tail is padded
     * by the backend, which is only harmless at the buffer's end (the
     * allocation's padding); elsewhere it would zero the next bytes. */
    if (offset % 4 != 0 || (len % 4 != 0 && offset + len != buf->size)) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_BUFFER;
    }
    /* A stalled device queues nothing more (audit 34). */
    if (stalled_refuse(ctx, dev)) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_BUSY;
    }

    int rc = ctx->backend->buffer_write(dev, buf,
                                        data, len, offset);
    /* Counted even when the backend refused it: a write split in two
     * (whole words, then a padded tail) can stage its first part and fail
     * its second (audit 35). */
    int wrc = writes_settle(ctx, dev, word_bytes(len));
    if (rc == HL_GPU_OK)
        rc = wrc;
    pthread_mutex_unlock(&dev->mutex);
    return rc;
}

int hl_cap_gpu_buffer_read(HlGpuCtx *ctx, int device, const char *name,
                           size_t max_len, void **data, size_t *len)
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
    /* The backend copies the whole buffer off the VM heap before the
     * binding converts it: a copy the heap could never hold is refused
     * first (audit 17). */
    if (max_len && buf->size > max_len) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_BUFFER;
    }
    /* On a stalled device the read would wait out its whole timeout behind
     * the hung work, holding the mutex (and on the sync path the event
     * loop), and leave its staging buffer behind (audit 34). */
    if (stalled_refuse(ctx, dev)) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_BUSY;
    }

    int rc = ctx->backend->buffer_read(dev, buf, data, len);
    submit_settle(dev, word_bytes(buf->size), rc);
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
            live_release(dev, dev->buffers[i].size);
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
    /* A name must fit its slot: stored truncated but looked up in full,
     * it never matched, and each call took a new slot until the device's
     * table was full. */
    if (strlen(name) >= HL_GPU_NAME_MAX)
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
    size_t bytes = hl_gpu_tex_bytes(width, height, format);
    if (live_reserve(dev, bytes) != 0) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_BUFFER;
    }

    HlGpuTexture *slot = &dev->textures[dev->texture_count];
    int rc = ctx->backend->texture_create(dev, width, height,
                                           format, storage, filter,
                                           address_u, address_v, slot);
    if (rc == HL_GPU_OK) {
        snprintf(slot->name, sizeof(slot->name), "%s", name);
        slot->bytes = bytes;
        dev->texture_count++;
    } else {
        live_release(dev, bytes);
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
    if (stalled_refuse(ctx, dev)) {          /* as buffer_write (audit 34) */
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_BUSY;
    }

    int rc = ctx->backend->texture_write(dev, tex, data, len);
    int wrc = writes_settle(ctx, dev, tex_staging_bytes(tex));   /* audit 35 */
    if (rc == HL_GPU_OK)
        rc = wrc;
    pthread_mutex_unlock(&dev->mutex);
    return rc;
}

int hl_cap_gpu_texture_read(HlGpuCtx *ctx, int device, const char *name,
                             size_t max_len, void **data, size_t *len,
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
    if (max_len && hl_gpu_tex_bytes(tex->width, tex->height, tex->format) > max_len) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_BUFFER;   /* as buffer_read (audit 17) */
    }
    if (stalled_refuse(ctx, dev)) {          /* as buffer_read (audit 34) */
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_BUSY;
    }

    int rc = ctx->backend->texture_read(dev, tex, data, len);
    submit_settle(dev, tex_staging_bytes(tex), rc);
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
            live_release(dev, dev->textures[i].bytes);
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

    /* WebGPU copies whole 4-byte words between 4-aligned offsets. The size
     * used to be rounded up after the bounds check: the extra bytes landed
     * past the requested range, over the destination's data. Now a rounded
     * copy is allowed only when the extra bytes fall in padding: the copy
     * ends at the destination's end, and stays inside the source's
     * (4-aligned) allocation. */
    size_t rounded = (copy_size + 3) & ~(size_t)3;
    size_t src_alloc = (src->size + 3) & ~(size_t)3;
    if (src_offset % 4 != 0 || dst_offset % 4 != 0 ||
        rounded < copy_size || rounded > src_alloc - src_offset ||
        (rounded != copy_size && dst_offset + copy_size != dst->size)) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_BUFFER;
    }
    copy_size = rounded;

    if (!ctx->backend->buffer_copy) {
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_INTERNAL;
    }
    if (stalled_refuse(ctx, dev)) {          /* as buffer_read (audit 34) */
        pthread_mutex_unlock(&dev->mutex);
        return HL_GPU_ERR_BUSY;
    }

    int rc = ctx->backend->buffer_copy(dev,
                                        src, dst,
                                        src_offset, dst_offset, copy_size);
    submit_settle(dev, 0, rc);
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
