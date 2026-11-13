/*
 * gpu_setup.c - the GPU context every entry point that runs app code opens
 * the same way.
 *
 * serve.c, the app.main runner (serve_cli.c, through the app context) and
 * `hull test` / `hull agent test` (also through the app context) each need
 * a context set up in one order: the backend (a composed --with=gpu
 * feature, else the compiled-in wgpu one), the device, and the init-time
 * shader policy - all before the context reaches a runtime and the app's
 * top level runs (audit convergence §7.3). Once the manifest is applied,
 * its device allowlist and gpu.dynamic_shaders opt-in follow. Only serve.c
 * opened one before, so `hull test` and a built app.main tool had no
 * hull.gpu module at all.
 *
 * The shader compile helper (cap/gpu_helper.c) stays the caller's: only a
 * built server binary can start it before its phase-1 sandbox.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/gpu.h"
#include "hull/cap/policy_seal.h"
#include "hull/manifest.h"
#include "hull/vfs.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

/* The backend this binary has: the composed --with=gpu feature, else the
 * compiled-in wgpu one, else none. */
static const HlGpuBackend *gpu_backend(void)
{
    size_t nfeat = 0;
    const HlGpuBackend *const *feats = hl_gpu_feature_backends(&nfeat);
    if (nfeat > 0 && feats && feats[0])
        return feats[0];
#ifdef HL_ENABLE_GPU
    return &hl_gpu_backend_wgpu;
#else
    return NULL;
#endif
}

int hl_gpu_backend_present(void)
{
    return gpu_backend() != NULL;
}

HlGpuCtx *hl_gpu_ctx_open(int device, const struct HlVfs *shader_vfs)
{
    const HlGpuBackend *be = gpu_backend();
    if (!be)
        return NULL;   /* plain base: gpu.* stays unexposed */
#ifdef HL_GPU_HELPER_TEST_HOOKS
    /* Test builds only: act as a host whose backend finds no device, so the
     * no-device path (hull.gpu loads, gpu.available() is false) is covered
     * on a machine that has one (tests/e2e_gpu_hull_test.sh). */
    {
        const char *nd = getenv("HULL_TEST_GPU_NO_DEVICE");
        if (nd && nd[0] == '1') {
            log_info("[hull:c] GPU compute unavailable - gpu.* disabled");
            return NULL;
        }
    }
#endif

    HlGpuCtx *ctx = hl_policy_page_alloc(sizeof *ctx);
    if (!ctx)
        return NULL;
    /* A failed init has already released what it made; an init that found
     * no device holds the backend instance, which destroy frees. */
    int rc = hl_cap_gpu_init(ctx, be);
    if (rc != HL_GPU_OK || !hl_cap_gpu_available(ctx)) {
        log_info("[hull:c] GPU compute unavailable - gpu.* disabled");
        if (rc == HL_GPU_OK)
            hl_cap_gpu_destroy(ctx);
        hl_policy_page_free(ctx, sizeof *ctx);
        return NULL;
    }
    if (device >= 0 && device < ctx->device_count)
        ctx->default_device = device;
    hl_cap_gpu_init_shader_policy(ctx, shader_vfs);
    return ctx;
}

void hl_gpu_ctx_apply_manifest(HlGpuCtx *ctx, const HlManifest *m,
                               int admitted, const struct HlVfs *shader_vfs)
{
    if (!ctx)
        return;
    int present = m && m->present;
    /* A gpu.devices list that names no valid device restricts to NONE
     * (audit 46): the caller also withholds the context from the runtime
     * (hl_gpu_manifest_admit), and this keeps the context itself closed
     * should it reach one. A valid list restricts to it. */
    if (present && (m->gpu_devices_invalid ||
                    (m->gpu && m->gpu_device_count > 0))) {
        int n = m->gpu_devices_invalid ? 0 : m->gpu_device_count;
        ctx->device_restriction = 1;
        memset(ctx->allowed_devices, 0, sizeof(ctx->allowed_devices));
        for (int i = 0; i < n; i++) {
            int d = m->gpu_devices[i];
            if (d >= 0 && d < ctx->device_count)
                ctx->allowed_devices[d] = 1;
        }
        log_info("[hull:c] gpu device restriction: %d of %d devices allowed",
                 n, ctx->device_count);
    }
    /* The mode was set at open, before the top level ran; this raises the
     * opt-in, which only the applied manifest of an app admitted gpu.* can
     * grant - @p admitted is hl_gpu_manifest_admit's answer, so the policy
     * and the compile helper's stop agree (audit 46). */
    hl_cap_gpu_set_shader_policy(ctx,
        hl_vfs_embedded(shader_vfs) ? HL_GPU_SHADERS_EMBEDDED
                                    : HL_GPU_SHADERS_DEV,
        admitted && present && m->gpu_dynamic_shaders &&
            !m->gpu_devices_invalid,
        shader_vfs);
}

void hl_gpu_ctx_close(HlGpuCtx *ctx)
{
    if (!ctx)
        return;
    hl_policy_page_unseal(ctx);   /* destroy writes the span */
    hl_cap_gpu_destroy(ctx);
    hl_policy_page_free(ctx, sizeof *ctx);
}
