/*
 * embed.c — implementation of the libhull embedding ABI (embed.h).
 *
 * Thin wrapper over the internal sandbox + capability layer. The opaque
 * HlEmbed handle accumulates a C-built policy; hl_embed_seal() resolves
 * it into an HlSandboxPolicy, applies the kernel sandbox, and seals the
 * one datum the capability layer reads on every call — the filesystem
 * base directory (HlFsConfig.base_dir) — into a page-backed read-only
 * arena (sh_seal_arena). After a successful seal there is no writable
 * alias of that path left in the process, so an arbitrary-write primitive
 * cannot repoint the app's filesystem root. See docs/security.md §4b and
 * the c-audit §5b sealed-runtime-table rules.
 *
 * Fail-closed: the fs capability calls refuse to run until the handle is
 * SEALED, and hl_embed_seal marks SEALED only after BOTH the arena seal
 * and the kernel sandbox have succeeded.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/embed.h"
#include "hull/embed_internal.h"

#include "hull/sandbox.h"
#include "hull/manifest.h"
#include "hull/cap/fs.h"
#include "hull/cap/crypto.h"
#include "hull/module_registry.h"
#include "hull/release_io.h"

#include <sh_seal_arena.h>

#include <stdlib.h>
#include <string.h>

enum { HL_EMBED_NEW = 0, HL_EMBED_SEALED = 1 };

/* One page is ample for a single base_dir path plus alignment slack. */
#define HL_EMBED_ARENA_SIZE 8192

struct HlEmbed {
    char       *app_dir;                          /* owned pre-seal; NULLed after seal */
    HlFsConfig  fs;                               /* base_dir: app_dir pre-seal, sealed copy post-seal */

    char       *reads[HL_MANIFEST_MAX_PATHS];     /* owned dups; freed at seal or free */
    int         nreads;
    char       *writes[HL_MANIFEST_MAX_PATHS];    /* owned dups */
    int         nwrites;

    int         net_inbound;
    int         net_outbound;
    int         gpu;
    int         tui;

    ShSealArena arena;                            /* holds the sealed base_dir */
    int         arena_ready;                      /* 1 once arena is initialised */
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

static void embed_free_heap_policy(HlEmbed *e)
{
    for (int i = 0; i < e->nreads; i++)  { free(e->reads[i]);  e->reads[i]  = NULL; }
    for (int i = 0; i < e->nwrites; i++) { free(e->writes[i]); e->writes[i] = NULL; }
    e->nreads = 0;
    e->nwrites = 0;
    free(e->app_dir);
    e->app_dir = NULL;
}

void hl_embed_free(HlEmbed *e)
{
    if (!e) return;
    /* Free heap-owned policy strings first, then destroy the sealed arena
     * LAST — fs.base_dir may alias into it (c-audit §5b: arena destroyed
     * after every consumer). No capability call can run during teardown. */
    embed_free_heap_policy(e);
    if (e->arena_ready) sh_seal_arena_destroy(&e->arena);
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

/*
 * Seal the per-call base_dir into the read-only arena. On success sets
 * e->fs.base_dir to the in-arena (RO) copy and returns 0. On any failure
 * the arena is torn down and e->fs.base_dir is left pointing at the
 * original heap copy (harmless — the caller aborts the seal). Must run
 * BEFORE hl_sandbox_apply so a seal failure aborts before the (on macOS
 * irreversible) kernel sandbox is applied.
 */
static int embed_seal_base_dir(HlEmbed *e)
{
    if (sh_seal_arena_init(&e->arena, HL_EMBED_ARENA_SIZE, "hl_embed") != 0)
        return -1;
    char *sealed = sh_seal_arena_strdup(&e->arena, e->app_dir);
    if (!sealed) {
        sh_seal_arena_destroy(&e->arena);
        return -1;
    }
    if (sh_seal_arena_seal(&e->arena) != 0) {  /* fail closed on seal error */
        sh_seal_arena_destroy(&e->arena);
        return -1;
    }
    e->arena_ready = 1;
    e->fs.base_dir = sealed;         /* now points into the RO mapping */
    e->fs.base_len = strlen(sealed);
    return 0;
}

int hl_embed_seal(HlEmbed *e, const char *db_path)
{
    if (!e || e->state == HL_EMBED_SEALED) return -1;

    /* 1. Seal the per-call base_dir first (nothing irreversible yet). */
    if (embed_seal_base_dir(e) != 0) return -1;

    /* 2. Build the resolved policy. Path arrays are read once by
     *    hl_sandbox_apply and never again, so they may stay on the heap;
     *    base_dir (read every cap call) is already the sealed copy. */
    HlSandboxPolicy policy;
    memset(&policy, 0, sizeof(policy));
    policy.fs_read          = (const char *const *)e->reads;
    policy.fs_read_count    = e->nreads;
    policy.fs_write         = (const char *const *)e->writes;
    policy.fs_write_count   = e->nwrites;
    policy.network_inbound  = e->net_inbound;
    policy.network_outbound = e->net_outbound;
    policy.gpu              = e->gpu;
    policy.tui              = e->tui;
    policy.wx_enforced      = 1;   /* no runtime dynamic code */
    /* allow_dynamic_code / allow_dynamic_libraries stay 0 (zeroed above). */

    /* 3. Apply the kernel sandbox against the sealed base_dir. */
    int rc = hl_sandbox_apply(&policy, e->fs.base_dir, db_path,
                              NULL, NULL, NULL);
    if (rc != 0) return -1;        /* fail closed: leave state NEW */

    /* 4. Success. Drop every writable alias of the policy — the sealed
     *    base_dir is the only path the cap layer will read from here on. */
    embed_free_heap_policy(e);

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

/* ── Internal test accessors (embed_internal.h — NOT the stable ABI) ── */

const char *hl_embed_base_dir(const HlEmbed *e)
{
    return e ? e->fs.base_dir : NULL;
}

int hl_embed_is_sealed(const HlEmbed *e)
{
    return (e && e->state == HL_EMBED_SEALED) ? 1 : 0;
}
