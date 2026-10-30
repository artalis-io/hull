/*
 * agent/eval.c - `hull agent eval <code>`: run a one-shot Lua/JS snippet
 * against the loaded app context and return the result as JSON.
 *
 * Dev-mode only (the agent CLI is trusted on the host). The runtime
 * sandbox + manifest still apply - the snippet cannot bypass capability
 * checks. Intended for quick inspection: "what's in this table",
 * "what does this helper return", "is this regex matching?".
 *
 * The result is JSON-encoded in C - QuickJS's JS_JSONStringify for JS, a
 * raw-accessor encoder for Lua (never the app's own json module: audit 35)
 * - and copied to the output with terminal control bytes escaped.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "limits.h"
#include "hull/app_context.h"
#include "hull/runtime.h"
#include "hull/cap/db_registry.h"   /* hl_db_registry_guard_stale_txns */

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#include <lua.h>
#include <lauxlib.h>
#endif
#ifdef HL_ENABLE_JS
#include "hull/runtime/js.h"
#include <quickjs.h>
#endif

#include <sh_json.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Write the JSON text `json` (valid JSON produced in this file, by C) as one
 * raw value, with the bytes a terminal acts on escaped: DEL and the
 * UTF-8-encoded C1 controls (U+0080..U+009F, CSI among them), which a JSON
 * encoder leaves raw inside a string, and any C0 control. Outside a string
 * valid JSON holds only ASCII structural bytes and whitespace, so the
 * rewritten text is the same JSON value. Built whole first: the writer puts
 * a comma before every raw write that follows a value. */
#if defined(HL_ENABLE_LUA) || defined(HL_ENABLE_JS)
static int eval_write_json_safe(ShJsonWriter *w, const char *json, size_t len)
{
    ShJsonBuf b;
    sh_json_buf_init(&b);
    size_t run = 0;
    int rc = 0;
    for (size_t i = 0; i < len && rc == 0; i++) {
        unsigned char c = (unsigned char)json[i];
        unsigned code;
        size_t skip = 1;
        if (c < 0x20 || c == 0x7f) {
            /* A raw tab / CR / LF can only be whitespace between tokens (the
             * encoders escape them inside strings); any other C0 or DEL can
             * only be inside a string, where its \u escape is the same. */
            if (c == '\t' || c == '\n' || c == '\r') continue;
            code = c;
        } else if (c == 0xc2 && i + 1 < len &&
                   (unsigned char)json[i + 1] >= 0x80 &&
                   (unsigned char)json[i + 1] <= 0x9f) {
            code = (unsigned char)json[i + 1];
            skip = 2;
        } else {
            continue;
        }
        if (i > run) rc |= sh_json_buf_write(&b, json + run, i - run);
        char esc[8];
        snprintf(esc, sizeof esc, "\\u%04x", code);
        rc |= sh_json_buf_write(&b, esc, 6);
        run = i + skip;
        i += skip - 1;
    }
    if (rc == 0 && len > run) rc |= sh_json_buf_write(&b, json + run, len - run);
    if (rc == 0 && b.buf && b.len) rc = sh_json_write_raw(w, b.buf, b.len);
    else                           rc = -1;
    sh_json_buf_free(&b);
    if (rc != 0) sh_json_write_null(w);
    return rc;
}
#endif

#ifdef HL_ENABLE_LUA
/*
 * The Lua eval result as JSON, encoded in C (audit 35). It used to be
 * encoded by json.encode inside the app's own VM - `require('hull.json')`,
 * `pcall` and `tostring` all looked up through app globals - and the string
 * that returned was copied into the output raw. A cloned app that replaced
 * require("hull.json").encode at top level then wrote raw ESC / OSC bytes to
 * the agent's terminal and appended forged JSON records an agent (or, inside
 * the MCP text, a client) read as the real result. This encoder reads the
 * value with raw accessors only (lua_rawget / lua_rawgeti / lua_next /
 * lua_rawlen), calls no Lua code and no metamethod, and is bounded in depth
 * (HL_AGENT_EVAL_MAX_DEPTH) and output (HL_AGENT_EVAL_MAX_RESULT); a cycle
 * is refused.
 *
 * Shape follows stdlib/lua/vendor/json.lua: a table that is empty, or has
 * [1] and keys exactly 1..#t, is an array; any other table an object whose
 * keys are strings or numbers (a number key written as its text), sorted.
 * nil and a non-finite number are null. A function / userdata / thread is
 * written as the string "<type>" (json.encode raised on it).
 */
typedef struct {
    ShJsonBuf *buf;
    size_t     cap;
    int        too_large;
} EvSink;

static int ev_sink_write(void *ctx, const char *data, size_t len)
{
    EvSink *s = (EvSink *)ctx;
    if (len > s->cap || s->buf->len > s->cap - len) {
        s->too_large = 1;
        return -1;
    }
    return sh_json_buf_write(s->buf, data, len);
}

typedef struct {
    const char *s;
    size_t      len;
    lua_Integer slot;     /* index of the original key in the key table */
} EvKey;

static int ev_key_cmp(const void *a, const void *b)
{
    const EvKey *x = (const EvKey *)a, *y = (const EvKey *)b;
    size_t n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->s, y->s, n);
    if (c) return c;
    return (x->len > y->len) - (x->len < y->len);
}

/* `visited` is the absolute index of a table holding the tables on the
 * current path. Raises (luaL_error) on a value it refuses; runs under
 * lua_pcall. */
static void ev_value(lua_State *L, int idx, ShJsonWriter *w, int depth,
                     int visited);

static void ev_table(lua_State *L, int idx, ShJsonWriter *w, int depth,
                     int visited)
{
    if (depth > HL_AGENT_EVAL_MAX_DEPTH)
        luaL_error(L, "result nests deeper than %d", HL_AGENT_EVAL_MAX_DEPTH);
    luaL_checkstack(L, 8, "result nests too deep");
    idx = lua_absindex(L, idx);

    lua_pushvalue(L, idx);
    if (lua_rawget(L, visited) != LUA_TNIL)
        luaL_error(L, "result contains a cycle");
    lua_pop(L, 1);
    lua_pushvalue(L, idx);
    lua_pushboolean(L, 1);
    lua_rawset(L, visited);

    /* Count the keys; note whether every one is a number. */
    lua_Integer n = 0;
    int all_num = 1;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        lua_pop(L, 1);
        int kt = lua_type(L, -1);
        if (kt != LUA_TNUMBER) all_num = 0;
        if (kt != LUA_TNUMBER && kt != LUA_TSTRING)
            luaL_error(L, "result has a %s table key", lua_typename(L, kt));
        n++;
    }
    lua_rawgeti(L, idx, 1);
    int has_first = !lua_isnil(L, -1);
    lua_pop(L, 1);

    if (n == 0 ||
        (has_first && all_num && (lua_Unsigned)n == lua_rawlen(L, idx))) {
        sh_json_write_array_start(w);
        for (lua_Integer i = 1; i <= n; i++) {
            lua_rawgeti(L, idx, i);
            ev_value(L, -1, w, depth + 1, visited);
            lua_pop(L, 1);
        }
        sh_json_write_array_end(w);
    } else {
        /* Object. The original keys go in one table (a number keeps its own
         * identity for the rawget), their texts in another, which keeps the
         * strings the sorted array points at alive. */
        if ((lua_Unsigned)n > (lua_Unsigned)(SIZE_MAX / sizeof(EvKey)) ||
            n > INT_MAX)
            luaL_error(L, "result too large");
        lua_createtable(L, (int)n, 0);
        int orig = lua_gettop(L);
        lua_createtable(L, (int)n, 0);
        int texts = lua_gettop(L);
        EvKey *keys = (EvKey *)lua_newuserdatauv(L, (size_t)n * sizeof *keys, 0);
        lua_Integer k = 0;
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            lua_pop(L, 1);
            if (k == n) luaL_error(L, "result changed while encoding");
            k++;
            lua_pushvalue(L, -1);
            lua_rawseti(L, orig, k);
            lua_pushvalue(L, -1);      /* a copy: tolstring converts in place */
            size_t kl = 0;
            const char *ks = lua_tolstring(L, -1, &kl);
            if (memchr(ks, '\0', kl))
                luaL_error(L, "result has a table key with a NUL byte");
            keys[k - 1].s = ks;
            keys[k - 1].len = kl;
            keys[k - 1].slot = k;
            lua_rawseti(L, texts, k);
        }
        qsort(keys, (size_t)k, sizeof *keys, ev_key_cmp);
        sh_json_write_object_start(w);
        for (lua_Integer i = 0; i < k; i++) {
            sh_json_write_key(w, keys[i].s);   /* escaped; no NUL (above) */
            lua_rawgeti(L, orig, keys[i].slot);
            lua_rawget(L, idx);
            ev_value(L, -1, w, depth + 1, visited);
            lua_pop(L, 1);
        }
        sh_json_write_object_end(w);
        lua_settop(L, orig - 1);
    }

    lua_pushvalue(L, idx);
    lua_pushnil(L);
    lua_rawset(L, visited);
}

static void ev_value(lua_State *L, int idx, ShJsonWriter *w, int depth,
                     int visited)
{
    switch (lua_type(L, idx)) {
    case LUA_TNIL:
    case LUA_TNONE:
        sh_json_write_null(w);
        break;
    case LUA_TBOOLEAN:
        sh_json_write_bool(w, lua_toboolean(L, idx) != 0);
        break;
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx))
            sh_json_write_int(w, (int64_t)lua_tointeger(L, idx));
        else   /* json.lua's "%.14g"; NaN / inf are written as null */
            sh_json_write_double_fmt(w, (double)lua_tonumber(L, idx), 14);
        break;
    case LUA_TSTRING: {
        size_t n = 0;
        const char *s = lua_tolstring(L, idx, &n);
        sh_json_write_string_n(w, s, n);
        break;
    }
    case LUA_TTABLE:
        ev_table(L, idx, w, depth, visited);
        break;
    default: {
        char tag[32];
        snprintf(tag, sizeof tag, "<%s>", luaL_typename(L, idx));
        sh_json_write_string(w, tag);
        break;
    }
    }
    if (sh_json_writer_error(w))
        luaL_error(L, "result too large");
}

/* lua_pcall'd with (value, lightuserdata ShJsonWriter). */
static int ev_encode_k(lua_State *L)
{
    ShJsonWriter *w = (ShJsonWriter *)lua_touserdata(L, 2);
    lua_newtable(L);
    ev_value(L, 1, w, 0, lua_gettop(L));
    return 0;
}

/* {"ok":false,"error":<msg>} with msg escaped for a terminal too. */
static void eval_write_failed(ShJsonWriter *w, const char *msg)
{
    sh_json_write_kv_bool(w, "ok", false);
    sh_json_write_key(w, "error");
    ShJsonBuf eb;
    sh_json_buf_init(&eb);
    ShJsonWriter ew;
    sh_json_writer_init(&ew, sh_json_buf_write, &eb);
    sh_json_write_string(&ew, msg ? msg : "unknown error");
    if (!sh_json_writer_error(&ew) && eb.buf)
        eval_write_json_safe(w, eb.buf, eb.len);
    else
        sh_json_write_string(w, "unknown error");
    sh_json_buf_free(&eb);
}

static int eval_lua(HlLua *lua, const char *code, ShJsonBuf *out)
{
    lua_State *L = lua->L;
    int top = lua_gettop(L);

    /* The snippet is the chunk itself: as an expression (`return <code>`)
     * first, and - when that does not compile - as statement(s). No Lua
     * wrapper: the call is protected from C and the result encoded in C,
     * so nothing the app replaced (require, pcall, tostring, hull.json) is
     * consulted (audit 35; the statement form ran through `load` until
     * audit 21). */
    int load_rc = LUA_ERRSYNTAX;
    for (int form = 0; form < 2 && load_rc != LUA_OK; form++) {
        if (form == 1) lua_pop(L, 1);   /* the expression form's error */
        luaL_Buffer b;
        luaL_buffinit(L, &b);
        if (form == 0) luaL_addstring(&b, "return ");
        luaL_addstring(&b, code);
        luaL_addstring(&b, "\n");
        luaL_pushresult(&b);
        size_t wrap_len;
        const char *wrap = lua_tolstring(L, -1, &wrap_len);
        load_rc = luaL_loadbuffer(L, wrap, wrap_len, "<agent-eval>");
        lua_remove(L, -2);   /* the wrap buffer string */
    }
    if (load_rc != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        int rc = hl_agent_write_error(out, err ? err : "load failed");
        lua_settop(L, top);
        return rc;
    }
    /* A run of its own: on a warm context (MCP) the budget kept counting
     * across calls and the run deadline of the context's last entry kept
     * running, so every eval failed once it was about a minute old. */
    hl_lua_arm_entry(lua);
    int run_rc = lua_pcall(L, 0, 1, 0);

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    if (run_rc != LUA_OK) {
        /* The error's text without running app code: a string or number as
         * itself, anything else named by its type (tostring would run its
         * __tostring). */
        char tmp[64];
        const char *err;
        int et = lua_type(L, -1);
        if (et == LUA_TSTRING || et == LUA_TNUMBER) {
            err = lua_tostring(L, -1);
        } else {
            snprintf(tmp, sizeof tmp, "(error object is a %s value)",
                     lua_typename(L, et));
            err = tmp;
        }
        eval_write_failed(&w, err);
        sh_json_write_object_end(&w);
        lua_settop(L, top);
        return 0;
    }

    /* Encode into a scratch buffer of bounded size, then copy it. */
    ShJsonBuf rb;
    sh_json_buf_init(&rb);
    EvSink sink = { &rb, HL_AGENT_EVAL_MAX_RESULT, 0 };
    ShJsonWriter rw;
    sh_json_writer_init(&rw, ev_sink_write, &sink);
    lua_pushcfunction(L, ev_encode_k);
    lua_insert(L, -2);                 /* fn, value */
    lua_pushlightuserdata(L, &rw);
    int enc_rc = lua_pcall(L, 2, 0, 0);
    if (enc_rc == LUA_OK && !sh_json_writer_error(&rw) && rb.buf && rb.len) {
        sh_json_write_kv_bool(&w, "ok", true);
        sh_json_write_key(&w, "result");
        eval_write_json_safe(&w, rb.buf, rb.len);
    } else if (sink.too_large) {
        eval_write_failed(&w, "result too large");
    } else {
        eval_write_failed(&w, (enc_rc != LUA_OK && lua_type(L, -1) == LUA_TSTRING)
                                  ? lua_tostring(L, -1)
                                  : "result is not serialisable");
    }
    sh_json_buf_free(&rb);
    sh_json_write_object_end(&w);
    lua_settop(L, top);
    return 0;
}
#endif

#ifdef HL_ENABLE_JS
static int eval_js(HlJS *js, const char *code, ShJsonBuf *out)
{
    JSContext *ctx = js->ctx;

    /* Evaluate the snippet as a global expression. Wrap in (() => (...))()
     * so a bare expression like `1+1` works as well as a statement block.
     * M-1 fix: snprintf with truncation check rather than raw sprintf. */
    size_t snip_cap = strlen(code) + HL_AGENT_EVAL_WRAP_HEADROOM;
    char *snippet = malloc(snip_cap);
    if (!snippet) return hl_agent_write_error(out, "out of memory");
    int snip_n = snprintf(snippet, snip_cap, "(()=>{return (%s);})()", code);
    if (snip_n < 0 || (size_t)snip_n >= snip_cap) {
        free(snippet);
        return hl_agent_write_error(out, "code too large");
    }
    hl_js_budget_arm(js);   /* a run of its own (see eval_lua) */
    /* The form is chosen by COMPILING: the expression form when it parses,
     * else the statement block. Falling back after a run re-ran a snippet
     * that threw at run time - its writes twice (audit 22; Lua since #803). */
    JSValue fn = JS_Eval(ctx, snippet, (size_t)snip_n, "<agent-eval>",
                         JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    free(snippet);
    if (JS_IsException(fn)) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        fn = JS_Eval(ctx, code, strlen(code), "<agent-eval-stmts>",
                     JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_COMPILE_ONLY);
    }
    JSValue v = JS_IsException(fn) ? fn : JS_EvalFunction(ctx, fn);

    /* The error or the result as text FIRST: the error is taken before any
     * job runs (the drain ran with it pending), and stringifying runs app
     * code (toJSON, getters) whose jobs the drain below then runs too -
     * stringified after it, they were left queued (audit 23). */
    const char *err = NULL;
    JSValue jv = JS_UNDEFINED;
    size_t jlen = 0;
    const char *jstr = NULL;
    int failed = JS_IsException(v);
    if (failed) {
        JSValue exc = JS_GetException(ctx);
        err = JS_ToCString(ctx, exc);
        if (!err) JS_FreeValue(ctx, JS_GetException(ctx));
        JS_FreeValue(ctx, exc);
    } else {
        /* Use JSON.stringify to serialise the result. */
        jv = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
        if (JS_IsException(jv)) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            jv = JS_UNDEFINED;
        } else if (!JS_IsUndefined(jv)) {
            jstr = JS_ToCStringLen(ctx, &jlen, jv);
            if (!jstr) JS_FreeValue(ctx, JS_GetException(ctx));
        }
    }
    /* The jobs the snippet queued are its own code (`await null; ...`):
     * run while the eval is the active run, as an entry's are, not left
     * for whatever runs next on a warm context (audit 22). */
    hl_js_run_jobs(js);
    /* The eval is an entry: a transaction the snippet left open (held
     * across an await of a promise Hull does not drive) is rolled back and
     * its run's tag failed, so the rest never continues in autocommit
     * (audit 36; the bare guards in hl_agent_eval_ctx only rolled back). */
    (void)hl_js_entry_end_txn_check(js);

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    if (failed) {
        sh_json_write_kv_bool(&w, "ok", false);
        sh_json_write_kv_string(&w, "error", err ? err : "unknown error");
        if (err) JS_FreeCString(ctx, err);
        JS_FreeValue(ctx, v);
        sh_json_write_object_end(&w);
        return 0;
    }

    sh_json_write_kv_bool(&w, "ok", true);
    sh_json_write_key(&w, "result");
    if (jstr && jlen > 0) {
        /* QuickJS's own JSON.stringify (frozen intrinsics); it leaves DEL
         * and C1 controls raw inside strings - escaped on the way out. */
        eval_write_json_safe(&w, jstr, jlen);
    } else {
        sh_json_write_null(&w);
    }
    sh_json_write_object_end(&w);

    if (jstr) JS_FreeCString(ctx, jstr);
    JS_FreeValue(ctx, jv);
    JS_FreeValue(ctx, v);
    return 0;
}
#endif

static int eval_dispatch(HlRuntime *rt, HlAppContext *ctx, const char *code,
                         ShJsonBuf *out);

int hl_agent_eval_ctx(HlAppContext *ctx, const char *code, ShJsonBuf *out)
{
    if (!code || !*code) return hl_agent_write_error(out, "code required");

    HlRuntime *rt = hl_app_context_runtime(ctx);
    if (!rt || !rt->vt) return hl_agent_write_error(out, "no runtime");

    /* An eval is an entry: a transaction left open on a warm context's
     * connections is rolled back at both ends, as for every other entry -
     * the snippet neither joins one an earlier call left nor leaves its own
     * for the next request or eval (audit 22). */
    hl_db_registry_guard_stale_txns(rt->db_registry);
    int rc = eval_dispatch(rt, ctx, code, out);
    hl_db_registry_guard_stale_txns(rt->db_registry);
    return rc;
}

static int eval_dispatch(HlRuntime *rt, HlAppContext *ctx, const char *code,
                         ShJsonBuf *out)
{
    (void)rt; (void)ctx; (void)code;
#ifdef HL_ENABLE_LUA
    if (rt->vt->kind == HL_RT_LUA) {
        HlLua *lua = hl_app_context_lua(ctx);
        if (!lua) return hl_agent_write_error(out, "no lua state");
        return eval_lua(lua, code, out);
    }
#endif
#ifdef HL_ENABLE_JS
    if (rt->vt->kind == HL_RT_JS) {
        HlJS *js = hl_app_context_js(ctx);
        if (!js) return hl_agent_write_error(out, "no js state");
        return eval_js(js, code, out);
    }
#endif
    return hl_agent_write_error(out, "unsupported runtime");
}
