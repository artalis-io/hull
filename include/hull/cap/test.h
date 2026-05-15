/*
 * cap/test.h — In-process HTTP dispatch helper for hull test
 *
 * The cap layer here is pure C — it knows nothing about Lua or JS.
 * It exposes the shared types + dispatch primitive used by both
 * runtime test bindings (declared in hull/runtime/test.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_TEST_H
#define HL_CAP_TEST_H

#include <stddef.h>

/* Forward declarations */
typedef struct KlRouter KlRouter;
typedef struct HlAllocator HlAllocator;

/* ── Test result collection ─────────────────────────────────────────── */

/* Per-test result for structured (JSON) output */
typedef struct HlTestCaseResult {
    char name[256];
    int  passed;       /* 1 = pass, 0 = fail */
    char error[1024];  /* error message if failed, empty if passed */
} HlTestCaseResult;

/* ── Shared dispatch result ─────────────────────────────────────────── */

typedef struct {
    int status;
    const char *body;
    size_t body_len;
    const char *hdr_buf;
    size_t hdr_len;
} HlTestResult;

/*
 * Build a KlRequest on the stack, match a route, dispatch the handler,
 * and return response info. Used by both Lua and JS bindings.
 */
int hl_cap_test_dispatch(KlRouter *router, const char *method,
                         const char *path, const char *body_data,
                         size_t body_len, const char **header_names,
                         const char **header_values, int num_headers,
                         const char *ctx_json, HlAllocator *hl_alloc,
                         int run_middleware,
                         HlTestResult *result);

#endif /* HL_CAP_TEST_H */
