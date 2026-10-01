/*
 * policy_seal.c - seal the capability configs a runtime reads per call
 *
 * See policy_seal.h. Shared by both entry points (serve.c, serve_cli.c), so
 * there is one copy of what "the sealed cap configs" means.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/policy_seal.h"
#include "hull/runtime.h"
#include "hull/cap/fs.h"
#include "hull/cap/fs_policy.h"
#include "hull/cap/env.h"
#ifdef HL_ENABLE_HTTP_CLIENT
#include "hull/cap/http.h"
#include "hull/cap/smtp.h"
#endif

#include <sh_seal_arena.h>

#include <string.h>

int hl_policy_seal_cap_configs(HlRuntime *rt, ShSealArena *arena)
{
    if (!rt || !arena) return -1;

    size_t need = 256;
    if (rt->fs_cfg) {
        need += sizeof(HlFsConfig) + 16;
        if (rt->fs_cfg->base_dir) need += strlen(rt->fs_cfg->base_dir) + 16;
        need += hl_fs_policy_sealed_size(rt->fs_cfg->policy);
    }
    if (rt->env_cfg) need += sizeof(HlEnvConfig) + 16;
#ifdef HL_ENABLE_HTTP_CLIENT
    if (rt->http_cfg) need += sizeof(HlHttpConfig) + 16;
    if (rt->smtp_cfg) need += sizeof(HlSmtpConfig) + 16;
#endif
    if (sh_seal_arena_init(arena, need, "cap-config") != 0)
        return -1;

    HlFsConfig  *fs  = NULL;
    HlEnvConfig *env = NULL;
    if (rt->fs_cfg) {
        fs = sh_seal_arena_memdup(arena, rt->fs_cfg, sizeof *fs);
        if (!fs) goto fail;
        if (rt->fs_cfg->base_dir) {
            fs->base_dir = sh_seal_arena_strdup(arena, rt->fs_cfg->base_dir);
            if (!fs->base_dir) goto fail;
        }
        if (rt->fs_cfg->policy) {
            fs->policy = hl_fs_policy_copy_sealed(rt->fs_cfg->policy, arena);
            if (!fs->policy) goto fail;
        }
    }
    if (rt->env_cfg) {
        env = sh_seal_arena_memdup(arena, rt->env_cfg, sizeof *env);
        if (!env) goto fail;
    }
#ifdef HL_ENABLE_HTTP_CLIENT
    HlHttpConfig *http = NULL;
    HlSmtpConfig *smtp = NULL;
    if (rt->http_cfg) {
        http = sh_seal_arena_memdup(arena, rt->http_cfg, sizeof *http);
        if (!http) goto fail;
    }
    if (rt->smtp_cfg) {
        smtp = sh_seal_arena_memdup(arena, rt->smtp_cfg, sizeof *smtp);
        if (!smtp) goto fail;
    }
#endif
    if (sh_seal_arena_seal(arena) != 0) goto fail;

    if (fs)  rt->fs_cfg  = fs;
    if (env) rt->env_cfg = env;
#ifdef HL_ENABLE_HTTP_CLIENT
    if (http) rt->http_cfg = http;
    if (smtp) rt->smtp_cfg = smtp;
#endif
    return 0;

fail:
    sh_seal_arena_destroy(arena);
    return -1;
}
