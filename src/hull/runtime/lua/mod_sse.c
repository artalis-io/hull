/*
 * mod_sse.c - SSE stream userdata for Lua
 *
 * Provides:
 *   - stream:event(name, data [, id])
 *   - stream:comment(text)
 *   - stream:close()
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/req_life.h"
#include "mod_buffer.h"

#include <string.h>

/* ── Metatable name ────────────────────────────────────────────────── */

#define HL_SSE_STREAM_MT "HlSseStream"

typedef struct HlSseStreamUD HlSseStreamUD;

/* ── Methods ───────────────────────────────────────────────────────── */

/* Closed by the script, or its request is over: a stream kept past its
 * request (fan-out lists are the usual case) holds a write path into a
 * connection that may have gone away, so it fails closed. */
static int stream_dead(const HlSseStreamUD *ud)
{
    return ud->closed || !hl_req_life_live(ud->life);
}

static int lua_sse_gc(lua_State *L)
{
    HlSseStreamUD *ud = (HlSseStreamUD *)luaL_checkudata(L, 1, HL_SSE_STREAM_MT);
    hl_req_life_release(ud->life);
    ud->life = NULL;
    return 0;
}

static int lua_sse_event(lua_State *L)
{
    HlSseStreamUD *ud = (HlSseStreamUD *)luaL_checkudata(L, 1, HL_SSE_STREAM_MT);
    if (stream_dead(ud))
        return luaL_error(L, "SSE stream is closed");

    const char *event_name = luaL_optstring(L, 2, NULL);
    size_t data_len = 0;
    const char *data = luaL_checklstring(L, 3, &data_len);
    const char *id = luaL_optstring(L, 4, NULL);

    int rc = kl_http_sse_event(&ud->sse, event_name, data, data_len, id);
    if (rc < 0)
        return luaL_error(L, "SSE write failed");

    return 0;
}

static int lua_sse_comment(lua_State *L)
{
    HlSseStreamUD *ud = (HlSseStreamUD *)luaL_checkudata(L, 1, HL_SSE_STREAM_MT);
    if (stream_dead(ud))
        return luaL_error(L, "SSE stream is closed");

    size_t len;
    const char *text = luaL_checklstring(L, 2, &len);

    int rc = kl_http_sse_comment(&ud->sse, text, len);
    if (rc < 0)
        return luaL_error(L, "SSE write failed");

    return 0;
}

static int lua_sse_close(lua_State *L)
{
    HlSseStreamUD *ud = (HlSseStreamUD *)luaL_checkudata(L, 1, HL_SSE_STREAM_MT);
    if (stream_dead(ud))
        return 0;

    kl_http_sse_end(&ud->sse);
    ud->closed = 1;
    return 0;
}

static const luaL_Reg sse_stream_methods[] = {
    {"event",   lua_sse_event},
    {"comment", lua_sse_comment},
    {"close",   lua_sse_close},
    {NULL, NULL}
};

void hl_lua_sse_register_mt(lua_State *L)
{
    luaL_newmetatable(L, HL_SSE_STREAM_MT);
    luaL_setfuncs(L, sse_stream_methods, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, lua_sse_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1); /* pop metatable */
}

/* Create and push an SSE stream userdata. Calls kl_http_sse_begin.
 * Returns pointer to the userdata, or NULL on error. */
struct HlSseStreamUD *hl_lua_sse_push_stream(lua_State *L, KlHttpResponse *res,
                                             struct HlReqLife *life)
{
    HlSseStreamUD *ud = (HlSseStreamUD *)lua_newuserdata(L, sizeof(HlSseStreamUD));
    ud->closed = 0;
    ud->life = NULL;

    if (kl_http_sse_begin(res, &ud->sse) < 0) {
        lua_pop(L, 1);
        return NULL;
    }

    ud->life = life;
    hl_req_life_retain(life);
    luaL_setmetatable(L, HL_SSE_STREAM_MT);
    return ud;
}
