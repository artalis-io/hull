/*
 * agent/request.c - `hull agent request`, `hull agent status`,
 *                   `hull agent errors` - dev-server-adjacent ops.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include <sh_json.h>
#include <sh_arena.h>

#include <limits.h>      /* PATH_MAX */
#include <stdint.h>      /* uint64_t (kl_monotonic_ms timing) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <keel/allocator.h>    /* kl_allocator_default */
#include <keel/clock.h>        /* kl_monotonic_ms: request elapsed timing */
#include <keel/error.h>        /* kl_strerror: name the failure, not just its number */
#include <keel/http_client.h>  /* kl_http_client_request (sync HTTP client) */

int hl_agent_request(const char *method, const char *path, int port,
                     const char *body, const char **headers, int nhdrs,
                     ShJsonBuf *out)
{
    /* Each caller header is a full "Name: Value" line; Keel's sync client
     * wants split {name, value} pairs. The caller's lines are const and not
     * NUL-terminated at the colon, so copy the split fields into one bounded
     * scratch buffer and point the pair array into it. */
    if (nhdrs < 0)
        nhdrs = 0;
    if (nhdrs > KL_HTTP_CLIENT_MAX_REQ_HEADERS)
        return hl_agent_write_error(out, "too many request headers");
    KlHttpClientHeader kh[KL_HTTP_CLIENT_MAX_REQ_HEADERS];
    char hbuf[8192];
    size_t hoff = 0;
    for (int i = 0; i < nhdrs; i++) {
        const char *hline = headers[i] ? headers[i] : "";
        const char *colon = strchr(hline, ':');
        if (!colon)
            return hl_agent_write_error(out, "malformed request header");
        size_t nlen = (size_t)(colon - hline);
        const char *val = colon + 1;
        while (*val == ' ')
            val++;
        size_t vlen = strlen(val);
        if (hoff + nlen + 1 + vlen + 1 > sizeof(hbuf))
            return hl_agent_write_error(out, "request headers too large");
        char *nm = hbuf + hoff;
        memcpy(nm, hline, nlen);
        nm[nlen] = '\0';
        hoff += nlen + 1;
        char *vl = hbuf + hoff;
        memcpy(vl, val, vlen);
        vl[vlen] = '\0';
        hoff += vlen + 1;
        kh[i].name  = nm;
        kh[i].value = vl;
    }

    char url[PATH_MAX + 64];
    int un = snprintf(url, sizeof(url), "http://127.0.0.1:%d%s",
                      port, path ? path : "/");
    if (un < 0 || (size_t)un >= sizeof(url))
        return hl_agent_write_error(out, "request path too long");

    /* Blocking HTTP/1.1 exchange via Keel's sync client: it owns connect,
     * send, recv, and the status-line + header parse. timeout_ms/max_response
     * preserve the former 5s socket timeout + 1 MB response cap. */
    KlHttpClientConfig cfg = {
        .timeout_ms        = 5000,
        .max_response_size = 1024 * 1024,
    };
    KlAllocator alloc = kl_allocator_default();
    KlHttpClientResponse resp;
    memset(&resp, 0, sizeof resp);

    size_t body_len = body ? strlen(body) : 0;
    uint64_t t0 = kl_monotonic_ms();
    int rc = kl_http_client_request(&alloc, &cfg, method, url,
                                    kh, nhdrs, body, body_len, &resp);
    long elapsed_ms = (long)(kl_monotonic_ms() - t0);
    if (rc != 0) {
        /* Report the URL ACTUALLY REQUESTED and a named reason, not just the
         * host:port we intended plus a bare number.
         *
         * The previous form was "request to 127.0.0.1:<port> failed (error N)",
         * which names the one part that is rarely at fault and reads as a
         * connection failure whatever went wrong. A real case: on Windows a
         * POSIX shell rewrites a bare "/health" ARGUMENT into a filesystem path
         * before hull runs, so the URL becomes
         *     http://127.0.0.1:39890C:/Program Files/Git/health
         * and Keel returns KL_ERR_URL (14). The old message blamed a host and
         * port that were reachable - curl had just fetched /health from them -
         * and hid the malformed URL that was the entire problem.
         *
         * kl_strerror gives the reason in words; the numeric code stays so an
         * existing report or grep still resolves. */
        char err[PATH_MAX + 256];
        snprintf(err, sizeof(err), "request to %s failed: %s (error %d)",
                 url, kl_strerror(resp.error), (int)resp.error);
        kl_http_client_response_free(&resp);
        return hl_agent_write_error(out, err);
    }

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_int(&w, "status", resp.status);
    sh_json_write_kv_int(&w, "elapsed_ms", (int)elapsed_ms);

    /* Response headers (Keel already parsed them; the status line is excluded). */
    sh_json_write_key(&w, "headers");
    sh_json_write_object_start(&w);
    for (int i = 0; i < resp.num_headers; i++) {
        if (resp.headers[i].name)
            sh_json_write_kv_string(&w, resp.headers[i].name,
                                    resp.headers[i].value ? resp.headers[i].value : "");
    }
    sh_json_write_object_end(&w);

    /* Body (length-aware: binary-safe, matches the former NUL-terminated
     * write for every real text/JSON response). */
    sh_json_write_key(&w, "body");
    sh_json_write_string_n(&w, resp.body ? resp.body : "", resp.body_len);

    sh_json_write_object_end(&w);
    kl_http_client_response_free(&resp);
    return 0;
}

/* ── hl_agent_status ───────────────────────────────────────────────── */

int hl_agent_status(const char *app_dir, int port, ShJsonBuf *out)
{
    /* The port of the live `hull dev --agent` session, when dev.json is
     * bound to one: a dev.json a repo shipped is not (audit 36). */
    int dev_port = 0;
    if (hl_agent_dev_session(app_dir, NULL, NULL, &dev_port) == 0 &&
        dev_port > 0)
        port = dev_port;

    /* Pure TCP-connect liveness probe (no app route or middleware invoked),
     * driven through Keel's socket provider + a private KlEventCtx watcher.
     * See agent/probe.c. */
    int running = hl_agent_tcp_probe(port, 1000);

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_bool(&w, "running", running != 0);
    sh_json_write_kv_int(&w, "port", port);
    sh_json_write_object_end(&w);
    return 0;
}

/* ── hl_agent_errors ───────────────────────────────────────────────── */

/* The record `hull dev --agent`'s served child writes on a load failure
 * (serve.c hl_serve_load_app): {"error":"failed to load <entry>",
 * "timestamp":N,"session":"<nonce>"}. Keep the two in step. */
#define HL_AGENT_ERR_PREFIX "failed to load "

static void errors_write_empty(ShJsonBuf *out, const char *ignored)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_key(&w, "errors");
    sh_json_write_array_start(&w);
    sh_json_write_array_end(&w);
    if (ignored)
        sh_json_write_kv_string(&w, "ignored", ignored);
    sh_json_write_object_end(&w);
}

int hl_agent_errors(const char *app_dir, ShJsonBuf *out)
{
    char err_path[PATH_MAX];
    int pn = snprintf(err_path, sizeof(err_path), "%s/.hull/last_error.json",
                      app_dir);
    if (pn < 0 || (size_t)pn >= sizeof(err_path))
        return hl_agent_write_error(out, "app_dir too long");

    size_t n = 0;
    char *buf = hl_agent_read_sidecar(err_path, 64 * 1024, &n);
    if (!buf) {
        errors_write_empty(out, NULL);
        return 0;
    }

    /* The app tree may be a cloned repo, and an agent reads this output as
     * Hull's own diagnostics: a committed last_error.json was served as is,
     * whatever it told the agent to do (audit 36). The record is served only
     * when it carries the nonce of the live `hull dev --agent` session, and
     * then only its known fields, re-encoded. */
    static const char unverified[] =
        ".hull/last_error.json was ignored: it was not written by a live "
        "`hull dev --agent` session (repo content, not Hull output)";
    char nonce[65];
    if (hl_agent_dev_session(app_dir, nonce, NULL, NULL) != 0) {
        free(buf);
        errors_write_empty(out, unverified);
        return 0;
    }

    SHArena *arena = hl_agent_text_is_terminal_safe(buf, n)
                         ? sh_arena_create(4096 + 8 * n) : NULL;
    ShJsonValue *v = NULL;
    const char *msg = NULL, *sess = NULL;
    double ts = -1;
    if (arena && sh_json_parse(buf, n, arena, &v) == SH_JSON_OK && v &&
        sh_json_type(v) == SH_JSON_OBJECT) {
        msg  = sh_json_as_string(sh_json_get(v, "error"), NULL);
        sess = sh_json_as_string(sh_json_get(v, "session"), NULL);
        ts   = sh_json_as_double(sh_json_get(v, "timestamp"), -1);
    }
    int ok = msg && sess && strcmp(sess, nonce) == 0 &&
             strncmp(msg, HL_AGENT_ERR_PREFIX,
                     sizeof(HL_AGENT_ERR_PREFIX) - 1) == 0 &&
             strlen(msg) < 1024 && ts >= 0 && ts < 1e13;
    if (ok) {
        ShJsonWriter w;
        sh_json_writer_init(&w, sh_json_buf_write, out);
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "error", msg);
        sh_json_write_kv_int(&w, "timestamp", (int64_t)ts);
        sh_json_write_object_end(&w);
    }
    if (arena) sh_arena_free(arena);
    free(buf);
    if (!ok)
        errors_write_empty(out, unverified);
    return 0;
}
