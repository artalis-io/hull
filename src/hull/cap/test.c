/*
 * cap/test.c — In-process test module for hull test
 *
 * Provides HTTP dispatch (no TCP) and assertions for testing Hull apps.
 * Routes are matched via kl_router_match, handlers called in-process,
 * and KlResponse fields inspected directly.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/test.h"

#include <keel/router.h>
#include <keel/request.h>
#include <keel/response.h>
#include <keel/allocator.h>
#include <keel/body_reader.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Registry keys for stored state */
#define TEST_ROUTER_KEY   "__hull_test_router"
#define TEST_CASES_KEY    "__hull_test_cases"

/* ── Shared C dispatch logic ───────────────────────────────────────── */

/*
 * Build a KlRequest on the stack, match a route, dispatch the handler,
 * and return response info. Used by both Lua and JS bindings.
 */
typedef struct {
    int status;
    const char *body;
    size_t body_len;
    const char *hdr_buf;
    size_t hdr_len;
} HlTestResult;

static int test_dispatch(KlRouter *router, const char *method,
                         const char *path, const char *body_data,
                         size_t body_len, const char **header_names,
                         const char **header_values, int num_headers,
                         HlTestResult *result)
{
    if (!router || !method || !path || !result) return -1;

    memset(result, 0, sizeof(*result));

    /* Split path at '?' for query string */
    const char *query = NULL;
    size_t path_len = strlen(path);
    size_t query_len = 0;
    const char *qmark = strchr(path, '?');
    if (qmark) {
        path_len = (size_t)(qmark - path);
        query = qmark + 1;
        query_len = strlen(query);
    }

    /* Match route */
    KlRoute *matched = NULL;
    KlParam params[KL_MAX_PARAMS];
    int num_params = 0;

    int match_status = kl_router_match(router, method, strlen(method),
                                        path, path_len,
                                        &matched, params, &num_params);

    if (match_status != 200 || !matched) {
        result->status = match_status;
        return 0;
    }

    /* Build request */
    KlRequest req;
    memset(&req, 0, sizeof(req));
    req.method = method;
    req.method_len = strlen(method);
    req.path = path;
    req.path_len = path_len;
    req.query = query;
    req.query_len = query_len;
    req.version_major = 1;
    req.version_minor = 1;
    req.keep_alive = 0;

    /* Copy matched params */
    req.num_params = num_params;
    for (int i = 0; i < num_params && i < KL_MAX_PARAMS; i++)
        req.params[i] = params[i];

    /* Set headers */
    for (int i = 0; i < num_headers && i < KL_MAX_HEADERS; i++) {
        req.headers[i].name = header_names[i];
        req.headers[i].name_len = strlen(header_names[i]);
        req.headers[i].value = header_values[i];
        req.headers[i].value_len = strlen(header_values[i]);
    }
    req.num_headers = num_headers;

    /* Fake body reader if body provided */
    KlBodyReader fake_body;
    memset(&fake_body, 0, sizeof(fake_body));
    if (body_data && body_len > 0) {
        req.body_reader = &fake_body;
        req.content_length = body_len;
    }

    /* Build response */
    KlAllocator alloc = kl_allocator_default();
    KlResponse res;
    if (kl_response_init(&res, &alloc) != 0) return -1;
    res.conn_fd = -1; /* no actual connection */

    /* Dispatch handler */
    matched->handler(&req, &res, matched->user_data);

    /* Extract results */
    result->status = res.status;
    result->body = res.body;
    result->body_len = res.body_len;
    result->hdr_buf = res.hdr_buf;
    result->hdr_len = res.hdr_len;

    /* Don't free response yet — caller reads the body pointer.
     * Caller is responsible for cleanup. We copy what we need. */
    /* Actually, body points into runtime-managed memory, so we copy it. */
    if (res.body && res.body_len > 0) {
        char *body_copy = malloc(res.body_len + 1);
        if (body_copy) {
            memcpy(body_copy, res.body, res.body_len);
            body_copy[res.body_len] = '\0';
            result->body = body_copy;
        }
    }

    kl_response_free(&res);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Lua bindings
 * ══════════════════════════════════════════════════════════════════════ */

#ifdef HL_ENABLE_LUA

#include "hull/runtime/lua.h"
#include "lua.h"
#include "lauxlib.h"

static KlRouter *get_test_router(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, TEST_ROUTER_KEY);
    KlRouter *r = (KlRouter *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return r;
}

static HlLua *get_test_lua(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_test_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return lua;
}

/* ── test(...) — register a test case ──────────────────────────────── */

static int l_test_call(lua_State *L)
{
    /* Called as test("desc", fn) — first arg is the test table (self) */
    const char *desc = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);

    /* Get test cases table */
    lua_getfield(L, LUA_REGISTRYINDEX, TEST_CASES_KEY);
    int idx = (int)luaL_len(L, -1) + 1;

    /* Store { desc = desc, fn = fn } */
    lua_newtable(L);
    lua_pushstring(L, desc);
    lua_setfield(L, -2, "desc");
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, "fn");

    lua_rawseti(L, -2, idx);
    lua_pop(L, 1); /* pop test cases table */
    return 0;
}

/* ── test.get/post/put/delete/patch ────────────────────────────────── */

static int l_test_http(lua_State *L, const char *method)
{
    const char *path = luaL_checkstring(L, 1);
    KlRouter *router = get_test_router(L);
    HlLua *lua = get_test_lua(L);

    if (!router || !lua) {
        return luaL_error(L, "test module not initialized");
    }

    /* Parse optional opts table (arg 2) */
    const char *body_str = NULL;
    size_t body_len = 0;
    const char *header_names[KL_MAX_HEADERS];
    const char *header_values[KL_MAX_HEADERS];
    int num_headers = 0;

    if (lua_istable(L, 2)) {
        /* opts.body */
        lua_getfield(L, 2, "body");
        if (lua_isstring(L, -1))
            body_str = lua_tolstring(L, -1, &body_len);
        lua_pop(L, 1);

        /* opts.headers */
        lua_getfield(L, 2, "headers");
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2) != 0 && num_headers < KL_MAX_HEADERS) {
                if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
                    header_names[num_headers] = lua_tostring(L, -2);
                    header_values[num_headers] = lua_tostring(L, -1);
                    num_headers++;
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    HlTestResult result;
    if (test_dispatch(router, method, path, body_str, body_len,
                      header_names, header_values, num_headers,
                      &result) != 0) {
        return luaL_error(L, "test dispatch failed");
    }

    /* Build result table */
    lua_newtable(L);

    lua_pushinteger(L, result.status);
    lua_setfield(L, -2, "status");

    if (result.body && result.body_len > 0) {
        lua_pushlstring(L, result.body, result.body_len);
        lua_setfield(L, -2, "body");

        /* Auto-decode JSON if body looks like JSON */
        if (result.body[0] == '{' || result.body[0] == '[') {
            /* Try json.decode via hull.json module */
            lua_getglobal(L, "require");
            lua_pushstring(L, "hull.json");
            if (lua_pcall(L, 1, 1, 0) == LUA_OK && lua_istable(L, -1)) {
                lua_getfield(L, -1, "decode");
                lua_pushlstring(L, result.body, result.body_len);
                if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
                    lua_setfield(L, -3, "json");
                } else {
                    lua_pop(L, 1); /* pop error */
                }
                lua_pop(L, 1); /* pop json module */
            } else {
                lua_pop(L, 1);
            }
        }

        free((void *)result.body);
    }

    return 1;
}

static int l_test_get(lua_State *L)    { return l_test_http(L, "GET"); }
static int l_test_post(lua_State *L)   { return l_test_http(L, "POST"); }
static int l_test_put(lua_State *L)    { return l_test_http(L, "PUT"); }
static int l_test_delete(lua_State *L) { return l_test_http(L, "DELETE"); }
static int l_test_patch(lua_State *L)  { return l_test_http(L, "PATCH"); }

/* ── test.eq(a, b, msg?) ──────────────────────────────────────────── */

static int l_test_eq(lua_State *L)
{
    int equal = 0;

    if (lua_type(L, 1) == lua_type(L, 2)) {
        if (lua_isstring(L, 1) && lua_isstring(L, 2)) {
            equal = (strcmp(lua_tostring(L, 1), lua_tostring(L, 2)) == 0);
        } else if (lua_isnumber(L, 1) && lua_isnumber(L, 2)) {
            equal = (lua_tonumber(L, 1) == lua_tonumber(L, 2));
        } else if (lua_isboolean(L, 1) && lua_isboolean(L, 2)) {
            equal = (lua_toboolean(L, 1) == lua_toboolean(L, 2));
        } else if (lua_isnil(L, 1) && lua_isnil(L, 2)) {
            equal = 1;
        } else {
            /* For tables/userdata, compare by reference */
            equal = lua_rawequal(L, 1, 2);
        }
    }

    if (!equal) {
        const char *msg = lua_isstring(L, 3) ? lua_tostring(L, 3) : NULL;
        const char *a_str = luaL_tolstring(L, 1, NULL);
        const char *b_str = luaL_tolstring(L, 2, NULL);
        if (msg)
            return luaL_error(L, "test.eq failed: %s\n  expected: %s\n  actual:   %s",
                              msg, b_str, a_str);
        else
            return luaL_error(L, "test.eq failed\n  expected: %s\n  actual:   %s",
                              b_str, a_str);
    }

    return 0;
}

/* ── test.ok(val, msg?) ───────────────────────────────────────────── */

static int l_test_ok(lua_State *L)
{
    if (!lua_toboolean(L, 1)) {
        const char *msg = lua_isstring(L, 2) ? lua_tostring(L, 2) : "test.ok failed";
        return luaL_error(L, "%s", msg);
    }
    return 0;
}

/* ── test.err(fn, pattern?) ───────────────────────────────────────── */

static int l_test_err(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    const char *pattern = lua_isstring(L, 2) ? lua_tostring(L, 2) : NULL;

    lua_pushvalue(L, 1);
    int rc = lua_pcall(L, 0, 0, 0);

    if (rc == LUA_OK)
        return luaL_error(L, "test.err: expected error but function succeeded");

    if (pattern) {
        const char *err = lua_tostring(L, -1);
        if (!err || strstr(err, pattern) == NULL) {
            return luaL_error(L, "test.err: error '%s' does not match pattern '%s'",
                              err ? err : "(nil)", pattern);
        }
    }

    lua_pop(L, 1); /* pop error message */
    return 0;
}

/* ── Registration ──────────────────────────────────────────────────── */

static const luaL_Reg test_methods[] = {
    { "get",    l_test_get },
    { "post",   l_test_post },
    { "put",    l_test_put },
    { "delete", l_test_delete },
    { "patch",  l_test_patch },
    { "eq",     l_test_eq },
    { "ok",     l_test_ok },
    { "err",    l_test_err },
    { NULL, NULL }
};

void hl_cap_test_register_lua(lua_State *L, KlRouter *router, HlLua *lua)
{
    /* Store router and lua context in registry */
    lua_pushlightuserdata(L, router);
    lua_setfield(L, LUA_REGISTRYINDEX, TEST_ROUTER_KEY);

    lua_pushlightuserdata(L, lua);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_test_lua");

    /* Create test cases table */
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, TEST_CASES_KEY);

    /* Create the test table with methods */
    luaL_newlib(L, test_methods);

    /* Create metatable with __call for test("desc", fn) */
    lua_newtable(L); /* metatable */
    lua_pushcfunction(L, l_test_call);
    lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2);

    lua_setglobal(L, "test");
}

void hl_cap_test_clear_lua(lua_State *L)
{
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, TEST_CASES_KEY);
}

void hl_cap_test_run_lua(lua_State *L, int *total, int *passed, int *failed)
{
    *total = 0;
    *passed = 0;
    *failed = 0;

    lua_getfield(L, LUA_REGISTRYINDEX, TEST_CASES_KEY);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return;
    }

    int count = (int)luaL_len(L, -1);
    for (int i = 1; i <= count; i++) {
        lua_rawgeti(L, -1, i);

        lua_getfield(L, -1, "desc");
        const char *desc = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, -1, "fn");

        (*total)++;
        int rc = lua_pcall(L, 0, 0, 0);
        if (rc == LUA_OK) {
            printf("  PASS  %s\n", desc ? desc : "(unnamed)");
            (*passed)++;
        } else {
            const char *err = lua_tostring(L, -1);
            printf("  FAIL  %s\n    %s\n", desc ? desc : "(unnamed)",
                   err ? err : "unknown error");
            lua_pop(L, 1); /* pop error */
            (*failed)++;
        }

        lua_pop(L, 1); /* pop test case table */
    }

    lua_pop(L, 1); /* pop test cases table */
}

#endif /* HL_ENABLE_LUA */

/* ══════════════════════════════════════════════════════════════════════
 *  JS bindings
 * ══════════════════════════════════════════════════════════════════════ */

#ifdef HL_ENABLE_JS

#include "hull/runtime/js.h"
#include "quickjs.h"

/* JS test state stored as opaque pointer on globalThis.__hull_test_state */

typedef struct {
    KlRouter *router;
    HlJS *js;
    JSValue cases; /* Array of { desc: string, fn: function } */
} HlJSTestState;

static HlJSTestState *get_js_test_state(JSContext *ctx)
{
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue state_val = JS_GetPropertyStr(ctx, global, "__hull_test_state");
    HlJSTestState *state = NULL;
    if (!JS_IsUndefined(state_val)) {
        state = JS_GetOpaque(state_val, 0);
    }
    JS_FreeValue(ctx, state_val);
    JS_FreeValue(ctx, global);
    return state;
}

/* test("desc", fn) — callable */
static JSValue js_test_call(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_UNDEFINED;

    HlJSTestState *state = get_js_test_state(ctx);
    if (!state) return JS_ThrowInternalError(ctx, "test not initialized");

    JSValue len_val = JS_GetPropertyStr(ctx, state->cases, "length");
    int32_t idx = 0;
    JS_ToInt32(ctx, &idx, len_val);
    JS_FreeValue(ctx, len_val);

    JSValue entry = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, entry, "desc", JS_DupValue(ctx, argv[0]));
    JS_SetPropertyStr(ctx, entry, "fn", JS_DupValue(ctx, argv[1]));
    JS_SetPropertyUint32(ctx, state->cases, (uint32_t)idx, entry);

    return JS_UNDEFINED;
}

/* test.get/post/put/delete/patch */
static JSValue js_test_http(JSContext *ctx, const char *method,
                            int argc, JSValueConst *argv)
{
    if (argc < 1) return JS_ThrowTypeError(ctx, "path required");

    HlJSTestState *state = get_js_test_state(ctx);
    if (!state) return JS_ThrowInternalError(ctx, "test not initialized");

    const char *path = JS_ToCString(ctx, argv[0]);
    if (!path) return JS_EXCEPTION;

    const char *body_str = NULL;
    size_t body_len = 0;
    const char *header_names[KL_MAX_HEADERS];
    const char *header_values[KL_MAX_HEADERS];
    int num_headers = 0;

    /* Parse opts (arg 1) */
    if (argc >= 2 && JS_IsObject(argv[1])) {
        JSValue body_val = JS_GetPropertyStr(ctx, argv[1], "body");
        if (JS_IsString(body_val))
            body_str = JS_ToCStringLen(ctx, &body_len, body_val);
        JS_FreeValue(ctx, body_val);

        JSValue hdrs_val = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (JS_IsObject(hdrs_val)) {
            JSPropertyEnum *props = NULL;
            uint32_t prop_count = 0;
            if (JS_GetOwnPropertyNames(ctx, &props, &prop_count, hdrs_val,
                                        JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < prop_count && num_headers < KL_MAX_HEADERS; i++) {
                    header_names[num_headers] = JS_AtomToCString(ctx, props[i].atom);
                    JSValue val = JS_GetProperty(ctx, hdrs_val, props[i].atom);
                    header_values[num_headers] = JS_ToCString(ctx, val);
                    JS_FreeValue(ctx, val);
                    if (header_names[num_headers] && header_values[num_headers])
                        num_headers++;
                }
                for (uint32_t i = 0; i < prop_count; i++)
                    JS_FreeAtom(ctx, props[i].atom);
                js_free(ctx, props);
            }
        }
        JS_FreeValue(ctx, hdrs_val);
    }

    HlTestResult result;
    int rc = test_dispatch(state->router, method, path, body_str, body_len,
                           header_names, header_values, num_headers, &result);

    /* Free C strings */
    // cppcheck-suppress knownConditionTrueFalse
    if (body_str) JS_FreeCString(ctx, body_str);
    // cppcheck-suppress knownConditionTrueFalse
    for (int i = 0; i < num_headers; i++) {
        if (header_names[i]) JS_FreeCString(ctx, header_names[i]);
        if (header_values[i]) JS_FreeCString(ctx, header_values[i]);
    }
    JS_FreeCString(ctx, path);

    if (rc != 0)
        return JS_ThrowInternalError(ctx, "test dispatch failed");

    /* Build result object */
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "status", JS_NewInt32(ctx, result.status));

    if (result.body && result.body_len > 0) {
        JS_SetPropertyStr(ctx, obj, "body",
                          JS_NewStringLen(ctx, result.body, result.body_len));

        /* Auto-decode JSON */
        if (result.body[0] == '{' || result.body[0] == '[') {
            JSValue json_str = JS_NewStringLen(ctx, result.body, result.body_len);
            JSValue parsed = JS_ParseJSON(ctx, result.body, result.body_len, "<test>");
            if (!JS_IsException(parsed))
                JS_SetPropertyStr(ctx, obj, "json", parsed);
            else
                JS_FreeValue(ctx, JS_GetException(ctx));
            JS_FreeValue(ctx, json_str);
        }

        free((void *)result.body);
    }

    return obj;
}

static JSValue js_test_get(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_test_http(ctx, "GET", argc, argv);
}

static JSValue js_test_post(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_test_http(ctx, "POST", argc, argv);
}

static JSValue js_test_put(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_test_http(ctx, "PUT", argc, argv);
}

static JSValue js_test_del(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_test_http(ctx, "DELETE", argc, argv);
}

static JSValue js_test_patch(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    return js_test_http(ctx, "PATCH", argc, argv);
}

/* test.eq(a, b, msg?) */
static JSValue js_test_eq(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 2) return JS_ThrowTypeError(ctx, "test.eq requires 2 arguments");

    int equal = 0;

    /* Use strict equality for primitives */
    if (JS_IsNumber(argv[0]) && JS_IsNumber(argv[1])) {
        double a, b;
        JS_ToFloat64(ctx, &a, argv[0]);
        JS_ToFloat64(ctx, &b, argv[1]);
        equal = (a == b);
    } else if (JS_IsString(argv[0]) && JS_IsString(argv[1])) {
        const char *a = JS_ToCString(ctx, argv[0]);
        const char *b = JS_ToCString(ctx, argv[1]);
        equal = (a && b && strcmp(a, b) == 0);
        if (a) JS_FreeCString(ctx, a);
        if (b) JS_FreeCString(ctx, b);
    } else if (JS_IsBool(argv[0]) && JS_IsBool(argv[1])) {
        equal = (JS_ToBool(ctx, argv[0]) == JS_ToBool(ctx, argv[1]));
    } else if (JS_IsNull(argv[0]) && JS_IsNull(argv[1])) {
        equal = 1;
    } else if (JS_IsUndefined(argv[0]) && JS_IsUndefined(argv[1])) {
        equal = 1;
    } else {
        /* Reference equality for objects */
        equal = (JS_VALUE_GET_PTR(argv[0]) == JS_VALUE_GET_PTR(argv[1]));
    }

    if (!equal) {
        const char *msg = (argc >= 3 && JS_IsString(argv[2]))
                          ? JS_ToCString(ctx, argv[2]) : NULL;
        JSValue a_str = JS_JSONStringify(ctx, argv[0], JS_UNDEFINED, JS_UNDEFINED);
        JSValue b_str = JS_JSONStringify(ctx, argv[1], JS_UNDEFINED, JS_UNDEFINED);
        const char *a = JS_ToCString(ctx, a_str);
        const char *b = JS_ToCString(ctx, b_str);

        JSValue err;
        if (msg)
            err = JS_ThrowTypeError(ctx, "test.eq failed: %s\n  expected: %s\n  actual:   %s",
                                    msg, b ? b : "?", a ? a : "?");
        else
            err = JS_ThrowTypeError(ctx, "test.eq failed\n  expected: %s\n  actual:   %s",
                                    b ? b : "?", a ? a : "?");

        if (a) JS_FreeCString(ctx, a);
        if (b) JS_FreeCString(ctx, b);
        JS_FreeValue(ctx, a_str);
        JS_FreeValue(ctx, b_str);
        if (msg) JS_FreeCString(ctx, msg);
        return err;
    }

    return JS_UNDEFINED;
}

/* test.ok(val, msg?) */
static JSValue js_test_ok(JSContext *ctx, JSValueConst this_val,
                          int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_ToBool(ctx, argv[0])) {
        const char *msg = (argc >= 2 && JS_IsString(argv[1]))
                          ? JS_ToCString(ctx, argv[1]) : "test.ok failed";
        JSValue err = JS_ThrowTypeError(ctx, "%s", msg);
        if (argc >= 2 && JS_IsString(argv[1]))
            JS_FreeCString(ctx, msg);
        return err;
    }
    return JS_UNDEFINED;
}

/* test.err(fn, pattern?) */
static JSValue js_test_err(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "test.err requires a function");

    JSValue ret = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
    if (!JS_IsException(ret)) {
        JS_FreeValue(ctx, ret);
        return JS_ThrowTypeError(ctx, "test.err: expected error but function succeeded");
    }

    JSValue exc = JS_GetException(ctx);
    if (argc >= 2 && JS_IsString(argv[1])) {
        const char *pattern = JS_ToCString(ctx, argv[1]);
        JSValue msg_val = JS_GetPropertyStr(ctx, exc, "message");
        const char *msg = JS_ToCString(ctx, msg_val);

        int matches = (msg && pattern && strstr(msg, pattern) != NULL);

        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, msg_val);
        JS_FreeCString(ctx, pattern);

        if (!matches) {
            JS_FreeValue(ctx, exc);
            return JS_ThrowTypeError(ctx, "test.err: error does not match pattern");
        }
    }

    JS_FreeValue(ctx, exc);
    return JS_UNDEFINED;
}

void hl_cap_test_register_js(JSContext *ctx, KlRouter *router, HlJS *js)
{
    HlJSTestState *state = js_malloc(ctx, sizeof(HlJSTestState));
    if (!state) return;

    state->router = router;
    state->js = js;
    state->cases = JS_NewArray(ctx);

    /* Store state on global */
    JSValue global = JS_GetGlobalObject(ctx);

    JSValue state_obj = JS_NewObjectClass(ctx, 0);
    JS_SetOpaque(state_obj, state);
    JS_SetPropertyStr(ctx, global, "__hull_test_state", state_obj);

    /* Create test function (callable + has properties) */
    JSValue test_fn = JS_NewCFunction(ctx, js_test_call, "test", 2);

    /* Add methods */
    JS_SetPropertyStr(ctx, test_fn, "get",
                      JS_NewCFunction(ctx, js_test_get, "get", 2));
    JS_SetPropertyStr(ctx, test_fn, "post",
                      JS_NewCFunction(ctx, js_test_post, "post", 2));
    JS_SetPropertyStr(ctx, test_fn, "put",
                      JS_NewCFunction(ctx, js_test_put, "put", 2));
    JS_SetPropertyStr(ctx, test_fn, "delete",
                      JS_NewCFunction(ctx, js_test_del, "delete", 2));
    JS_SetPropertyStr(ctx, test_fn, "patch",
                      JS_NewCFunction(ctx, js_test_patch, "patch", 2));
    JS_SetPropertyStr(ctx, test_fn, "eq",
                      JS_NewCFunction(ctx, js_test_eq, "eq", 3));
    JS_SetPropertyStr(ctx, test_fn, "ok",
                      JS_NewCFunction(ctx, js_test_ok, "ok", 2));
    JS_SetPropertyStr(ctx, test_fn, "err",
                      JS_NewCFunction(ctx, js_test_err, "err", 2));

    JS_SetPropertyStr(ctx, global, "test", test_fn);
    JS_FreeValue(ctx, global);
}

void hl_cap_test_clear_js(JSContext *ctx)
{
    HlJSTestState *state = get_js_test_state(ctx);
    if (!state) return;

    JS_FreeValue(ctx, state->cases);
    state->cases = JS_NewArray(ctx);
}

void hl_cap_test_run_js(JSContext *ctx, int *total, int *passed, int *failed)
{
    *total = 0;
    *passed = 0;
    *failed = 0;

    HlJSTestState *state = get_js_test_state(ctx);
    if (!state) return;

    JSValue len_val = JS_GetPropertyStr(ctx, state->cases, "length");
    int32_t count = 0;
    JS_ToInt32(ctx, &count, len_val);
    JS_FreeValue(ctx, len_val);

    for (int32_t i = 0; i < count; i++) {
        JSValue entry = JS_GetPropertyUint32(ctx, state->cases, (uint32_t)i);
        JSValue desc_val = JS_GetPropertyStr(ctx, entry, "desc");
        JSValue fn = JS_GetPropertyStr(ctx, entry, "fn");

        const char *desc = JS_ToCString(ctx, desc_val);

        (*total)++;
        JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 0, NULL);

        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(ctx);
            JSValue msg_val = JS_GetPropertyStr(ctx, exc, "message");
            const char *msg = JS_ToCString(ctx, msg_val);
            printf("  FAIL  %s\n    %s\n", desc ? desc : "(unnamed)",
                   msg ? msg : "unknown error");
            if (msg) JS_FreeCString(ctx, msg);
            JS_FreeValue(ctx, msg_val);
            JS_FreeValue(ctx, exc);
            (*failed)++;
        } else {
            printf("  PASS  %s\n", desc ? desc : "(unnamed)");
            (*passed)++;
        }

        JS_FreeValue(ctx, ret);
        if (desc) JS_FreeCString(ctx, desc);
        JS_FreeValue(ctx, fn);
        JS_FreeValue(ctx, desc_val);
        JS_FreeValue(ctx, entry);
    }
}

#endif /* HL_ENABLE_JS */
