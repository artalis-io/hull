/*
 * test_js_generation.c - Slice 6: the C-owned JS frontend generation/session manager.
 * Proves monotonic tokens, operation-specific stale shapes, the old-token/new-session ABA case,
 * and shutdown-preserves-next_token. See docs/js_frontend_slice6_dispatcher.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "hull/frontend/js_generation.h"

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

static int has(const char *hay, const char *needle) { return hay && strstr(hay, needle) != NULL; }
static char *an(int64_t t, const char *src) { char *o = NULL; size_t l = 0; hl_js_gen_analyze(t, (const uint8_t *)src, strlen(src), "a.js", &o, &l); return o; }
static char *sem(int64_t t, int64_t d) { char *o = NULL; size_t l = 0; hl_js_gen_declaration_semantics(t, d, &o, &l); return o; }
static char *scp(int64_t t, int64_t u) { char *o = NULL; size_t l = 0; hl_js_gen_scope(t, u, &o, &l); return o; }

UTEST(js_generation, available) { EXPECT_TRUE(hl_js_gen_available()); }

/* Open issues a positive token; analyze/semantics/scope on it work; close destroys it. */
UTEST(js_generation, open_analyze_close)
{
    int64_t t = hl_js_gen_open();
    ASSERT_TRUE(t > 0);
    char *a = an(t, "const q = bar();");
    EXPECT_TRUE(has(a, "\"status\":\"analyzed\""));
    EXPECT_TRUE(has(a, "\"name\":\"q\""));
    free(a);
    char *s = sem(t, 1);
    EXPECT_TRUE(has(s, "\"form\":\"value\""));
    free(s);
    char *sc = scp(t, 1);
    EXPECT_TRUE(has(sc, "\"ok\":true"));
    free(sc);
    hl_js_gen_close(t);
}

/* Tokens are strictly monotonic across open/close. */
UTEST(js_generation, monotonic_tokens)
{
    int64_t a = hl_js_gen_open(); int64_t b = hl_js_gen_open();
    EXPECT_TRUE(b > a);
    hl_js_gen_close(a);
    int64_t c = hl_js_gen_open();
    EXPECT_TRUE(c > b);                 /* the closed token a is NOT reissued */
    hl_js_gen_close(b);
    hl_js_gen_close(c);
}

/* A stale/unknown/<=0 token yields the OPERATION-SPECIFIC stale shape, never a crash. */
UTEST(js_generation, stale_shapes)
{
    char *a = an(999999, "const x = 1;");
    EXPECT_TRUE(has(a, "\"status\":\"error\"") && has(a, "\"unit_id\":-1") && has(a, "stale frontend session"));
    free(a);
    char *s = sem(999999, 1);
    EXPECT_TRUE(has(s, "\"error\":{") && has(s, "stale frontend session"));
    EXPECT_FALSE(has(s, "\"form\":"));
    free(s);
    char *sc = scp(999999, 1);
    EXPECT_TRUE(has(sc, "\"ok\":false") && has(sc, "\"error\":{"));
    free(sc);
    char *z = an(0, "const x = 1;");    /* token 0 always invalid */
    EXPECT_TRUE(has(z, "stale frontend session"));
    free(z);
    char *neg = sem(-5, 1);
    EXPECT_TRUE(has(neg, "stale frontend session"));
    free(neg);
}

/* The old-token/new-session ABA case: session A issues decl_id 1; A closed; session B reuses the
 * (session-relative) decl_id 1; the OLD token + 1 is rejected as stale, the LIVE token resolves. */
UTEST(js_generation, aba_old_token_rejected)
{
    int64_t A = hl_js_gen_open();
    ASSERT_TRUE(A > 0);
    char *a = an(A, "const z = 1;"); free(a);       /* A: decl_id 1 */
    hl_js_gen_close(A);
    int64_t B = hl_js_gen_open();
    ASSERT_TRUE(B > A);                              /* B strictly higher, may reuse A's heap slot */
    char *b = an(B, "const w = 2;"); free(b);        /* B: decl_id 1 (session-relative) */
    char *staleA = sem(A, 1);                        /* OLD token + reused decl_id -> STALE */
    EXPECT_TRUE(has(staleA, "stale frontend session"));
    EXPECT_FALSE(has(staleA, "\"form\":"));
    free(staleA);
    char *liveB = sem(B, 1);                         /* LIVE token -> resolves */
    EXPECT_TRUE(has(liveB, "\"form\":\"value\""));
    free(liveB);
    hl_js_gen_close(B);
}

/* Shutdown destroys live sessions but MUST NOT reset next_token (else ABA within the process). */
UTEST(js_generation, shutdown_preserves_next_token)
{
    int64_t a = hl_js_gen_open();       /* intentionally NOT closed - shutdown must reap it */
    ASSERT_TRUE(a > 0);
    hl_js_gen_shutdown();
    int64_t b = hl_js_gen_open();
    EXPECT_TRUE(b > a);                 /* counter preserved across shutdown */
    hl_js_gen_close(b);
}

/* The concurrent-generation cap fails closed. */
UTEST(js_generation, concurrency_cap)
{
    int64_t held[16]; int n = 0;
    for (int i = 0; i < 16; i++) { int64_t t = hl_js_gen_open(); if (t > 0) held[n++] = t; else break; }
    EXPECT_TRUE(n >= 1 && n <= 8);      /* capped at HL_JS_GEN_MAX (8) */
    int64_t over = hl_js_gen_open();
    EXPECT_TRUE(over == 0);             /* one past the cap fails closed */
    for (int i = 0; i < n; i++) hl_js_gen_close(held[i]);
}

UTEST_MAIN()
