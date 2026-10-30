/*
 * hull_cap_env.c - Shared environment variable capability
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

/* The requested name, at most HL_ENV_AUDIT_NAME_MAX bytes of it: the record
 * is built in a 4 KiB buffer that drops an oversized one anyway, and the
 * writer escaped every byte of an app-supplied name of any length first
 * (audit 39). */
#define HL_ENV_AUDIT_NAME_MAX 256
static void audit_name(ShJsonWriter *w, const char *name)
{
    if (w->error) return;   /* audit off: nothing to write */
    sh_json_write_key(w, "name");
    if (!name) { sh_json_write_null(w); return; }
    size_t n = 0;
    while (n < HL_ENV_AUDIT_NAME_MAX && name[n]) n++;
    sh_json_write_string_n(w, name, n);
}

const char *hl_cap_env_get(const HlEnvConfig *cfg, const char *name)
{
    if (!cfg || !name || !cfg->allowed) {
        ShJsonWriter w = hl_audit_begin("env.get");
        audit_name(&w, name);
        sh_json_write_kv_string(&w, "result", "denied");
        hl_audit_end(&w);
        return NULL;
    }

    /* Check if the requested variable is in the allowlist */
    for (int i = 0; i < cfg->count; i++) {
        if (cfg->allowed[i] && strcmp(cfg->allowed[i], name) == 0) {
            const char *val = getenv(name);
            ShJsonWriter w = hl_audit_begin("env.get");
            audit_name(&w, name);
            sh_json_write_kv_string(&w, "result", val ? "ok" : "missing");
            hl_audit_end(&w);
            return val;
        }
    }

    /* Not in allowlist - denied */
    ShJsonWriter w = hl_audit_begin("env.get");
    audit_name(&w, name);
    sh_json_write_kv_string(&w, "result", "denied");
    hl_audit_end(&w);
    return NULL;
}

int hl_cap_env_allowed(const HlEnvConfig *cfg, const char *name)
{
    if (!cfg || !name || !cfg->allowed) {
        return 0;
    }
    for (int i = 0; i < cfg->count; i++) {
        if (cfg->allowed[i] && strcmp(cfg->allowed[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}
