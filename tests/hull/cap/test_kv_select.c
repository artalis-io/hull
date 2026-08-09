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
#include <stddef.h>

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

UTEST_MAIN();
