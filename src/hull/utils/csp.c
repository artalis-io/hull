/*
 * csp.c - Content-Security-Policy preset registry.
 *
 * The default policy (HL_DEFAULT_CSP, exposed for callers in csp.h)
 * is the deny-by-default baseline applied to every HTML response
 * when the manifest doesn't set `csp`. Apps that opt into a richer
 * interaction model declare a named preset via
 * `app.manifest({ csp = "<name>" })`; hl_csp_resolve() expands the
 * name to a concrete header value once at startup.
 *
 * Unknown preset names pass through as literal CSP strings - this
 * is deliberate, so an app that writes `csp = "default-src 'self';
 * ..."` keeps working. Typo-as-preset-name silently becoming
 * `default-src 'none'` would be a bad failure mode.
 *
 * Adding a preset: drop a row into HL_CSP_PRESETS[] below. Keep the
 * list short and curated; presets must describe what they enable
 * (e.g. "htmx" = "this app is server-rendered htmx with stdlib JS
 * from /static/"), not who's using the app.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/utils/csp.h"

#include <string.h>

/* "htmx" preset - htmx-driven SSR apps with stdlib JS / CSS served
 * from /static/. Allows:
 *   - same-origin scripts (htmx itself + any widget JS shipped
 *     from stdlib/static/hull/...);
 *   - inline styles (Pico classless concession);
 *   - same-origin XHR (htmx's bread and butter - without
 *     connect-src 'self' every htmx request is blocked by
 *     default-src 'none');
 *   - `data:` images (inline SVG favicons + Pico/Lucide-style
 *     inline icons are a de-facto standard for hypermedia apps;
 *     `data:` for images is XSS-safe - browsers don't interpret
 *     SVG inside <img> as scriptable).
 *
 * Apps that want stricter nonce-based CSP should use the
 * hull/web/middleware/csp@1 module with the htmx profile instead
 * of this static preset. */
#define HL_CSP_HTMX \
    "default-src 'none'; script-src 'self'; " \
    "style-src 'self' 'unsafe-inline'; img-src 'self' data:; " \
    "connect-src 'self'; form-action 'self'; frame-ancestors 'none'"

typedef struct {
    const char *name;
    const char *policy;
} HlCspPreset;

static const HlCspPreset HL_CSP_PRESETS[] = {
    { "htmx", HL_CSP_HTMX },
    { NULL, NULL },
};

const char *hl_csp_resolve(const char *value)
{
    if (!value)
        return NULL;
    for (const HlCspPreset *p = HL_CSP_PRESETS; p->name; p++) {
        if (strcmp(value, p->name) == 0)
            return p->policy;
    }
    return value;
}

const char *hl_csp_preset_name_for(const char *policy)
{
    if (!policy)
        return NULL;
    for (const HlCspPreset *p = HL_CSP_PRESETS; p->name; p++) {
        if (strcmp(policy, p->policy) == 0)
            return p->name;
    }
    return NULL;
}
