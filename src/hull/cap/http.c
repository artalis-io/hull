/*
 * http.c - HTTP client capability (thin wrapper over Keel)
 *
 * Adds host allowlist checking and audit logging around
 * kl_http_client_request() from Keel's HTTP client module.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/http.h"
#include "hull/cap/audit.h"
#include "hull/host_match.h"

#include <keel/allocator.h>
#include <keel/clock.h>
#include <keel/http_client_pool.h>
#include <keel/error.h>
#include <keel/http_redirect.h>
#include <keel/url.h>

#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "log.h"

/* ── Host allowlist check ────────────────────────────────────────── */

int hl_http_check_host(const HlHttpConfig *cfg,
                        const char *host, size_t host_len)
{
    if (!cfg || !host)
        return -1;
    /* manifest.hosts patterns: exact (case-insensitive) + "*" + "*.suffix"
     * glob + CIDR (IP-literal hosts only) + "$VAR" env refs. Shared with
     * ws.connect (same cfg) and smtp.send (§2.8). */
    return hl_host_match_any_env_n(cfg->allowed_hosts, cfg->count, host, host_len)
           ? 0 : -1;
}

/* ── Audit-log URL redaction ─────────────────────────────────────── */

/* Append @p n bytes of @p p at @p o, truncating to leave room for the NUL. */
static size_t redact_put(char *out, size_t outsz, size_t o,
                         const char *p, size_t n)
{
    if (n > outsz - 1 - o) n = outsz - 1 - o;
    memcpy(out + o, p, n);
    return o + n;
}

/* The audit log wrote every URL verbatim, so "https://user:pw@host/x?token=y"
 * put the password and the token in the log (audit 17). */
size_t hl_http_url_redact(const char *url, char *out, size_t outsz)
{
    if (!out || outsz == 0) return 0;
    out[0] = '\0';
    if (!url) return 0;

    const char *sep = strstr(url, "://");
    size_t scheme_len = sep ? (size_t)(sep - url) + 3 : 0;
    const char *rest = url + scheme_len;
    size_t end = strcspn(rest, "?#");           /* query and fragment go */

    const char *at = NULL;
    for (size_t i = 0; i < end; i++)
        if (rest[i] == '@') at = rest + i;      /* the last one ends userinfo */
    size_t slash = strcspn(rest, "/");
    const char *keep = rest;
    size_t keep_len = end;
    int ambiguous = 0;
    if (at) {
        if ((size_t)(at - rest) < slash) {
            keep = at + 1;
            keep_len = end - (size_t)(keep - rest);
        } else {
            ambiguous = 1;   /* a password holding '/', or '@' in the path */
        }
    }

    size_t o = redact_put(out, outsz, 0, url, scheme_len);
    if (ambiguous)
        o = redact_put(out, outsz, o, "[redacted]", sizeof("[redacted]") - 1);
    else
        o = redact_put(out, outsz, o, keep, keep_len);
    out[o] = '\0';
    return o;
}

/* ── Redirects ───────────────────────────────────────────────────── */

/* Keel's per-hop redirect check (KlHttpRedirectConfig.on_redirect). The host
 * allowlist used to be checked against the FIRST URL only: Keel followed every
 * Location after it, so an allowed host could redirect http.fetch to a cloud
 * metadata endpoint (169.254.169.254) or an internal service, and the body
 * came back to the app. Every hop now passes the same check. @p data is the
 * HlHttpConfig, which lives (sealed) for the whole process. */
int hl_http_redirect_allowed(const char *next_url, void *data)
{
    const HlHttpConfig *cfg = (const HlHttpConfig *)data;
    KlUrl parsed;
    if (!next_url || kl_url_parse(next_url, &parsed) != 0 ||
        hl_http_check_host(cfg, parsed.host, parsed.host_len) != 0) {
        ShJsonWriter w = hl_audit_begin("http.redirect");
        char red_url[512];   /* no userinfo, query or fragment (audit 17) */
        hl_http_url_redact(next_url, red_url, sizeof red_url);
        sh_json_write_kv_string(&w, "url", red_url);
        sh_json_write_kv_string(&w, "result", "denied");
        hl_audit_end(&w);
        log_warn("[hull:http] redirect to a host outside manifest.hosts refused");
        return -1;
    }
    return 0;
}

/* ── Whole-chain deadline (sync path) ────────────────────────────── */

/* Keel's sync redirect loop calls kl_http_client_request once per hop with
 * the SAME config pointer, and each call starts a fresh deadline from its
 * timeout_ms: a 60 s ceiling over max_redirects hops blocked the event loop
 * for ~11 minutes. Hull keeps the absolute deadline here, and before every
 * hop sets the config's timeout to what is left of it (the loop re-reads the
 * config on each hop), refusing the hop once nothing is left. */
typedef struct {
    const HlHttpConfig *cfg;
    KlHttpClientConfig *kl_cfg;      /* the config Keel re-reads per hop */
    uint64_t            deadline_ms; /* absolute, monotonic */
    int                 timed_out;
} HlHttpSyncChain;

int hl_http_chain_remaining_ms(uint64_t deadline_ms, uint64_t now_ms)
{
    if (now_ms >= deadline_ms)
        return 0;
    uint64_t left = deadline_ms - now_ms;
    return left > (uint64_t)INT32_MAX ? INT32_MAX : (int)left;
}

static int hl_http_sync_redirect_hop(const char *next_url, void *data)
{
    HlHttpSyncChain *chain = (HlHttpSyncChain *)data;
    if (hl_http_redirect_allowed(next_url, (void *)(uintptr_t)chain->cfg) != 0)
        return -1;
    int left = hl_http_chain_remaining_ms(chain->deadline_ms,
                                          kl_monotonic_ms());
    if (left <= 0) {
        chain->timed_out = 1;
        log_warn("[hull:http] request timed out across its redirect chain");
        return -1;
    }
    chain->kl_cfg->timeout_ms = left;
    return 0;
}

/* ── Public API ──────────────────────────────────────────────────── */

int hl_cap_http_request(const HlHttpConfig *cfg, int timeout_ms,
                        const char *method, const char *url,
                        const HlHttpHeader *headers, int num_headers,
                        const char *body, size_t body_len,
                        KlHttpClientResponse *resp)
{
    if (!cfg || !method || !url || !resp)
        return -1;

    memset(resp, 0, sizeof(*resp));

    /* Parse URL for allowlist check */
    KlUrl parsed;
    if (kl_url_parse(url, &parsed) != 0)
        return -1;

    /* Check host allowlist */
    if (hl_http_check_host(cfg, parsed.host, parsed.host_len) != 0) {
        ShJsonWriter w = hl_audit_begin("http.request");
        sh_json_write_kv_string(&w, "method", method);
        char red_url[512];   /* no userinfo, query or fragment (audit 17) */
        hl_http_url_redact(url, red_url, sizeof red_url);
        sh_json_write_kv_string(&w, "url", red_url);
        sh_json_write_kv_string(&w, "result", "denied");
        hl_audit_end(&w);
        return -1;
    }

    /* KlHttpClientConfig from HlHttpConfig. ONE deadline for the whole
     * request, clamped to the sync ceiling: this call blocks the event loop
     * until it returns. Keel (>= 3.3.0) bounds one hop by timeout_ms; the
     * redirect hook below keeps the whole chain inside the same deadline. */
    int timeout = hl_http_timeout_resolve(timeout_ms, cfg->timeout_ms,
                                          HL_HTTP_SYNC_MAX_TIMEOUT_MS);
    KlHttpClientConfig kl_cfg = {
        .timeout_ms        = timeout,
        .max_response_size = cfg->max_response_size,
        .tls               = cfg->tls,
        .decompress        = cfg->decompress,
    };

    /* Delegate to Keel - prefer redirect+pooled > redirect > pooled > plain */
    KlAllocator alloc = kl_allocator_default();
    int rc;
    if (cfg->follow_redirects) {
        HlHttpSyncChain chain = {
            .cfg         = cfg,
            .kl_cfg      = &kl_cfg,
            .deadline_ms = kl_monotonic_ms() + (uint64_t)timeout,
            .timed_out   = 0,
        };
        KlHttpRedirectConfig redir = {
            .max_redirects    = hl_http_max_redirects(cfg->max_redirects),
            .on_redirect      = hl_http_sync_redirect_hop,
            .on_redirect_data = &chain,
        };
        if (cfg->pool)
            rc = kl_http_redirect_request_pooled(cfg->pool, &alloc, &kl_cfg, &redir,
                                             method, url,
                                             (const KlHttpClientHeader *)headers,
                                             num_headers, body, body_len, resp);
        else
            rc = kl_http_redirect_request(&alloc, &kl_cfg, &redir, method, url,
                                      (const KlHttpClientHeader *)headers,
                                      num_headers, body, body_len, resp);
        if (rc != 0 && chain.timed_out)
            resp->error = KL_ERR_TIMEOUT;
    } else if (cfg->pool) {
        rc = kl_http_client_request_pooled(cfg->pool, &alloc, &kl_cfg, method, url,
                                       (const KlHttpClientHeader *)headers,
                                       num_headers, body, body_len, resp);
    } else {
        rc = kl_http_client_request(&alloc, &kl_cfg, method, url,
                                (const KlHttpClientHeader *)headers, num_headers,
                                body, body_len, resp);
    }

    /* Audit */
    {
        ShJsonWriter w = hl_audit_begin("http.request");
        sh_json_write_kv_string(&w, "method", method);
        char red_url[512];   /* no userinfo, query or fragment (audit 17) */
        hl_http_url_redact(url, red_url, sizeof red_url);
        sh_json_write_kv_string(&w, "url", red_url);
        if (rc == 0)
            sh_json_write_kv_int(&w, "status", resp->status);
        else
            sh_json_write_kv_string(&w, "error", kl_strerror(resp->error));
        sh_json_write_kv_int(&w, "result", rc);
        hl_audit_end(&w);
    }
    return rc;
}
