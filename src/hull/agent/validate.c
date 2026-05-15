/*
 * agent/validate.c — `hull agent validate <file>`: parse one Lua/JS
 * file in isolation, emit syntax + basic sandbox-violation findings.
 *
 * Cheap iterative feedback that doesn't require restarting `hull dev`.
 * For full semantic validation (capability use vs manifest), use
 * `hull agent capabilities`.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "limits.h"

#ifdef HL_ENABLE_LUA
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#endif
#ifdef HL_ENABLE_JS
#include <quickjs.h>
#endif

#include <sh_json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Patterns that indicate a sandbox-violation attempt. The C runtime
 * rejects these at execution time; surfacing them at validate time
 * gives a faster signal. Each pattern is a substring match (not a
 * regex) — keep it simple.
 *
 * L-5 fix: named struct (`BadPattern`) shared between Lua/JS tables.
 * The previous code declared two anonymous-struct types with identical
 * layouts and cast through `(void *)` to a third anonymous-struct
 * type for iteration — strict-aliasing UB. */
typedef struct {
    const char *needle;
    const char *severity;
    const char *what;
} BadPattern;

static const BadPattern LUA_BAD_PATTERNS[] = {
    { "loadstring(",  "high",   "loadstring is removed in the Lua sandbox" },
    { "loadfile(",    "high",   "loadfile is removed in the Lua sandbox" },
    { "dofile(",      "high",   "dofile is removed in the Lua sandbox" },
    { "io.",          "medium", "io library is not available in the sandbox; use fs.*" },
    { "os.execute",   "high",   "os.execute is not available; use tool.* in build mode" },
    { "debug.",       "medium", "debug library is not loaded" },
    { "rawset(_G",    "medium", "writing to _G pollutes globals; use locals" },
    { NULL, NULL, NULL }
};

static const BadPattern JS_BAD_PATTERNS[] = {
    { "eval(",                 "high",   "eval is removed in the QuickJS sandbox" },
    { "new Function(",         "high",   "Function constructor is removed in the sandbox" },
    { "Object.prototype.",     "medium", "modifying Object.prototype pollutes globals" },
    { "Array.prototype.",      "medium", "modifying Array.prototype pollutes globals" },
    { "globalThis.",           "low",    "writing to globalThis is discouraged" },
    { NULL, NULL, NULL }
};

/* Walk substring patterns and emit findings. */
static void scan_patterns(ShJsonWriter *w, const char *content, size_t len,
                          int is_lua)
{
    /* Build a NUL-terminated copy for strstr scanning. */
    char *buf = malloc(len + 1);
    if (!buf) return;
    memcpy(buf, content, len);
    buf[len] = '\0';

    const BadPattern *p = is_lua ? LUA_BAD_PATTERNS : JS_BAD_PATTERNS;
    for (; p->needle; p++) {
        const char *hit = strstr(buf, p->needle);
        if (!hit) continue;
        /* Compute line number */
        int line = 1;
        for (const char *c = buf; c < hit; c++)
            if (*c == '\n') line++;
        sh_json_write_object_start(w);
        sh_json_write_kv_string(w, "severity", p->severity);
        sh_json_write_kv_string(w, "pattern",  p->needle);
        sh_json_write_kv_string(w, "message",  p->what);
        sh_json_write_kv_int(w, "line", line);
        sh_json_write_object_end(w);
    }

    free(buf);
}

#ifdef HL_ENABLE_LUA
static int validate_lua(const char *path, const char *content, size_t len,
                        ShJsonBuf *out)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "file", path);
    sh_json_write_kv_string(&w, "runtime", "lua");

    /* Pure parse — no execution. luaL_loadbuffer with no environment
     * binding is the same path the runtime uses for app load. */
    lua_State *L = luaL_newstate();
    if (!L) {
        sh_json_write_kv_bool(&w, "ok", false);
        sh_json_write_kv_string(&w, "error", "out of memory creating Lua state");
        sh_json_write_object_end(&w);
        return 0;
    }
    int rc = luaL_loadbuffer(L, content, len, path);
    int ok = (rc == LUA_OK);
    sh_json_write_kv_bool(&w, "ok", ok);
    if (!ok) {
        const char *err = lua_tostring(L, -1);
        sh_json_write_kv_string(&w, "error", err ? err : "parse error");
    }

    sh_json_write_key(&w, "findings");
    sh_json_write_array_start(&w);
    scan_patterns(&w, content, len, /* is_lua */ 1);
    sh_json_write_array_end(&w);

    lua_close(L);
    sh_json_write_object_end(&w);
    return 0;
}
#endif

#ifdef HL_ENABLE_JS
static int validate_js(const char *path, const char *content, size_t len,
                       ShJsonBuf *out)
{
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, out);
    sh_json_write_object_start(&w);
    sh_json_write_kv_string(&w, "file", path);
    sh_json_write_kv_string(&w, "runtime", "js");

    /* JS_Eval with EVAL_FLAG_COMPILE_ONLY — parses but doesn't execute. */
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) {
        sh_json_write_kv_bool(&w, "ok", false);
        sh_json_write_kv_string(&w, "error", "out of memory creating JS runtime");
        sh_json_write_object_end(&w);
        return 0;
    }
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
        JS_FreeRuntime(rt);
        sh_json_write_kv_bool(&w, "ok", false);
        sh_json_write_kv_string(&w, "error", "out of memory creating JS context");
        sh_json_write_object_end(&w);
        return 0;
    }

    JSValue v = JS_Eval(ctx, content, len, path,
                        JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    int ok = !JS_IsException(v);
    sh_json_write_kv_bool(&w, "ok", ok);
    if (!ok) {
        JSValue exc = JS_GetException(ctx);
        const char *err = JS_ToCString(ctx, exc);
        sh_json_write_kv_string(&w, "error", err ? err : "parse error");
        if (err) JS_FreeCString(ctx, err);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, v);

    sh_json_write_key(&w, "findings");
    sh_json_write_array_start(&w);
    scan_patterns(&w, content, len, /* is_lua */ 0);
    sh_json_write_array_end(&w);

    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    sh_json_write_object_end(&w);
    return 0;
}
#endif

int hl_agent_validate(const char *file_path, ShJsonBuf *out)
{
    if (!file_path)
        return hl_agent_write_error(out, "file path required");

    /* Read file */
    FILE *f = fopen(file_path, "rb");
    if (!f) return hl_agent_write_error(out, "cannot open file");
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return hl_agent_write_error(out, "seek failed"); }
    long n = ftell(f);
    if (n < 0 || n > HL_AGENT_VALIDATE_MAX_FILE) {
        fclose(f);
        return hl_agent_write_error(out, "file too large (max 4 MiB)");
    }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return hl_agent_write_error(out, "seek failed"); }
    char *content = malloc((size_t)n + 1);
    if (!content) { fclose(f); return hl_agent_write_error(out, "out of memory"); }
    size_t got = fread(content, 1, (size_t)n, f);
    int read_err = ferror(f);
    fclose(f);
    if (read_err || got != (size_t)n) {
        free(content);
        return hl_agent_write_error(out, "read failed");
    }
    content[got] = '\0';

    /* Pick validator by extension */
    size_t plen = strlen(file_path);
    int is_lua = (plen > 4 && strcmp(file_path + plen - 4, ".lua") == 0);
    int is_js  = (plen > 3 && strcmp(file_path + plen - 3, ".js") == 0);

    int rc;
#ifdef HL_ENABLE_LUA
    if (is_lua) {
        rc = validate_lua(file_path, content, got, out);
        free(content);
        return rc;
    }
#endif
#ifdef HL_ENABLE_JS
    if (is_js) {
        rc = validate_js(file_path, content, got, out);
        free(content);
        return rc;
    }
#endif

    free(content);
    (void)is_lua; (void)is_js; (void)rc;
    return hl_agent_write_error(out,
        "unsupported file extension (expected .lua or .js)");
}
