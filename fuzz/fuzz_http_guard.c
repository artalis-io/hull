/*
 * fuzz_http_guard.c: libFuzzer harness for the outbound-HTTP text guards in
 * cap/http.c that audits 17 / 22 / 23 / 26 added:
 *
 *   - hl_http_url_redact (audit 17): the audit log's form of a URL - no
 *     userinfo, query or fragment.
 *   - hl_http_reserved_header (audit 22) / hl_http_method_valid (audit 23):
 *     which caller headers and methods may reach Keel's request writer.
 *   - hl_http_header_redirect_safe (audit 26): which caller headers may
 *     follow a redirect to another origin.
 *
 * The first byte picks a mode; the rest are NUL-separated fields.
 *
 * Mode 0 (URL): bounds (exact-size buffers, NUL inside outsz, return ==
 * strlen), truncation is a prefix of the untruncated form, and for a URL
 * Keel's own parser accepts (kl_url_parse, linked for real) with no '@'
 * before its query, the redaction is exactly the URL up to its first '?' /
 * '#' - nothing of the authority or path is lost, nothing of the query kept.
 *
 * Mode 1 (credentials): URL = scheme "://" user ":" "PW!" pass "@" host path.
 * The redaction must never contain the password marker "PW!" (the user,
 * host and path fields have '!' stripped, so only the password can supply
 * it).
 *
 * Mode 2 (headers / method): every result agrees with an independent
 * reference: RFC 9110 tchar tokens, the reserved / Proxy-* names, the
 * redirect-safe list, all compared without case.
 *
 * Mode 3 (redirect hop, audits 25 / 26): hl_http_redirect_guard_init over an
 * origin URL, caller headers and a body flag, then hl_http_redirect_allowed
 * for a next URL against an allowlist. The verdict must equal the reference:
 * the next URL parses, its host is allowed, an https chain stays on https, and
 * a request carrying a body or a non-safe header stays on its first origin
 * (same host without case, same scheme and port; or http:80 -> https:443 on
 * the same host).
 *
 * Keel's request functions, clock, allocator and error strings are stubbed
 * in fuzz_http_guard_stubs.c (abort if reached): the guards never call them, and linking Keel's client
 * would drag in mbedTLS. kl_url_parse is the real one (vendor/keel/src/url.c).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/http.h"

#include <keel/url.h>
#include "log.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keel stubs for the request paths: fuzz/fuzz_http_guard_stubs.c. */

/* Characters kept out of mode 1's password (none: '?' / '#' leaked the
 * password's start until audit 28 fixed hl_http_url_redact). */
#define PASS_BAN ""

static void fail(const char *what, const char *s)
{
    fprintf(stderr, "fuzz_http_guard: %s\n  input: \"", what);
    for (const char *p = s ? s : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') fputc(c, stderr);
        else fprintf(stderr, "\\x%02x", c);
    }
    fprintf(stderr, "\"\n");
    abort();
}

#define MAX_FIELDS 12
typedef struct { char *f[MAX_FIELDS]; int n; char *buf; } Fields;

static int split(const uint8_t *d, size_t n, Fields *out)
{
    out->buf = malloc(n + 1);
    if (!out->buf) return -1;
    memcpy(out->buf, d, n);
    out->buf[n] = '\0';
    out->n = 0;
    char *p = out->buf, *end = out->buf + n;
    while (out->n < MAX_FIELDS) {
        out->f[out->n++] = p;
        char *z = memchr(p, '\0', (size_t)(end - p));
        if (!z || z >= end) break;
        p = z + 1;
    }
    while (out->n < MAX_FIELDS) out->f[out->n++] = end;   /* "" */
    return 0;
}

/* Redact @p url into an exact-size buffer; check the bounds contract. */
static char *redact(const char *url, size_t outsz)
{
    char *out = malloc(outsz ? outsz : 1);
    if (!out) abort();
    size_t r = hl_http_url_redact(url, out, outsz);
    if (outsz == 0) {
        if (r != 0) fail("outsz 0 returned non-zero", url);
        out[0] = '\0';
        return out;
    }
    const char *nul = memchr(out, '\0', outsz);
    if (!nul) fail("redaction not NUL-terminated inside outsz", url);
    if ((size_t)(nul - out) != r) fail("return value != strlen(out)", url);
    return out;
}

static void mode_url(const char *url, size_t small)
{
    size_t big = strlen(url) + 16;   /* the redaction never grows past this */
    char *full = redact(url, big);
    size_t flen = strlen(full);
    if (flen + 1 >= big) fail("redaction filled a buffer sized past the URL", url);

    char *trunc = redact(url, small);
    if (strncmp(full, trunc, strlen(trunc)) != 0)
        fail("truncated redaction is not a prefix of the full one", url);
    free(trunc);

    KlUrl u;
    size_t cut = strcspn(url, "?#");
    /* No '@' anywhere: an '@' past the authority (in the path or query) is
     * indistinguishable from a password holding '/', '?' or '#', so the
     * whole authority is redacted then (audit 28). */
    if (kl_url_parse(url, &u) == 0 && !strchr(url, '@')) {
        if (flen != cut || memcmp(full, url, cut) != 0)
            fail("redaction of a Keel-valid URL is not the URL up to '?'/'#'", url);
    }
    free(full);
}

static void strip_bang(char *s)
{
    char *w = s;
    for (char *p = s; *p; p++) if (*p != '!') *w++ = *p;
    *w = '\0';
}

static void strip_chars(char *s, const char *ban)
{
    char *w = s;
    for (char *p = s; *p; p++) if (!strchr(ban, *p)) *w++ = *p;
    *w = '\0';
}

static void mode_creds(Fields *fl)
{
    static const char *const schemes[] = {
        "https://", "http://", "wss://", "ws://", "http+unix://", "HTTPS://"
    };
    char *user = fl->f[1], *pass = fl->f[2], *host = fl->f[3], *path = fl->f[4];
    strip_bang(user); strip_bang(host); strip_bang(path);
    /* The user ends at ':' and holds no authority delimiter; the host holds
     * no '@' (else it is part of the password by the last-'@' rule). */
    strip_chars(user, "@:/?#");
    strip_chars(host, "@");
    strip_chars(pass, PASS_BAN);
    const char *scheme = schemes[(unsigned char)fl->f[0][0] % 6];

    size_t n = strlen(scheme) + strlen(user) + strlen(pass) + strlen(host) +
               strlen(path) + 16;
    char *url = malloc(n);
    if (!url) return;
    snprintf(url, n, "%s%s:PW!%s@%s%s", scheme, user, pass, host, path);
    char *out = redact(url, n + 16);
    if (strstr(out, "PW!"))
        fail("URL redaction kept the password", url);
    free(out);
    free(url);
}

/* ── Mode 2 references ──────────────────────────────────────────────── */

static int ref_tchar(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || (c != 0 && strchr("!#$%&'*+-.^_`|~", c));
}

static int ref_token(const char *s)
{
    if (!*s) return 0;
    for (; *s; s++) if (!ref_tchar((unsigned char)*s)) return 0;
    return 1;
}

static int ref_ieq(const char *a, const char *b)
{
    for (;; a++, b++) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static int ref_reserved(const char *name)
{
    static const char *const owned[] = {
        "host", "content-length", "transfer-encoding", "te", "trailer",
        "connection", "keep-alive", "upgrade"
    };
    if (!ref_token(name)) return 1;
    char pre[7];
    size_t i = 0;
    for (; i < 6 && name[i]; i++) pre[i] = name[i];
    pre[i] = '\0';
    if (i == 6 && ref_ieq(pre, "proxy-")) return 1;
    for (size_t k = 0; k < sizeof owned / sizeof owned[0]; k++)
        if (ref_ieq(name, owned[k])) return 1;
    return 0;
}

static int ref_redirect_safe(const char *name)
{
    static const char *const safe[] = {
        "accept", "accept-language", "accept-encoding", "user-agent",
        "content-type", "content-language", "cache-control", "pragma",
        "if-none-match", "if-modified-since", "if-match",
        "if-unmodified-since", "if-range", "range",
        "authorization", "cookie", "proxy-authorization"
    };
    for (size_t k = 0; k < sizeof safe / sizeof safe[0]; k++)
        if (ref_ieq(name, safe[k])) return 1;
    return 0;
}

static void mode_headers(Fields *fl)
{
    HlHttpHeader h[MAX_FIELDS];
    int n = 0;
    for (int i = 0; i < fl->n && n < MAX_FIELDS; i++) {
        h[n].name = fl->f[i];
        h[n].value = "v";
        n++;
    }
    const char *got = hl_http_reserved_header(h, n);
    const char *want = NULL;
    for (int i = 0; i < n && !want; i++)
        if (ref_reserved(h[i].name)) want = h[i].name;
    if (got != want)
        fail("hl_http_reserved_header disagrees with the reference",
             want ? want : (got ? got : ""));
    if (hl_http_reserved_header(NULL, 3) != NULL)
        fail("NULL header list reported a reserved header", "");

    for (int i = 0; i < n; i++) {
        if (hl_http_header_redirect_safe(h[i].name) != ref_redirect_safe(h[i].name))
            fail("hl_http_header_redirect_safe disagrees with the reference", h[i].name);
        if (hl_http_method_valid(h[i].name) != ref_token(h[i].name))
            fail("hl_http_method_valid disagrees with the RFC 9110 token rule", h[i].name);
    }
    if (hl_http_header_redirect_safe(NULL) != 0 || hl_http_method_valid(NULL) != 0)
        fail("NULL name accepted", "");
}

/* ── Mode 3: redirect hop ───────────────────────────────────────────── */

static int ref_same_origin(const KlUrl *o, const KlUrl *n)
{
    if (o->is_unix || n->is_unix)
        return o->is_unix && n->is_unix && o->is_https == n->is_https &&
               strcmp(o->unix_path, n->unix_path) == 0;
    if (o->host_len != n->host_len || o->host_len >= HL_HTTP_GUARD_ORIGIN_MAX)
        return 0;
    for (size_t i = 0; i < o->host_len; i++) {
        unsigned char a = (unsigned char)o->host[i], b = (unsigned char)n->host[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + 32);
        if (a != b) return 0;
    }
    if (o->is_https == n->is_https) return o->port == n->port;
    return !o->is_https && o->port == 80 && n->port == 443;
}

static void mode_redirect(Fields *fl, uint8_t flags)
{
    const char *origin = fl->f[0], *next = fl->f[1];
    const char *pats[4] = { fl->f[2], fl->f[3], fl->f[4], fl->f[5] };
    int npats = 1 + (flags >> 2) % 4;
    HlHttpHeader h[3];
    int nh = 0;
    for (int i = 6; i < 9; i++)
        if (fl->f[i][0]) { h[nh].name = fl->f[i]; h[nh].value = "v"; nh++; }
    size_t body_len = (flags & 0x10) ? 1 : 0;

    HlHttpConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.allowed_hosts = pats;
    cfg.count = npats;

    KlUrl o, n;
    int o_ok = kl_url_parse(origin, &o) == 0;
    HlHttpRedirectGuard g;
    hl_http_redirect_guard_init(&g, &cfg, o_ok ? &o : NULL, h, nh, body_len);
    int got = hl_http_redirect_allowed(next, &g) == 0;

    int secrets = !o_ok || body_len > 0;
    for (int i = 0; i < nh; i++)
        if (!ref_redirect_safe(h[i].name)) secrets = 1;
    int want = kl_url_parse(next, &n) == 0 &&
               hl_http_check_host(&cfg, n.host, n.host_len) == 0 &&
               !(o_ok && o.is_https && !n.is_https) &&
               (!secrets || (o_ok && ref_same_origin(&o, &n)));
    if (got != want) {
        char msg[64];
        snprintf(msg, sizeof msg, "redirect verdict %d, reference %d", got, want);
        fprintf(stderr, "fuzz_http_guard: origin=\"%s\"\n", origin);
        fail(msg, next);
    }
}

/* A refused hop logs a WARN (redirect_refused): silence log.c, or every
 * refusal writes a line and the run's stderr grows by gigabytes. */
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    log_set_quiet(true);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1 || size > 4096) return 0;
    int mode = data[0] % 4;
    if (mode == 0) {
        if (size < 2) return 0;
        size_t small = data[1];
        char *url = malloc(size - 1);
        if (!url) return 0;
        memcpy(url, data + 2, size - 2);
        url[size - 2] = '\0';
        mode_url(url, small);
        free(url);
        return 0;
    }
    Fields fl;
    if (split(data + 1, size - 1, &fl) != 0) return 0;
    if (mode == 1)      mode_creds(&fl);
    else if (mode == 2) mode_headers(&fl);
    else                mode_redirect(&fl, data[0]);
    free(fl.buf);
    return 0;
}
