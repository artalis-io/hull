/*
 * mod_request.c - JS bindings for streaming-multipart request bodies
 *
 * Mirrors src/hull/runtime/lua/mod_request.c for QuickJS. Routes that
 * declared `{ multipart: {...} }` get `req.multipart()` returning an
 * async iterator:
 *
 *   app.post("/upload", async (req, res) => {
 *       for await (const part of req.multipart()) {
 *           if (part.filename) {
 *               for await (const chunk of part.chunks()) {
 *                   ...
 *               }
 *           } else {
 *               const value = await part.read();
 *           }
 *       }
 *       res.json({ ok: true });
 *   }, { multipart: { maxPartSize: 64 * 1024 * 1024 } });
 *
 * Lifecycle of one iter.next() (or chunks.next() / part.read()) call:
 *
 *   1. Drive kl_multipart_next() against the parkable wrapper. If the
 *      parser emits a synchronous event (PART_BEGIN / PART_DATA /
 *      PART_END / DONE / ERROR), build the {value, done} object and
 *      return a resolved (or rejected) Promise immediately.
 *   2. On NEED_DATA, allocate a custom HlJsMpCont, stash resolve/reject,
 *      park the cont's resume as the body wrapper's wake callback, set
 *      c->state = KL_CONN_READING_BODY so Keel keeps reading socket
 *      bytes, return the pending Promise.
 *   3. When on_data fires inside Keel's read loop, the park callback -
 *      which IS the cont's resume - runs. It re-drives the parser. If
 *      still NEED_DATA, re-parks (the cont survives). Otherwise it
 *      resolves / rejects the iter Promise, drains microtasks (which
 *      lets the for-await loop continue), checks the outer handler
 *      Promise, and either transfers it to the next cont (PENDING),
 *      cleans up + sets c->state = SENDING (FULFILLED), or writes a
 *      500 + cleans up (REJECTED).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/runtime/js.h"
#include "hull/utils/alloc.h"
#include "hull/shared/async.h"
#include "hull/cap/body.h"

#include <keel/body_reader.h>
#include <keel/body_reader_multipart.h>
#include <keel/connection.h>
#include <keel/request.h>

#include "quickjs.h"

#include <stdlib.h>
#include <string.h>

/* ── Forward declarations (cross-runtime helpers) ────────────────────── */

/* Defined in async.c. Used by mp_js_pump to transfer the outer
 * handler-Promise to whichever cont type the handler creates next
 * (could be our own HlJsMpCont or a standard HlJsAsyncCont - the
 * dispatcher routes through the cont's set_handler_promise vtable
 * slot, so this file doesn't have to know which type). */
extern void hl_js_async_cont_set_handler_promise(HlAsyncCont *cont,
                                                  JSContext *ctx,
                                                  JSValue promise);

/* ── Class IDs (registered once during runtime init) ─────────────────── */

static JSClassID hl_mp_iter_class_id;
static JSClassID hl_mp_part_class_id;
static JSClassID hl_mp_chunks_class_id;

/* ── Iterator state ─────────────────────────────────────────────────── */

/*
 * Shared parser-driver state. The iter, all Part JSValues that came out
 * of it, and any Chunks JSValues spawned off a Part all point at the
 * SAME HlJsMpIter - they're views into one in-flight parse.
 *
 * Lifetime: ref-counted. Each holder calls hl_mp_iter_ref on construction
 * and hl_mp_iter_unref on finalize. The last unref frees the meta copies
 * and the struct itself. The wrapper (KlBodyReader) is owned by Keel and
 * outlives all of this.
 */
typedef struct HlJsMpIter {
    int           refs;        /* iter object + each live Part + each live Chunks */
    KlBodyReader *wrapper;
    KlBodyReader *inner;
    HlJS         *js;
    HlAllocator  *alloc;

    int           done;        /* parser returned DONE */
    int           errored;     /* parser returned ERROR */
    int           in_part;     /* between PART_BEGIN..PART_END for the active part */

    /* Snapshot of the active Part's metadata. The borrowed pointers
     * returned by kl_multipart_next() are valid only until the next
     * mutation, so we copy bytes on PART_BEGIN. Cleared+rewritten on
     * each subsequent PART_BEGIN. */
    char         *name;          size_t name_len;
    char         *filename;      size_t filename_len;
    char         *content_type;  size_t content_type_len;
} HlJsMpIter;

/* Part view: opaque ID for the user, plus a pointer into iter for
 * reading meta + draining body via read() / chunks(). */
typedef struct {
    HlJsMpIter *iter;
    int         spent;          /* PART_END reached for this Part */
} HlJsMpPart;

/* Chunks-iter view: pumps PART_DATA events out of iter for one part.
 *
 * Note: we DON'T hold a pointer back to HlJsMpPart. Part could be GC'd
 * while Chunks is still alive (e.g. user stashed Chunks outside the
 * for-await loop), and the resulting use-after-free would be silent.
 * The chunks iterator stops on `ended` (set when PART_END / DONE was
 * observed for the part it was reading) or on `!iter->in_part` (the
 * outer for-await advanced past the part - see the Part-lifetime note
 * in the docstring at the top of this file).
 */
typedef struct {
    HlJsMpIter *iter;
    int         ended;          /* PART_END consumed; further next() = done */
} HlJsMpChunks;

/* ── Continuation type ──────────────────────────────────────────────── */

/*
 * Custom continuation for the multipart pump. Implements HlAsyncCont so
 * dispatch can hl_js_async_cont_set_handler_promise on it, but the
 * resume function is our own - it pumps the parser, resolves the iter
 * promise, runs microtasks, and handles handler-promise transitions.
 */
typedef enum {
    MP_MODE_ITER = 0,    /* iter.next() awaiting next Part / done */
    MP_MODE_CHUNKS,      /* chunks.next() awaiting next chunk / part end */
    MP_MODE_READ,        /* part.read() awaiting full body of one part */
} MpMode;

typedef struct HlJsMpCont {
    HlAsyncCont   base;           /* resume = mp_js_pump */
    HlJS         *js;
    HlAllocator  *alloc;
    JSValue       resolve;        /* of the iter.next/chunks.next/read Promise */
    JSValue       reject;
    JSValue       handler_promise;/* the OUTER (handler) Promise, set by dispatch */
    KlConn       *conn;
    HlJsMpIter   *iter;           /* not owned; ref-bumped at construction */
    MpMode        mode;
    /* MP_MODE_READ accumulator: grows as PART_DATA events arrive across
     * park-resume cycles. Built from the runtime allocator so the
     * runtime's memory cap applies. */
    char         *read_buf;
    size_t        read_len;
    size_t        read_cap;
    /* MP_MODE_CHUNKS state - the chunks iter (for ended-flag updates). */
    HlJsMpChunks *chunks;
    /* MP_MODE_READ state - the Part that owns this read pump (so we can
     * mark p->spent on completion). NOT used by MP_MODE_CHUNKS - see the
     * note on HlJsMpChunks. */
    HlJsMpPart   *part;
} HlJsMpCont;

/* ── Helpers ────────────────────────────────────────────────────────── */

static void hl_mp_iter_unref(HlJsMpIter *it);

static HlJsMpIter *hl_mp_iter_ref(HlJsMpIter *it)
{
    if (it) it->refs++;
    return it;
}

static void hl_mp_iter_clear_meta(HlJsMpIter *it)
{
    if (it->name)         { hl_alloc_free(it->alloc, it->name,         it->name_len);         it->name = NULL;         it->name_len = 0; }
    if (it->filename)     { hl_alloc_free(it->alloc, it->filename,     it->filename_len);     it->filename = NULL;     it->filename_len = 0; }
    if (it->content_type) { hl_alloc_free(it->alloc, it->content_type, it->content_type_len); it->content_type = NULL; it->content_type_len = 0; }
}

static void hl_mp_iter_unref(HlJsMpIter *it)
{
    if (!it) return;
    if (--it->refs > 0) return;
    hl_mp_iter_clear_meta(it);
    hl_alloc_free(it->alloc, it, sizeof(*it));
}

static int hl_mp_iter_copy_meta(HlJsMpIter *it, const KlMultipartPartMeta *meta)
{
    hl_mp_iter_clear_meta(it);
    if (meta->name && meta->name_len > 0) {
        it->name = hl_alloc_malloc(it->alloc, meta->name_len);
        if (!it->name) { it->errored = 1; return -1; }
        memcpy(it->name, meta->name, meta->name_len);
        it->name_len = meta->name_len;
    }
    if (meta->filename && meta->filename_len > 0) {
        it->filename = hl_alloc_malloc(it->alloc, meta->filename_len);
        if (!it->filename) { it->errored = 1; return -1; }
        memcpy(it->filename, meta->filename, meta->filename_len);
        it->filename_len = meta->filename_len;
    }
    if (meta->content_type && meta->content_type_len > 0) {
        it->content_type = hl_alloc_malloc(it->alloc, meta->content_type_len);
        if (!it->content_type) { it->errored = 1; return -1; }
        memcpy(it->content_type, meta->content_type, meta->content_type_len);
        it->content_type_len = meta->content_type_len;
    }
    return 0;
}

/* Grow the MP_MODE_READ accumulator to fit at least `add` more bytes.
 * Doubles capacity to amortize. Returns 0 on success, -1 on OOM. */
static int read_buf_append(HlJsMpCont *jc, const char *data, size_t add)
{
    if (add == 0) return 0;
    if (jc->read_len + add > SIZE_MAX / 2) return -1;
    if (jc->read_len + add > jc->read_cap) {
        size_t new_cap = jc->read_cap ? jc->read_cap * 2 : 1024;
        while (new_cap < jc->read_len + add) {
            if (new_cap > SIZE_MAX / 2) return -1;
            new_cap *= 2;
        }
        char *nb = hl_alloc_realloc(jc->alloc, jc->read_buf,
                                     jc->read_cap, new_cap);
        if (!nb) return -1;
        jc->read_buf = nb;
        jc->read_cap = new_cap;
    }
    memcpy(jc->read_buf + jc->read_len, data, add);
    jc->read_len += add;
    return 0;
}

/* Build a {value, done} iter-result object. Consumes `value`. */
static JSValue make_iter_result(JSContext *ctx, JSValue value, int done)
{
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) { JS_FreeValue(ctx, value); return obj; }
    JS_SetPropertyStr(ctx, obj, "value", value);
    JS_SetPropertyStr(ctx, obj, "done", JS_NewBool(ctx, done));
    return obj;
}

/* Build a JS Error with message "multipart: ..." */
static JSValue make_mp_error(JSContext *ctx, const char *msg)
{
    JSValue err = JS_NewError(ctx);
    JS_DefinePropertyValueStr(ctx, err, "message",
        JS_NewString(ctx, msg),
        JS_PROP_C_W_E);
    return err;
}

/* ── Part / Chunks JS object construction ───────────────────────────── */

/*
 * Build a JS Part object wrapping the given iter. Caller-side state is
 * an HlJsMpPart on the JS object's opaque; the iter's refcount is
 * bumped here (released in the finalizer).
 */
static JSValue make_js_part(JSContext *ctx, HlJsMpIter *it)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)hl_mp_part_class_id);
    if (JS_IsException(obj)) return obj;
    HlJsMpPart *p = hl_alloc_malloc(it->alloc, sizeof(*p));
    if (!p) { JS_FreeValue(ctx, obj); return JS_ThrowOutOfMemory(ctx); }
    p->iter = hl_mp_iter_ref(it);
    p->spent = 0;
    JS_SetOpaque(obj, p);
    return obj;
}

static JSValue make_js_chunks(JSContext *ctx, HlJsMpIter *it)
{
    JSValue obj = JS_NewObjectClass(ctx, (int)hl_mp_chunks_class_id);
    if (JS_IsException(obj)) return obj;
    HlJsMpChunks *c = hl_alloc_malloc(it->alloc, sizeof(*c));
    if (!c) { JS_FreeValue(ctx, obj); return JS_ThrowOutOfMemory(ctx); }
    c->iter  = hl_mp_iter_ref(it);
    c->ended = 0;
    JS_SetOpaque(obj, c);
    return obj;
}

/* ── Pump (the cont's resume + the park callback in one) ────────────── */

static void mp_js_pump(HlAsyncCont *self, void *driver);
static void mp_js_park_thunk(void *ctx, HlMultipartResumeReason reason);

/* set_handler_promise vtable slot for HlJsMpCont - wired into the
 * cont via jc->base.set_handler_promise = mp_js_cont_set_handler_promise_impl
 * at every alloc site. The public dispatcher in async.c dispatches
 * through this slot without needing to know the concrete cont type. */
static void mp_js_cont_set_handler_promise_impl(HlAsyncCont *self,
                                                  void *ctx_v,
                                                  void *promise_v)
{
    HlJsMpCont *jc = (HlJsMpCont *)self;
    JSContext *ctx = (JSContext *)ctx_v;
    JSValue promise = *(JSValue *)promise_v;
    jc->handler_promise = JS_DupValue(ctx, promise);
}

/* Drive the parser; return either:
 *   { ready = 1, result = <JSValue to resolve with>, error = JS_UNDEFINED }
 *   { ready = 1, result = JS_UNDEFINED, error = <JSValue to reject with> }
 *   { ready = 0, ... } meaning NEED_DATA (caller should re-park)
 *
 * Each MpMode has its own pump.
 */
typedef struct { int ready; JSValue result; JSValue error; } PumpStep;

static PumpStep pump_iter_step(JSContext *ctx, HlJsMpCont *jc)
{
    PumpStep s = {0, JS_UNDEFINED, JS_UNDEFINED};
    HlJsMpIter *it = jc->iter;

    for (;;) {
        if (it->errored) {
            s.ready = 1;
            s.error = make_mp_error(ctx, "multipart: parser error");
            return s;
        }
        if (it->done) {
            s.ready = 1;
            s.result = make_iter_result(ctx, JS_UNDEFINED, 1);
            return s;
        }

        KlMultipartPartMeta meta;
        const char *data = NULL;
        size_t data_len = 0;
        KlMultipartEvent ev = kl_multipart_next(it->inner, &meta,
                                                  &data, &data_len);
        switch (ev) {
        case KL_MP_EVT_PART_BEGIN:
            if (hl_mp_iter_copy_meta(it, &meta) != 0) {
                s.ready = 1;
                s.error = make_mp_error(ctx, "multipart: out of memory");
                return s;
            }
            it->in_part = 1;
            s.ready = 1;
            s.result = make_iter_result(ctx, make_js_part(ctx, it), 0);
            return s;
        case KL_MP_EVT_PART_DATA:
        case KL_MP_EVT_PART_END:
            it->in_part = (ev == KL_MP_EVT_PART_DATA) ? 1 : 0;
            continue; /* auto-drain when user didn't iterate chunks */
        case KL_MP_EVT_DONE:
            it->done = 1;
            it->in_part = 0;
            s.ready = 1;
            s.result = make_iter_result(ctx, JS_UNDEFINED, 1);
            return s;
        case KL_MP_EVT_NEED_DATA:
            return s; /* ready stays 0 */
        case KL_MP_EVT_ERROR:
        default:
            it->errored = 1;
            s.ready = 1;
            s.error = make_mp_error(ctx, "multipart: parser error");
            return s;
        }
    }
}

static PumpStep pump_chunks_step(JSContext *ctx, HlJsMpCont *jc)
{
    PumpStep s = {0, JS_UNDEFINED, JS_UNDEFINED};
    HlJsMpIter *it = jc->iter;
    HlJsMpChunks *c = jc->chunks;

    if (c->ended || !it->in_part) {
        s.ready = 1;
        s.result = make_iter_result(ctx, JS_UNDEFINED, 1);
        return s;
    }

    for (;;) {
        if (it->errored) {
            s.ready = 1;
            s.error = make_mp_error(ctx, "multipart: parser error");
            return s;
        }

        KlMultipartPartMeta meta;
        const char *data = NULL;
        size_t data_len = 0;
        KlMultipartEvent ev = kl_multipart_next(it->inner, &meta,
                                                  &data, &data_len);
        switch (ev) {
        case KL_MP_EVT_PART_DATA:
            if (data && data_len > 0) {
                /* Binary-safe: ArrayBuffer, not JS_NewStringLen (which
                 * validates UTF-8 and silently mangles binary data). */
                s.ready = 1;
                s.result = make_iter_result(ctx,
                    JS_NewArrayBufferCopy(ctx, (const uint8_t *)data,
                                            data_len), 0);
                return s;
            }
            continue;
        case KL_MP_EVT_PART_END:
            it->in_part = 0;
            c->ended = 1;
            s.ready = 1;
            s.result = make_iter_result(ctx, JS_UNDEFINED, 1);
            return s;
        case KL_MP_EVT_DONE:
            it->done = 1;
            it->in_part = 0;
            c->ended = 1;
            s.ready = 1;
            s.result = make_iter_result(ctx, JS_UNDEFINED, 1);
            return s;
        case KL_MP_EVT_NEED_DATA:
            return s;
        case KL_MP_EVT_ERROR:
        default:
            it->errored = 1;
            s.ready = 1;
            s.error = make_mp_error(ctx, "multipart: parser error");
            return s;
        }
    }
}

static PumpStep pump_read_step(JSContext *ctx, HlJsMpCont *jc)
{
    PumpStep s = {0, JS_UNDEFINED, JS_UNDEFINED};
    HlJsMpIter *it = jc->iter;
    HlJsMpPart *p = jc->part;

    if ((p && p->spent) || !it->in_part) {
        /* Already drained - return whatever we accumulated (likely empty). */
        s.ready = 1;
        s.result = JS_NewArrayBufferCopy(ctx,
            (const uint8_t *)(jc->read_buf ? jc->read_buf : ""),
            jc->read_len);
        return s;
    }

    for (;;) {
        if (it->errored) {
            s.ready = 1;
            s.error = make_mp_error(ctx, "multipart: parser error");
            return s;
        }

        KlMultipartPartMeta meta;
        const char *data = NULL;
        size_t data_len = 0;
        KlMultipartEvent ev = kl_multipart_next(it->inner, &meta,
                                                  &data, &data_len);
        switch (ev) {
        case KL_MP_EVT_PART_DATA:
            if (data && data_len > 0) {
                if (read_buf_append(jc, data, data_len) != 0) {
                    s.ready = 1;
                    s.error = make_mp_error(ctx, "multipart: out of memory");
                    return s;
                }
            }
            continue;
        case KL_MP_EVT_PART_END:
            it->in_part = 0;
            if (p) p->spent = 1;
            s.ready = 1;
            /* Binary-safe - ArrayBuffer. read() on a text field can be
             * decoded JS-side via `new TextDecoder().decode(buf)`. */
            s.result = JS_NewArrayBufferCopy(ctx,
                (const uint8_t *)(jc->read_buf ? jc->read_buf : ""),
                jc->read_len);
            return s;
        case KL_MP_EVT_DONE:
            it->done = 1;
            it->in_part = 0;
            if (p) p->spent = 1;
            s.ready = 1;
            /* Binary-safe - ArrayBuffer. read() on a text field can be
             * decoded JS-side via `new TextDecoder().decode(buf)`. */
            s.result = JS_NewArrayBufferCopy(ctx,
                (const uint8_t *)(jc->read_buf ? jc->read_buf : ""),
                jc->read_len);
            return s;
        case KL_MP_EVT_NEED_DATA:
            return s;
        case KL_MP_EVT_ERROR:
        default:
            it->errored = 1;
            s.ready = 1;
            s.error = make_mp_error(ctx, "multipart: parser error");
            return s;
        }
    }
}

/*
 * The shared cont resume + park callback. Walks the appropriate per-mode
 * pump; if it needs more data, re-parks (cont survives). If it produced
 * a result, resolves / rejects the iter Promise, drains microtasks,
 * checks the outer handler Promise, then frees the cont.
 */
static void mp_js_pump(HlAsyncCont *self, void *driver)
{
    (void)driver;
    HlJsMpCont *jc = (HlJsMpCont *)self;
    HlJS *js = jc->js;
    JSContext *ctx = js->ctx;
    KlConn *conn = jc->conn;

    /* Restore per-request context (in case a nested op inspects it) */
    js->active_conn = conn;

    PumpStep s;
    switch (jc->mode) {
    case MP_MODE_ITER:   s = pump_iter_step(ctx, jc);   break;
    case MP_MODE_CHUNKS: s = pump_chunks_step(ctx, jc); break;
    case MP_MODE_READ:   s = pump_read_step(ctx, jc);   break;
    default:
        s.ready = 1;
        s.result = JS_UNDEFINED;
        s.error = make_mp_error(ctx, "multipart: internal pump mode error");
        break;
    }

    if (!s.ready) {
        /* Still NEED_DATA. Re-park; cont survives unchanged. */
        hl_cap_multipart_park(jc->iter->wrapper, mp_js_park_thunk, jc);
        return;
    }

    /* Resolve or reject the iter Promise */
    if (!JS_IsUndefined(s.error)) {
        JSValue ret = JS_Call(ctx, jc->reject, JS_UNDEFINED, 1, &s.error);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, s.error);
    } else {
        JSValue ret = JS_Call(ctx, jc->resolve, JS_UNDEFINED, 1, &s.result);
        JS_FreeValue(ctx, ret);
        JS_FreeValue(ctx, s.result);
    }
    JS_FreeValue(ctx, jc->resolve);
    JS_FreeValue(ctx, jc->reject);
    jc->resolve = JS_UNDEFINED;
    jc->reject  = JS_UNDEFINED;

    /* Drain microtasks - the for-await loop body runs here. */
    hl_js_run_jobs(js);

    /* Check outer handler-Promise state */
    JSPromiseStateEnum state = JS_PROMISE_PENDING;
    if (!JS_IsUndefined(jc->handler_promise))
        state = JS_PromiseState(ctx, jc->handler_promise);

    if (state == JS_PROMISE_FULFILLED) {
        JS_FreeValue(ctx, jc->handler_promise);
        jc->handler_promise = JS_UNDEFINED;
        jc->conn = NULL;
        js->async_pending = 0;
        js->active_conn = NULL;
        js->dispatch_depth--;
        if (conn) {
            if (conn->res.body_mode == KL_BODY_STREAM)
                conn->state = KL_CONN_CLOSED;
            else
                conn->state = KL_CONN_SENDING;
        }
    } else if (state == JS_PROMISE_REJECTED) {
        JSValue err = JS_PromiseResult(ctx, jc->handler_promise);
        const char *msg = JS_ToCString(ctx, err);
        /* log via stderr to match the standard async-resume path */
        if (msg) {
            fprintf(stderr, "[hull:c] async js handler error: %s\n", msg);
            JS_FreeCString(ctx, msg);
        }
        JS_FreeValue(ctx, err);
        JS_FreeValue(ctx, jc->handler_promise);
        jc->handler_promise = JS_UNDEFINED;
        jc->conn = NULL;
        js->async_pending = 0;
        js->active_conn = NULL;
        js->dispatch_depth--;
        if (conn) {
            kl_response_status(&conn->res, 500);
            kl_response_header(&conn->res, "Content-Type", "text/plain");
            kl_response_body_borrow(&conn->res, "Internal Server Error", 21);
            conn->state = KL_CONN_SENDING;
        }
    } else if (!JS_IsUndefined(jc->handler_promise)) {
        /* PENDING - handler awaited again. A new cont was created
         * during the microtask drain; transfer the handler-Promise
         * via the cont's vtable (could be HlJsMpCont or the standard
         * HlJsAsyncCont - the slot dispatches to the right setter). */
        if (js->last_async_cont) {
            hl_js_async_cont_set_handler_promise(
                (HlAsyncCont *)js->last_async_cont, ctx, jc->handler_promise);
            js->last_async_cont = NULL;
        }
        JS_FreeValue(ctx, jc->handler_promise);
        jc->handler_promise = JS_UNDEFINED;
        jc->conn = NULL;
    }

    /* Done. Free the cont. */
    self->destroy(self);
}

static void mp_js_park_thunk(void *ctx, HlMultipartResumeReason reason)
{
    (void)reason;
    HlAsyncCont *cont = (HlAsyncCont *)ctx;
    if (cont && cont->resume) cont->resume(cont, NULL);
}

static void mp_js_cont_cancel(HlAsyncCont *self)
{
    HlJsMpCont *jc = (HlJsMpCont *)self;
    JSContext *ctx = jc->js->ctx;
    if (!JS_IsUndefined(jc->resolve))         { JS_FreeValue(ctx, jc->resolve);         jc->resolve = JS_UNDEFINED; }
    if (!JS_IsUndefined(jc->reject))          { JS_FreeValue(ctx, jc->reject);          jc->reject  = JS_UNDEFINED; }
    if (!JS_IsUndefined(jc->handler_promise)) { JS_FreeValue(ctx, jc->handler_promise); jc->handler_promise = JS_UNDEFINED; }
    jc->conn = NULL;
}

static void mp_js_cont_destroy(HlAsyncCont *self)
{
    HlJsMpCont *jc = (HlJsMpCont *)self;
    if (jc->read_buf) {
        hl_alloc_free(jc->alloc, jc->read_buf, jc->read_cap);
        jc->read_buf = NULL;
        jc->read_cap = 0;
        jc->read_len = 0;
    }
    if (jc->iter) {
        hl_mp_iter_unref(jc->iter);
        jc->iter = NULL;
    }
    hl_alloc_free(jc->alloc, jc, sizeof(*jc));
}

/*
 * Allocate + wire a pending cont. Stashes resolve/reject from the iter
 * Promise capability, parks the cont's resume on the body wrapper, sets
 * c->state, and side-effect-sets js->last_async_cont so dispatch can
 * attach the outer handler Promise.
 *
 * Caller owns nothing afterwards - the cont takes ownership of resolve
 * and reject; on failure paths the caller is responsible for freeing
 * those + the promise capability themselves before returning.
 */
static int mp_js_park(JSContext *ctx, HlJsMpIter *it, MpMode mode,
                       HlJsMpPart *part, HlJsMpChunks *chunks,
                       JSValue resolve, JSValue reject)
{
    HlJS *js = it->js;
    KlConn *conn = js->active_conn;
    if (!conn) {
        /* Streaming routes only work behind a live connection. */
        return -1;
    }

    HlJsMpCont *jc = hl_alloc_malloc(it->alloc, sizeof(*jc));
    if (!jc) return -1;

    jc->base.resume              = mp_js_pump;
    jc->base.cancel              = mp_js_cont_cancel;
    jc->base.destroy             = mp_js_cont_destroy;
    jc->base.set_handler_promise = mp_js_cont_set_handler_promise_impl;
    jc->js              = js;
    jc->alloc           = it->alloc;
    jc->resolve         = resolve;
    jc->reject          = reject;
    jc->handler_promise = JS_UNDEFINED;
    jc->conn            = conn;
    jc->iter            = hl_mp_iter_ref(it);
    jc->mode            = mode;
    jc->read_buf        = NULL;
    jc->read_len        = 0;
    jc->read_cap        = 0;
    jc->chunks          = chunks;
    jc->part            = part;

    /* Side-effect so dispatch picks up the cont for handler_promise wiring. */
    js->last_async_cont = jc;

    if (hl_cap_multipart_park(it->wrapper, mp_js_park_thunk, jc) != 0) {
        jc->base.destroy(&jc->base);
        return -1;
    }

    /* Tell Keel to keep reading socket bytes (the v2.1.1 streaming
     * dispatch keystone). */
    conn->state = KL_CONN_READING_BODY;
    (void)ctx;
    return 0;
}

/*
 * Carries the seed pump's output if it produced an event, or signals
 * "pending" so the caller knows it must park (with an already-allocated
 * cont and Promise).
 *
 * Usage pattern: every iter.next() / chunks.next() / read() entry point
 * first calls a seed pump synchronously. If ready, return resolved/
 * rejected Promise immediately (no cont). If not ready, allocate the
 * Promise + park as described above.
 */

/* ── iter.next() / chunks.next() / part.read() ──────────────────────── */

/* Build a resolved Promise wrapping `value`. Consumes `value`.
 *
 * QuickJS doesn't expose a direct "make an already-resolved Promise"
 * primitive, so we go through Promise.resolve on the global Promise
 * constructor. Used by the synchronous fast path of iter.next() /
 * chunks.next() - when the parser already has an event queued, the
 * for-await receiver doesn't need to wait. */
static JSValue resolve_with(JSContext *ctx, JSValue value)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue Promise = JS_GetPropertyStr(ctx, global, "Promise");
    JS_FreeValue(ctx, global);
    JSValue fn = JS_GetPropertyStr(ctx, Promise, "resolve");
    JSValue ret = JS_Call(ctx, fn, Promise, 1, &value);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, Promise);
    JS_FreeValue(ctx, value);
    return ret;
}

static JSValue reject_with(JSContext *ctx, JSValue err)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue Promise = JS_GetPropertyStr(ctx, global, "Promise");
    JS_FreeValue(ctx, global);
    JSValue fn = JS_GetPropertyStr(ctx, Promise, "reject");
    JSValue ret = JS_Call(ctx, fn, Promise, 1, &err);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, Promise);
    JS_FreeValue(ctx, err);
    return ret;
}

static JSValue js_iter_next(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlJsMpIter *it = JS_GetOpaque2(ctx, this_val, hl_mp_iter_class_id);
    if (!it) return JS_EXCEPTION;

    /* Stage a temporary cont struct on the stack to share the pump impl.
     * If the seed call returns ready, we don't allocate the heap cont. */
    HlJsMpCont stage;
    memset(&stage, 0, sizeof(stage));
    stage.js              = it->js;
    stage.alloc           = it->alloc;
    stage.resolve         = JS_UNDEFINED;
    stage.reject          = JS_UNDEFINED;
    stage.handler_promise = JS_UNDEFINED;
    stage.conn            = it->js->active_conn;
    stage.iter            = it; /* not refcounted - stage is stack only */
    stage.mode            = MP_MODE_ITER;

    PumpStep s = pump_iter_step(ctx, &stage);
    if (s.ready) {
        if (!JS_IsUndefined(s.error)) return reject_with(ctx, s.error);
        return resolve_with(ctx, s.result);
    }

    /* Need data - allocate the real cont + Promise + park */
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return JS_EXCEPTION;
    if (mp_js_park(ctx, it, MP_MODE_ITER, NULL, NULL,
                    resolving[0], resolving[1]) != 0) {
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx,
            "req.multipart(): no active connection (streaming routes "
            "require a live server)");
    }
    return promise;
}

/* iter[Symbol.asyncIterator]() returns the iter itself. */
static JSValue js_iter_async_iterator(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)ctx; (void)argc; (void)argv;
    return JS_DupValue(ctx, this_val);
}

/* part.read() - returns a Promise<string> over the full part body. */
static JSValue js_part_read(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlJsMpPart *p = JS_GetOpaque2(ctx, this_val, hl_mp_part_class_id);
    if (!p) return JS_EXCEPTION;
    HlJsMpIter *it = p->iter;

    if (p->spent || !it->in_part)
        return resolve_with(ctx, JS_NewStringLen(ctx, "", 0));

    /* Allocate a real cont upfront - read() accumulates across yields,
     * so the staging-on-stack trick doesn't help. */
    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return JS_EXCEPTION;

    HlJsMpCont *jc = hl_alloc_malloc(it->alloc, sizeof(*jc));
    if (!jc) {
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        JS_FreeValue(ctx, promise);
        return JS_ThrowOutOfMemory(ctx);
    }
    memset(jc, 0, sizeof(*jc));
    jc->base.resume              = mp_js_pump;
    jc->base.cancel              = mp_js_cont_cancel;
    jc->base.destroy             = mp_js_cont_destroy;
    jc->base.set_handler_promise = mp_js_cont_set_handler_promise_impl;
    jc->js              = it->js;
    jc->alloc           = it->alloc;
    jc->resolve         = resolving[0];
    jc->reject          = resolving[1];
    jc->handler_promise = JS_UNDEFINED;
    jc->conn            = it->js->active_conn;
    jc->iter            = hl_mp_iter_ref(it);
    jc->mode            = MP_MODE_READ;
    jc->part            = p;

    /* Run the first pump synchronously; if it completes with no NEED_DATA,
     * resolve immediately and skip the park dance. */
    PumpStep s = pump_read_step(ctx, jc);
    if (s.ready) {
        if (!JS_IsUndefined(s.error)) {
            JSValue r = JS_Call(ctx, jc->reject, JS_UNDEFINED, 1, &s.error);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, s.error);
        } else {
            JSValue r = JS_Call(ctx, jc->resolve, JS_UNDEFINED, 1, &s.result);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, s.result);
        }
        JS_FreeValue(ctx, jc->resolve); jc->resolve = JS_UNDEFINED;
        JS_FreeValue(ctx, jc->reject);  jc->reject  = JS_UNDEFINED;
        jc->base.destroy(&jc->base);
        return promise;
    }

    /* Not ready - park */
    KlConn *conn = it->js->active_conn;
    if (!conn) {
        jc->base.destroy(&jc->base);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx,
            "req.multipart(): no active connection");
    }
    it->js->last_async_cont = jc;
    if (hl_cap_multipart_park(it->wrapper, mp_js_park_thunk, jc) != 0) {
        jc->base.destroy(&jc->base);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx,
            "req.multipart(): body reader is not multipart");
    }
    conn->state = KL_CONN_READING_BODY;
    return promise;
}

/* chunks.next() - returns a Promise<{value, done}>. */
static JSValue js_chunks_next(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlJsMpChunks *c = JS_GetOpaque2(ctx, this_val, hl_mp_chunks_class_id);
    if (!c) return JS_EXCEPTION;
    HlJsMpIter *it = c->iter;

    HlJsMpCont stage;
    memset(&stage, 0, sizeof(stage));
    stage.js     = it->js;
    stage.alloc  = it->alloc;
    stage.iter   = it;
    stage.mode   = MP_MODE_CHUNKS;
    stage.chunks = c;
    stage.resolve = stage.reject = stage.handler_promise = JS_UNDEFINED;
    stage.conn   = it->js->active_conn;

    PumpStep s = pump_chunks_step(ctx, &stage);
    if (s.ready) {
        if (!JS_IsUndefined(s.error)) return reject_with(ctx, s.error);
        return resolve_with(ctx, s.result);
    }

    JSValue resolving[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving);
    if (JS_IsException(promise)) return JS_EXCEPTION;
    if (mp_js_park(ctx, it, MP_MODE_CHUNKS, NULL, c,
                    resolving[0], resolving[1]) != 0) {
        JS_FreeValue(ctx, resolving[0]);
        JS_FreeValue(ctx, resolving[1]);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx,
            "req.multipart(): no active connection");
    }
    return promise;
}

/* chunks[Symbol.asyncIterator]() returns itself. */
static JSValue js_chunks_async_iterator(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    return JS_DupValue(ctx, this_val);
}

/* part.chunks() returns a chunks-iter. Accepts an optional advisory
 * min-bytes hint (currently ignored - see Lua side comment). */
static JSValue js_part_chunks(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    HlJsMpPart *p = JS_GetOpaque2(ctx, this_val, hl_mp_part_class_id);
    if (!p) return JS_EXCEPTION;
    if (argc > 0 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0])) {
        int64_t hint;
        if (JS_ToInt64(ctx, &hint, argv[0]) != 0) return JS_EXCEPTION;
    }
    return make_js_chunks(ctx, p->iter);
}

/* Part getters: name / filename / contentType */
static JSValue js_part_get_name(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlJsMpPart *p = JS_GetOpaque2(ctx, this_val, hl_mp_part_class_id);
    if (!p) return JS_EXCEPTION;
    if (p->iter->name)
        return JS_NewStringLen(ctx, p->iter->name, p->iter->name_len);
    return JS_NewString(ctx, "");
}

static JSValue js_part_get_filename(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlJsMpPart *p = JS_GetOpaque2(ctx, this_val, hl_mp_part_class_id);
    if (!p) return JS_EXCEPTION;
    if (p->iter->filename)
        return JS_NewStringLen(ctx, p->iter->filename, p->iter->filename_len);
    return JS_NULL;
}

static JSValue js_part_get_content_type(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlJsMpPart *p = JS_GetOpaque2(ctx, this_val, hl_mp_part_class_id);
    if (!p) return JS_EXCEPTION;
    if (p->iter->content_type)
        return JS_NewStringLen(ctx, p->iter->content_type,
                                 p->iter->content_type_len);
    return JS_NULL;
}

/* ── Finalizers ─────────────────────────────────────────────────────── */

static void js_iter_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlJsMpIter *it = JS_GetOpaque(val, hl_mp_iter_class_id);
    if (it) hl_mp_iter_unref(it);
}

static void js_part_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlJsMpPart *p = JS_GetOpaque(val, hl_mp_part_class_id);
    if (!p) return;
    HlAllocator *a = p->iter->alloc;
    hl_mp_iter_unref(p->iter);
    hl_alloc_free(a, p, sizeof(*p));
}

static void js_chunks_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlJsMpChunks *c = JS_GetOpaque(val, hl_mp_chunks_class_id);
    if (!c) return;
    HlAllocator *a = c->iter->alloc;
    hl_mp_iter_unref(c->iter);
    hl_alloc_free(a, c, sizeof(*c));
}

static JSClassDef js_iter_class   = { "MultipartIter",   .finalizer = js_iter_finalizer };
static JSClassDef js_part_class   = { "MultipartPart",   .finalizer = js_part_finalizer };
static JSClassDef js_chunks_class = { "MultipartChunks", .finalizer = js_chunks_finalizer };

/* ── req.multipart() entry point ─────────────────────────────────────── */

/*
 * Closure invariants: upvalue 0 = pointer (as a number JSValue) to the
 * KlBodyReader wrapper. QuickJS doesn't expose lightuserdata; we pack
 * the pointer as an Int64 the same way the existing bindings do for the
 * handful of other native-state-bearing closures.
 */

static JSValue js_req_multipart(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv,
                                 int magic, JSValueConst *func_data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;

    int64_t wrapper_addr;
    if (JS_ToInt64(ctx, &wrapper_addr, func_data[0]) != 0)
        return JS_ThrowInternalError(ctx,
            "req.multipart(): missing body-reader address");

    KlBodyReader *wrapper = (KlBodyReader *)(uintptr_t)wrapper_addr;
    if (!wrapper)
        return JS_ThrowInternalError(ctx,
            "req.multipart(): no body reader");

    KlBodyReader *inner = hl_cap_multipart_inner(wrapper);
    if (!inner)
        return JS_ThrowInternalError(ctx,
            "req.multipart(): body reader is not a streaming-multipart wrapper");

    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js) return JS_ThrowInternalError(ctx,
        "req.multipart(): no JS runtime context");

    HlJsMpIter *it = hl_alloc_malloc(js->base.alloc, sizeof(*it));
    if (!it) return JS_ThrowOutOfMemory(ctx);
    memset(it, 0, sizeof(*it));
    it->wrapper = wrapper;
    it->inner   = inner;
    it->js      = js;
    it->alloc   = js->base.alloc;
    it->refs    = 1; /* the iter JS object holds the first ref */

    JSValue obj = JS_NewObjectClass(ctx, (int)hl_mp_iter_class_id);
    if (JS_IsException(obj)) {
        hl_alloc_free(it->alloc, it, sizeof(*it));
        return obj;
    }
    JS_SetOpaque(obj, it);
    return obj;
}

/* ── Public installer ───────────────────────────────────────────────── */

/*
 * Install req.multipart on the given JS request object. Called from
 * hl_js_make_request only when the body_reader is a multipart wrapper
 * (i.e. the route was registered via kl_server_route_streaming).
 */
void hl_js_request_install_multipart(JSContext *ctx, JSValue req_obj,
                                      KlBodyReader *body_reader)
{
    if (!body_reader || !hl_cap_multipart_inner(body_reader)) return;

    /* Wrapper pointer as Int64 - QuickJS doesn't have lightuserdata. */
    JSValue addr_val = JS_NewInt64(ctx, (int64_t)(intptr_t)body_reader);
    JSValueConst func_data[1] = { addr_val };
    JSValue fn = JS_NewCFunctionData(ctx, js_req_multipart, 0, 0, 1, func_data);
    JS_FreeValue(ctx, addr_val);
    JS_SetPropertyStr(ctx, req_obj, "multipart", fn);
}

/* ── Class registration ─────────────────────────────────────────────── */

void hl_js_request_register(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);

    JS_NewClassID(&hl_mp_iter_class_id);
    JS_NewClass(rt, hl_mp_iter_class_id, &js_iter_class);

    JS_NewClassID(&hl_mp_part_class_id);
    JS_NewClass(rt, hl_mp_part_class_id, &js_part_class);

    JS_NewClassID(&hl_mp_chunks_class_id);
    JS_NewClass(rt, hl_mp_chunks_class_id, &js_chunks_class);

    /* MultipartIter prototype: next() + [Symbol.asyncIterator]() */
    {
        JSValue proto = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, proto, "next",
            JS_NewCFunction(ctx, js_iter_next, "next", 0));
        JSAtom async_iter_atom = JS_NewAtom(ctx, "Symbol.asyncIterator");
        /* QuickJS exposes Symbol.asyncIterator as a property of the
         * Symbol global - we install both a Symbol.asyncIterator slot
         * AND a string fallback for engines that don't resolve the
         * Symbol path through atoms. */
        JS_FreeAtom(ctx, async_iter_atom);
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue Symbol = JS_GetPropertyStr(ctx, global, "Symbol");
        JSValue asyncIter = JS_GetPropertyStr(ctx, Symbol, "asyncIterator");
        if (JS_IsSymbol(asyncIter)) {
            JSAtom atom = JS_ValueToAtom(ctx, asyncIter);
            JS_DefinePropertyValue(ctx, proto, atom,
                JS_NewCFunction(ctx, js_iter_async_iterator,
                                 "[Symbol.asyncIterator]", 0),
                JS_PROP_C_W_E);
            JS_FreeAtom(ctx, atom);
        }
        JS_FreeValue(ctx, asyncIter);
        JS_FreeValue(ctx, Symbol);
        JS_FreeValue(ctx, global);
        JS_SetClassProto(ctx, hl_mp_iter_class_id, proto);
    }

    /* MultipartPart prototype: read(), chunks(), name/filename/contentType */
    {
        JSValue proto = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, proto, "read",
            JS_NewCFunction(ctx, js_part_read, "read", 0));
        JS_SetPropertyStr(ctx, proto, "chunks",
            JS_NewCFunction(ctx, js_part_chunks, "chunks", 1));
        JSAtom name_atom         = JS_NewAtom(ctx, "name");
        JSAtom filename_atom     = JS_NewAtom(ctx, "filename");
        JSAtom content_type_atom = JS_NewAtom(ctx, "contentType");
        JS_DefinePropertyGetSet(ctx, proto, name_atom,
            JS_NewCFunction(ctx, js_part_get_name, "name", 0),
            JS_UNDEFINED, JS_PROP_C_W_E);
        JS_DefinePropertyGetSet(ctx, proto, filename_atom,
            JS_NewCFunction(ctx, js_part_get_filename, "filename", 0),
            JS_UNDEFINED, JS_PROP_C_W_E);
        JS_DefinePropertyGetSet(ctx, proto, content_type_atom,
            JS_NewCFunction(ctx, js_part_get_content_type, "contentType", 0),
            JS_UNDEFINED, JS_PROP_C_W_E);
        JS_FreeAtom(ctx, name_atom);
        JS_FreeAtom(ctx, filename_atom);
        JS_FreeAtom(ctx, content_type_atom);
        JS_SetClassProto(ctx, hl_mp_part_class_id, proto);
    }

    /* MultipartChunks prototype: next() + [Symbol.asyncIterator]() */
    {
        JSValue proto = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, proto, "next",
            JS_NewCFunction(ctx, js_chunks_next, "next", 0));
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue Symbol = JS_GetPropertyStr(ctx, global, "Symbol");
        JSValue asyncIter = JS_GetPropertyStr(ctx, Symbol, "asyncIterator");
        if (JS_IsSymbol(asyncIter)) {
            JSAtom atom = JS_ValueToAtom(ctx, asyncIter);
            JS_DefinePropertyValue(ctx, proto, atom,
                JS_NewCFunction(ctx, js_chunks_async_iterator,
                                 "[Symbol.asyncIterator]", 0),
                JS_PROP_C_W_E);
            JS_FreeAtom(ctx, atom);
        }
        JS_FreeValue(ctx, asyncIter);
        JS_FreeValue(ctx, Symbol);
        JS_FreeValue(ctx, global);
        JS_SetClassProto(ctx, hl_mp_chunks_class_id, proto);
    }
}
