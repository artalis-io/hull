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
