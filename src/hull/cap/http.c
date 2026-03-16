/*
 * http.c — HTTP client capability (thin wrapper over Keel)
 *
 * Adds host allowlist checking and audit logging around
 * kl_client_request() from Keel's HTTP client module.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/http.h"
#include "hull/cap/audit.h"

#include <keel/allocator.h>
#include <keel/client_pool.h>
#include <keel/error.h>
#include <keel/redirect.h>
#include <keel/url.h>

#include <string.h>
#include <strings.h>

#include "log.h"

/* ── Host allowlist check ────────────────────────────────────────── */

int hl_http_check_host(const HlHttpConfig *cfg,
                        const char *host, size_t host_len)
{
    if (!cfg || !host)
        return -1;

    for (int i = 0; i < cfg->count; i++) {
        const char *allowed = cfg->allowed_hosts[i];
        size_t alen = strlen(allowed);
        if (alen == host_len && strncasecmp(allowed, host, host_len) == 0)
            return 0;
    }

    return -1;  /* not allowed */
}

/* ── Public API ──────────────────────────────────────────────────── */

int hl_cap_http_request(const HlHttpConfig *cfg,
                        const char *method, const char *url,
                        const HlHttpHeader *headers, int num_headers,
                        const char *body, size_t body_len,
                        KlClientResponse *resp)
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
        sh_json_write_kv_string(&w, "url", url);
        sh_json_write_kv_string(&w, "result", "denied");
        hl_audit_end(&w);
        return -1;
    }

    /* Construct KlClientConfig from HlHttpConfig */
    KlClientConfig kl_cfg = {
        .timeout_ms        = cfg->timeout_ms,
        .max_response_size = cfg->max_response_size,
        .tls               = cfg->tls,
        .decompress        = cfg->decompress,
    };

    /* Delegate to Keel — prefer redirect+pooled > redirect > pooled > plain */
    KlAllocator alloc = kl_allocator_default();
    int rc;
    if (cfg->follow_redirects) {
        KlRedirectConfig redir = { .max_redirects = cfg->max_redirects };
        if (cfg->pool)
            rc = kl_redirect_request_pooled(cfg->pool, &alloc, &kl_cfg, &redir,
                                             method, url,
                                             (const KlClientHeader *)headers,
                                             num_headers, body, body_len, resp);
        else
            rc = kl_redirect_request(&alloc, &kl_cfg, &redir, method, url,
                                      (const KlClientHeader *)headers,
                                      num_headers, body, body_len, resp);
    } else if (cfg->pool) {
        rc = kl_client_request_pooled(cfg->pool, &alloc, &kl_cfg, method, url,
                                       (const KlClientHeader *)headers,
                                       num_headers, body, body_len, resp);
    } else {
        rc = kl_client_request(&alloc, &kl_cfg, method, url,
                                (const KlClientHeader *)headers, num_headers,
                                body, body_len, resp);
    }

    /* Audit */
    {
        ShJsonWriter w = hl_audit_begin("http.request");
        sh_json_write_kv_string(&w, "method", method);
        sh_json_write_kv_string(&w, "url", url);
        if (rc == 0)
            sh_json_write_kv_int(&w, "status", resp->status);
        else
            sh_json_write_kv_string(&w, "error", kl_strerror(resp->error));
        sh_json_write_kv_int(&w, "result", rc);
        hl_audit_end(&w);
    }
    return rc;
}
