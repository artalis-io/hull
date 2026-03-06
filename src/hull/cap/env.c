/*
 * hull_cap_env.c — Shared environment variable capability
 *
 * Access is gated by an allowlist. Only env vars explicitly declared
 * by the application can be read.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/env.h"
#include "hull/cap/audit.h"
#include <stdlib.h>
#include <string.h>

const char *hl_cap_env_get(const HlEnvConfig *cfg, const char *name)
{
    if (!cfg || !name || !cfg->allowed) {
        ShJsonWriter w = hl_audit_begin("env.get");
        sh_json_write_kv_string(&w, "name", name);
        sh_json_write_kv_string(&w, "result", "denied");
        hl_audit_end(&w);
        return NULL;
    }

    /* Check if the requested variable is in the allowlist */
    for (int i = 0; i < cfg->count; i++) {
        if (cfg->allowed[i] && strcmp(cfg->allowed[i], name) == 0) {
            const char *val = getenv(name);
            ShJsonWriter w = hl_audit_begin("env.get");
            sh_json_write_kv_string(&w, "name", name);
            sh_json_write_kv_string(&w, "result", val ? "ok" : "missing");
            hl_audit_end(&w);
            return val;
        }
    }

    /* Not in allowlist — denied */
    ShJsonWriter w = hl_audit_begin("env.get");
    sh_json_write_kv_string(&w, "name", name);
    sh_json_write_kv_string(&w, "result", "denied");
    hl_audit_end(&w);
    return NULL;
}
