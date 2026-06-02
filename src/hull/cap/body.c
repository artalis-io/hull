/*
 * hull_cap_body.c — Body reader factory and extraction for Hull runtimes
 *
 * Wraps Keel's kl_body_reader_buffer with a 1 MB limit.
 * Both JS and Lua bindings use hl_cap_body_data() to extract
 * the buffered body after on_complete fires.
 *
 * Also provides hl_cap_multipart_factory — a streaming wrapper around
 * Keel's kl_body_reader_multipart that holds a parked-handler slot.
 * Used by routes registered with kl_server_route_streaming so the
 * handler can yield on NEED_DATA and be resumed by on_data callbacks.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/body.h"
#include "hull/limits/core.h"
#include <keel/body_reader_multipart.h>
#include <stddef.h>
#include <string.h>

KlBodyReader *hl_cap_body_factory(KlAllocator *alloc, const KlRequest *req,
                                  void *user_data)
{
    (void)user_data;
    return kl_body_reader_buffer(alloc, req,
                                 (void *)(size_t)HL_BODY_MAX_SIZE);
}

size_t hl_cap_body_data(const KlBodyReader *reader, const char **out_data)
{
    if (!out_data)
        return 0;
    if (!reader) {
        *out_data = NULL;
        return 0;
    }
    const KlBufReader *br = (const KlBufReader *)reader;
    *out_data = br->data;
    return br->len;
}

/* ── Streaming multipart wrapper ─────────────────────────────────── */

/*
 * Wrapper struct: holds the inner Keel multipart reader plus a
 * single-shot parked-handler callback. The wrapper IS a KlBodyReader
 * (so Keel can pump on_data into it); the inner reader is owned by
 * the wrapper and freed on destroy.
 */
typedef struct {
    KlBodyReader        base;
    KlAllocator        *alloc;
    KlBodyReader       *inner;       /* kl_body_reader_multipart */
    HlMultipartResumeFn on_resume;   /* parked handler — NULL = none */
    void               *resume_ctx;  /* opaque ctx for on_resume */
    int                 stream_ended;
    int                 errored;
} HlMultipartWrapper;

static void mp_fire_park(HlMultipartWrapper *w, HlMultipartResumeReason reason)
{
    /* Single-shot: clear the registration BEFORE invoking so the
     * handler can re-park from inside the resume callback. */
    HlMultipartResumeFn fn = w->on_resume;
    void *ctx = w->resume_ctx;
    if (!fn) return;
    w->on_resume = NULL;
    w->resume_ctx = NULL;
    fn(ctx, reason);
}

static int mp_wrap_on_data(KlBodyReader *self, const char *data, size_t len)
{
    HlMultipartWrapper *w = (HlMultipartWrapper *)self;
    int rc = w->inner->on_data(w->inner, data, len);
    if (rc < 0) {
        w->errored = 1;
        mp_fire_park(w, HL_MP_RESUME_ERROR);
        return rc;
    }
    /* Resume the handler — it can pull whatever fresh events the new
     * bytes made available. */
    mp_fire_park(w, HL_MP_RESUME_DATA);
    return 0;
}

static void mp_wrap_on_complete(KlBodyReader *self)
{
    HlMultipartWrapper *w = (HlMultipartWrapper *)self;
    w->inner->on_complete(w->inner);
    w->stream_ended = 1;
    mp_fire_park(w, HL_MP_RESUME_DONE);
}

static void mp_wrap_on_error(KlBodyReader *self)
{
    HlMultipartWrapper *w = (HlMultipartWrapper *)self;
    w->inner->on_error(w->inner);
    w->errored = 1;
    mp_fire_park(w, HL_MP_RESUME_ERROR);
}

static void mp_wrap_destroy(KlBodyReader *self)
{
    HlMultipartWrapper *w = (HlMultipartWrapper *)self;
    if (w->inner) w->inner->destroy(w->inner);
    kl_free(w->alloc, w, sizeof(*w));
}

KlBodyReader *hl_cap_multipart_factory(KlAllocator *alloc,
                                       const KlRequest *req,
                                       void *user_data)
{
    if (!alloc || !req) return NULL;

    KlBodyReader *inner = kl_body_reader_multipart(alloc, req, user_data);
    if (!inner) return NULL;

    HlMultipartWrapper *w = kl_malloc(alloc, sizeof(*w));
    if (!w) {
        inner->destroy(inner);
        return NULL;
    }
    memset(w, 0, sizeof(*w));

    w->base.on_data     = mp_wrap_on_data;
    w->base.on_complete = mp_wrap_on_complete;
    w->base.on_error    = mp_wrap_on_error;
    w->base.destroy     = mp_wrap_destroy;
    w->alloc            = alloc;
    w->inner            = inner;

    return &w->base;
}

KlBodyReader *hl_cap_multipart_inner(KlBodyReader *wrapper)
{
    if (!wrapper || wrapper->on_data != mp_wrap_on_data) return NULL;
    return ((HlMultipartWrapper *)wrapper)->inner;
}

int hl_cap_multipart_park(KlBodyReader *wrapper,
                          HlMultipartResumeFn on_resume,
                          void *ctx)
{
    if (!wrapper || wrapper->on_data != mp_wrap_on_data) return -1;
    HlMultipartWrapper *w = (HlMultipartWrapper *)wrapper;

    /* If the stream is already done or errored, fire immediately —
     * the caller's "wait for more data" assumption can't be satisfied. */
    if (on_resume) {
        if (w->errored)      { on_resume(ctx, HL_MP_RESUME_ERROR); return 0; }
        if (w->stream_ended) { on_resume(ctx, HL_MP_RESUME_DONE);  return 0; }
    }
    w->on_resume  = on_resume;
    w->resume_ctx = ctx;
    return 0;
}
