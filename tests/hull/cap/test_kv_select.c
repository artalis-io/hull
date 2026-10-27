/*
 * test_kv_select.c: hl_kv_backend_select() DSN-scheme routing + the NULL guard.
 *
 * Independently unit-covers the selector in cap/kv_feature.c - in particular the
 * `bs && bs[i]` guard whose static-analysis false positive is inline-suppressed
 * there (cppcheck sees only the weak default; the guard is live for a composed
 * --with=valkey strong override). This test provides its OWN strong override of
 * hl_kv_feature_backends returning a test-controlled table, so it can drive every
 * guard branch from one binary:
 *   - bs == NULL, n == 0  (the weak-default shape) -> NULL
 *   - bs == NULL, n  > 0  (malformed override)     -> NULL, NOT a deref
 *   - a NULL entry in the table                    -> skipped
 *   - scheme match / miss / lowercasing / bad DSN
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/kv_backend.h"
#include "hull/cap/kv.h"
#include <stddef.h>
#include <string.h>

/* Strong override of the base-resident weak hook (cap/kv_feature.c). Returns a
 * test-controlled table so each case sets the shape the selector sees. */
static const HlKvBackend *const *g_bs;
static size_t g_n;
const HlKvBackend *const *hl_kv_feature_backends(size_t *count) {
    if (count) *count = g_n;
    return g_bs;
}

static const char *const REDIS_SCHEMES[] = { "redis", "valkey", NULL };
static const HlKvBackend FAKE = { .name = "fake", .schemes = REDIS_SCHEMES };

UTEST(kv_select, no_backend_returns_null) {
    g_bs = NULL; g_n = 0;                                  /* weak-default shape */
    ASSERT_TRUE(hl_kv_backend_select("redis://h:6379") == NULL);
    /* Malformed strong override (NULL table but n>0): the `bs &&` guard must
     * fail closed, never dereference a NULL table. */
    g_bs = NULL; g_n = 3;
    ASSERT_TRUE(hl_kv_backend_select("redis://h") == NULL);
}

UTEST(kv_select, matches_by_scheme) {
    static const HlKvBackend *const arr[] = { &FAKE };
    g_bs = arr; g_n = 1;
    ASSERT_TRUE(hl_kv_backend_select("redis://h:6379")   == &FAKE);
    ASSERT_TRUE(hl_kv_backend_select("valkey://h")        == &FAKE);
    ASSERT_TRUE(hl_kv_backend_select("REDIS://h")         == &FAKE);  /* lowercased */
    ASSERT_TRUE(hl_kv_backend_select("mysql://h")         == NULL);   /* scheme miss */
    ASSERT_TRUE(hl_kv_backend_select("rediss://h")        == NULL);   /* not in table's schemes */
}

UTEST(kv_select, skips_null_entries) {
    static const HlKvBackend *const arr[] = { NULL, &FAKE };
    g_bs = arr; g_n = 2;                                   /* bs[i]==NULL guard */
    ASSERT_TRUE(hl_kv_backend_select("valkey://h") == &FAKE);
}

UTEST(kv_select, bad_dsn_returns_null) {
    static const HlKvBackend *const arr[] = { &FAKE };
    g_bs = arr; g_n = 1;
    ASSERT_TRUE(hl_kv_backend_select(NULL)        == NULL);
    ASSERT_TRUE(hl_kv_backend_select("")          == NULL);
    ASSERT_TRUE(hl_kv_backend_select("noscheme")  == NULL);   /* no "://" */
    ASSERT_TRUE(hl_kv_backend_select("://h")       == NULL);   /* empty scheme */
}

/* Audit 30: live KV connections are capped (HL_KV_OPEN_MAX), a refused open
 * dials nothing, close and a failed open give the slot back, and every op but
 * get() trims the backend's receive buffer before it returns. */
static int g_opens, g_closes, g_trims, g_fail_open;
static int fake_open(HlKvHandle **out, const char *dsn, int timeout_ms,
                     HlAllocator *alloc, char *errbuf, size_t errlen) {
    (void)dsn; (void)timeout_ms; (void)alloc;
    if (g_fail_open) {
        if (errbuf && errlen) errbuf[0] = '\0';
        return -1;
    }
    static int token;
    *out = (HlKvHandle *)&token;
    g_opens++;
    return 0;
}
static void fake_close(HlKvHandle *h) { (void)h; g_closes++; }
static void fake_trim(HlKvHandle *h) { (void)h; g_trims++; }
static int fake_set(HlKvHandle *h, const uint8_t *k, size_t kl,
                    const uint8_t *v, size_t vl, int64_t ttl) {
    (void)h; (void)k; (void)kl; (void)v; (void)vl; (void)ttl;
    return 0;
}
static int fake_get(HlKvHandle *h, const uint8_t *k, size_t kl,
                    const uint8_t **v, size_t *vl, int *found) {
    (void)h; (void)k; (void)kl;
    *v = (const uint8_t *)"x"; *vl = 1; *found = 1;
    return 0;
}
static const HlKvBackend OPENABLE = {
    .name = "fake", .schemes = REDIS_SCHEMES,
    .open = fake_open, .close = fake_close,
    .get = fake_get, .set = fake_set, .trim = fake_trim,
};

UTEST(kv_cap, live_connections_are_capped) {
    static const HlKvBackend *const arr[] = { &OPENABLE };
    g_bs = arr; g_n = 1;
    g_opens = g_closes = g_trims = g_fail_open = 0;
    HlKvConn *c[HL_KV_OPEN_MAX + 1];
    char err[256];
    for (int i = 0; i < HL_KV_OPEN_MAX; i++)
        ASSERT_EQ(0, hl_cap_kv_open(&c[i], "redis://h", 0, NULL, err, sizeof err));

    err[0] = '\0';
    ASSERT_EQ(HL_KV_ERR_TOO_MANY,
              hl_cap_kv_open(&c[HL_KV_OPEN_MAX], "redis://h", 0, NULL, err, sizeof err));
    ASSERT_TRUE(c[HL_KV_OPEN_MAX] == NULL);
    ASSERT_TRUE(strstr(err, "too many open connections") != NULL);
    ASSERT_EQ(HL_KV_OPEN_MAX, g_opens);            /* the refused one dialed nothing */

    /* A close frees a slot; a failed open does not keep one. */
    hl_cap_kv_close(c[0]);
    g_fail_open = 1;
    ASSERT_EQ(-1, hl_cap_kv_open(&c[0], "redis://h", 0, NULL, err, sizeof err));
    g_fail_open = 0;
    ASSERT_EQ(0, hl_cap_kv_open(&c[0], "redis://h", 0, NULL, err, sizeof err));

    /* set() trims on its own; get() leaves it to the caller's release. */
    ASSERT_EQ(0, hl_cap_kv_set(c[0], (const uint8_t *)"k", 1, (const uint8_t *)"v", 1, 0));
    ASSERT_EQ(1, g_trims);
    const uint8_t *v; size_t vl; int found = 0;
    ASSERT_EQ(0, hl_cap_kv_get(c[0], (const uint8_t *)"k", 1, &v, &vl, &found));
    ASSERT_EQ(1, g_trims);
    hl_cap_kv_release(c[0]);
    ASSERT_EQ(2, g_trims);

    for (int i = 0; i < HL_KV_OPEN_MAX; i++) hl_cap_kv_close(c[i]);
    ASSERT_EQ(HL_KV_OPEN_MAX + 1, g_closes);
    ASSERT_EQ(0, hl_cap_kv_open(&c[0], "redis://h", 0, NULL, err, sizeof err));
    hl_cap_kv_close(c[0]);
}

UTEST_MAIN();
