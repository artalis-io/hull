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

    /* The authority ends at the first '/', '?' or '#'; the last '@' before
     * it ends the userinfo. An '@' anywhere after it means either a password
     * holding an unencoded '/', '?' or '#' (`u:hunter2#x@h` printed
     * "u:hunter2" - audit 28) or an '@' in the path / query; the two cannot
     * be told apart, so neither is printed (as hl_db_dsn_redact). */
    size_t auth_end = strcspn(rest, "/?#");
    const char *at = NULL;
    for (size_t i = 0; i < auth_end; i++)
        if (rest[i] == '@') at = rest + i;
    int ambiguous = strchr(rest + auth_end, '@') != NULL;
    const char *keep = at ? at + 1 : rest;
    size_t keep_len = end - (size_t)(keep - rest);

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
 * came back to the app. Every hop now passes the same check. A chain that
 * began on https may not continue on http (audit 25): Keel re-sends a 307 /
 * 308 body and every header but Authorization / Cookie / Proxy-Authorization,
 * so an X-Api-Key went out in cleartext. @p data is the request's
 * HlHttpRedirectGuard. */
static void redirect_refused(const char *next_url, const char *reason,
                             const char *msg)
{
    ShJsonWriter w = hl_audit_begin("http.redirect");
    char red_url[512];   /* no userinfo, query or fragment (audit 17) */
    hl_http_url_redact(next_url, red_url, sizeof red_url);
    sh_json_write_kv_string(&w, "url", red_url);
    sh_json_write_kv_string(&w, "result", "denied");
    sh_json_write_kv_string(&w, "reason", reason);
    hl_audit_end(&w);
    log_warn("[hull:http] %s", msg);
}

int hl_http_header_redirect_safe(const char *name)
{
    /* Content negotiation and cache validators carry nothing a server could
     * use as a credential; Authorization / Cookie / Proxy-Authorization are
     * dropped by Keel itself on a cross-origin hop. Anything else - an
     * X-Api-Key, a request signature, a tenant token - would be re-sent to
     * the next origin as is. */
    static const char *const safe[] = {
        "accept", "accept-language", "accept-encoding", "user-agent",
        "content-type", "content-language", "cache-control", "pragma",
        "if-none-match", "if-modified-since", "if-match",
        "if-unmodified-since", "if-range", "range",
        "authorization", "cookie", "proxy-authorization", NULL
    };
    if (!name) return 0;
    for (int i = 0; safe[i]; i++)
        if (strcasecmp(name, safe[i]) == 0) return 1;
    return 0;
}

void hl_http_redirect_guard_init(HlHttpRedirectGuard *g,
                                 const HlHttpConfig *cfg, const KlUrl *origin,
                                 const HlHttpHeader *headers, int num_headers,
                                 size_t body_len)
{
    memset(g, 0, sizeof *g);
    g->cfg = cfg;
    if (!origin) {
        g->carries_secrets = 1;   /* no origin to compare: every hop moves */
        return;
    }
    g->origin_https = origin->is_https;
    g->origin_unix  = origin->is_unix;
    g->origin_port  = origin->port;
    const char *h = origin->is_unix ? origin->unix_path : origin->host;
    size_t hl = origin->is_unix ? strlen(origin->unix_path) : origin->host_len;
    if (hl < sizeof g->origin_host && (h || hl == 0)) {
        if (hl) memcpy(g->origin_host, h, hl);
        g->origin_host[hl] = '\0';
        g->origin_len   = hl;
        g->origin_known = 1;
    }
    if (body_len > 0)
        g->carries_secrets = 1;
    for (int i = 0; headers && i < num_headers; i++)
        if (!hl_http_header_redirect_safe(headers[i].name))
            g->carries_secrets = 1;
}

/* 1 when @p next is the guard's first origin. An http -> https upgrade to the
 * same host on the default ports counts as the same server: what it carries
 * went out in cleartext already, and the next hop only encrypts it. */
static int redirect_same_origin(const HlHttpRedirectGuard *g, const KlUrl *next)
{
    if (!g->origin_known || g->origin_unix != next->is_unix)
        return 0;
    if (g->origin_unix)
        return strcmp(g->origin_host, next->unix_path) == 0 &&
               g->origin_https == next->is_https;
    if (next->host_len != g->origin_len ||
        (g->origin_len && strncasecmp(g->origin_host, next->host,
                                      g->origin_len) != 0))
        return 0;
    if (g->origin_https == next->is_https)
        return g->origin_port == next->port;
    return !g->origin_https && g->origin_port == 80 && next->port == 443;
}

int hl_http_redirect_allowed(const char *next_url, void *data)
{
    const HlHttpRedirectGuard *g = (const HlHttpRedirectGuard *)data;
    KlUrl parsed;
    if (!g || !g->cfg || !next_url || kl_url_parse(next_url, &parsed) != 0 ||
        hl_http_check_host(g->cfg, parsed.host, parsed.host_len) != 0) {
        redirect_refused(next_url, "host",
                         "redirect to a host outside manifest.hosts refused");
        return -1;
    }
    if (g->origin_https && !parsed.is_https) {
        redirect_refused(next_url, "downgrade",
                         "redirect from https to http refused");
        return -1;
    }
    /* Audit 26: Keel re-sends a 307 / 308 body and every header but
     * Authorization / Cookie / Proxy-Authorization to the next origin, so a
     * request carrying either may not leave its first origin, even for an
     * allowed host. */
    if (g->carries_secrets && !redirect_same_origin(g, &parsed)) {
        redirect_refused(next_url, "cross_origin",
                         "cross-origin redirect refused: the request carries "
                         "a body or a header that would be re-sent there");
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
    HlHttpRedirectGuard guard;       /* allowlist + the chain's first scheme */
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
    if (hl_http_redirect_allowed(next_url, &chain->guard) != 0)
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

static int http_is_token(const char *s)
{
    if (!s || !*s) return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z')) continue;
        if (!strchr("!#$%&'*+-.^_`|~", c)) return 0;
    }
    return 1;
}

int hl_http_method_valid(const char *method)
{
    return http_is_token(method);
}

const char *hl_http_reserved_header(const HlHttpHeader *headers, int n)
{
    static const char *const owned[] = {
        "host", "content-length", "transfer-encoding", "te", "trailer",
        "connection", "keep-alive", "upgrade", NULL
    };
    for (int i = 0; headers && i < n; i++) {
        const char *name = headers[i].name;
        if (!http_is_token(name)) return name ? name : "";
        if (strncasecmp(name, "proxy-", 6) == 0) return name;
        for (int k = 0; owned[k]; k++)
            if (strcasecmp(name, owned[k]) == 0) return name;
    }
    return NULL;
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
    if (hl_http_reserved_header(headers, num_headers))
        return -1;   /* the bindings say which (audit 22) */
    if (!hl_http_method_valid(method))
        return -1;   /* not an RFC 9110 token (audit 23) */

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
            .kl_cfg      = &kl_cfg,
            .deadline_ms = kl_monotonic_ms() + (uint64_t)timeout,
            .timed_out   = 0,
        };
        hl_http_redirect_guard_init(&chain.guard, cfg, &parsed,
                                    headers, num_headers, body_len);
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
