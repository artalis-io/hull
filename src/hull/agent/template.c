/*
 * agent/template.c — `hull agent template <name> <data.json>`:
 * render a template with provided JSON data and return the output.
 *
 * Useful for verifying a template renders correctly in isolation,
 * without spinning up the full request pipeline.
 *
 * Implementation: delegate to the loaded runtime via a small snippet
 * (`template.render_string(...)` for Lua, dynamic import for JS) so
 * we benefit from the same compile cache, inheritance, includes, and
 * escaping rules as production.
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
static int render_lua(HlLua *lua, const char *template_name,
                      const char *data_json, ShJsonBuf *out)
{
    lua_State *L = lua->L;
    /* Push:  require("hull.template").render(name, json.decode(data_json)) */
    /* Build a small Lua snippet to avoid manual stack juggling. */
    char snippet[HL_AGENT_SNIPPET_BUF];
    snprintf(snippet, sizeof(snippet),
        "local template = require('hull.template')\n"
        "local json = require('hull.json')\n"
        "local ok, data = pcall(json.decode, ...)\n"
        "if not ok then error('invalid JSON: ' .. tostring(data)) end\n"
        "return template.render(%q, data)\n",
        template_name);

    if (luaL_loadstring(L, snippet) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        int rc = hl_agent_write_error(out, err ? err : "load failed");
        lua_pop(L, 1);
        return rc;
    }
    lua_pushstring(L, data_json ? data_json : "{}");
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        int rc = hl_agent_write_error(out, err ? err : "render failed");
        lua_pop(L, 1);
        return rc;
    }

    size_t rlen;
    const char *result = lua_tolstring(L, -1, &rlen);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "template", template_name);
    sh_json_write_key(&w, "output");
    if (result) sh_json_write_string_n(&w, result, rlen);
    else        sh_json_write_string(&w, "");
    sh_json_write_kv_int(&w, "bytes", rlen);
    sh_json_write_object_end(&w);
    lua_pop(L, 1);
    return 0;
}
#endif

#ifdef HL_ENABLE_JS
static int render_js(HlJS *js, const char *template_name,
                     const char *data_json, ShJsonBuf *out)
{
    /* Use JS_ParseJSON for the data argument and JS_NewString for the
     * template name — both safe regardless of contents. (An earlier
     * snprintf-based snippet builder was dead code; removed per M-2.) */
    JSContext *ctx = js->ctx;
    JSValue tpl_arg = JS_NewString(ctx, template_name ? template_name : "");
    JSValue data_arg = JS_ParseJSON(ctx, data_json ? data_json : "{}", strlen(data_json ? data_json : "{}"), "<agent-data>");
    if (JS_IsException(data_arg)) {
        JS_FreeValue(ctx, tpl_arg);
        JS_FreeValue(ctx, data_arg);
        return hl_agent_write_error(out, "invalid JSON in data argument");
    }

    /* Call: (await import("hull:template")).template.render(name, data) */
    JSValue global = JS_GetGlobalObject(ctx);
    /* Stash args on globalThis for the eval call to pick up. */
    JS_SetPropertyStr(ctx, global, "__hull_agent_tpl_name", tpl_arg);
    JS_SetPropertyStr(ctx, global, "__hull_agent_tpl_data", data_arg);

    const char *code =
        "import { template } from 'hull:template';\n"
        "globalThis.__hull_agent_tpl_result = template.render("
            "globalThis.__hull_agent_tpl_name, "
            "globalThis.__hull_agent_tpl_data);\n";
    JSValue v = JS_Eval(ctx, code, strlen(code), "<agent-template>",
                        JS_EVAL_TYPE_MODULE);
    int err_path = JS_IsException(v);
    JS_FreeValue(ctx, v);

    if (err_path) {
        JSValue exc = JS_GetException(ctx);
        const char *err = JS_ToCString(ctx, exc);
        int rc = hl_agent_write_error(out, err ? err : "render failed");
        if (err) JS_FreeCString(ctx, err);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, global);
        return rc;
    }

    JSValue result = JS_GetPropertyStr(ctx, global, "__hull_agent_tpl_result");
    size_t rlen = 0;
    const char *rstr = JS_ToCStringLen(ctx, &rlen, result);

    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "template", template_name);
    sh_json_write_key(&w, "output");
    if (rstr) sh_json_write_string_n(&w, rstr, rlen);
    else      sh_json_write_string(&w, "");
    sh_json_write_kv_int(&w, "bytes", rlen);
    sh_json_write_object_end(&w);

    if (rstr) JS_FreeCString(ctx, rstr);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, global);
    return 0;
}
#endif

int hl_agent_template_ctx(HlAppContext *ctx, const char *template_name,
                          const char *data_json, ShJsonBuf *out)
{
    if (!template_name)
        return hl_agent_write_error(out, "template name required");

    HlRuntime *rt = hl_app_context_runtime(ctx);
    if (!rt || !rt->vt) return hl_agent_write_error(out, "no runtime");

#ifdef HL_ENABLE_LUA
    if (rt->vt == &hl_lua_vtable) {
        HlLua *lua = hl_app_context_lua(ctx);
        if (!lua) return hl_agent_write_error(out, "no lua state");
        return render_lua(lua, template_name, data_json, out);
    }
#endif
#ifdef HL_ENABLE_JS
    if (rt->vt == &hl_js_vtable) {
        HlJS *js = hl_app_context_js(ctx);
        if (!js) return hl_agent_write_error(out, "no js state");
        return render_js(js, template_name, data_json, out);
    }
#endif
    return hl_agent_write_error(out, "unsupported runtime");
}
