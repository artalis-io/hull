/*
 * http_feature.c — weak no-op defaults for the HTTP-feature seam (issue #114).
 *
 * The runtime-agnostic base always links these. A full base (Phase A) or a
 * composed `http` feature (Phase C) supplies strong overrides from
 * runtime/{lua,js}/http_register.c that register the http/ws/sse/smtp modules;
 * a reduced base with no HTTP keeps these no-ops, so the web bindings are never
 * pulled. Mirrors cap/tui_feature.c / cap/gpu_feature.c.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/http_feature.h"

__attribute__((weak)) void hl_lua_register_http_modules(void *lua_state)
{
    (void)lua_state;
}

__attribute__((weak)) void hl_js_register_http_modules(void *js_ctx)
{
    (void)js_ctx;
}

__attribute__((weak)) int hl_http_feature_present(void)
{
    return 0;
}
