/*
 * Pre-fix stand-ins for hl_gpu_manifest_admit and
 * hl_gpu_manifest_dynamic_shaders (src/hull/module_resolver.c), linked only
 * by scripts/check_regression_test.sh (docs/design/audit_convergence.md
 * 5.1). Before the fix serve.c and the app context admitted gpu from the
 * module set alone when the manifest had a modules list (an invalid
 * gpu.devices list did not refuse it), and raised the dynamic_shaders
 * opt-in whether or not gpu was admitted.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/manifest.h"
#include "hull/module_resolver.h"

bool hl_gpu_manifest_admit(const HlManifest *m, const HlResolvedModuleSet *s)
{
    if (!m)
        return true;
    if (m->modules_declared)
        return s && hl_module_set_contains_name(s, "hull/gpu");
    return !m->present || m->gpu;
}

bool hl_gpu_manifest_dynamic_shaders(const HlManifest *m,
                                     const HlResolvedModuleSet *s)
{
    (void)s;
    return m && m->present && m->gpu_dynamic_shaders;
}
