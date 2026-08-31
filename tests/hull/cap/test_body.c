/*
 * test_hull_cap_body.c - Tests for body reader capability
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/body.h"
#include <keel/http_body_reader.h>
#include <keel/http_body_reader_multipart.h>
#include <keel/http_request.h>
#include <keel/allocator.h>
#include <string.h>

UTEST(hl_cap_body, null_reader_returns_zero)
{
    const char *data = (const char *)0x1; /* sentinel */
    size_t len = hl_cap_body_data(NULL, &data);
    ASSERT_EQ((size_t)0, len);
    ASSERT_EQ(NULL, data);
}

UTEST(hl_cap_body, fake_buf_reader_extracts_data)
{
    /* Construct a fake KlHttpBufReader with known data */
    KlHttpBufReader br;
    memset(&br, 0, sizeof(br));
    br.data = (char *)"hello";
    br.len = 5;

    const char *data;
    size_t len = hl_cap_body_data((KlHttpBodyReader *)&br, &data);
    ASSERT_EQ((size_t)5, len);
    ASSERT_EQ(0, memcmp(data, "hello", 5));
}

UTEST(hl_cap_body, empty_buf_reader_returns_zero)
{
    KlHttpBufReader br;
    memset(&br, 0, sizeof(br));
    br.data = NULL;
    br.len = 0;

    const char *data;
    size_t len = hl_cap_body_data((KlHttpBodyReader *)&br, &data);
    ASSERT_EQ((size_t)0, len);
}

/* ── Streaming multipart wrapper ──────────────────────────────────── */

/* Build a minimal fake request with a Content-Type header. */
static KlHttpRequest mp_make_req(const char *ct) {
    KlHttpRequest req = {0};
    req.method = "POST"; req.method_len = 4;
    req.path = "/upload"; req.path_len = 7;
    req.headers[0].name = "Content-Type";
    req.headers[0].name_len = 12;
    req.headers[0].value = ct;
    req.headers[0].value_len = strlen(ct);
    req.num_headers = 1;
    return req;
}

/* Per-test resume tracker. Counts callback invocations and records the
 * last reason seen. */
typedef struct {
    int                       calls;
    HlMultipartResumeReason   last_reason;
} ResumeTracker;

static void test_resume_cb(void *ctx, HlMultipartResumeReason reason) {
    ResumeTracker *t = ctx;
    t->calls++;
    t->last_reason = reason;
}

UTEST(hl_cap_multipart, factory_rejects_non_multipart_content_type) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = mp_make_req("application/json");
    KlHttpBodyReader *br = hl_cap_multipart_factory(&a, &req, NULL);
    ASSERT_TRUE(br == NULL);
}

UTEST(hl_cap_multipart, factory_rejects_null_alloc_or_req) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = mp_make_req("multipart/form-data; boundary=AB");
    ASSERT_TRUE(hl_cap_multipart_factory(NULL, &req, NULL) == NULL);
    ASSERT_TRUE(hl_cap_multipart_factory(&a, NULL, NULL) == NULL);
}

UTEST(hl_cap_multipart, factory_accepts_multipart_request) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = mp_make_req("multipart/form-data; boundary=AB");
    KlHttpBodyReader *br = hl_cap_multipart_factory(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    /* inner reader exposed for kl_http_multipart_next */
    KlHttpBodyReader *inner = hl_cap_multipart_inner(br);
    ASSERT_TRUE(inner != NULL);
    ASSERT_TRUE(inner != br);  /* wrapper != inner */

    br->destroy(br);
}

UTEST(hl_cap_multipart, park_fires_on_data) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = mp_make_req("multipart/form-data; boundary=AB");
    KlHttpBodyReader *br = hl_cap_multipart_factory(&a, &req, NULL);

    ResumeTracker t = {0};
    ASSERT_EQ(hl_cap_multipart_park(br, test_resume_cb, &t), 0);
    ASSERT_EQ(t.calls, 0);

    ASSERT_EQ(br->on_data(br, "--AB\r\n", 6), 0);
    ASSERT_EQ(t.calls, 1);
    ASSERT_EQ((int)t.last_reason, (int)HL_MP_RESUME_DATA);

    br->destroy(br);
}

UTEST(hl_cap_multipart, park_fires_on_complete) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = mp_make_req("multipart/form-data; boundary=AB");
    KlHttpBodyReader *br = hl_cap_multipart_factory(&a, &req, NULL);

    ResumeTracker t = {0};
    hl_cap_multipart_park(br, test_resume_cb, &t);

    br->on_complete(br);
    ASSERT_EQ(t.calls, 1);
    ASSERT_EQ((int)t.last_reason, (int)HL_MP_RESUME_DONE);

    br->destroy(br);
}

UTEST(hl_cap_multipart, park_fires_on_error) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = mp_make_req("multipart/form-data; boundary=AB");
    KlHttpBodyReader *br = hl_cap_multipart_factory(&a, &req, NULL);

    ResumeTracker t = {0};
    hl_cap_multipart_park(br, test_resume_cb, &t);

    br->on_error(br);
    ASSERT_EQ(t.calls, 1);
    ASSERT_EQ((int)t.last_reason, (int)HL_MP_RESUME_ERROR);

    br->destroy(br);
}

UTEST(hl_cap_multipart, late_park_after_done_fires_immediately) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = mp_make_req("multipart/form-data; boundary=AB");
    KlHttpBodyReader *br = hl_cap_multipart_factory(&a, &req, NULL);

    /* Stream ended with no parked handler. */
    br->on_complete(br);

    /* Park AFTER stream ended → should fire immediately. */
    ResumeTracker t = {0};
    ASSERT_EQ(hl_cap_multipart_park(br, test_resume_cb, &t), 0);
    ASSERT_EQ(t.calls, 1);
    ASSERT_EQ((int)t.last_reason, (int)HL_MP_RESUME_DONE);

    br->destroy(br);
}

UTEST(hl_cap_multipart, late_park_after_error_fires_immediately) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = mp_make_req("multipart/form-data; boundary=AB");
    KlHttpBodyReader *br = hl_cap_multipart_factory(&a, &req, NULL);

    br->on_error(br);

    ResumeTracker t = {0};
    ASSERT_EQ(hl_cap_multipart_park(br, test_resume_cb, &t), 0);
    ASSERT_EQ(t.calls, 1);
    ASSERT_EQ((int)t.last_reason, (int)HL_MP_RESUME_ERROR);

    br->destroy(br);
}

UTEST(hl_cap_multipart, error_takes_precedence_over_done_in_late_park) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = mp_make_req("multipart/form-data; boundary=AB");
    KlHttpBodyReader *br = hl_cap_multipart_factory(&a, &req, NULL);

    /* Both flags set: error THEN complete. */
    br->on_error(br);
    br->on_complete(br);

    ResumeTracker t = {0};
    hl_cap_multipart_park(br, test_resume_cb, &t);
    ASSERT_EQ(t.calls, 1);
    ASSERT_EQ((int)t.last_reason, (int)HL_MP_RESUME_ERROR);  /* not DONE */

    br->destroy(br);
}

UTEST(hl_cap_multipart, park_is_single_shot) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = mp_make_req("multipart/form-data; boundary=AB");
    KlHttpBodyReader *br = hl_cap_multipart_factory(&a, &req, NULL);

    ResumeTracker t = {0};
    hl_cap_multipart_park(br, test_resume_cb, &t);

    br->on_data(br, "--AB\r\n", 6);   /* fires once */
    ASSERT_EQ(t.calls, 1);

    /* Second on_data with no re-park should NOT fire. */
    br->on_data(br, "Content-Disposition: form-data; name=\"x\"\r\n", 42);
    ASSERT_EQ(t.calls, 1);  /* still 1 */

    /* Re-park. */
    hl_cap_multipart_park(br, test_resume_cb, &t);
    br->on_data(br, "\r\n", 2);
    ASSERT_EQ(t.calls, 2);

    br->destroy(br);
}

UTEST(hl_cap_multipart, park_null_unparks) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = mp_make_req("multipart/form-data; boundary=AB");
    KlHttpBodyReader *br = hl_cap_multipart_factory(&a, &req, NULL);

    ResumeTracker t = {0};
    hl_cap_multipart_park(br, test_resume_cb, &t);
    hl_cap_multipart_park(br, NULL, NULL);   /* clear */

    br->on_data(br, "--AB\r\n", 6);
    ASSERT_EQ(t.calls, 0);  /* should not fire */

    br->destroy(br);
}

/* Dummy vtable functions for the wrong-reader-kind tests. Building a
 * KlHttpBodyReader here in test-instrumented code (rather than via
 * kl_http_body_reader_buffer) avoids MSan false positives: keel is built
 * without MSan instrumentation in the msan target, so reads of vtable
 * bytes written by libkeel.a code would trip use-of-uninitialized-
 * value warnings. Constructing the struct in this file ensures the
 * shadow bytes are properly tracked. */
static int   dummy_on_data(KlHttpBodyReader *s, const char *d, size_t n) {
    (void)s; (void)d; (void)n; return 0;
}
static void  dummy_on_complete(KlHttpBodyReader *s) { (void)s; }
static void  dummy_on_error(KlHttpBodyReader *s)    { (void)s; }
static void  dummy_destroy(KlHttpBodyReader *s)     { (void)s; }

UTEST(hl_cap_multipart, inner_returns_NULL_for_wrong_reader_kind) {
    /* Pass a non-multipart reader to hl_cap_multipart_inner - must
     * safely return NULL via the vtable-identity check on on_data. */
    KlHttpBodyReader fake = {
        .on_data     = dummy_on_data,
        .on_complete = dummy_on_complete,
        .on_error    = dummy_on_error,
        .destroy     = dummy_destroy,
    };
    ASSERT_TRUE(hl_cap_multipart_inner(&fake) == NULL);
}

UTEST(hl_cap_multipart, park_returns_error_for_wrong_reader_kind) {
    KlHttpBodyReader fake = {
        .on_data     = dummy_on_data,
        .on_complete = dummy_on_complete,
        .on_error    = dummy_on_error,
        .destroy     = dummy_destroy,
    };
    ResumeTracker t = {0};
    ASSERT_EQ(hl_cap_multipart_park(&fake, test_resume_cb, &t), -1);
    ASSERT_EQ(t.calls, 0);
}

/* End-to-end test that drives kl_http_multipart_next directly. SKIPPED
 * under MemorySanitizer: keel is built without -fsanitize=memory in
 * Hull's msan target, so reads of kl_http_multipart_next's return value
 * (and the keel-populated meta/data slots) are flagged as
 * use-of-uninitialized-value - MSan didn't see the writes. The
 * wrapper's forward-to-inner correctness is still verified by the
 * other tests (park_fires_on_data, park_is_single_shot, etc.) which
 * exercise the on_data/on_complete/on_error paths without
 * propagating keel-typed values into instrumented assertions.
 *
 * `__has_feature` is a clang builtin; GCC doesn't define it. The
 * fallback below makes the guard parse cleanly on both. */
#ifndef __has_feature
#  define __has_feature(x) 0
#endif
#if !__has_feature(memory_sanitizer)
UTEST(hl_cap_multipart, inner_drives_kl_multipart_next_normally) {
    KlAllocator a = kl_allocator_default();
    KlHttpRequest req = mp_make_req("multipart/form-data; boundary=AB");
    KlHttpBodyReader *br = hl_cap_multipart_factory(&a, &req, NULL);
    ASSERT_TRUE(br != NULL);

    const char *body =
        "--AB\r\n"
        "Content-Disposition: form-data; name=\"f\"\r\n\r\n"
        "hello\r\n"
        "--AB--\r\n";
    ASSERT_EQ(br->on_data(br, body, strlen(body)), 0);
    br->on_complete(br);

    KlHttpBodyReader *inner = hl_cap_multipart_inner(br);
    ASSERT_TRUE(inner != NULL);

    KlHttpMultipartPartMeta meta;
    const char *d = NULL;
    size_t      dn = 0;
    ASSERT_EQ((int)kl_http_multipart_next(inner, &meta, &d, &dn),
              (int)KL_HTTP_MP_EVT_PART_BEGIN);
    ASSERT_EQ(meta.name_len, (size_t)1);
    ASSERT_EQ(meta.name[0], 'f');

    ASSERT_EQ((int)kl_http_multipart_next(inner, &meta, &d, &dn),
              (int)KL_HTTP_MP_EVT_PART_DATA);
    ASSERT_EQ(dn, (size_t)5);
    ASSERT_TRUE(memcmp(d, "hello", 5) == 0);

    ASSERT_EQ((int)kl_http_multipart_next(inner, &meta, &d, &dn),
              (int)KL_HTTP_MP_EVT_PART_END);
    ASSERT_EQ((int)kl_http_multipart_next(inner, &meta, &d, &dn),
              (int)KL_HTTP_MP_EVT_DONE);

    br->destroy(br);
}
#endif

UTEST_MAIN();
