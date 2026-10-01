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
#include "hull/tls_transport.h"   /* HlClientTls, KlTlsConfig */
#ifdef HL_ENABLE_HTTP_CLIENT
#include "hull/cap/http.h"
#include "hull/cap/smtp.h"
#endif

#include <sh_seal_arena.h>

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ── Policy spans ─────────────────────────────────────────────────── */

static size_t page_size(void)
{
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (size_t)p : 4096u;
}

static size_t round_to_pages(size_t n)
{
    size_t pg = page_size();
    return (n + pg - 1) / pg * pg;
}

void *hl_policy_page_alloc(size_t size)
{
    if (size == 0 || size > SIZE_MAX / 2) return NULL;
    void *p = mmap(NULL, round_to_pages(size), PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;   /* anonymous maps are zeroed */
}

void hl_policy_page_free(void *p, size_t size)
{
    if (!p) return;
    munmap(p, round_to_pages(size));
}

static int span_protect(void *p, int prot)
{
    if (!p) return -1;
    size_t pg = page_size();
    if ((uintptr_t)p % pg != 0 || HL_POLICY_SPAN % pg != 0) return -1;
    return mprotect(p, HL_POLICY_SPAN, prot) == 0 ? 0 : -1;
}

int hl_policy_page_seal(void *p)   { return span_protect(p, PROT_READ); }
int hl_policy_page_unseal(void *p) { return span_protect(p, PROT_READ | PROT_WRITE); }

int hl_policy_seal_runtime(HlRuntime *rt)
{
    if (!rt || !rt->policy_page_owned) return -1;
    if (hl_policy_page_seal(rt) != 0) return -1;
    rt->policy_sealed = 1;
    return 0;
}

int hl_policy_unseal_runtime(HlRuntime *rt)
{
    if (!rt || !rt->policy_sealed) return 0;
    if (hl_policy_page_unseal(rt) != 0) return -1;
    rt->policy_sealed = 0;
    return 0;
}

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
    /* The outbound TLS config: its `factory` pointer is called for every
     * TLS connection, so it is a control-flow target, not only policy. One
     * sealed copy serves client_tls, http and smtp. */
    need += sizeof(HlClientTls) + sizeof(KlTlsConfig) + 32;
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

    /* The TLS config every client uses, copied once; each config that
     * pointed at the original points at the copy. */
    const KlTlsConfig *tls_src = rt->client_tls ? rt->client_tls->cfg : NULL;
#ifdef HL_ENABLE_HTTP_CLIENT
    if (!tls_src && rt->http_cfg) tls_src = rt->http_cfg->tls;
#endif
    KlTlsConfig *tls = NULL;
    if (tls_src) {
        tls = sh_seal_arena_memdup(arena, tls_src, sizeof *tls);
        if (!tls) goto fail;
    }
    HlClientTls *ctls = NULL;
    if (rt->client_tls) {
        ctls = sh_seal_arena_memdup(arena, rt->client_tls, sizeof *ctls);
        if (!ctls) goto fail;
        if (ctls->cfg == tls_src) ctls->cfg = tls;
    }
#ifdef HL_ENABLE_HTTP_CLIENT
    HlHttpConfig *http = NULL;
    HlSmtpConfig *smtp = NULL;
    if (rt->http_cfg) {
        http = sh_seal_arena_memdup(arena, rt->http_cfg, sizeof *http);
        if (!http) goto fail;
        if (http->tls && http->tls == tls_src) http->tls = tls;
    }
    if (rt->smtp_cfg) {
        smtp = sh_seal_arena_memdup(arena, rt->smtp_cfg, sizeof *smtp);
        if (!smtp) goto fail;
        if (smtp->tls && smtp->tls == (const void *)tls_src) smtp->tls = tls;
    }
#endif
    if (sh_seal_arena_seal(arena) != 0) goto fail;

    if (fs)   rt->fs_cfg     = fs;
    if (env)  rt->env_cfg    = env;
    if (ctls) rt->client_tls = ctls;
#ifdef HL_ENABLE_HTTP_CLIENT
    if (http) rt->http_cfg = http;
    if (smtp) rt->smtp_cfg = smtp;
#endif
    return 0;

fail:
    sh_seal_arena_destroy(arena);
    return -1;
}
