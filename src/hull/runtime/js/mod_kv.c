/*
 * mod_kv.c: QuickJS native binding for the app-facing KV connection cap
 * (cap/kv.c). Registers the internal module "hull:kv:_native" exporting
 * open(dsn) -> connection object. The stdlib valkey backend (hull:kv:_valkey)
 * is the sole importer; apps reach it through hull:kv / hull:cache with
 * backend="valkey".
 *
 * Binary safety + borrow-copy: keys and values cross the C boundary as
 * ArrayBuffers (js_get_buffer extracts bytes without UTF-8 mangling; the stdlib
 * wrapper converts to/from the byte-string kv API). get() returns the value the
 * backend BORROWS into its receive buffer as a FRESH ArrayBuffer via
 * JS_NewArrayBufferCopy - a copy into runtime-owned memory BEFORE returning, so
 * the script value survives any later op on the same connection. Regression-
 * tested in tests/e2e_valkey.sh (get -> another op -> first value unchanged).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"        /* get_hl_js, js_get_buffer, HlBufferView */
#include "hull/cap/kv.h"

#include <string.h>

static JSClassID hull_kv_conn_class_id;

static void js_kv_conn_finalizer(JSRuntime *rt, JSValue val)
{
    (void)rt;
    HlKvConn *c = (HlKvConn *)JS_GetOpaque(val, hull_kv_conn_class_id);
    if (c) hl_cap_kv_close(c);
}

static JSClassDef js_kv_conn_class = {
    "hull.kv.conn",
    .finalizer = js_kv_conn_finalizer,
};

static HlKvConn *kv_self(JSContext *ctx, JSValueConst this_val)
{
    HlKvConn *c = (HlKvConn *)JS_GetOpaque(this_val, hull_kv_conn_class_id);
    if (!c) { JS_ThrowTypeError(ctx, "kv: connection is closed"); return NULL; }
    return c;
}

/* Extract a byte buffer from a JS value (ArrayBuffer/typed array/MappedBuffer/
 * WasmBuffer/string). On success fills *view and *cstr/*needs_free (free with
 * kv_arg_free). Returns 1, or 0 after throwing. */
static int kv_arg(JSContext *ctx, JSValueConst v, const char *what,
                  HlBufferView *view, const char **cstr, int *needs_free)
{
    if (!js_get_buffer(ctx, v, view, cstr, needs_free)) {
        JS_ThrowTypeError(ctx, "kv: %s must be a buffer", what);
        return 0;
    }
    return 1;
}
static void kv_arg_free(JSContext *ctx, const char *cstr, int needs_free)
{
    if (needs_free && cstr) JS_FreeCString(ctx, cstr);
}

/* Optional TTL in milliseconds at argv[i] (undefined/absent/null -> 0). */
static int64_t kv_opt_ttl_ms(JSContext *ctx, int argc, JSValueConst *argv, int i)
{
    if (i >= argc || JS_IsUndefined(argv[i]) || JS_IsNull(argv[i])) return 0;
    int64_t t = 0;
    JS_ToInt64(ctx, &t, argv[i]);
    return t;
}

/* conn.get(keyBuf) -> ArrayBuffer | null (COPY of the borrowed value). */
static JSValue js_kv_get(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    HlKvConn *c = kv_self(ctx, this_val); if (!c) return JS_EXCEPTION;
    HlBufferView kv; const char *ks; int kf;
    if (argc < 1 || !kv_arg(ctx, argv[0], "key", &kv, &ks, &kf)) return JS_EXCEPTION;
    const uint8_t *val; size_t vlen; int found = 0;
    int rc = hl_cap_kv_get(c, (const uint8_t *)kv.data, kv.len, &val, &vlen, &found);
    kv_arg_free(ctx, ks, kf);
    if (rc != 0) return JS_ThrowInternalError(ctx, "%s", hl_cap_kv_error(c));
    if (!found) return JS_NULL;
    return JS_NewArrayBufferCopy(ctx, val, vlen);   /* COPY: borrow-copy guard */
}

/* conn.set(keyBuf, valBuf, ttlMs?) -> true. */
static JSValue js_kv_set(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    HlKvConn *c = kv_self(ctx, this_val); if (!c) return JS_EXCEPTION;
    HlBufferView kb, vb; const char *ks, *vs; int kf, vf;
    if (argc < 2 || !kv_arg(ctx, argv[0], "key", &kb, &ks, &kf)) return JS_EXCEPTION;
    if (!kv_arg(ctx, argv[1], "value", &vb, &vs, &vf)) { kv_arg_free(ctx, ks, kf); return JS_EXCEPTION; }
    int64_t ttl = kv_opt_ttl_ms(ctx, argc, argv, 2);
    int rc = hl_cap_kv_set(c, (const uint8_t *)kb.data, kb.len,
                           (const uint8_t *)vb.data, vb.len, ttl);
    kv_arg_free(ctx, ks, kf); kv_arg_free(ctx, vs, vf);
    if (rc != 0) return JS_ThrowInternalError(ctx, "%s", hl_cap_kv_error(c));
    return JS_TRUE;
}

/* conn.del(keyBuf) -> bool. */
static JSValue js_kv_del(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    HlKvConn *c = kv_self(ctx, this_val); if (!c) return JS_EXCEPTION;
    HlBufferView kb; const char *ks; int kf;
    if (argc < 1 || !kv_arg(ctx, argv[0], "key", &kb, &ks, &kf)) return JS_EXCEPTION;
    int deleted = 0;
    int rc = hl_cap_kv_del(c, (const uint8_t *)kb.data, kb.len, &deleted);
    kv_arg_free(ctx, ks, kf);
    if (rc != 0) return JS_ThrowInternalError(ctx, "%s", hl_cap_kv_error(c));
    return JS_NewBool(ctx, deleted);
}

/* conn.has(keyBuf) -> bool. */
static JSValue js_kv_has(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    HlKvConn *c = kv_self(ctx, this_val); if (!c) return JS_EXCEPTION;
    HlBufferView kb; const char *ks; int kf;
    if (argc < 1 || !kv_arg(ctx, argv[0], "key", &kb, &ks, &kf)) return JS_EXCEPTION;
    int present = 0;
    int rc = hl_cap_kv_exists(c, (const uint8_t *)kb.data, kb.len, &present);
    kv_arg_free(ctx, ks, kf);
    if (rc != 0) return JS_ThrowInternalError(ctx, "%s", hl_cap_kv_error(c));
    return JS_NewBool(ctx, present);
}

/* conn.incr(keyBuf, by, ttlMs?) -> number. */
static JSValue js_kv_incr(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    HlKvConn *c = kv_self(ctx, this_val); if (!c) return JS_EXCEPTION;
    HlBufferView kb; const char *ks; int kf;
    if (argc < 2 || !kv_arg(ctx, argv[0], "key", &kb, &ks, &kf)) return JS_EXCEPTION;
    int64_t by = 0; JS_ToInt64(ctx, &by, argv[1]);
    int64_t ttl = kv_opt_ttl_ms(ctx, argc, argv, 2);
    int64_t nv = 0;
    int rc = hl_cap_kv_incr(c, (const uint8_t *)kb.data, kb.len, by, ttl, &nv);
    kv_arg_free(ctx, ks, kf);
    if (rc != 0) return JS_ThrowInternalError(ctx, "%s", hl_cap_kv_error(c));
    return JS_NewInt64(ctx, nv);
}

/* conn.cas(keyBuf, expectedBuf|null, newBuf, ttlMs?) -> 0 ok / 1 mismatch /
 * 2 conflict. A transport ERROR throws. */
static JSValue js_kv_cas(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    HlKvConn *c = kv_self(ctx, this_val); if (!c) return JS_EXCEPTION;
    HlBufferView kb, eb, nb; const char *ks, *es = NULL, *ns; int kf, ef = 0, nf;
    if (argc < 3 || !kv_arg(ctx, argv[0], "key", &kb, &ks, &kf)) return JS_EXCEPTION;
    int has_expected = !JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1]);
    if (has_expected && !kv_arg(ctx, argv[1], "expected", &eb, &es, &ef)) {
        kv_arg_free(ctx, ks, kf); return JS_EXCEPTION;
    }
    if (!kv_arg(ctx, argv[2], "value", &nb, &ns, &nf)) {
        kv_arg_free(ctx, ks, kf); if (has_expected) kv_arg_free(ctx, es, ef);
        return JS_EXCEPTION;
    }
    int64_t ttl = kv_opt_ttl_ms(ctx, argc, argv, 3);
    HlKvCasResult r = hl_cap_kv_cas(c, (const uint8_t *)kb.data, kb.len,
                                    has_expected ? (const uint8_t *)eb.data : NULL,
                                    has_expected ? eb.len : 0, has_expected,
                                    (const uint8_t *)nb.data, nb.len, ttl);
    kv_arg_free(ctx, ks, kf); if (has_expected) kv_arg_free(ctx, es, ef); kv_arg_free(ctx, ns, nf);
    if (r == HL_KV_CAS_ERROR) return JS_ThrowInternalError(ctx, "%s", hl_cap_kv_error(c));
    return JS_NewInt32(ctx, (int)r);
}

/* conn.clear(prefixBuf) -> number removed. */
static JSValue js_kv_clear(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    HlKvConn *c = kv_self(ctx, this_val); if (!c) return JS_EXCEPTION;
    HlBufferView pb; const char *ps; int pf;
    if (argc < 1 || !kv_arg(ctx, argv[0], "prefix", &pb, &ps, &pf)) return JS_EXCEPTION;
    int64_t removed = 0;
    int rc = hl_cap_kv_clear(c, (const uint8_t *)pb.data, pb.len, &removed);
    kv_arg_free(ctx, ps, pf);
    if (rc != 0) return JS_ThrowInternalError(ctx, "%s", hl_cap_kv_error(c));
    return JS_NewInt64(ctx, removed);
}

/* scan collector: append each borrowed key as a fresh ArrayBuffer (copy) into a
 * JS array. */
struct kv_scan_js { JSContext *ctx; JSValue arr; uint32_t n; int err; };
static int kv_scan_js_cb(void *ctx_, const uint8_t *key, size_t klen)
{
    struct kv_scan_js *s = (struct kv_scan_js *)ctx_;
    JSValue ab = JS_NewArrayBufferCopy(s->ctx, key, klen);   /* COPY inside cb */
    if (JS_IsException(ab)) { s->err = 1; return 1; }
    JS_SetPropertyUint32(s->ctx, s->arr, s->n++, ab);
    return 0;
}

/* conn.scan(prefixBuf, limit?) -> Array<ArrayBuffer> (prefix-stripped keys). */
static JSValue js_kv_scan(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    HlKvConn *c = kv_self(ctx, this_val); if (!c) return JS_EXCEPTION;
    HlBufferView pb; const char *ps; int pf;
    if (argc < 1 || !kv_arg(ctx, argv[0], "prefix", &pb, &ps, &pf)) return JS_EXCEPTION;
    int64_t limit = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) JS_ToInt64(ctx, &limit, argv[1]);
    struct kv_scan_js s = { ctx, JS_NewArray(ctx), 0, 0 };
    int rc = hl_cap_kv_scan(c, (const uint8_t *)pb.data, pb.len, (size_t)limit, kv_scan_js_cb, &s);
    kv_arg_free(ctx, ps, pf);
    if (rc != 0 || s.err) { JS_FreeValue(ctx, s.arr); return JS_ThrowInternalError(ctx, "%s", hl_cap_kv_error(c)); }
    return s.arr;
}

/* conn.caps() -> number bitmask. */
static JSValue js_kv_caps(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlKvConn *c = kv_self(ctx, this_val); if (!c) return JS_EXCEPTION;
    return JS_NewInt64(ctx, (int64_t)hl_cap_kv_caps(c));
}

/* conn.backendName() -> string. */
static JSValue js_kv_backend_name(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlKvConn *c = kv_self(ctx, this_val); if (!c) return JS_EXCEPTION;
    return JS_NewString(ctx, hl_cap_kv_backend_name(c));
}

/* conn.close() -> undefined (idempotent). */
static JSValue js_kv_close(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    HlKvConn *c = (HlKvConn *)JS_GetOpaque(this_val, hull_kv_conn_class_id);
    if (c) { hl_cap_kv_close(c); JS_SetOpaque(this_val, NULL); }
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_kv_conn_methods[] = {
    JS_CFUNC_DEF("get", 1, js_kv_get),
    JS_CFUNC_DEF("set", 3, js_kv_set),
    JS_CFUNC_DEF("del", 1, js_kv_del),
    JS_CFUNC_DEF("has", 1, js_kv_has),
    JS_CFUNC_DEF("incr", 3, js_kv_incr),
    JS_CFUNC_DEF("cas", 4, js_kv_cas),
    JS_CFUNC_DEF("clear", 1, js_kv_clear),
    JS_CFUNC_DEF("scan", 2, js_kv_scan),
    JS_CFUNC_DEF("caps", 0, js_kv_caps),
    JS_CFUNC_DEF("backendName", 0, js_kv_backend_name),
    JS_CFUNC_DEF("close", 0, js_kv_close),
};

/* _native.open(dsn [, timeoutMs]) -> connection object. */
static JSValue js_kv_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsString(argv[0]))
        return JS_ThrowTypeError(ctx, "kv.open requires a dsn string");
    const char *dsn = JS_ToCString(ctx, argv[0]);
    if (!dsn) return JS_EXCEPTION;
    int timeout_ms = 0;
    if (argc > 1 && !JS_IsUndefined(argv[1])) { int64_t t = 0; JS_ToInt64(ctx, &t, argv[1]); timeout_ms = (int)t; }

    HlJS *js = get_hl_js(ctx);
    char err[256];
    HlKvConn *c = NULL;
    int rc = hl_cap_kv_open(&c, dsn, timeout_ms, js ? js->base.alloc : NULL, err, sizeof err);
    JS_FreeCString(ctx, dsn);
    if (rc != 0) return JS_ThrowInternalError(ctx, "%s", err);

    JSValue obj = JS_NewObjectClass(ctx, (int)hull_kv_conn_class_id);
    if (JS_IsException(obj)) { hl_cap_kv_close(c); return obj; }
    JS_SetOpaque(obj, c);
    return obj;
}

static int js_kv_module_init(JSContext *ctx, JSModuleDef *m)
{
    if (hull_kv_conn_class_id == 0) {
        JS_NewClassID(&hull_kv_conn_class_id);
        JS_NewClass(JS_GetRuntime(ctx), hull_kv_conn_class_id, &js_kv_conn_class);
        JSValue proto = JS_NewObject(ctx);
        JS_SetPropertyFunctionList(ctx, proto, js_kv_conn_methods,
                                   (int)(sizeof js_kv_conn_methods / sizeof js_kv_conn_methods[0]));
        JS_SetClassProto(ctx, hull_kv_conn_class_id, proto);
    }
    JS_SetModuleExport(ctx, m, "open", JS_NewCFunction(ctx, js_kv_open, "open", 1));
    return 0;
}

int hl_js_init_kv_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    JSModuleDef *m = JS_NewCModule(ctx, "hull:kv:_native", js_kv_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "open");
    return 0;
}
