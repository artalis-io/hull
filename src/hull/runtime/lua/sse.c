/*
 * sse.c - Lua Server-Sent Events handler
 *
 * Adapts Keel SSE routes (registered via `app.sse()`) to Lua handler
 * coroutines. Begins the chunked stream, pushes a stream userdata,
 * and supports async yield between events (e.g. `hull.sleep`).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/cap/db.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_registry.h"

#include "mod_buffer.h"

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <keel/keel.h>
#include <keel/http_sse.h>

#include <sh_arena.h>

#include "log.h"

#include <limits.h>

void hl_lua_sse_handler(KlHttpRequest *req, KlHttpResponse *res,
                                 void *user_data)
{
    HlLuaSseRoute *route = (HlLuaSseRoute *)user_data;
    HlLua *lua = route->lua;

    if (!lua || !lua->L || !req || !res)
        return;

    lua->dispatch_depth++;

    /* Guard stale transactions */
    hl_db_guard_stale_txn(hl_db_registry_default(lua->base.db_registry));

    /* Re-arm instruction limit */
    if (lua->max_instructions > 0)
        lua_sethook(lua->L, hl_lua_instruction_hook, LUA_MASKCOUNT,
                    INSTR_COUNT(lua->max_instructions));

    /* Reset scratch arena */
    sh_arena_reset(lua->scratch);

    /* Set per-request async context */
    lua->active_conn = kl_http_request_conn(req);
    lua->active_req = req;

    /* Get handler function */
    lua_getfield(lua->L, LUA_REGISTRYINDEX, "__hull_routes");
    if (!lua_istable(lua->L, -1)) {
        lua_pop(lua->L, 1);
        lua->active_conn = NULL;
        lua->active_req = NULL;
        lua->dispatch_depth--;
        return;
    }

    lua_rawgeti(lua->L, -1, route->handler_id);
    if (!lua_isfunction(lua->L, -1)) {
        lua_pop(lua->L, 2);
        lua->active_conn = NULL;
        lua->active_req = NULL;
        lua->dispatch_depth--;
        return;
    }

    /* Create coroutine */
    lua_State *co = lua_newthread(lua->L);
    int thread_ref = luaL_ref(lua->L, LUA_REGISTRYINDEX);

    /* Move handler to coroutine */
    lua_xmove(lua->L, co, 1);

    /* Build request object */
    hl_lua_make_request(co, req);

    /* Create SSE stream userdata (calls kl_http_sse_begin) */
    struct HlSseStreamUD *stream_ud = hl_lua_sse_push_stream(co, res);
    if (!stream_ud) {
        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua_pop(lua->L, 1); /* pop routes table */
        lua->active_conn = NULL;
        lua->active_req = NULL;
        lua->dispatch_depth--;
        kl_http_response_status(res, 500);
        kl_http_response_header(res, "Content-Type", "text/plain");
        kl_http_response_body_borrow(res, "SSE init failed", 15);
        return;
    }

    /* Set coroutine state for async C functions */
    lua->active_co = co;
    lua->active_thread_ref = thread_ref;

    /* Re-arm instruction hook on coroutine */
    if (lua->max_instructions > 0)
        lua_sethook(co, hl_lua_instruction_hook, LUA_MASKCOUNT,
                    INSTR_COUNT(lua->max_instructions));

    /* Resume: handler(req, stream) */
    int nres = 0;
    int status = lua_resume(co, lua->L, 2, &nres);

    if (status == LUA_OK) {
        /* Synchronous completion - end stream if not already closed */
        if (!stream_ud->closed)
            kl_http_sse_end(&stream_ud->sse);

        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->active_conn = NULL;
        lua->active_req = NULL;
        lua->dispatch_depth--;

        lua_pop(lua->L, 1); /* pop routes table */
    } else if (status == LUA_YIELD) {
        /* Handler yielded - streaming with hull.sleep() between events.
         * The stream will be ended when the async resume completes.
         * Routes table cleaned up on resume. */
        lua_pop(lua->L, 1); /* pop routes table */
    } else {
        /* Error - end stream, log */
        const char *err = lua_tostring(co, -1);
        log_error("[hull:web:sse] handler error: %s", err ? err : "unknown");

        if (!stream_ud->closed)
            kl_http_sse_end(&stream_ud->sse);

        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->active_conn = NULL;
        lua->active_req = NULL;
        lua->dispatch_depth--;

        lua_pop(lua->L, 1); /* pop routes table */
    }
}
