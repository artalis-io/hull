/*
 * module_registry.c — Canonical first-party Hull module table.
 *
 * Sorted by `name` in `strcmp` order so lookups can binary-search.
 * Adding a new module: insert in sort order, then bump the sort-order
 * sanity check at the bottom of `module_registry_self_check()` if any.
 *
 * The set of modules here is the v1 universe of names an app manifest
 * may reference. Whether a given module's runtime bindings actually
 * compile in is gated separately by HL_ENABLE_* flags (the resolver
 * checks `required_caps` against those).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/module_registry.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ── The canonical table ───────────────────────────────────────────── */

/*
 * Field shorthand:
 *   .name = "hull/foo", .api_major = 1, .intrinsic = 0|1, .pure = 0|1,
 *   .required_caps = HL_MOD_CAP_*, .deps = {"hull/bar", NULL}
 *
 * Entries MUST stay sorted by `name` (strcmp). Insertions in the
 * middle of the table are intentional — keep them sorted.
 */
static const HlModuleSpec REGISTRY[] = {
    /* ── Intrinsic core (always present, no declaration needed) ───── */
    {
        .name = "hull/app",
        .api_major = 1, .intrinsic = 1, .pure = 0,
        .required_caps = 0, .deps = {0},
    },

    /* ── C-native side-effect modules ─────────────────────────────── */
    {
        .name = "hull/compute",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_WASM, .deps = {0},
    },
    {
        .name = "hull/cookie",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        .name = "hull/crypto",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {0},
    },
    {
        .name = "hull/csv",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        .name = "hull/db",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_DB, .deps = {0},
    },
    {
        .name = "hull/email",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        /* email dispatches to either SMTP or HTTPS API providers — both
         * are reachable from the same email.send() entry point, so both
         * are hard deps. */
        .required_caps = 0,
        .deps = {"hull/http", "hull/smtp", 0},
    },
    {
        .name = "hull/env",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_ENV, .deps = {0},
    },
    {
        .name = "hull/form",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        .name = "hull/fs",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_FS, .deps = {0},
    },
    {
        .name = "hull/gpu",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_GPU, .deps = {0},
    },
    {
        .name = "hull/http",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        /* http.fetch — outbound HTTP/HTTPS client. */
        .required_caps = HL_MOD_CAP_HOSTS | HL_MOD_CAP_HTTP_CLIENT,
        .deps = {0},
    },
    {
        .name = "hull/i18n",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        .name = "hull/image",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {0},
    },
    {
        .name = "hull/inspect",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },

    /* ── Intrinsic: pure data codec ───────────────────────────────── */
    {
        .name = "hull/json",
        .api_major = 1, .intrinsic = 1, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        .name = "hull/jwt",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0,
        .deps = {"hull/crypto", 0},
    },

    /* ── Intrinsic: logger ────────────────────────────────────────── */
    {
        .name = "hull/log",
        .api_major = 1, .intrinsic = 1, .pure = 0,
        .required_caps = 0, .deps = {0},
    },

    /* ── Middleware ───────────────────────────────────────────────────
     * Every middleware consumes KlRequest/KlResponse and registers via
     * app.use() / app.use_post(), all of which need Keel's HTTP server.
     * They share HL_MOD_CAP_HTTP_SERVER — apps targeting an
     * HL_ENABLE_HTTP_SERVER=0 build can't declare any of them. */
    {
        .name = "hull/middleware/auth",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        .deps = {"hull/db", "hull/crypto", 0},
    },
    {
        .name = "hull/middleware/cors",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = HL_MOD_CAP_HTTP_SERVER, .deps = {0},
    },
    {
        .name = "hull/middleware/csrf",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        .deps = {"hull/crypto", 0},
    },
    {
        .name = "hull/middleware/etag",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        .deps = {"hull/crypto", 0},
        /* No db dep — etag only computes a SHA-256 from the body. */
    },
    {
        .name = "hull/middleware/health",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        /* health pings the DB and inspects server stats; both are hard
         * deps even when db_check is disabled at runtime, because the
         * required modules need to be importable. */
        .deps = {"hull/db", "hull/server", "hull/time", 0},
    },
    {
        .name = "hull/middleware/idempotency",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        .deps = {"hull/db", "hull/crypto", 0},
    },
    {
        .name = "hull/middleware/inbox",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        .deps = {"hull/db", 0},
    },
    {
        .name = "hull/middleware/logger",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER, .deps = {0},
    },
    {
        .name = "hull/middleware/outbox",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        .deps = {"hull/db", "hull/http", 0},
    },
    {
        .name = "hull/middleware/ratelimit",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER, .deps = {0},
    },
    {
        .name = "hull/middleware/rbac",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        .deps = {"hull/db", 0},
    },
    {
        .name = "hull/middleware/session",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        .deps = {"hull/db", "hull/crypto", 0},
    },
    {
        .name = "hull/middleware/transaction",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = HL_MOD_CAP_HTTP_SERVER,
        .deps = {"hull/db", 0},
    },

    /* ── Pure stdlib + remaining side-effect ─────────────────────── */
    {
        .name = "hull/search",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0,
        .deps = {"hull/db", 0},
    },
    {
        .name = "hull/server",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        /* app.get/post/use/etc. — inbound HTTP server registration. */
        .required_caps = HL_MOD_CAP_HTTP_SERVER, .deps = {0},
    },
    {
        .name = "hull/smtp",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        /* SMTP client — outbound mail delivery. */
        .required_caps = HL_MOD_CAP_HOSTS | HL_MOD_CAP_HTTP_CLIENT,
        .deps = {0},
    },
    {
        .name = "hull/template",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        .name = "hull/time",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {0},
    },
    {
        .name = "hull/validate",
        .api_major = 1, .intrinsic = 0, .pure = 1,
        .required_caps = 0, .deps = {0},
    },
    {
        .name = "hull/worker",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        .required_caps = 0, .deps = {0},
    },
    {
        .name = "hull/ws",
        .api_major = 1, .intrinsic = 0, .pure = 0,
        /* WebSocket server — app.ws() endpoint registration. */
        .required_caps = HL_MOD_CAP_HTTP_SERVER, .deps = {0},
    },
};

#define REGISTRY_COUNT (sizeof(REGISTRY) / sizeof(REGISTRY[0]))

/* ── Binary search by name ─────────────────────────────────────────── */

const HlModuleSpec *hl_module_registry_find(const char *name)
{
    if (!name) return NULL;

    size_t lo = 0, hi = REGISTRY_COUNT;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(name, REGISTRY[mid].name);
        if (cmp == 0) return &REGISTRY[mid];
        if (cmp < 0)  hi = mid;
        else          lo = mid + 1;
    }
    return NULL;
}

const HlModuleSpec *hl_module_registry_find_short(const char *short_name)
{
    if (!short_name) return NULL;

    /* Names that already start with the "hull/" canonical prefix are
     * looked up as-is. Anything else — including names with embedded
     * slashes like "middleware/session" — is treated as a short alias
     * inside the hull/ namespace and gets the prefix prepended.
     *
     * Once third-party vendors are supported the rule will need to be
     * "starts with any recognized vendor prefix"; for v1, hull/ is the
     * only first-party namespace. */
    if (strncmp(short_name, "hull/", 5) == 0)
        return hl_module_registry_find(short_name);

    char buf[HL_MODULE_NAME_MAX];
    size_t n = strlen(short_name);
    if (n + 5 + 1 > sizeof(buf)) return NULL;
    memcpy(buf, "hull/", 5);
    memcpy(buf + 5, short_name, n + 1);
    return hl_module_registry_find(buf);
}

const HlModuleSpec *hl_module_registry_find_runtime(const char *runtime_name,
                                                     char separator)
{
    if (!runtime_name) return NULL;
    if (separator == '/' || separator == '\0')
        return hl_module_registry_find(runtime_name);

    /* Convert in-place into a local buffer: every `separator` becomes
     * '/'. Bail if the name overflows our small canonical-name budget. */
    char buf[HL_MODULE_NAME_MAX];
    size_t n = strlen(runtime_name);
    if (n + 1 > sizeof(buf)) return NULL;
    for (size_t i = 0; i < n; i++)
        buf[i] = (runtime_name[i] == separator) ? '/' : runtime_name[i];
    buf[n] = '\0';
    return hl_module_registry_find(buf);
}

const HlModuleSpec *hl_module_registry_all(size_t *out_total)
{
    if (out_total) *out_total = REGISTRY_COUNT;
    return REGISTRY;
}

int hl_module_registry_index(const HlModuleSpec *spec)
{
    if (!spec) return -1;
    if (spec < &REGISTRY[0] || spec >= &REGISTRY[REGISTRY_COUNT]) return -1;
    return (int)(spec - &REGISTRY[0]);
}

size_t hl_module_registry_count(void)
{
    return REGISTRY_COUNT;
}

void hl_module_registry_format_deps(const HlModuleSpec *spec,
                                     char *out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!spec) return;

    size_t pos = 0;
    int first = 1;
    for (int i = 0; i < HL_MODULE_MAX_DEPS && spec->deps[i]; i++) {
        const char *name = spec->deps[i];
        /* Strip "hull/" prefix in the short form: easier to type back. */
        if (strncmp(name, "hull/", 5) == 0) name += 5;
        const char *sep = first ? "" : ", ";
        int n = snprintf(out + pos, cap - pos, "%s%s", sep, name);
        if (n < 0 || (size_t)n >= cap - pos) {
            /* Out of room — leave whatever fit, ensure NUL. */
            out[cap - 1] = '\0';
            return;
        }
        pos += (size_t)n;
        first = 0;
    }
}

/* ── Suggestion (Levenshtein) ───────────────────────────────────────── */

/* Longest registry name today is "middleware/idempotency" (22 chars) — pad
 * generously so future entries don't silently bust the bound. */
#define SUGGEST_MAX_LEN 64

/* Two-row Levenshtein. O(m*n) time, O(min(m,n)) extra space. */
static int levenshtein(const char *a, size_t alen,
                       const char *b, size_t blen)
{
    if (alen == 0) return (int)blen;
    if (blen == 0) return (int)alen;

    /* Swap so b is the shorter side — bounds the row to SUGGEST_MAX_LEN+1. */
    if (blen > alen) {
        const char *t = a; a = b; b = t;
        size_t tl = alen; alen = blen; blen = tl;
    }
    if (blen > SUGGEST_MAX_LEN) return (int)alen;  /* would overflow row */

    int prev[SUGGEST_MAX_LEN + 1];
    int curr[SUGGEST_MAX_LEN + 1];

    for (size_t j = 0; j <= blen; j++) prev[j] = (int)j;

    for (size_t i = 1; i <= alen; i++) {
        curr[0] = (int)i;
        for (size_t j = 1; j <= blen; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int del  = prev[j] + 1;
            int ins  = curr[j - 1] + 1;
            int sub  = prev[j - 1] + cost;
            int m    = del < ins ? del : ins;
            curr[j]  = m < sub ? m : sub;
        }
        for (size_t j = 0; j <= blen; j++) prev[j] = curr[j];
    }

    return prev[blen];
}

/* Normalize an input into a short form: strip "hull/", lowercase nothing
 * (we match case-sensitively — registry names are all lowercase). Output
 * buffer must be SUGGEST_MAX_LEN+1 bytes. Returns the strlen of out, or
 * SIZE_MAX if input is too long / NULL. */
static size_t normalize_input(const char *input, char *out)
{
    if (!input) return (size_t)-1;
    if (strncmp(input, "hull/", 5) == 0) input += 5;
    size_t len = strlen(input);
    if (len > SUGGEST_MAX_LEN) return (size_t)-1;
    memcpy(out, input, len);
    out[len] = '\0';
    return len;
}

const HlModuleSpec *hl_module_registry_suggest(const char *input)
{
    char norm[SUGGEST_MAX_LEN + 1];
    size_t in_len = normalize_input(input, norm);
    if (in_len == (size_t)-1 || in_len == 0) return NULL;

    /* Length-scaled threshold:
     *   ≤ 4 chars → 1 edit
     *   5–8       → 2 edits
     *   ≥ 9       → 3 edits
     * Avoids matching e.g. "fs" → "ws" on a single edit when the user
     * really did mean "fs"; and lets longer names ("middleware/sesssion")
     * tolerate the typo budget they need. */
    int threshold;
    if      (in_len <= 4) threshold = 1;
    else if (in_len <= 8) threshold = 2;
    else                  threshold = 3;

    int best_dist = threshold + 1;
    const HlModuleSpec *best = NULL;

    for (size_t i = 0; i < REGISTRY_COUNT; i++) {
        const char *cand = REGISTRY[i].name;
        if (strncmp(cand, "hull/", 5) == 0) cand += 5;
        size_t clen = strlen(cand);

        /* Cheap reject: if the length difference alone exceeds the
         * threshold, no single-substitution/insert/delete budget can
         * bridge the gap. */
        size_t diff = (clen > in_len) ? (clen - in_len) : (in_len - clen);
        if ((int)diff > threshold) continue;

        int d = levenshtein(norm, in_len, cand, clen);
        if (d < best_dist) {
            best_dist = d;
            best = &REGISTRY[i];
            if (d == 0) break;  /* exact (shouldn't happen — caller checks) */
        }
    }

    return best;
}
