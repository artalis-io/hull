/*
 * frontend/js_generation.c - the C-owned JS frontend generation/session manager.
 * See include/hull/frontend/js_generation.h + docs/js_frontend_slice6_dispatcher.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/frontend/js_generation.h"
#include "hull/frontend/js_session.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>

#define HL_JS_GEN_MAX 8        /* concurrent live generations cap */

/* The production entry module (registers globalThis.__hull_frontend). */
static const char *ENTRY_MODULE = "hull:source:frontend_entry";

typedef struct { int64_t token; HlJsSession *session; } HlGenSlot;

static struct {
    pthread_mutex_t mu;
    int64_t         next_token;                 /* monotonic; NEVER reset (ABA safety) */
    HlGenSlot       live[HL_JS_GEN_MAX];
    int             live_count;
    int             mu_init;
} G = { PTHREAD_MUTEX_INITIALIZER, 0, { {0, NULL} }, 0, 1 };

int hl_js_gen_available(void) { return 1; }    /* this TU is only compiled when the engine is linked */

/* Find the live slot for a token (caller holds the mutex), or NULL. */
static HlGenSlot *find_locked(int64_t token)
{
    if (token <= 0) return NULL;
    for (int i = 0; i < G.live_count; i++)
        if (G.live[i].token == token) return &G.live[i];
    return NULL;
}

int64_t hl_js_gen_open(void)
{
    pthread_mutex_lock(&G.mu);
    if (G.live_count >= HL_JS_GEN_MAX || G.next_token >= INT64_MAX) { pthread_mutex_unlock(&G.mu); return 0; }
    HlJsSessionLimits def = HL_JS_SESSION_LIMITS_DEFAULT;
    HlJsSession *s = hl_js_session_create(&def);
    if (!s) { pthread_mutex_unlock(&G.mu); return 0; }
    int64_t token = ++G.next_token;             /* strictly monotonic, never reused */
    G.live[G.live_count].token = token;
    G.live[G.live_count].session = s;
    G.live_count++;
    pthread_mutex_unlock(&G.mu);
    return token;
}

/* A malloc'd copy of a constant stale-JSON string (caller frees), for a dead/unknown token. */
static int emit_stale(const char *json, char **out_json, size_t *out_len)
{
    size_t n = strlen(json);
    char *c = (char *)malloc(n + 1);
    if (!c) { if (out_json) *out_json = NULL; if (out_len) *out_len = 0; return -1; }
    memcpy(c, json, n + 1);
    if (out_json) *out_json = c;
    if (out_len) *out_len = n;
    return -1;
}
#define STALE_DIAG "{\"severity\":\"error\",\"code\":\"javascript.internal\",\"message\":\"stale frontend session\",\"range\":null}"
static const char *STALE_ANALYZE = "{\"schema_version\":1,\"status\":\"error\",\"unit_id\":-1,\"declarations\":[],\"diagnostics\":[" STALE_DIAG "]}";
static const char *STALE_SEM     = "{\"error\":" STALE_DIAG "}";
static const char *STALE_SCOPE   = "{\"ok\":false,\"error\":" STALE_DIAG "}";

int hl_js_gen_analyze(int64_t token, const uint8_t *src, size_t src_len, const char *path,
                      char **out_json, size_t *out_len)
{
    pthread_mutex_lock(&G.mu);                   /* held across lookup + invocation (close-safe) */
    HlGenSlot *slot = find_locked(token);
    if (!slot) { pthread_mutex_unlock(&G.mu); return emit_stale(STALE_ANALYZE, out_json, out_len); }
    int rc = hl_js_session_analyze(slot->session, ENTRY_MODULE, "analyze", src, src_len, path, NULL, 0, out_json, out_len);
    pthread_mutex_unlock(&G.mu);
    return rc;
}

/* declarationSemantics / scope pass the id via a small options JSON. */
static int invoke_id(int64_t token, const char *method, const char *opt_key, int64_t id,
                     const char *stale_json, char **out_json, size_t *out_len)
{
    char opts[64];
    int n = snprintf(opts, sizeof(opts), "{\"%s\":%" PRId64 "}", opt_key, id);
    if (n < 0 || (size_t)n >= sizeof(opts)) return emit_stale(stale_json, out_json, out_len);
    pthread_mutex_lock(&G.mu);
    HlGenSlot *slot = find_locked(token);
    if (!slot) { pthread_mutex_unlock(&G.mu); return emit_stale(stale_json, out_json, out_len); }
    int rc = hl_js_session_analyze(slot->session, ENTRY_MODULE, method, (const uint8_t *)"", 0, NULL, opts, (size_t)n, out_json, out_len);
    pthread_mutex_unlock(&G.mu);
    return rc;
}
int hl_js_gen_declaration_semantics(int64_t token, int64_t decl_id, char **out_json, size_t *out_len)
{
    return invoke_id(token, "declarationSemantics", "declId", decl_id, STALE_SEM, out_json, out_len);
}
int hl_js_gen_scope(int64_t token, int64_t unit_id, char **out_json, size_t *out_len)
{
    return invoke_id(token, "scope", "unitId", unit_id, STALE_SCOPE, out_json, out_len);
}

void hl_js_gen_close(int64_t token)
{
    pthread_mutex_lock(&G.mu);
    HlGenSlot *slot = find_locked(token);
    if (slot) {
        hl_js_session_destroy(slot->session);
        int idx = (int)(slot - G.live);
        G.live[idx] = G.live[G.live_count - 1];  /* compact */
        G.live[G.live_count - 1].token = 0;
        G.live[G.live_count - 1].session = NULL;
        G.live_count--;
    }
    pthread_mutex_unlock(&G.mu);                 /* token stays permanently dead (counter unchanged) */
}

void hl_js_gen_shutdown(void)
{
    pthread_mutex_lock(&G.mu);
    for (int i = 0; i < G.live_count; i++) { hl_js_session_destroy(G.live[i].session); G.live[i].session = NULL; G.live[i].token = 0; }
    G.live_count = 0;
    /* CRITICAL: do NOT reset G.next_token - monotonic for the whole process (ABA safety). */
    pthread_mutex_unlock(&G.mu);
}
