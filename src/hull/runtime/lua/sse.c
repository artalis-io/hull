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
#include "hull/shared/req_life.h"

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

typedef struct {
    KlHttpRequest         *req;
    KlHttpResponse        *res;
    HlReqLife             *life;
    struct HlSseStreamUD  *stream;   /* out */
    int                    stream_ref; /* out: registry ref rooting it */
} HlSseArgs;

/* Under the protected entry (hl_lua_entry_prepare): req, stream. */
static int push_req_stream(lua_State *L, void *ud)
{
    HlSseArgs *a = (HlSseArgs *)ud;
    hl_lua_make_request(L, a->req, a->life);
    /* Create SSE stream userdata (calls kl_http_sse_begin) */
    a->stream = hl_lua_sse_push_stream(L, a->res, a->life);
    if (!a->stream)
        return luaL_error(L, "SSE init failed");
    /* Rooted for as long as the dispatcher reads it: the handler's
     * parameter was its only reference, so `stream = nil; collectgarbage()`
     * freed it, and the dispatcher then ended the stream through memory the
     * app could refill (an app-chosen pointer written through). */
    lua_pushvalue(L, -1);
    a->stream_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 2;
}

void hl_lua_sse_handler(KlHttpRequest *req, KlHttpResponse *res,
                                 void *user_data)
{
    HlLuaSseRoute *route = (HlLuaSseRoute *)user_data;
    HlLua *lua = route->lua;

    if (!lua || !lua->L || !req || !res)
        return;


    /* Guard stale transactions */
    hl_db_registry_guard_stale_txns(lua->base.db_registry);

    /* Reset scratch arena */
    sh_arena_reset(lua->scratch);

    /* Set per-request async context */
    lua->active_conn = kl_http_request_conn(req);
    lua->active_req = req;

    /* The request's life: the stream holds it, and it ends when this handler
     * is done - including when the client goes away mid-stream (cancel). A
     * stream kept for fan-out then fails closed instead of writing into a
     * connection that is gone. */
    HlReqLife *life = hl_req_life_new();
    if (!life) {
        lua->active_conn = NULL;
        lua->active_req = NULL;
        return;
    }

    /* The coroutine with handler(req, stream) on it, built protected. */
    HlSseArgs args = { req, res, life, NULL, LUA_NOREF };
    int thread_ref = LUA_NOREF, nargs = 0;
    lua_State *co = hl_lua_entry_prepare(lua, "__hull_routes", route->handler_id,
                                         push_req_stream, &args,
                                         &thread_ref, &nargs);
    struct HlSseStreamUD *stream_ud = args.stream;
    if (!co) {
        /* A stream that began is ended; one that never did gets a 500. */
        if (stream_ud && !stream_ud->closed)
            kl_http_sse_end(&stream_ud->sse);
        luaL_unref(lua->L, LUA_REGISTRYINDEX, args.stream_ref);
        hl_req_life_end(life);
        lua->active_conn = NULL;
        lua->active_req = NULL;
        if (!stream_ud) {
            kl_http_response_status(res, 500);
            kl_http_response_header(res, "Content-Type", "text/plain");
            kl_http_response_body_borrow(res, "SSE init failed", 15);
        }
        return;
    }

    /* Set coroutine state for async C functions */
    lua->active_co = co;
    lua->active_thread_ref = thread_ref;

    /* Arm the instruction budget for this run */
    HL_LUA_ARM(lua, co);

    lua->active_on_complete     = hl_lua_req_life_end_cb;
    lua->active_on_complete_ctx = life;

    /* Resume: handler(req, stream) */
    int nres = 0;
    int status = lua_resume(co, lua->L, nargs, &nres);
    status = hl_lua_resume_status(co, status);

    lua->active_on_complete     = NULL;
    lua->active_on_complete_ctx = NULL;

    if (status == LUA_OK) {
        /* Synchronous completion - end stream if not already closed */
        if (!stream_ud->closed)
            kl_http_sse_end(&stream_ud->sse);
        luaL_unref(lua->L, LUA_REGISTRYINDEX, args.stream_ref);
        hl_req_life_end(life);

        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->active_conn = NULL;
        lua->active_req = NULL;
    } else if (status == LUA_YIELD) {
        /* Handler yielded - streaming with hull.sleep() between events.
         * The stream will be ended when the async resume completes; this
         * dispatcher does not read stream_ud again. The continuation holds
         * the request's state, not the globals. */
        luaL_unref(lua->L, LUA_REGISTRYINDEX, args.stream_ref);
        lua->active_co = NULL;
        lua->active_thread_ref = LUA_NOREF;
        lua->active_conn = NULL;
        lua->active_req = NULL;
    } else {
        /* Error - end stream, log */
        char ebuf[512];
        log_error("[hull:web:sse] handler error: %s",
                  hl_lua_error_text(lua, co, -1, ebuf, sizeof(ebuf)));

        if (!stream_ud->closed)
            kl_http_sse_end(&stream_ud->sse);
        luaL_unref(lua->L, LUA_REGISTRYINDEX, args.stream_ref);
        hl_req_life_end(life);

        luaL_unref(lua->L, LUA_REGISTRYINDEX, thread_ref);
        lua->active_thread_ref = LUA_NOREF;
        lua->active_co = NULL;
        lua->active_conn = NULL;
        lua->active_req = NULL;
    }
}
