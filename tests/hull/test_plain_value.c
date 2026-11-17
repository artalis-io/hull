/*
 * test_plain_value.c - the build plugin channel's value encoding
 * (shared/plain_value.c) and its Lua and QuickJS walkers: canonical form,
 * hostile encodings, refusals, no app code run by an encode, and the same
 * value from either runtime encoding to the same bytes.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"

#include "hull/shared/plain_value.h"
#include "../../src/hull/runtime/lua/plain_value.h"
#include "../../src/hull/runtime/js/plain_value.h"

#include "lauxlib.h"
#include "lualib.h"

#include <string.h>

/* ── The codec ──────────────────────────────────────────────────────── */

UTEST(plain_value, scalars_round_trip)
{
    HlPvWriter w;
    hl_pv_writer_init(&w, 1024);
    hl_pv_put_array(&w, 6);
    hl_pv_put_nil(&w);
    hl_pv_put_bool(&w, 1);
    hl_pv_put_int(&w, INT64_MIN);
    hl_pv_put_text(&w, "h\xc3\xa9", 3);
    hl_pv_put_bytes(&w, "\0\xff", 2);
    hl_pv_put_map(&w, 0);
    ASSERT_EQ(HL_PV_OK, w.err);
    ASSERT_EQ(HL_PV_OK, hl_pv_check(w.buf, w.len));
    HlPvReader r;
    HlPvItem it;
    hl_pv_reader_init(&r, w.buf, w.len);
    ASSERT_EQ(0, hl_pv_next(&r, &it)); ASSERT_EQ(HL_PV_ARRAY, it.type); ASSERT_EQ(6u, it.n);
    ASSERT_EQ(0, hl_pv_next(&r, &it)); ASSERT_EQ(HL_PV_NIL, it.type);
    ASSERT_EQ(0, hl_pv_next(&r, &it)); ASSERT_EQ(HL_PV_TRUE, it.type);
    ASSERT_EQ(0, hl_pv_next(&r, &it)); ASSERT_EQ(HL_PV_INT, it.type); ASSERT_TRUE(it.i == INT64_MIN);
    ASSERT_EQ(0, hl_pv_next(&r, &it)); ASSERT_EQ(HL_PV_TEXT, it.type); ASSERT_EQ(3u, it.n);
    ASSERT_EQ(0, hl_pv_next(&r, &it)); ASSERT_EQ(HL_PV_BYTES, it.type); ASSERT_EQ(0xff, it.s[1]);
    ASSERT_EQ(0, hl_pv_next(&r, &it)); ASSERT_EQ(HL_PV_MAP, it.type); ASSERT_EQ(0u, it.n);
    ASSERT_EQ(w.len, r.pos);
    hl_pv_writer_free(&w);
}

UTEST(plain_value, writer_refusals)
{
    HlPvWriter w;
    hl_pv_writer_init(&w, 1024);
    hl_pv_put_text(&w, "\xff", 1);
    ASSERT_EQ(HL_PV_E_TEXT, w.err);
    hl_pv_put_nil(&w);                       /* sticky */
    ASSERT_EQ(HL_PV_E_TEXT, w.err);
    hl_pv_writer_free(&w);

    hl_pv_writer_init(&w, 8);
    hl_pv_put_bytes(&w, "0123456789", 10);
    ASSERT_EQ(HL_PV_E_TOO_BIG, w.err);
    ASSERT_STREQ("too_big", hl_pv_error_name(w.err));
    hl_pv_writer_free(&w);

    hl_pv_writer_init(&w, 1024);
    hl_pv_put_key(&w, "\xed\xa0\x80", 3);   /* a surrogate */
    ASSERT_EQ(HL_PV_E_TEXT, w.err);
    hl_pv_writer_free(&w);
}

UTEST(plain_value, sort_map_in_place)
{
    HlPvWriter w;
    hl_pv_writer_init(&w, 1024);
    hl_pv_put_map(&w, 3);
    size_t start = w.len, ends[3];
    hl_pv_put_key(&w, "b", 1); hl_pv_put_int(&w, 2); ends[0] = w.len;
    hl_pv_put_key(&w, "ab", 2); hl_pv_put_text(&w, "x", 1); ends[1] = w.len;
    hl_pv_put_key(&w, "a", 1); hl_pv_put_bool(&w, 0); ends[2] = w.len;
    ASSERT_EQ(HL_PV_E_SHAPE, hl_pv_check(w.buf, w.len));   /* unsorted */
    hl_pv_sort_map(&w, start, ends, 3);
    ASSERT_EQ(HL_PV_OK, w.err);
    ASSERT_EQ(HL_PV_OK, hl_pv_check(w.buf, w.len));
    HlPvReader r;
    HlPvItem it;
    const uint8_t *k;
    size_t kn;
    hl_pv_reader_init(&r, w.buf, w.len);
    hl_pv_next(&r, &it);
    hl_pv_next_key(&r, &k, &kn); ASSERT_EQ(1u, kn); ASSERT_EQ('a', k[0]);
    hl_pv_next(&r, &it); ASSERT_EQ(HL_PV_FALSE, it.type);
    hl_pv_next_key(&r, &k, &kn); ASSERT_EQ(2u, kn);
    hl_pv_next(&r, &it);
    hl_pv_next_key(&r, &k, &kn); ASSERT_EQ('b', k[0]);
    hl_pv_writer_free(&w);

    hl_pv_writer_init(&w, 1024);                            /* a duplicate */
    hl_pv_put_map(&w, 2);
    start = w.len;
    hl_pv_put_key(&w, "a", 1); hl_pv_put_nil(&w); ends[0] = w.len;
    hl_pv_put_key(&w, "a", 1); hl_pv_put_nil(&w); ends[1] = w.len;
    hl_pv_sort_map(&w, start, ends, 2);
    ASSERT_EQ(HL_PV_E_SHAPE, w.err);
    hl_pv_writer_free(&w);
}

UTEST(plain_value, check_refuses_hostile_encodings)
{
    static const struct { const char *name; const char *b; size_t n; int want; } cases[] = {
        { "empty",            "", 0, HL_PV_E_SHAPE },
        { "unknown tag",      "\x08", 1, HL_PV_E_SHAPE },
        { "trailing byte",    "\x00\x00", 2, HL_PV_E_SHAPE },
        { "short int",        "\x03\x01\x02", 3, HL_PV_E_SHAPE },
        { "text past end",    "\x04\x05\x00\x00\x00" "ab", 7, HL_PV_E_SHAPE },
        { "huge count",       "\x06\xff\xff\xff\xff\x00", 6, HL_PV_E_SHAPE },
        { "map count > room", "\x07\x02\x00\x00\x00" "\x01\x00\x00\x00" "a\x00", 12, HL_PV_E_SHAPE },
        { "bad text",         "\x04\x01\x00\x00\x00\xc0", 6, HL_PV_E_TEXT },
        { "bad key",          "\x07\x01\x00\x00\x00\x01\x00\x00\x00\xff\x00", 11, HL_PV_E_TEXT },
        { "equal keys",       "\x07\x02\x00\x00\x00\x01\x00\x00\x00" "a\x00\x01\x00\x00\x00" "a\x00", 19, HL_PV_E_SHAPE },
        { "descending keys",  "\x07\x02\x00\x00\x00\x01\x00\x00\x00" "b\x00\x01\x00\x00\x00" "a\x00", 19, HL_PV_E_SHAPE },
        { "bytes may be any", "\x05\x01\x00\x00\x00\xff", 6, HL_PV_OK },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int rc = hl_pv_check(cases[i].b, cases[i].n);
        if (rc != cases[i].want) printf("  case: %s\n", cases[i].name);
        ASSERT_EQ(cases[i].want, rc);
    }
    /* Depth: 64 nested arrays pass, 65 are refused. */
    uint8_t deep[66 * 5 + 1];
    for (int d = 64; d <= 65; d++) {
        size_t n = 0;
        for (int i = 0; i < d; i++) {
            memcpy(deep + n, "\x06\x01\x00\x00\x00", 5);
            n += 5;
        }
        deep[n++] = 0;
        ASSERT_EQ(d == 64 ? HL_PV_OK : HL_PV_E_DEPTH, hl_pv_check(deep, n));
    }
}

/* ── Lua ────────────────────────────────────────────────────────────── */

static lua_State *lua_new(void)
{
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    return L;
}

/* Run @p src (returning one value) and encode it. */
static int lua_enc(lua_State *L, const char *src, HlPvWriter *w)
{
    if (luaL_dostring(L, src) != LUA_OK) {
        printf("lua: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -100;
    }
    hl_pv_writer_init(w, 1 << 20);
    int rc = hl_lua_pv_encode(L, -1, w);
    lua_pop(L, 1);
    return rc;
}

UTEST(plain_value, lua_round_trip)
{
    lua_State *L = lua_new();
    HlPvWriter w;
    ASSERT_EQ(HL_PV_OK, lua_enc(L, "return { z = { 1, 2, { k = 'v' } }, a = true, "
                                  "m = math.mininteger, e = {}, s = 'h\\u{e9}' }", &w));
    ASSERT_EQ(HL_PV_OK, hl_pv_check(w.buf, w.len));
    ASSERT_EQ(HL_PV_OK, hl_lua_pv_decode(L, w.buf, w.len));
    HlPvWriter w2;
    hl_pv_writer_init(&w2, 1 << 20);
    ASSERT_EQ(HL_PV_OK, hl_lua_pv_encode(L, -1, &w2));
    ASSERT_EQ(w.len, w2.len);
    ASSERT_EQ(0, memcmp(w.buf, w2.buf, w.len));
    lua_getfield(L, -1, "z");
    lua_rawgeti(L, -1, 3);
    lua_getfield(L, -1, "k");
    ASSERT_STREQ("v", lua_tostring(L, -1));
    hl_pv_writer_free(&w);
    hl_pv_writer_free(&w2);
    lua_close(L);
}

UTEST(plain_value, lua_bytes_box)
{
    lua_State *L = lua_new();
    lua_pushlstring(L, "\0\xff", 2);
    hl_lua_pv_push_bytes(L, -1);
    HlPvWriter w;
    hl_pv_writer_init(&w, 64);
    ASSERT_EQ(HL_PV_OK, hl_lua_pv_encode(L, -1, &w));
    ASSERT_EQ(HL_PV_BYTES, w.buf[0]);
    ASSERT_EQ(HL_PV_OK, hl_lua_pv_decode(L, w.buf, w.len));
    size_t n;
    const char *s = lua_tolstring(L, -1, &n);
    ASSERT_EQ(2u, n);
    ASSERT_EQ(0, memcmp(s, "\0\xff", 2));
    /* Without the box, the same bytes are not text. */
    lua_pushlstring(L, "\0\xff", 2);
    HlPvWriter w2;
    hl_pv_writer_init(&w2, 64);
    ASSERT_EQ(HL_PV_E_TEXT, hl_lua_pv_encode(L, -1, &w2));
    hl_pv_writer_free(&w);
    hl_pv_writer_free(&w2);
    lua_close(L);
}

UTEST(plain_value, lua_refusals)
{
    static const char *const bad[] = {
        "return 1.5",
        "return print",
        "return { 1, nil, 3 }",
        "return { [0] = 1 }",
        "return { 1, k = 2 }",
        "return { [true] = 1 }",
        "return setmetatable({}, {})",
        "return setmetatable({}, { __pairs = function() error('ran') end })",
        "local t = {}; t.t = t; return t",
        "return io.stdout",
        "return coroutine.create(print)",
    };
    lua_State *L = lua_new();
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        HlPvWriter w;
        int rc = lua_enc(L, bad[i], &w);
        if (rc == HL_PV_OK || rc == -100) printf("  accepted: %s\n", bad[i]);
        ASSERT_TRUE(rc != HL_PV_OK && rc != -100);
        hl_pv_writer_free(&w);
    }
    lua_close(L);
}

/* ── QuickJS ────────────────────────────────────────────────────────── */

typedef struct { JSRuntime *rt; JSContext *ctx; } Js;

static Js js_new(void)
{
    Js j;
    j.rt = JS_NewRuntime();
    j.ctx = JS_NewContext(j.rt);
    return j;
}

static void js_close(Js *j)
{
    JS_FreeContext(j->ctx);
    JS_FreeRuntime(j->rt);
}

static JSValue js_eval(Js *j, const char *src)
{
    JSValue v = JS_Eval(j->ctx, src, strlen(src), "<test>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(v)) {
        JSValue e = JS_GetException(j->ctx);
        const char *s = JS_ToCString(j->ctx, e);
        printf("js: %s\n", s ? s : "?");
        JS_FreeCString(j->ctx, s);
        JS_FreeValue(j->ctx, e);
    }
    return v;
}

static int js_enc(Js *j, const char *src, HlPvWriter *w)
{
    JSValue v = js_eval(j, src);
    if (JS_IsException(v)) return -100;
    hl_pv_writer_init(w, 1 << 20);
    int rc = hl_js_pv_encode(j->ctx, v, w);
    JS_FreeValue(j->ctx, v);
    return rc;
}

static int js_global_true(Js *j, const char *name)
{
    JSValue g = JS_GetGlobalObject(j->ctx);
    JSValue v = JS_GetPropertyStr(j->ctx, g, name);
    int t = JS_ToBool(j->ctx, v);
    JS_FreeValue(j->ctx, v);
    JS_FreeValue(j->ctx, g);
    return t;
}

UTEST(plain_value, js_round_trip)
{
    Js j = js_new();
    HlPvWriter w;
    ASSERT_EQ(HL_PV_OK, js_enc(&j, "({ z: [1, 2, { k: 'v' }], a: true, gone: null, "
                                   "m: -(2n ** 63n), e: {}, s: 'h\\u00e9', "
                                   "b: new Uint8Array([0, 255]), "
                                   "['__proto__']: 1 })", &w));
    ASSERT_EQ(HL_PV_OK, hl_pv_check(w.buf, w.len));
    int err = 0;
    JSValue v = hl_js_pv_decode(j.ctx, w.buf, w.len, &err);
    ASSERT_EQ(HL_PV_OK, err);
    HlPvWriter w2;
    hl_pv_writer_init(&w2, 1 << 20);
    ASSERT_EQ(HL_PV_OK, hl_js_pv_encode(j.ctx, v, &w2));
    ASSERT_EQ(w.len, w2.len);
    ASSERT_EQ(0, memcmp(w.buf, w2.buf, w.len));
    /* bytes come back as an ArrayBuffer, a big int as a BigInt, and
     * "__proto__" as an own property */
    JSValue g = JS_GetGlobalObject(j.ctx);
    JS_SetPropertyStr(j.ctx, g, "decoded", v);
    JS_FreeValue(j.ctx, g);
    JSValue t = js_eval(&j, "decoded.b instanceof ArrayBuffer && decoded.b.byteLength === 2 && "
                            "typeof decoded.m === 'bigint' && !('gone' in decoded) && "
                            "Object.getOwnPropertyNames(decoded).includes('__proto__') && "
                            "Object.getPrototypeOf(decoded) === Object.prototype");
    ASSERT_TRUE(JS_ToBool(j.ctx, t));
    hl_pv_writer_free(&w);
    hl_pv_writer_free(&w2);
    js_close(&j);
}

UTEST(plain_value, js_refusals_run_no_app_code)
{
    static const char *const bad[] = {
        "1.5", "NaN", "2 ** 53", "2n ** 63n", "Symbol('x')", "(function () {})",
        "[1, , 3]", "[1, null]", "new Date(0)", "new String('x')", "'\\ud800'",
        "new Proxy({}, { ownKeys() { globalThis.ran = 1; return []; } })",
        "new Proxy([], { get() { globalThis.ran = 1; } })",
        "({ get x() { globalThis.ran = 1; return 1; } })",
        "[{ get x() { globalThis.ran = 1; return 1; } }]",
        "(() => { const a = [1]; Object.defineProperty(a, 0, { get() { globalThis.ran = 1; } }); return a; })()",
        "(() => { const o = {}; o.o = o; return o; })()",
        "new Map()",
    };
    Js j = js_new();
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        HlPvWriter w;
        int rc = js_enc(&j, bad[i], &w);
        if (rc == HL_PV_OK || rc == -100) printf("  accepted: %s\n", bad[i]);
        ASSERT_TRUE(rc != HL_PV_OK && rc != -100);
        ASSERT_FALSE(JS_HasException(j.ctx));
        hl_pv_writer_free(&w);
    }
    ASSERT_FALSE(js_global_true(&j, "ran"));
    js_close(&j);
}

/* ── Parity ─────────────────────────────────────────────────────────── */

UTEST(plain_value, lua_and_js_encode_alike)
{
    lua_State *L = lua_new();
    ASSERT_EQ(LUA_OK, luaL_dostring(L, "return { c = true, a = 'x', b = { 1, -2, 3 }, "
                                      "d = {}, n = { k = { 'deep' } } }"));
    lua_pushlstring(L, "\0\x01", 2);
    hl_lua_pv_push_bytes(L, -1);
    lua_setfield(L, -3, "bytes");
    lua_pop(L, 1);
    HlPvWriter lw;
    hl_pv_writer_init(&lw, 1 << 20);
    ASSERT_EQ(HL_PV_OK, hl_lua_pv_encode(L, -1, &lw));

    Js j = js_new();
    HlPvWriter jw;
    ASSERT_EQ(HL_PV_OK, js_enc(&j, "({ n: { k: ['deep'] }, d: {}, bytes: new Uint8Array([0, 1]), "
                                    "b: [1, -2, 3], a: 'x', c: true })", &jw));
    ASSERT_EQ(lw.len, jw.len);
    ASSERT_EQ(0, memcmp(lw.buf, jw.buf, lw.len));
    hl_pv_writer_free(&lw);
    hl_pv_writer_free(&jw);
    js_close(&j);
    lua_close(L);
}

UTEST_MAIN()
