/*
 * embed.c — implementation of the libhull embedding ABI (embed.h).
 *
 * Thin, allocation-light wrapper over the internal sandbox + capability
 * layer. The opaque HlEmbed handle accumulates a C-built policy, then
 * hl_embed_seal() resolves it into an HlSandboxPolicy and applies the
 * kernel sandbox. The filesystem capability calls fail closed until that
 * seal has succeeded.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/embed.h"

#include "hull/sandbox.h"
#include "hull/manifest.h"
#include "hull/cap/fs.h"
#include "hull/cap/crypto.h"
#include "hull/module_registry.h"
#include "hull/release_io.h"

#include <stdlib.h>
#include <string.h>

enum { HL_EMBED_NEW = 0, HL_EMBED_SEALED = 1 };

struct HlEmbed {
    char       *app_dir;                          /* absolute, owned */
    HlFsConfig  fs;                               /* base_dir aliases app_dir */

    char       *reads[HL_MANIFEST_MAX_PATHS];     /* owned dups */
    int         nreads;
    char       *writes[HL_MANIFEST_MAX_PATHS];    /* owned dups */
    int         nwrites;

    int         net_inbound;
    int         net_outbound;
    int         gpu;
    int         tui;

    int         state;                            /* HL_EMBED_NEW / _SEALED */
};

int hl_embed_abi_version(void)
{
    return HL_EMBED_ABI_VERSION;
}

/* ── Construction ──────────────────────────────────────────────────── */

HlEmbed *hl_embed_new(const char *app_dir)
{
    if (!app_dir || app_dir[0] != '/') return NULL;

    HlEmbed *e = calloc(1, sizeof(*e));
    if (!e) return NULL;

    e->app_dir = strdup(app_dir);
    if (!e->app_dir) {
        free(e);
        return NULL;
    }
    e->fs.base_dir = e->app_dir;
    e->fs.base_len = strlen(e->app_dir);
    e->state = HL_EMBED_NEW;
    return e;
}

void hl_embed_free(HlEmbed *e)
{
    if (!e) return;
    for (int i = 0; i < e->nreads; i++) free(e->reads[i]);
    for (int i = 0; i < e->nwrites; i++) free(e->writes[i]);
    free(e->app_dir);
    free(e);
}

/* ── Policy ────────────────────────────────────────────────────────── */

static int embed_add_path(char **list, int *count, const char *rel_path)
{
    if (*count >= HL_MANIFEST_MAX_PATHS) return -1;
    char *dup = strdup(rel_path);
    if (!dup) return -1;
    list[*count] = dup;
    (*count)++;
    return 0;
}

int hl_embed_allow_read(HlEmbed *e, const char *rel_path)
{
    if (!e || !rel_path || e->state != HL_EMBED_NEW) return -1;
    return embed_add_path(e->reads, &e->nreads, rel_path);
}

int hl_embed_allow_write(HlEmbed *e, const char *rel_path)
{
    if (!e || !rel_path || e->state != HL_EMBED_NEW) return -1;
    return embed_add_path(e->writes, &e->nwrites, rel_path);
}

void hl_embed_allow_network(HlEmbed *e, int inbound, int outbound)
{
    if (!e || e->state != HL_EMBED_NEW) return;
    e->net_inbound  = inbound ? 1 : 0;
    e->net_outbound = outbound ? 1 : 0;
}

void hl_embed_allow_gpu(HlEmbed *e, int enabled)
{
    if (!e || e->state != HL_EMBED_NEW) return;
    e->gpu = enabled ? 1 : 0;
}

void hl_embed_allow_tui(HlEmbed *e, int enabled)
{
    if (!e || e->state != HL_EMBED_NEW) return;
    e->tui = enabled ? 1 : 0;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

int hl_embed_sandbox_phase1(HlEmbed *e)
{
    if (!e) return -1;
    return hl_sandbox_apply_pledge();
}

int hl_embed_seal(HlEmbed *e, const char *db_path)
{
    if (!e || e->state == HL_EMBED_SEALED) return -1;

    HlSandboxPolicy policy;
    memset(&policy, 0, sizeof(policy));
    policy.fs_read         = (const char *const *)e->reads;
    policy.fs_read_count   = e->nreads;
    policy.fs_write        = (const char *const *)e->writes;
    policy.fs_write_count  = e->nwrites;
    policy.network_inbound  = e->net_inbound;
    policy.network_outbound = e->net_outbound;
    policy.gpu              = e->gpu;
    policy.tui              = e->tui;
    policy.wx_enforced      = 1;   /* no runtime dynamic code */
    /* allow_dynamic_code / allow_dynamic_libraries stay 0 (zeroed above). */

    int rc = hl_sandbox_apply(&policy, e->app_dir, db_path, NULL, NULL, NULL);
    if (rc != 0) return -1;        /* fail closed: leave state NEW */

    e->state = HL_EMBED_SEALED;
    return 0;
}

/* ── Capabilities ──────────────────────────────────────────────────── */

static int embed_sealed(const HlEmbed *e, const char **err)
{
    if (e && e->state == HL_EMBED_SEALED) return 1;
    if (err) *err = "hl_embed: capabilities unavailable before hl_embed_seal()";
    return 0;
}

int64_t hl_embed_fs_read(HlEmbed *e, const char *path,
                         char *buf, size_t buf_size, const char **err)
{
    if (!embed_sealed(e, err)) return -1;
    return hl_cap_fs_read(&e->fs, path, buf, buf_size, err);
}

int hl_embed_fs_write(HlEmbed *e, const char *path,
                      const void *data, size_t len, const char **err)
{
    if (!embed_sealed(e, err)) return -1;
    return hl_cap_fs_write(&e->fs, path, (const char *)data, len, err);
}

int hl_embed_fs_exists(HlEmbed *e, const char *path, const char **err)
{
    if (!embed_sealed(e, err)) return -1;
    return hl_cap_fs_exists(&e->fs, path, err);
}

int hl_embed_fs_delete(HlEmbed *e, const char *path, const char **err)
{
    if (!embed_sealed(e, err)) return -1;
    return hl_cap_fs_delete(&e->fs, path, err);
}

int hl_embed_sha256(const void *data, size_t len, uint8_t out[32])
{
    return hl_cap_crypto_sha256(data, len, out);
}

/* ── Identity ──────────────────────────────────────────────────────── */

const char *hl_embed_platform(void)
{
    return hl_release_io_platform();
}

size_t hl_embed_module_count(void)
{
    return hl_module_registry_count();
}
