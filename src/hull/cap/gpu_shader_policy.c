/*
 * gpu_shader_policy.c - which WGSL sources hl_cap_gpu_compile accepts
 * (audit convergence §7.3).
 *
 * The shader compiler (naga + the platform's driver compiler) is a black
 * box. The WGSL pre-checks in gpu.c estimate its stack depth and time from
 * the source text, and every audit round found a shape they missed. So a
 * deployed app compiles only the shaders it was built with:
 *
 *   - A built binary (HL_GPU_SHADERS_EMBEDDED): a source must be
 *     byte-identical to an embedded shaders/<name>.wgsl entry - what
 *     `hull build` embedded and pre-checked - unless the manifest sets
 *     gpu.dynamic_shaders (JS gpu.dynamicShaders). gpu.load(name) passes by
 *     construction; gpu.compile(name, src) passes when src is one of them.
 *   - Development (HL_GPU_SHADERS_DEV, no embedded app files): any source
 *     compiles, so a shader can be edited inline; a gpu.compile of source
 *     that did not come from shaders/ logs one WARN without the opt-in,
 *     since the built binary will refuse it.
 *   - No policy (HL_GPU_SHADERS_ANY): unit tests and embedders.
 *
 * With the opt-in, dynamic source goes through the in-process compile and
 * its deadline / abandoned-compile machinery (audit 40). A helper process
 * with a hard kill at the deadline is a tracked follow-up.
 *
 * The policy lives in the context's sealed policy span (serve.c seals it
 * after hl_cap_gpu_set_shader_policy), so app code cannot widen it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/gpu.h"
#include "hull/vfs.h"
#include "log.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hl_cap_gpu_set_shader_policy(HlGpuCtx *ctx, int mode, int dynamic_shaders,
                                  const struct HlVfs *vfs)
{
    if (!ctx) return;
    ctx->shader_mode = mode;
    ctx->dynamic_shaders = dynamic_shaders ? 1 : 0;
    ctx->shader_vfs = vfs;
}

/* An entry is a shader when it is shaders/<something>.wgsl. */
static int is_shader_entry(const HlEntry *e)
{
    size_t n = strlen(e->name);
    return n > 8 + 5 && strncmp(e->name, "shaders/", 8) == 0 &&
           strcmp(e->name + n - 5, ".wgsl") == 0;
}

int hl_gpu_shader_is_embedded(const struct HlVfs *vfs, const char *wgsl,
                              size_t len)
{
    if (!vfs || !wgsl) return 0;
    const HlEntry *first = NULL;
    size_t n = hl_vfs_prefix(vfs, "shaders/", &first);
    for (size_t i = 0; i < n; i++) {
        const HlEntry *e = &first[i];
        if (!e->data || (size_t)e->len != len || !is_shader_entry(e))
            continue;
        /* Only same-length entries are compared, so the work is bounded
         * by the source length times the shaders of that length. */
        if ((const char *)e->data == wgsl ||
            memcmp(e->data, wgsl, len) == 0)
            return 1;
    }
    return 0;
}

static atomic_int g_dev_warned;

int hl_gpu_shader_admit(const HlGpuCtx *ctx, const char *name,
                        const char *wgsl, size_t len, int flags)
{
    if (!ctx) return HL_GPU_OK;
    switch (ctx->shader_mode) {
    case HL_GPU_SHADERS_EMBEDDED:
        if (ctx->dynamic_shaders ||
            hl_gpu_shader_is_embedded(ctx->shader_vfs, wgsl, len))
            return HL_GPU_OK;
        log_warn("[hull:gpu] shader '%s' refused: its source is not a "
                 "shader embedded at build time (shaders/<name>.wgsl). "
                 "Move it to shaders/ and use gpu.load, or set "
                 "gpu.dynamic_shaders = true (JS gpu.dynamicShaders) in the "
                 "manifest to compile runtime source",
                 name ? name : "?");
        return HL_GPU_ERR_POLICY;
    case HL_GPU_SHADERS_DEV:
        if (!ctx->dynamic_shaders && !(flags & HL_GPU_COMPILE_FROM_SHADERS) &&
            atomic_exchange(&g_dev_warned, 1) == 0)
            log_warn("[hull:gpu] gpu.compile('%s') compiles inline source; "
                     "a built binary refuses source that is not byte-identical "
                     "to a shaders/<name>.wgsl file unless the manifest sets "
                     "gpu.dynamic_shaders = true (JS gpu.dynamicShaders)",
                     name ? name : "?");
        return HL_GPU_OK;
    default:
        return HL_GPU_OK;
    }
}

/* ── Build-time check (hull build) ─────────────────────────────────── */

int hl_gpu_shaders_build_check(const HlGpuBackend *backend,
                               const char *const *srcs, const size_t *lens,
                               size_t n, size_t *bad, const char **why,
                               int *compiled)
{
    if (compiled) *compiled = 0;
    for (size_t i = 0; i < n; i++) {
        const char *w = NULL;
        if (lens[i] > HL_GPU_WGSL_MAX_BYTES) {
            w = "larger than the WGSL size cap";
        } else if (hl_gpu_wgsl_precheck(srcs[i], lens[i], &w) != 0) {
            if (!w) w = "refused by the WGSL pre-check";
        }
        if (w) {
            if (bad) *bad = i;
            if (why) *why = w;
            return -1;
        }
    }
    if (!backend || n == 0)
        return 0;

    /* A test compile on a real device, when the toolchain has a backend and
     * the host an adapter: the same compile (thread, deadline) the runtime
     * runs. No adapter is not an error - the pre-check above still ran. */
    HlGpuCtx *ctx = calloc(1, sizeof *ctx);
    if (!ctx) return 0;
    int rc = 0;
    if (hl_cap_gpu_init(ctx, backend) == HL_GPU_OK &&
        hl_cap_gpu_available(ctx)) {
        size_t limit = n < HL_GPU_PIPELINE_MAX ? n : HL_GPU_PIPELINE_MAX;
        for (size_t i = 0; i < limit; i++) {
            char name[32];
            snprintf(name, sizeof name, "build_check_%zu", i);
            int crc = hl_cap_gpu_compile(ctx, -1, name, srcs[i], lens[i]);
            if (crc != HL_GPU_OK) {
                if (bad) *bad = i;
                if (why) *why = crc == HL_GPU_ERR_TIMEOUT
                    ? "did not compile within the compile deadline"
                    : "does not compile";
                rc = -1;
                break;
            }
        }
        if (compiled && rc == 0) *compiled = 1;
    }
    hl_cap_gpu_destroy(ctx);
    free(ctx);
    return rc;
}
