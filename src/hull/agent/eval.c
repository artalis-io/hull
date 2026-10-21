/*
 * agent/eval.c - `hull agent eval <code>`: run a one-shot Lua/JS snippet
 * against the loaded app context and return the result as JSON.
 *
 * Dev-mode only (the agent CLI is trusted on the host). The runtime
 * sandbox + manifest still apply - the snippet cannot bypass capability
 * checks. Intended for quick inspection: "what's in this table",
 * "what does this helper return", "is this regex matching?".
 *
 * The snippet is wrapped so its return value is JSON-encoded by the
 * runtime's own json module - that way we get a faithful round-trip
 * even for tables/arrays/nested structures.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "limits.h"
#include "hull/app_context.h"
#include "hull/runtime.h"

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HL_ENABLE_LUA
static int eval_lua(HlLua *lua, const char *code, ShJsonBuf *out)
{
    lua_State *L = lua->L;

    /* Wrap the snippet so we can capture its return value via the
     * runtime's own JSON encoder: as an expression first, and - when that
     * does not compile - as statement(s). The statement form used to be a
     * runtime fallback through `load`, which the app VM does not have, so
     * every runtime error came back as "attempt to call a nil value (global
     * 'load')" and statements never ran (audit 21). */
    int load_rc = LUA_ERRSYNTAX;
    for (int form = 0; form < 2 && load_rc != LUA_OK; form++) {
        if (form == 1) lua_pop(L, 1);   /* the expression form's error */
        luaL_Buffer b;
        luaL_buffinit(L, &b);
        luaL_addstring(&b, "local json = require('hull.json')\n");
        luaL_addstring(&b, form == 0 ? "local fn = function() return "
                                     : "local fn = function()\n");
        luaL_addstring(&b, code);
        luaL_addstring(&b, "\nend\n");
        luaL_addstring(&b, "local ok, v = pcall(fn)\n");
        luaL_addstring(&b, "if not ok then return nil, tostring(v) end\n");
        luaL_addstring(&b, "return json.encode(v), nil\n");
        luaL_pushresult(&b);
        size_t wrap_len;
        const char *wrap = lua_tolstring(L, -1, &wrap_len);
        load_rc = luaL_loadbuffer(L, wrap, wrap_len, "<agent-eval>");
        lua_remove(L, -2);   /* the wrap buffer string */
    }
    if (load_rc != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        int rc = hl_agent_write_error(out, err ? err : "load failed");
        lua_pop(L, 1);
        return rc;
    }
    /* A run of its own: on a warm context (MCP) the budget kept counting
     * across calls and the run deadline of the context's last entry kept
     * running, so every eval failed once it was about a minute old. */
    hl_lua_arm_entry(lua);
    if (lua_pcall(L, 0, 2, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        int rc = hl_agent_write_error(out, err ? err : "eval failed");
        lua_pop(L, 1);
        return rc;
    }

    /* Stack: encoded_json (string or nil), error_msg (string or nil) */
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    if (lua_isstring(L, -2)) {
        size_t jlen;
        const char *jstr = lua_tolstring(L, -2, &jlen);
        sh_json_write_kv_bool(&w, "ok", true);
        sh_json_write_key(&w, "result");
        /* result is already a JSON-encoded string - embed it raw. */
        if (jstr && jlen > 0) {
            /* The writer streams; raw pass-through requires writing the
             * literal bytes via the writer's underlying sink. */
            sh_json_write_raw(&w, jstr, jlen);
        } else {
            sh_json_write_null(&w);
        }
    } else {
        sh_json_write_kv_bool(&w, "ok", false);
        const char *err = lua_tostring(L, -1);
        sh_json_write_kv_string(&w, "error", err ? err : "unknown error");
    }
    sh_json_write_object_end(&w);
    lua_pop(L, 2);
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

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);

    if (JS_IsException(v)) {
        JSValue exc = JS_GetException(ctx);
        const char *err = JS_ToCString(ctx, exc);
        sh_json_write_kv_bool(&w, "ok", false);
        sh_json_write_kv_string(&w, "error", err ? err : "unknown error");
        if (err) JS_FreeCString(ctx, err);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, v);
        sh_json_write_object_end(&w);
        return 0;
    }

    /* Use JSON.stringify to serialise the result. */
    JSValue jv = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
    size_t jlen = 0;
    const char *jstr = NULL;
    if (!JS_IsException(jv) && !JS_IsUndefined(jv))
        jstr = JS_ToCStringLen(ctx, &jlen, jv);

    sh_json_write_kv_bool(&w, "ok", true);
    sh_json_write_key(&w, "result");
    if (jstr && jlen > 0) {
        sh_json_write_raw(&w, jstr, jlen);
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

int hl_agent_eval_ctx(HlAppContext *ctx, const char *code, ShJsonBuf *out)
{
    if (!code || !*code) return hl_agent_write_error(out, "code required");

    HlRuntime *rt = hl_app_context_runtime(ctx);
    if (!rt || !rt->vt) return hl_agent_write_error(out, "no runtime");

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
