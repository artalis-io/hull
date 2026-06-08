/* mod_blob.c — hull:blob bindings (content-addressed blob storage)
 *
 * Public API mirrors docs/blob.md (camelCase per the JS conventions):
 *
 *   import { blob } from "hull:blob";
 *
 *   blob.init({ dir, shardDepth?, tmpMaxAge? })
 *
 *   blob.put(bytes)               → { id, size }
 *   blob.putVerified(bytes, sha)  → { id, size }   (throws on mismatch)
 *   blob.writer({ expected? })    → Writer
 *     w.write(chunk)              → w (chainable)
 *     w.finalize()                → { id, size }
 *     w.abort()
 *
 *   blob.get(id, { trackAccess? }) → ArrayBuffer or null
 *   blob.reader(id, { trackAccess? }) → Reader
 *     r.read(n?)                  → ArrayBuffer or null at EOF
 *     r.close()
 *
 *   blob.exists(id)               → bool
 *   blob.size(id)                 → int or null
 *   blob.atime(id)                → int or null
 *   blob.delete(id)               → bool
 *
 *   for (const { id, size } of blob.iter()) {...}
 *   blob.totalSize()              → int
 *   blob.count()                  → int
 *
 *   blob.cleanup({ maxTotalSize?, maxAge?, strategy?, dryRun? })
 *                                  → { removed, freedBytes }
 *
 * Singleton: blob.init() creates an HlBlob* wrapped in a Store JSValue;
 * the JSValue is stashed on globalThis under "__hullBlobInternal" so
 * the finalizer fires on context teardown. All other methods retrieve
 * the store from there. Re-init is allowed (drops the previous Store
 * which then gets GC'd, freeing the previous HlBlob).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/blob.h"
#include "hull/cap/fs.h"
#include "hull/alloc.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define HL_BLOB_STASH_PROP "__hullBlobInternal"

/* Accept ArrayBuffer (binary-safe) or string (UTF-8) — mirror of the
 * crypto.sha256 hasher's accept-anything-bytes pattern. The caller is
 * responsible for JS_FreeCString() on `*free_cstr` if non-NULL. */
static const uint8_t *bytes_arg(JSContext *ctx, JSValueConst v,
                                  size_t *out_len, const char **free_cstr)
{
    *free_cstr = NULL;
    const uint8_t *bytes = JS_GetArrayBuffer(ctx, out_len, v);
    if (bytes) return bytes;
    const char *cstr = JS_ToCStringLen(ctx, out_len, v);
    if (!cstr) return NULL;
    *free_cstr = cstr;
    return (const uint8_t *)cstr;
}

/* Validate `id`: 64 lowercase hex chars. Returns NULL on rejection
 * (with a JS exception set); otherwise a JS-owned C string the caller
 * must JS_FreeCString. */
static const char *check_id(JSContext *ctx, JSValueConst v)
{
    size_t n;
    const char *s = JS_ToCStringLen(ctx, &n, v);
    if (!s) return NULL;
    if (n != HL_BLOB_ID_HEX_LEN) {
        JS_FreeCString(ctx, s);
        JS_ThrowTypeError(ctx, "blob: invalid id (expected 64 hex chars)");
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            JS_FreeCString(ctx, s);
            JS_ThrowTypeError(ctx, "blob: id must be lowercase hex");
            return NULL;
        }
    }
    return s;
}

/* Extract trackAccess from optional options object at argv[idx]. */
static int read_track_access(JSContext *ctx, int argc, JSValueConst *argv, int idx)
{
    if (idx >= argc) return 1;
    JSValueConst v = argv[idx];
    if (JS_IsUndefined(v) || JS_IsNull(v) || !JS_IsObject(v)) return 1;
    JSValue ta = JS_GetPropertyStr(ctx, v, "trackAccess");
    int track = JS_IsUndefined(ta) ? 1 : JS_ToBool(ctx, ta);
    JS_FreeValue(ctx, ta);
    return track;
}

/* Extract durable from optional options object at argv[idx]. */
static int read_durable(JSContext *ctx, int argc, JSValueConst *argv, int idx)
{
    if (idx >= argc) return 0;
    JSValueConst v = argv[idx];
    if (JS_IsUndefined(v) || JS_IsNull(v) || !JS_IsObject(v)) return 0;
    JSValue d = JS_GetPropertyStr(ctx, v, "durable");
    int durable = JS_IsUndefined(d) ? 0 : JS_ToBool(ctx, d);
    JS_FreeValue(ctx, d);
    return durable;
}

/* ── Store class (singleton wrapping HlBlob*) ─────────────────────── */

static JSClassID hl_js_blob_store_class_id;

static void js_blob_store_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlBlob *b = JS_GetOpaque(val, hl_js_blob_store_class_id);
    if (b) hl_cap_blob_free(b);
}

static JSClassDef js_blob_store_class = {
    "HlBlobStore",
    .finalizer = js_blob_store_finalizer,
};

/* Fetch the singleton from globalThis. Throws if blob.init() hasn't
 * been called. */
static HlBlob *get_store(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue stash = JS_GetPropertyStr(ctx, global, HL_BLOB_STASH_PROP);
    JS_FreeValue(ctx, global);
    if (JS_IsUndefined(stash) || JS_IsNull(stash)) {
        JS_FreeValue(ctx, stash);
        JS_ThrowInternalError(ctx, "blob: call blob.init({dir}) first");
        return NULL;
    }
    HlBlob *b = JS_GetOpaque(stash, hl_js_blob_store_class_id);
    JS_FreeValue(ctx, stash);
    if (!b) {
        JS_ThrowInternalError(ctx, "blob: store unavailable");
        return NULL;
    }
    return b;
}

/* ── blob.init ───────────────────────────────────────────────────── */

static JSValue js_blob_init(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (!js || !js->base.fs_cfg)
        return JS_ThrowInternalError(ctx, "blob.init: fs config unavailable");

    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "blob.init requires (opts)");

    JSValue dir_v = JS_GetPropertyStr(ctx, argv[0], "dir");
    /* Explicit undefined check — JS_GetPropertyStr returns JS_UNDEFINED
     * for a missing property, and JS_ToCString on undefined returns
     * the literal string "undefined" which would silently propagate
     * through to the cap layer's error message ("…declares
     * 'undefined'") instead of a clean "dir required". */
    if (JS_IsUndefined(dir_v) || JS_IsNull(dir_v)) {
        JS_FreeValue(ctx, dir_v);
        return JS_ThrowTypeError(ctx, "blob.init: opts.dir required");
    }
    const char *dir = JS_ToCString(ctx, dir_v);
    JS_FreeValue(ctx, dir_v);
    if (!dir) return JS_EXCEPTION;

    JSValue sd_v = JS_GetPropertyStr(ctx, argv[0], "shardDepth");
    int shard_depth = 1;
    if (!JS_IsUndefined(sd_v)) {
        int32_t v = 1;
        JS_ToInt32(ctx, &v, sd_v);
        shard_depth = (int)v;
    }
    JS_FreeValue(ctx, sd_v);

    JSValue tma_v = JS_GetPropertyStr(ctx, argv[0], "tmpMaxAge");
    uint64_t tmp_max_age = 0;
    if (!JS_IsUndefined(tma_v)) {
        int64_t v = 0;
        JS_ToInt64(ctx, &v, tma_v);
        tmp_max_age = (uint64_t)v;
    }
    JS_FreeValue(ctx, tma_v);

    HlBlob *b = NULL;
    int rc = hl_cap_blob_init(&b, js->base.fs_cfg, js->base.alloc,
                                dir, shard_depth, tmp_max_age);
    if (rc != 0) {
        /* Stash dir into a stack buffer for the error message before
         * releasing the cstring — paths fit comfortably in PATH_MAX. */
        char dir_buf[PATH_MAX];
        snprintf(dir_buf, sizeof(dir_buf), "%s", dir);
        JS_FreeCString(ctx, dir);
        return JS_ThrowInternalError(ctx,
            "blob.init: failed (check fs.write declares '%s')",
            dir_buf);
    }
    JS_FreeCString(ctx, dir);

    /* Wrap in a Store JSValue + stash on globalThis. Replacing any
     * previous stash drops its refcount → finalizer runs → previous
     * HlBlob freed. */
    JSValue store = JS_NewObjectClass(ctx, (int)hl_js_blob_store_class_id);
    if (JS_IsException(store)) { hl_cap_blob_free(b); return store; }
    JS_SetOpaque(store, b);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, HL_BLOB_STASH_PROP, store);
    JS_FreeValue(ctx, global);

    return JS_UNDEFINED;
}

/* ── Result object helpers ───────────────────────────────────────── */

static JSValue make_put_result(JSContext *ctx, const char *id, size_t size)
{
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) return obj;
    JS_SetPropertyStr(ctx, obj, "id",
                      JS_NewStringLen(ctx, id, HL_BLOB_ID_HEX_LEN));
    JS_SetPropertyStr(ctx, obj, "size", JS_NewInt64(ctx, (int64_t)size));
    return obj;
}

/* ── blob.put / blob.putVerified ─────────────────────────────────── */

static JSValue js_blob_put(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    HlBlob *b = get_store(ctx);
    if (!b) return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "blob.put requires (bytes)");

    size_t len = 0;
    const char *cstr = NULL;
    const uint8_t *bytes = bytes_arg(ctx, argv[0], &len, &cstr);
    if (!bytes) return JS_EXCEPTION;
    int durable = read_durable(ctx, argc, argv, 1);

    char id[HL_BLOB_ID_BUF_SIZE];
    int rc = durable
        ? hl_cap_blob_put_durable(b, bytes, len, NULL, id)
        : hl_cap_blob_put        (b, bytes, len, NULL, id);
    if (cstr) JS_FreeCString(ctx, cstr);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "blob.put: failed");
    return make_put_result(ctx, id, len);
}

static JSValue js_blob_put_verified(JSContext *ctx, JSValueConst this_val,
                                       int argc, JSValueConst *argv)
{
    (void)this_val;
    HlBlob *b = get_store(ctx);
    if (!b) return JS_EXCEPTION;
    if (argc < 2)
        return JS_ThrowTypeError(ctx, "blob.putVerified requires (bytes, sha)");

    size_t len = 0;
    const char *cstr = NULL;
    const uint8_t *bytes = bytes_arg(ctx, argv[0], &len, &cstr);
    if (!bytes) return JS_EXCEPTION;
    const char *expected = check_id(ctx, argv[1]);
    if (!expected) { if (cstr) JS_FreeCString(ctx, cstr); return JS_EXCEPTION; }
    int durable = read_durable(ctx, argc, argv, 2);

    char id[HL_BLOB_ID_BUF_SIZE];
    int rc = durable
        ? hl_cap_blob_put_durable(b, bytes, len, expected, id)
        : hl_cap_blob_put        (b, bytes, len, expected, id);
    JS_FreeCString(ctx, expected);
    if (cstr) JS_FreeCString(ctx, cstr);
    if (rc != 0)
        return JS_ThrowInternalError(ctx,
            "blob.putVerified: SHA mismatch or I/O failure");
    return make_put_result(ctx, id, len);
}

/* ── Writer class ────────────────────────────────────────────────── */

static JSClassID hl_js_blob_writer_class_id;

static void js_blob_writer_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlBlobWriter *w = JS_GetOpaque(val, hl_js_blob_writer_class_id);
    if (w) hl_cap_blob_writer_abort(w);   /* silent abort on GC */
}

static JSClassDef js_blob_writer_class = {
    "HlBlobWriter",
    .finalizer = js_blob_writer_finalizer,
};

static JSValue js_blob_writer_new(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val;
    HlBlob *b = get_store(ctx);
    if (!b) return JS_EXCEPTION;

    const char *expected = NULL;
    char id_buf[HL_BLOB_ID_BUF_SIZE];
    int durable = read_durable(ctx, argc, argv, 0);
    if (argc > 0 && JS_IsObject(argv[0])) {
        JSValue ev = JS_GetPropertyStr(ctx, argv[0], "expected");
        if (!JS_IsUndefined(ev) && !JS_IsNull(ev)) {
            const char *e = check_id(ctx, ev);
            if (!e) { JS_FreeValue(ctx, ev); return JS_EXCEPTION; }
            memcpy(id_buf, e, HL_BLOB_ID_BUF_SIZE);
            JS_FreeCString(ctx, e);
            expected = id_buf;
        }
        JS_FreeValue(ctx, ev);
    }

    HlBlobWriter *w = NULL;
    int open_rc = durable
        ? hl_cap_blob_writer_open_durable(b, expected, &w)
        : hl_cap_blob_writer_open        (b, expected, &w);
    if (open_rc != 0)
        return JS_ThrowInternalError(ctx, "blob.writer: open failed");

    JSValue obj = JS_NewObjectClass(ctx, (int)hl_js_blob_writer_class_id);
    if (JS_IsException(obj)) { hl_cap_blob_writer_abort(w); return obj; }
    JS_SetOpaque(obj, w);
    return obj;
}

static JSValue js_writer_write(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    HlBlobWriter *w = JS_GetOpaque2(ctx, this_val, hl_js_blob_writer_class_id);
    if (!w) return JS_ThrowInternalError(ctx,
        "blob.writer: write after finalize/abort");
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "writer.write requires (chunk)");

    size_t len = 0;
    const char *cstr = NULL;
    const uint8_t *bytes = bytes_arg(ctx, argv[0], &len, &cstr);
    if (!bytes) return JS_EXCEPTION;

    int rc = hl_cap_blob_writer_write(w, bytes, len);
    if (cstr) JS_FreeCString(ctx, cstr);
    if (rc != 0) {
        hl_cap_blob_writer_abort(w);
        JS_SetOpaque(this_val, NULL);
        return JS_ThrowInternalError(ctx, "blob.writer: write failed (aborted)");
    }
    return JS_DupValue(ctx, this_val);   /* chainable */
}

static JSValue js_writer_finalize(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlBlobWriter *w = JS_GetOpaque2(ctx, this_val, hl_js_blob_writer_class_id);
    if (!w) return JS_ThrowInternalError(ctx,
        "blob.writer: already finalized/aborted");
    char id[HL_BLOB_ID_BUF_SIZE]; size_t size = 0;
    int rc = hl_cap_blob_writer_finalize(w, id, &size);
    JS_SetOpaque(this_val, NULL);    /* writer always consumed */
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "blob.writer: finalize failed");
    return make_put_result(ctx, id, size);
}

static JSValue js_writer_abort(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlBlobWriter *w = JS_GetOpaque2(ctx, this_val, hl_js_blob_writer_class_id);
    if (w) {
        hl_cap_blob_writer_abort(w);
        JS_SetOpaque(this_val, NULL);
    }
    return JS_UNDEFINED;
}

/* ── Reader class ────────────────────────────────────────────────── */

static JSClassID hl_js_blob_reader_class_id;

static void js_blob_reader_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlBlobReader *r = JS_GetOpaque(val, hl_js_blob_reader_class_id);
    if (r) hl_cap_blob_reader_close(r);
}

static JSClassDef js_blob_reader_class = {
    "HlBlobReader",
    .finalizer = js_blob_reader_finalizer,
};

static JSValue js_blob_reader_new(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val;
    HlBlob *b = get_store(ctx);
    if (!b) return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "blob.reader requires (id)");

    const char *id = check_id(ctx, argv[0]);
    if (!id) return JS_EXCEPTION;
    int track = read_track_access(ctx, argc, argv, 1);

    HlBlobReader *r = NULL;
    int rc = hl_cap_blob_reader_open(b, id, track, &r);
    JS_FreeCString(ctx, id);
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "blob.reader: blob not found");

    JSValue obj = JS_NewObjectClass(ctx, (int)hl_js_blob_reader_class_id);
    if (JS_IsException(obj)) { hl_cap_blob_reader_close(r); return obj; }
    JS_SetOpaque(obj, r);
    return obj;
}

static JSValue js_reader_read(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    HlBlobReader *r = JS_GetOpaque2(ctx, this_val, hl_js_blob_reader_class_id);
    if (!r) return JS_ThrowInternalError(ctx, "blob.reader: read after close");

    int64_t cap = 65536;
    if (argc > 0 && !JS_IsUndefined(argv[0])) JS_ToInt64(ctx, &cap, argv[0]);
    if (cap <= 0) return JS_NewArrayBufferCopy(ctx, NULL, 0);

    uint8_t *buf = js_malloc(ctx, (size_t)cap);
    if (!buf) return JS_ThrowOutOfMemory(ctx);
    size_t got = 0;
    if (hl_cap_blob_reader_read(r, buf, (size_t)cap, &got) != 0) {
        js_free(ctx, buf);
        return JS_ThrowInternalError(ctx, "blob.reader: read failed");
    }
    if (got == 0) {
        js_free(ctx, buf);
        return JS_NULL;
    }
    JSValue ab = JS_NewArrayBufferCopy(ctx, buf, got);
    js_free(ctx, buf);
    return ab;
}

static JSValue js_reader_close(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlBlobReader *r = JS_GetOpaque2(ctx, this_val, hl_js_blob_reader_class_id);
    if (r) {
        hl_cap_blob_reader_close(r);
        JS_SetOpaque(this_val, NULL);
    }
    return JS_UNDEFINED;
}

/* ── blob.get (buffer-mode read) ─────────────────────────────────── */

static JSValue js_blob_get(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    HlBlob *b = get_store(ctx);
    if (!b) return JS_EXCEPTION;
    if (argc < 1)
        return JS_ThrowTypeError(ctx, "blob.get requires (id)");

    const char *id = check_id(ctx, argv[0]);
    if (!id) return JS_EXCEPTION;
    int track = read_track_access(ctx, argc, argv, 1);

    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    uint8_t *buf = NULL;
    size_t len = 0;
    int rc = hl_cap_blob_get(b, id, track, &buf, &len);
    JS_FreeCString(ctx, id);
    if (rc != 0) return JS_NULL;

    /* Empty-blob contract: (NULL, 0). JS_NewArrayBufferCopy accepts
     * NULL when len is 0 and returns an empty ArrayBuffer — no
     * allocation to free. */
    JSValue ab = JS_NewArrayBufferCopy(ctx, buf, len);
    if (buf) hl_alloc_free(js->base.alloc, buf, len);
    return ab;
}

/* ── Metadata ────────────────────────────────────────────────────── */

static JSValue js_blob_exists(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    HlBlob *b = get_store(ctx);
    if (!b) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "blob.exists requires (id)");
    const char *id = check_id(ctx, argv[0]);
    if (!id) return JS_EXCEPTION;
    int rc = hl_cap_blob_exists(b, id);
    JS_FreeCString(ctx, id);
    if (rc < 0) return JS_ThrowInternalError(ctx, "blob.exists: stat failed");
    return JS_NewBool(ctx, rc);
}

static JSValue js_blob_size(JSContext *ctx, JSValueConst this_val,
                               int argc, JSValueConst *argv)
{
    (void)this_val;
    HlBlob *b = get_store(ctx);
    if (!b) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "blob.size requires (id)");
    const char *id = check_id(ctx, argv[0]);
    if (!id) return JS_EXCEPTION;
    size_t size = 0;
    int rc = hl_cap_blob_stat(b, id, &size, NULL);
    JS_FreeCString(ctx, id);
    if (rc != 0) return JS_NULL;
    return JS_NewInt64(ctx, (int64_t)size);
}

static JSValue js_blob_atime(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val;
    HlBlob *b = get_store(ctx);
    if (!b) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "blob.atime requires (id)");
    const char *id = check_id(ctx, argv[0]);
    if (!id) return JS_EXCEPTION;
    int64_t at = 0;
    int rc = hl_cap_blob_stat(b, id, NULL, &at);
    JS_FreeCString(ctx, id);
    if (rc != 0) return JS_NULL;
    return JS_NewInt64(ctx, at);
}

static JSValue js_blob_delete(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    HlBlob *b = get_store(ctx);
    if (!b) return JS_EXCEPTION;
    if (argc < 1) return JS_ThrowTypeError(ctx, "blob.delete requires (id)");
    const char *id = check_id(ctx, argv[0]);
    if (!id) return JS_EXCEPTION;
    int rc = hl_cap_blob_delete(b, id);
    JS_FreeCString(ctx, id);
    if (rc < 0) return JS_ThrowInternalError(ctx, "blob.delete: unlink failed");
    return JS_NewBool(ctx, rc);
}

/* ── Enumeration ─────────────────────────────────────────────────── */

typedef struct {
    char    id[HL_BLOB_ID_BUF_SIZE];
    size_t  size;
} IterItem;

typedef struct {
    JSContext *ctx;
    IterItem  *items;
    size_t     count;
    size_t     capacity;
    int        oom;
} IterAcc;

static int iter_collect_cb(const char *id, size_t size, void *user)
{
    IterAcc *a = user;
    if (a->count == a->capacity) {
        size_t new_cap = a->capacity == 0 ? 64 : a->capacity * 2;
        /* L1: refuse multiplication overflow (only reachable at
         * billions of blobs but the sibling cap-layer entries_push
         * has the check — keep parity). */
        if (new_cap > SIZE_MAX / sizeof(IterItem)) { a->oom = 1; return -1; }
        IterItem *grown = js_realloc(a->ctx, a->items,
                                       new_cap * sizeof(IterItem));
        if (!grown) { a->oom = 1; return -1; }
        a->items = grown;
        a->capacity = new_cap;
    }
    memcpy(a->items[a->count].id, id, HL_BLOB_ID_BUF_SIZE);
    a->items[a->count].size = size;
    a->count++;
    return 0;
}

static JSValue js_blob_iter(JSContext *ctx, JSValueConst this_val,
                              int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    HlBlob *b = get_store(ctx);
    if (!b) return JS_EXCEPTION;

    IterAcc acc = { ctx, NULL, 0, 0, 0 };
    if (hl_cap_blob_iter(b, iter_collect_cb, &acc) != 0 || acc.oom) {
        if (acc.items) js_free(ctx, acc.items);
        return JS_ThrowInternalError(ctx, "blob.iter: walk failed");
    }

    /* Build a JS array of {id, size} objects. Snapshot semantics: the
     * array is materialized before return; mutations during iteration
     * are invisible. */
    JSValue arr = JS_NewArray(ctx);
    if (JS_IsException(arr)) {
        if (acc.items) js_free(ctx, acc.items);
        return arr;
    }
    for (size_t i = 0; i < acc.count; i++) {
        JSValue item = JS_NewObject(ctx);
        if (JS_IsException(item)) {
            JS_FreeValue(ctx, arr);
            if (acc.items) js_free(ctx, acc.items);
            return item;
        }
        JS_SetPropertyStr(ctx, item, "id",
            JS_NewStringLen(ctx, acc.items[i].id, HL_BLOB_ID_HEX_LEN));
        JS_SetPropertyStr(ctx, item, "size",
            JS_NewInt64(ctx, (int64_t)acc.items[i].size));
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, item);
    }
    if (acc.items) js_free(ctx, acc.items);
    return arr;
}

static JSValue js_blob_total_size(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    HlBlob *b = get_store(ctx);
    if (!b) return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)hl_cap_blob_total_size(b));
}

static JSValue js_blob_count(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    HlBlob *b = get_store(ctx);
    if (!b) return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)hl_cap_blob_count(b));
}

/* ── Cleanup ─────────────────────────────────────────────────────── */

static JSValue js_blob_cleanup(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val;
    HlBlob *b = get_store(ctx);
    if (!b) return JS_EXCEPTION;
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "blob.cleanup requires (opts)");

    HlBlobCleanupOpts opts = {0};

    JSValue v;
    v = JS_GetPropertyStr(ctx, argv[0], "maxTotalSize");
    if (!JS_IsUndefined(v)) {
        int64_t n = 0; JS_ToInt64(ctx, &n, v);
        opts.max_total_size = (uint64_t)n;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, argv[0], "maxAge");
    if (!JS_IsUndefined(v)) {
        int64_t n = 0; JS_ToInt64(ctx, &n, v);
        opts.max_age_sec = (uint64_t)n;
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, argv[0], "strategy");
    opts.strategy = HL_BLOB_LRU;
    if (JS_IsString(v)) {
        const char *s = JS_ToCString(ctx, v);
        if (s && strcmp(s, "fifo") == 0) opts.strategy = HL_BLOB_FIFO;
        if (s) JS_FreeCString(ctx, s);
    }
    JS_FreeValue(ctx, v);

    v = JS_GetPropertyStr(ctx, argv[0], "dryRun");
    opts.dry_run = JS_ToBool(ctx, v);
    JS_FreeValue(ctx, v);

    uint64_t removed = 0, freed = 0;
    if (hl_cap_blob_cleanup(b, &opts, &removed, &freed) != 0)
        return JS_ThrowInternalError(ctx, "blob.cleanup: failed");

    JSValue out = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, out, "removed", JS_NewInt64(ctx, (int64_t)removed));
    JS_SetPropertyStr(ctx, out, "freedBytes", JS_NewInt64(ctx, (int64_t)freed));
    return out;
}

/* ── Module registration ─────────────────────────────────────────── */

static void register_writer_class(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(&hl_js_blob_writer_class_id);
    JS_NewClass(rt, hl_js_blob_writer_class_id, &js_blob_writer_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "write",
        JS_NewCFunction(ctx, js_writer_write, "write", 1));
    JS_SetPropertyStr(ctx, proto, "finalize",
        JS_NewCFunction(ctx, js_writer_finalize, "finalize", 0));
    JS_SetPropertyStr(ctx, proto, "abort",
        JS_NewCFunction(ctx, js_writer_abort, "abort", 0));
    JS_SetClassProto(ctx, hl_js_blob_writer_class_id, proto);
}

static void register_reader_class(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(&hl_js_blob_reader_class_id);
    JS_NewClass(rt, hl_js_blob_reader_class_id, &js_blob_reader_class);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, proto, "read",
        JS_NewCFunction(ctx, js_reader_read, "read", 1));
    JS_SetPropertyStr(ctx, proto, "close",
        JS_NewCFunction(ctx, js_reader_close, "close", 0));
    JS_SetClassProto(ctx, hl_js_blob_reader_class_id, proto);
}

static void register_store_class(JSContext *ctx)
{
    JSRuntime *rt = JS_GetRuntime(ctx);
    JS_NewClassID(&hl_js_blob_store_class_id);
    JS_NewClass(rt, hl_js_blob_store_class_id, &js_blob_store_class);
}

static int js_blob_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hl_js_check_module_declared(ctx, "hull/blob", "hull:blob") != 0)
        return -1;

    JSValue blob = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, blob, "init",
        JS_NewCFunction(ctx, js_blob_init, "init", 1));
    JS_SetPropertyStr(ctx, blob, "put",
        JS_NewCFunction(ctx, js_blob_put, "put", 1));
    JS_SetPropertyStr(ctx, blob, "putVerified",
        JS_NewCFunction(ctx, js_blob_put_verified, "putVerified", 2));
    JS_SetPropertyStr(ctx, blob, "writer",
        JS_NewCFunction(ctx, js_blob_writer_new, "writer", 1));
    JS_SetPropertyStr(ctx, blob, "get",
        JS_NewCFunction(ctx, js_blob_get, "get", 2));
    JS_SetPropertyStr(ctx, blob, "reader",
        JS_NewCFunction(ctx, js_blob_reader_new, "reader", 2));
    JS_SetPropertyStr(ctx, blob, "exists",
        JS_NewCFunction(ctx, js_blob_exists, "exists", 1));
    JS_SetPropertyStr(ctx, blob, "size",
        JS_NewCFunction(ctx, js_blob_size, "size", 1));
    JS_SetPropertyStr(ctx, blob, "atime",
        JS_NewCFunction(ctx, js_blob_atime, "atime", 1));
    JS_SetPropertyStr(ctx, blob, "delete",
        JS_NewCFunction(ctx, js_blob_delete, "delete", 1));
    JS_SetPropertyStr(ctx, blob, "iter",
        JS_NewCFunction(ctx, js_blob_iter, "iter", 0));
    JS_SetPropertyStr(ctx, blob, "totalSize",
        JS_NewCFunction(ctx, js_blob_total_size, "totalSize", 0));
    JS_SetPropertyStr(ctx, blob, "count",
        JS_NewCFunction(ctx, js_blob_count, "count", 0));
    JS_SetPropertyStr(ctx, blob, "cleanup",
        JS_NewCFunction(ctx, js_blob_cleanup, "cleanup", 1));

    JS_SetModuleExport(ctx, m, "blob", blob);
    return 0;
}

int hl_js_init_blob_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    register_store_class(ctx);
    register_writer_class(ctx);
    register_reader_class(ctx);

    JSModuleDef *m = JS_NewCModule(ctx, "hull:blob", js_blob_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "blob");
    return 0;
}
