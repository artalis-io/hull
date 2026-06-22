/* csp.h. Content-Security-Policy preset registry.
 *
 * The presets are defined and resolved in src/hull/serve.c. This
 * header exposes the lookup helpers needed by callers outside
 * serve.c (inspect, agent, doctor) so they can show preset names
 * in human / machine output rather than raw policy strings.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HULL_CSP_H
#define HULL_CSP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve a manifest csp value: if it matches a preset name, return
 * the expanded policy; otherwise return the input unchanged (literal
 * policy). NULL in -> NULL out (csp=false / disabled). */
const char *hl_csp_resolve(const char *value);

/* Return the canonical preset name when `policy` exactly matches one
 * of the named presets (e.g. the expanded "htmx" policy returns
 * "htmx"). Returns NULL when `policy` is a custom literal or NULL. */
const char *hl_csp_preset_name_for(const char *policy);

#ifdef __cplusplus
}
#endif

#endif /* HULL_CSP_H */
