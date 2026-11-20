/*
 * plain_value.c - QuickJS values to and from the build plugin channel's
 * value encoding (see plain_value.h).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "plain_value.h"
#include "mod_buffer.h"

#include <math.h>
#include <stdlib.h>

#define MAX_SAFE 9007199254740991.0   /* 2^53 - 1 */

typedef struct {
    JSClassID object, array;          /* a plain object's / an array's class */
} PvClasses;

static int fail(HlPvWriter *w, int err)
{
    if (!w->err) w->err = err;
    return w->err;
}

static int encode(JSContext *ctx, JSValueConst v, HlPvWriter *w,
                  const PvClasses *cls, int depth);

/* An own data property (JS_UNDEFINED when absent; -1 for an accessor). */
static int own_data(JSContext *ctx, JSValueConst obj, JSAtom a, JSValue *out)
{
    JSPropertyDescriptor d;
    *out = JS_UNDEFINED;
    int has = JS_GetOwnProperty(ctx, &d, obj, a);
    if (has < 0) { JS_FreeValue(ctx, JS_GetException(ctx)); return -1; }
    if (!has) return 0;
    JS_FreeValue(ctx, d.getter);
    JS_FreeValue(ctx, d.setter);
    if (d.flags & JS_PROP_GETSET) { JS_FreeValue(ctx, d.value); return -1; }
    *out = d.value;
    return 0;
}

static int encode_array(JSContext *ctx, JSValueConst v, HlPvWriter *w,
                        const PvClasses *cls, int depth)
{
    JSValue lv;
    JSAtom la = JS_NewAtom(ctx, "length");
    int rc = la == JS_ATOM_NULL ? -1 : own_data(ctx, v, la, &lv);
    if (la != JS_ATOM_NULL) JS_FreeAtom(ctx, la);
    if (rc != 0) return fail(w, HL_PV_E_TYPE);
    double d = 0;
    if (!JS_IsNumber(lv) || JS_ToFloat64(ctx, &d, lv) != 0 ||
        d < 0 || d > UINT32_MAX) {
        JS_FreeValue(ctx, lv);
        return fail(w, HL_PV_E_TYPE);
    }
    JS_FreeValue(ctx, lv);
    uint32_t n = (uint32_t)d;
    hl_pv_put_array(w, n);
    for (uint32_t i = 0; i < n && !w->err; i++) {
        JSAtom a = JS_NewAtomUInt32(ctx, i);
        if (a == JS_ATOM_NULL) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return fail(w, HL_PV_E_NOMEM);
        }
        JSValue e;
        rc = own_data(ctx, v, a, &e);
        JS_FreeAtom(ctx, a);
        /* A hole or a null / undefined element: an array never holds a
         * nil (a Lua array cannot). */
        if (rc != 0 || JS_IsUndefined(e) || JS_IsNull(e)) {
            JS_FreeValue(ctx, e);
            return fail(w, HL_PV_E_TYPE);
        }
        encode(ctx, e, w, cls, depth + 1);
        JS_FreeValue(ctx, e);
    }
    return w->err;
}

static int encode_map(JSContext *ctx, JSValueConst v, HlPvWriter *w,
                      const PvClasses *cls, int depth)
{
    JSPropertyEnum *tab = NULL;
    uint32_t n = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &n, v,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return fail(w, HL_PV_E_TYPE);
    }
    size_t header = w->len;
    hl_pv_put_map(w, n);
    size_t start = w->len;
    size_t *ends = malloc((n ? (size_t)n : 1) * sizeof *ends);
    if (!ends) {
        JS_FreePropertyEnum(ctx, tab, n);
        return fail(w, HL_PV_E_NOMEM);
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n && !w->err; i++) {
        JSValue e;
        if (own_data(ctx, v, tab[i].atom, &e) != 0) {
            fail(w, HL_PV_E_TYPE);
            break;
        }
        if (JS_IsUndefined(e) || JS_IsNull(e)) continue;   /* absent */
        size_t kl = 0;
        const char *k = JS_AtomToCStringLen(ctx, &kl, tab[i].atom);
        if (!k) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, e);
            fail(w, HL_PV_E_NOMEM);
            break;
        }
        hl_pv_put_key(w, k, kl);
        JS_FreeCString(ctx, k);
        encode(ctx, e, w, cls, depth + 1);
        JS_FreeValue(ctx, e);
        ends[kept++] = w->len;
    }
    JS_FreePropertyEnum(ctx, tab, n);
    if (!w->err) {
        hl_pv_set_count(w, header, kept);
        hl_pv_sort_map(w, start, ends, kept);
    }
    free(ends);
    return w->err;
}

static int encode(JSContext *ctx, JSValueConst v, HlPvWriter *w,
                  const PvClasses *cls, int depth)
{
    if (w->err) return w->err;
    if (JS_IsUndefined(v) || JS_IsNull(v)) { hl_pv_put_nil(w); return w->err; }
    if (JS_IsBool(v)) { hl_pv_put_bool(w, JS_ToBool(ctx, v)); return w->err; }
    if (JS_IsNumber(v)) {
        double d = 0;
        if (JS_ToFloat64(ctx, &d, v) != 0) {   /* a number: runs no app code */
            JS_FreeValue(ctx, JS_GetException(ctx));
            return fail(w, HL_PV_E_TYPE);
        }
        if (!(d >= -MAX_SAFE && d <= MAX_SAFE) || d != floor(d))
            return fail(w, HL_PV_E_TYPE);
        hl_pv_put_int(w, (int64_t)d);
        return w->err;
    }
    if (JS_IsBigInt(ctx, v)) {
        int64_t i = 0;
        if (JS_ToBigInt64(ctx, &i, v) != 0) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return fail(w, HL_PV_E_TYPE);
        }
        JSValue back = JS_NewBigInt64(ctx, i);   /* wrapped modulo 2^64? */
        int same = !JS_IsException(back) && JS_StrictEq(ctx, back, v);
        if (JS_IsException(back)) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, back);
        if (!same) return fail(w, HL_PV_E_TYPE);
        hl_pv_put_int(w, i);
        return w->err;
    }
    if (JS_IsString(v)) {
        size_t n = 0;
        const char *s = JS_ToCStringLen(ctx, &n, v);
        if (!s) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return fail(w, HL_PV_E_NOMEM);
        }
        hl_pv_put_text(w, s, n);   /* a lone surrogate is not UTF-8 */
        JS_FreeCString(ctx, s);
        return w->err;
    }
    if (!JS_IsObject(v)) return fail(w, HL_PV_E_TYPE);
    size_t bn = 0;
    const uint8_t *b = hl_js_bytes_view(ctx, &bn, v);
    if (b) {
        hl_pv_put_bytes(w, b, bn);   /* copied before any app code can run */
        return w->err;
    }
    if (depth >= HL_PV_MAX_DEPTH) return fail(w, HL_PV_E_DEPTH);
    JSClassID id = JS_GetClassID(v);
    if (id == cls->array) return encode_array(ctx, v, w, cls, depth);
    if (id == cls->object) return encode_map(ctx, v, w, cls, depth);
    return fail(w, HL_PV_E_TYPE);   /* a Proxy, a function, a Date, ... */
}

int hl_js_pv_encode(JSContext *ctx, JSValueConst v, HlPvWriter *w)
{
    PvClasses cls;
    JSValue o = JS_NewObject(ctx), a = JS_NewArray(ctx);
    int ok = !JS_IsException(o) && !JS_IsException(a);
    if (ok) {
        cls.object = JS_GetClassID(o);
        cls.array = JS_GetClassID(a);
    }
    JS_FreeValue(ctx, o);
    JS_FreeValue(ctx, a);
    if (!ok) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        return fail(w, HL_PV_E_NOMEM);
    }
    return encode(ctx, v, w, &cls, w->depth);
}

static JSValue decode(JSContext *ctx, HlPvReader *r)
{
    HlPvItem it;
    (void)hl_pv_next(r, &it);              /* checked: cannot fail */
    switch (it.type) {
    case HL_PV_NIL:   return JS_NULL;
    case HL_PV_FALSE: return JS_FALSE;
    case HL_PV_TRUE:  return JS_TRUE;
    case HL_PV_INT:
        if (it.i >= -(int64_t)MAX_SAFE && it.i <= (int64_t)MAX_SAFE)
            return JS_NewInt64(ctx, it.i);
        return JS_NewBigInt64(ctx, it.i);
    case HL_PV_TEXT:
        return JS_NewStringLen(ctx, (const char *)it.s, it.n);
    case HL_PV_BYTES:
        return JS_NewArrayBufferCopy(ctx, it.s, it.n);
    case HL_PV_ARRAY: {
        JSValue arr = JS_NewArray(ctx);
        if (JS_IsException(arr)) return arr;
        for (size_t i = 0; i < it.n; i++) {
            JSValue e = decode(ctx, r);
            if (JS_IsException(e) ||
                JS_DefinePropertyValueUint32(ctx, arr, (uint32_t)i, e,
                                             JS_PROP_C_W_E) < 0) {
                JS_FreeValue(ctx, arr);
                return JS_EXCEPTION;
            }
        }
        return arr;
    }
    case HL_PV_MAP: {
        JSValue obj = JS_NewObject(ctx);
        if (JS_IsException(obj)) return obj;
        for (size_t i = 0; i < it.n; i++) {
            const uint8_t *k;
            size_t kn;
            (void)hl_pv_next_key(r, &k, &kn);
            JSAtom a = JS_NewAtomLen(ctx, (const char *)k, kn);
            JSValue e = a == JS_ATOM_NULL ? JS_EXCEPTION : decode(ctx, r);
            /* Defined, not set: a "__proto__" key is an own property. */
            int bad = JS_IsException(e) ||
                      JS_DefinePropertyValue(ctx, obj, a, e, JS_PROP_C_W_E) < 0;
            if (a != JS_ATOM_NULL) JS_FreeAtom(ctx, a);
            if (bad) {
                JS_FreeValue(ctx, obj);
                return JS_EXCEPTION;
            }
        }
        return obj;
    }
    }
    return JS_EXCEPTION;
}

JSValue hl_js_pv_decode(JSContext *ctx, const void *buf, size_t len, int *err)
{
    *err = hl_pv_check(buf, len);
    if (*err != HL_PV_OK) return JS_EXCEPTION;
    HlPvReader r;
    hl_pv_reader_init(&r, buf, len);
    JSValue v = decode(ctx, &r);
    if (JS_IsException(v)) *err = HL_PV_E_NOMEM;
    return v;
}
