/*
 * cap/tui_feature.c - composable-feature presence hook for the TUI subsystem.
 *
 * Base-resident weak default: a base build carries no TUI cap layer, so this
 * returns 0 (not present). A `hull build --with=tui` build links a STRONG
 * override (in libhull_feature-tui.a) that returns 1; the resolver's
 * build_provided_caps then reports HL_MOD_CAP_TUI so a composed binary admits
 * hull/tui at app load even though HL_ENABLE_TUI was never compiled into the
 * base. A monolithic HL_ENABLE_TUI build reports TUI via the compile flag.
 *
 * TUI is a "subsystem feature", not a "backend feature": there is no backend
 * vtable (contrast HlGpuBackend / HlDbBackend), so the seam is a single
 * presence hook plus the per-runtime registration hooks the feature provides.
 * Always compiled (picked up by the cap-directory wildcard) so the symbol
 * always resolves whether or not TUI is present.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/tui.h"

__attribute__((weak))
int hl_tui_feature_present(void)
{
    return 0;
}

/* Weak no-op registration hooks: a base build registers no tui bridge. The
 * feature archive's strong overrides do the real per-runtime registration. */
__attribute__((weak))
void hl_tui_feature_register_lua(void *lua_state)
{
    (void)lua_state;
}

__attribute__((weak))
int hl_tui_feature_register_js(void *js_ctx, void *hl_js)
{
    (void)js_ctx;
    (void)hl_js;
    return 0;
}

/* ── Admission gate (audit 31) ─────────────────────────────────────── *
 *
 * hl_cap_tui_acquire refuses (EPERM) unless this flag is set. The runtime
 * sets it only once the app's manifest has been applied - module resolution
 * admitted hull/tui AND the manifest says `tui = true`
 * (hl_tui_admit_resolved: serve.c, app_context.c) - and the hull toolchain
 * sets it for its own `--tui` tool modules (tool.c). App top-level code runs
 * before either, and the manifest-extraction VMs (`hull build` / `manifest`
 * / `check` / ...) never set it: an untrusted repo's top level could
 * otherwise take the terminal or paste-jack the clipboard (OSC 52).
 *
 * Strong, base-resident (not in the tui feature archive), so the base sets
 * it whether or not the cap layer is linked. Read and written on the
 * thread that runs the app / tool VM only. */
static int g_tui_admitted;

int hl_tui_admit_set(int on)
{
    int prev = g_tui_admitted;
    g_tui_admitted = on ? 1 : 0;
    return prev;
}

int hl_tui_admitted(void)
{
    return g_tui_admitted;
}
