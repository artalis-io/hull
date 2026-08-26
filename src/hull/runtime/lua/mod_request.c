/*
 * mod_request.c - Lua bindings for streaming-multipart request bodies
 *
 * Wires `req:multipart()` for routes registered with
 *   app.post("/upload", handler, { multipart = {...} })
 *
 * The handler iterates parts like:
 *
 *   for part in req:multipart() do
 *       if part.filename then
 *           for chunk in part:chunks() do
 *               file:write(chunk)
 *           end
 *       else
 *           local value = part:read()
 *       end
 *   end
 *
 * The iterator drives Keel's kl_multipart_next() against the parkable
 * wrapper (see hl_cap_multipart_factory in cap/body.c). When the parser
 * reports NEED_DATA the iterator parks a resume callback on the wrapper,
 * sets c->state = KL_CONN_READING_BODY, and lua_yieldk's. Bytes off the
 * socket fire mp_wrap_on_data which fires the park callback which calls
 * hl_lua_async_resume which calls lua_resume on the handler's coroutine
 * - the continuation re-enters mp_iter_drive and re-tries the parser.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"

#include "hull/utils/alloc.h"
#include "hull/shared/async.h"
#include "hull/cap/body.h"

#include <keel/body_reader.h>
#include <keel/body_reader_multipart.h>
#include <keel/connection.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <stdlib.h>
#include <string.h>

/* ── Lua metatable names ────────────────────────────────────────────── */

#define HL_MP_ITER_MT   "HlMpIter"
#define HL_MP_PART_MT   "HlMpPart"
#define HL_MP_CHUNKS_MT "HlMpChunks"

/* ── Internal state ─────────────────────────────────────────────────── */

/* Iterator state. Lifetime owned by Lua GC (it's a userdata).
 *
 * Holds heap-allocated COPIES of the current part's metadata strings:
 * kl_multipart_next() docs say the borrowed name/filename/content_type
 * pointers are valid only until the next mutation, which for us means
 * the next kl_multipart_next call OR the next on_data callback (which
 * fires inside our park-callback / lua_resume path). The coroutine can
 * yield between reading the meta and the chunks loop - so we always
 * snapshot meta the moment PART_BEGIN fires. The copies stay valid for
 * the lifetime of one Part.
 */
typedef struct {
    KlBodyReader *wrapper;     /* parkable wrapper (from hl_cap_multipart_factory) */
    KlBodyReader *inner;       /* inner kl_body_reader_multipart for kl_multipart_next */
    HlLua        *lua;         /* runtime - for allocator + async-cont creation */
    HlAllocator  *alloc;       /* shorthand for lua->base.alloc */

    int           done;        /* parser hit DONE; subsequent iter:step returns nil */
    int           errored;     /* parser/IO error - iter:step raises */
    int           in_part;     /* between PART_BEGIN..PART_END for the active Part */

    /* Snapshot of the active Part's metadata. NULL when no part is active. */
    char         *name;
    size_t        name_len;
    char         *filename;    /* NULL when not a file upload */
    size_t        filename_len;
    char         *content_type;/* NULL when no Content-Type header on the part */
    size_t        content_type_len;
} HlMpIter;

/* Part userdata. Holds a borrowed pointer to its parent iter; the iter
 * is anchored against GC via uservalue slot 1 of this userdata. */
typedef struct {
    HlMpIter *iter;
    /* When the user advances the outer iter (or the chunks iter finishes),
     * this Part is "spent": further read/chunks return empty/end. We don't
     * tear down the iter's meta-copies on spend - that happens when the
     * NEXT PART_BEGIN snapshot overwrites them, or on iter GC. */
    int       spent;
} HlMpPart;

/* Chunks-iterator userdata. Holds a borrowed pointer to the parent
 * Part (anchored via uservalue slot 1). */
typedef struct {
    HlMpPart *part;
    int       ended;           /* 1 once PART_END consumed for THIS chunks iter */
} HlMpChunks;

/* ── Forward declarations ───────────────────────────────────────────── */

static int mp_iter_drive(lua_State *L);
static int mp_iter_continue(lua_State *L, int status, lua_KContext ctx);
static int mp_chunks_drive(lua_State *L);
static int mp_chunks_continue(lua_State *L, int status, lua_KContext ctx);

/* ── Meta-copy helpers ──────────────────────────────────────────────── */

/* Free the currently-stashed Part metadata (if any). */
static void mp_iter_clear_meta(HlMpIter *it)
{
    if (it->name) {
        hl_alloc_free(it->alloc, it->name, it->name_len);
        it->name = NULL;
        it->name_len = 0;
    }
    if (it->filename) {
        hl_alloc_free(it->alloc, it->filename, it->filename_len);
        it->filename = NULL;
        it->filename_len = 0;
    }
    if (it->content_type) {
        hl_alloc_free(it->alloc, it->content_type, it->content_type_len);
        it->content_type = NULL;
        it->content_type_len = 0;
    }
}

/* Snapshot the borrowed meta into iter-owned heap buffers. On allocation
 * failure, leaves iter->errored set and returns -1 (caller raises). */
static int mp_iter_copy_meta(HlMpIter *it, const KlMultipartPartMeta *meta)
{
    mp_iter_clear_meta(it);
    if (meta->name && meta->name_len > 0) {
        it->name = hl_alloc_malloc(it->alloc, meta->name_len);
        if (!it->name) { it->errored = 1; return -1; }
        memcpy(it->name, meta->name, meta->name_len);
        it->name_len = meta->name_len;
    }
    if (meta->filename && meta->filename_len > 0) {
        it->filename = hl_alloc_malloc(it->alloc, meta->filename_len);
        if (!it->filename) { it->errored = 1; return -1; }
        memcpy(it->filename, meta->filename, meta->filename_len);
        it->filename_len = meta->filename_len;
    }
    if (meta->content_type && meta->content_type_len > 0) {
        it->content_type = hl_alloc_malloc(it->alloc, meta->content_type_len);
        if (!it->content_type) { it->errored = 1; return -1; }
        memcpy(it->content_type, meta->content_type, meta->content_type_len);
        it->content_type_len = meta->content_type_len;
    }
    return 0;
}

/* ── Yield/resume helper ────────────────────────────────────────────── */

/* Forward to the existing Lua async-resume machinery - extern from async.c
 * (declared without a header so the symbol stays internal to the runtime). */
extern HlAsyncCont *hl_lua_async_cont_create(HlLua *lua, HlAllocator *alloc,
                                              HlLuaPushResultFn push_result);

/* Park callback: fires on next on_data / on_complete / on_error event.
 *
 * We own the cont (we created it via hl_lua_async_cont_create and did NOT
 * register it with an HlAsyncCtx), so we destroy it here after resume.
 * If the coroutine re-yields from within resume, hl_lua_async_resume's
 * YIELD branch does not free the cont - but a fresh cont has been created
 * for the next park, so freeing this one is correct.
 */
static void mp_park_resume(void *ctx, HlMultipartResumeReason reason)
{
    (void)reason; /* iter's drive loop figures out from the next parser event */
    HlAsyncCont *cont = (HlAsyncCont *)ctx;
    if (!cont) return;
    if (cont->resume)  cont->resume(cont, NULL);
    if (cont->destroy) cont->destroy(cont);
}

/* Park + yield on NEED_DATA. Returns lua_yieldk's "return" so callers
 * just `return mp_iter_park(L, it, kfunc, kctx)`. On allocation failure,
 * raises a Lua error (does not return). */
static int mp_park_and_yield(lua_State *L, HlMpIter *it,
                              lua_KFunction kfunc, lua_KContext kctx)
{
    /* Streaming-multipart routes only run when a connection is bound -
     * dispatch sets lua->active_conn before the handler is entered. If
     * it isn't set we have nothing to park against (e.g. an in-process
     * test harness call). Fail loudly rather than hang. */
    KlConn *conn = it->lua->active_conn;
    if (!conn)
        return luaL_error(L,
            "req:multipart(): no active connection - streaming routes "
            "require a live server (hull dev / built binary)");

    HlAsyncCont *cont = hl_lua_async_cont_create(it->lua, it->alloc, NULL);
    if (!cont)
        return luaL_error(L, "req:multipart(): out of memory");

    /* Register the park callback. hl_cap_multipart_park fires the cb
     * immediately if the stream has already ended/errored (covering the
     * race where on_complete already fired before we re-park). The cb
     * is single-shot; we re-park on every yield. */
    if (hl_cap_multipart_park(it->wrapper, mp_park_resume, cont) != 0) {
        cont->destroy(cont);
        return luaL_error(L, "req:multipart(): body reader is not multipart");
    }

    /* Tell Keel to keep reading socket bytes. v2.1.1's streaming dispatch
     * checks this state on return from the handler; without it Keel would
     * either suspend the connection (no FD progress) or pump the body to
     * completion before re-entering the handler. */
    conn->state = KL_CONN_READING_BODY;

    return lua_yieldk(L, 0, kctx, kfunc);
}

/* ── Part: read + chunks ────────────────────────────────────────────── */

static HlMpPart *check_part(lua_State *L, int idx)
{
    return (HlMpPart *)luaL_checkudata(L, idx, HL_MP_PART_MT);
}

/* part:read() - accumulate every PART_DATA event until PART_END, return
 * one big string. Convenience for small fields; large uploads should use
 * part:chunks() instead. */
static int mp_part_read(lua_State *L);
static int mp_part_read_continue(lua_State *L, int status, lua_KContext ctx);

/* Shared drive loop for read(). Stack contract:
 *   in : Part at index 1, plus an optional pre-existing accumulator at -1
 *        (a Lua string from a previous park-yield-resume cycle).
 *   out: pushes one result (the assembled Lua string) and returns 1, OR
 *        parks + yields (continuation re-enters this function).
 *
 * `seed` (when non-NULL) is appended into the fresh buffer first, so the
 * resume path doesn't lose bytes accumulated before a yield.
 */
static int mp_part_read_pump(lua_State *L)
{
    HlMpPart *p = check_part(L, 1);
    HlMpIter *it = p->iter;

    luaL_Buffer b;
    luaL_buffinit(L, &b);

    /* If we were resumed after a yield, the top of the stack is the
     * partial accumulated string. Prepend it into the new buffer so
     * we don't lose what we already collected. */
    if (lua_type(L, -1) == LUA_TSTRING) {
        size_t plen = 0;
        const char *p_bytes = lua_tolstring(L, -1, &plen);
        if (p_bytes && plen > 0) luaL_addlstring(&b, p_bytes, plen);
        lua_pop(L, 1);
    }

    for (;;) {
        if (it->errored)
            return luaL_error(L, "req:multipart(): parser error");

        if (p->spent || !it->in_part) {
            /* Already drained for this part (e.g. called twice). */
            luaL_pushresult(&b);
            return 1;
        }

        KlMultipartPartMeta meta;
        const char *data = NULL;
        size_t data_len = 0;
        KlMultipartEvent ev = kl_multipart_next(it->inner, &meta,
                                                  &data, &data_len);
        switch (ev) {
        case KL_MP_EVT_PART_DATA:
            if (data && data_len > 0)
                luaL_addlstring(&b, data, data_len);
            continue;
        case KL_MP_EVT_PART_END:
            it->in_part = 0;
            p->spent = 1;
            luaL_pushresult(&b);
            return 1;
        case KL_MP_EVT_NEED_DATA:
            /* Stash the partial as a Lua string at the stack top so the
             * continuation can re-prepend it. luaL_Buffer is C-stack
             * scoped and would lose its contents across the yield. */
            luaL_pushresult(&b);
            return mp_park_and_yield(L, it, mp_part_read_continue, 0);
        case KL_MP_EVT_DONE:
            /* DONE mid-part is unexpected (the parser should emit
             * PART_END first), but treat it as end-of-part anyway. */
            it->in_part = 0;
            it->done = 1;
            p->spent = 1;
            luaL_pushresult(&b);
            return 1;
        case KL_MP_EVT_ERROR:
            it->errored = 1;
            return luaL_error(L, "req:multipart(): parser error (code %d)",
                              (int)kl_multipart_last_error(it->inner));
        case KL_MP_EVT_PART_BEGIN:
        default:
            it->errored = 1;
            return luaL_error(L,
                "req:multipart(): unexpected parser event %d inside part",
                (int)ev);
        }
    }
}

static int mp_part_read(lua_State *L)
{
    return mp_part_read_pump(L);
}

static int mp_part_read_continue(lua_State *L, int status, lua_KContext ctx)
{
    (void)status; (void)ctx;
    return mp_part_read_pump(L);
}

/* part:chunks([min_bytes]) - returns a chunks iterator. min_bytes is
 * currently an advisory hint (accepted, ignored): each parser event is
 * surfaced as one chunk. A future slice can coalesce small events. */
static int mp_part_chunks(lua_State *L)
{
    HlMpPart *p = check_part(L, 1);
    /* arg 2 is the optional advisory min-bytes hint; ignored for now. */
    if (!lua_isnoneornil(L, 2)) (void)luaL_checkinteger(L, 2);

    HlMpChunks *c = (HlMpChunks *)lua_newuserdatauv(L, sizeof(*c), 1);
    c->part = p;
    c->ended = 0;
    luaL_setmetatable(L, HL_MP_CHUNKS_MT);
    /* Anchor part (and transitively iter) against GC for the chunks-iter
     * lifetime: uservalue 1 = the Part userdata. */
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1);

    /* Return a step closure capturing the chunks userdata as upvalue 1.
     * for chunk in part:chunks() do ... end then drives mp_chunks_drive. */
    lua_pushcclosure(L, mp_chunks_drive, 1);
    return 1;
}

static int mp_chunks_drive(lua_State *L)
{
    HlMpChunks *c = (HlMpChunks *)luaL_checkudata(L, lua_upvalueindex(1),
                                                    HL_MP_CHUNKS_MT);
    HlMpPart *p = c->part;
    HlMpIter *it = p->iter;

    if (c->ended || p->spent || !it->in_part) {
        lua_pushnil(L);
        return 1;
    }

    for (;;) {
        if (it->errored)
            return luaL_error(L, "req:multipart(): parser error");

        KlMultipartPartMeta meta;
        const char *data = NULL;
        size_t data_len = 0;
        KlMultipartEvent ev = kl_multipart_next(it->inner, &meta,
                                                  &data, &data_len);
        switch (ev) {
        case KL_MP_EVT_PART_DATA:
            if (data && data_len > 0) {
                lua_pushlstring(L, data, data_len);
                return 1;
            }
            /* Empty PART_DATA is unusual but harmless - loop. */
            continue;
        case KL_MP_EVT_PART_END:
            it->in_part = 0;
            p->spent = 1;
            c->ended = 1;
            lua_pushnil(L);
            return 1;
        case KL_MP_EVT_NEED_DATA:
            return mp_park_and_yield(L, it, mp_chunks_continue, 0);
        case KL_MP_EVT_DONE:
            it->in_part = 0;
            it->done = 1;
            p->spent = 1;
            c->ended = 1;
            lua_pushnil(L);
            return 1;
        case KL_MP_EVT_ERROR:
            it->errored = 1;
            return luaL_error(L, "req:multipart(): parser error (code %d)",
                              (int)kl_multipart_last_error(it->inner));
        case KL_MP_EVT_PART_BEGIN:
        default:
            it->errored = 1;
            return luaL_error(L,
                "req:multipart(): unexpected parser event %d inside part",
                (int)ev);
        }
    }
}

static int mp_chunks_continue(lua_State *L, int status, lua_KContext ctx)
{
    (void)status; (void)ctx;
    return mp_chunks_drive(L);
}

/* part.<field> dispatcher - name / filename / content_type, plus the
 * method names that resolve via the metatable's __index table fallback. */
static int mp_part_index(lua_State *L)
{
    HlMpPart *p = check_part(L, 1);
    HlMpIter *it = p->iter;
    const char *key = luaL_checkstring(L, 2);

    if (strcmp(key, "name") == 0) {
        if (it->name) lua_pushlstring(L, it->name, it->name_len);
        else          lua_pushlstring(L, "", 0);
        return 1;
    }
    if (strcmp(key, "filename") == 0) {
        if (it->filename) lua_pushlstring(L, it->filename, it->filename_len);
        else              lua_pushnil(L);
        return 1;
    }
    if (strcmp(key, "content_type") == 0) {
        if (it->content_type)
            lua_pushlstring(L, it->content_type, it->content_type_len);
        else
            lua_pushnil(L);
        return 1;
    }
    if (strcmp(key, "read") == 0) {
        lua_pushcfunction(L, mp_part_read);
        return 1;
    }
    if (strcmp(key, "chunks") == 0) {
        lua_pushcfunction(L, mp_part_chunks);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

/* ── Outer iterator: req:multipart() ────────────────────────────────── */

/* Drain any leftover PART_DATA/PART_END for the previous Part, then
 * advance to PART_BEGIN or DONE. Build + return Part userdata (or nil). */
static int mp_iter_drive(lua_State *L)
{
    HlMpIter *it = (HlMpIter *)luaL_checkudata(L, lua_upvalueindex(1),
                                                 HL_MP_ITER_MT);

    if (it->done) { lua_pushnil(L); return 1; }
    if (it->errored)
        return luaL_error(L, "req:multipart(): parser error");

    for (;;) {
        KlMultipartPartMeta meta;
        const char *data = NULL;
        size_t data_len = 0;
        KlMultipartEvent ev = kl_multipart_next(it->inner, &meta,
                                                  &data, &data_len);
        switch (ev) {
        case KL_MP_EVT_PART_BEGIN:
            if (mp_iter_copy_meta(it, &meta) != 0)
                return luaL_error(L,
                    "req:multipart(): out of memory copying part metadata");
            it->in_part = 1;

            HlMpPart *p = (HlMpPart *)lua_newuserdatauv(L, sizeof(*p), 1);
            p->iter = it;
            p->spent = 0;
            luaL_setmetatable(L, HL_MP_PART_MT);
            /* Anchor iter against GC for the Part's lifetime. */
            lua_pushvalue(L, lua_upvalueindex(1));
            lua_setiuservalue(L, -2, 1);
            return 1;
        case KL_MP_EVT_PART_DATA:
            /* User skipped the previous part's body without iterating
             * chunks - auto-drain. */
            continue;
        case KL_MP_EVT_PART_END:
            it->in_part = 0;
            continue;
        case KL_MP_EVT_DONE:
            it->done = 1;
            it->in_part = 0;
            lua_pushnil(L);
            return 1;
        case KL_MP_EVT_NEED_DATA:
            return mp_park_and_yield(L, it, mp_iter_continue, 0);
        case KL_MP_EVT_ERROR:
            it->errored = 1;
            return luaL_error(L, "req:multipart(): parser error (code %d)",
                              (int)kl_multipart_last_error(it->inner));
        default:
            it->errored = 1;
            return luaL_error(L,
                "req:multipart(): unexpected parser event %d", (int)ev);
        }
    }
}

static int mp_iter_continue(lua_State *L, int status, lua_KContext ctx)
{
    (void)status; (void)ctx;
    return mp_iter_drive(L);
}

/* Iter __gc - free heap-allocated meta-copy buffers. The iter struct
 * itself is freed by Lua. */
static int mp_iter_gc(lua_State *L)
{
    HlMpIter *it = (HlMpIter *)luaL_checkudata(L, 1, HL_MP_ITER_MT);
    mp_iter_clear_meta(it);
    return 0;
}

/* req:multipart() entry point. Captured upvalues:
 *   1 = lightuserdata: KlBodyReader * (the wrapper)
 *   2 = lightuserdata: HlLua *
 */
static int lua_req_multipart(lua_State *L)
{
    KlBodyReader *wrapper =
        (KlBodyReader *)lua_touserdata(L, lua_upvalueindex(1));
    HlLua *lua =
        (HlLua *)lua_touserdata(L, lua_upvalueindex(2));

    if (!wrapper || !lua)
        return luaL_error(L,
            "req:multipart(): no body reader (not a streaming-multipart route?)");

    KlBodyReader *inner = hl_cap_multipart_inner(wrapper);
    if (!inner)
        return luaL_error(L,
            "req:multipart(): body reader is not a streaming-multipart wrapper");

    HlMpIter *it = (HlMpIter *)lua_newuserdatauv(L, sizeof(*it), 0);
    memset(it, 0, sizeof(*it));
    it->wrapper = wrapper;
    it->inner   = inner;
    it->lua     = lua;
    it->alloc   = lua->base.alloc;
    luaL_setmetatable(L, HL_MP_ITER_MT);

    /* Return the step closure capturing the iter userdata as upvalue 1. */
    lua_pushcclosure(L, mp_iter_drive, 1);
    return 1;
}

/* ── Public installer ───────────────────────────────────────────────── */

/*
 * Install the `multipart` method on the request table at stack top.
 * Called from hl_lua_make_request immediately after the table is built
 * - only for routes registered with kl_server_route_streaming, where
 * req->body_reader is hl_cap_multipart_factory's wrapper.
 *
 * No-op if body_reader isn't a streaming-multipart wrapper.
 */
void hl_lua_request_install_multipart(lua_State *L, HlLua *lua,
                                       KlBodyReader *body_reader)
{
    if (!body_reader || !hl_cap_multipart_inner(body_reader))
        return; /* not a streaming-multipart route - skip silently */

    lua_pushlightuserdata(L, body_reader);
    lua_pushlightuserdata(L, lua);
    lua_pushcclosure(L, lua_req_multipart, 2);
    lua_setfield(L, -2, "multipart");
}

/* ── Metatable registration ─────────────────────────────────────────── */

/*
 * Register HlMpIter / HlMpPart / HlMpChunks metatables. Called once
 * during runtime init from hl_lua_register_modules.
 */
void hl_lua_request_register(lua_State *L)
{
    /* HlMpIter - only needs __gc to free meta-copy buffers. The iter
     * itself is opaque to user code (only reached via the closure). */
    luaL_newmetatable(L, HL_MP_ITER_MT);
    lua_pushcfunction(L, mp_iter_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    /* HlMpPart - __index dispatches name/filename/content_type/read/chunks. */
    luaL_newmetatable(L, HL_MP_PART_MT);
    lua_pushcfunction(L, mp_part_index);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);

    /* HlMpChunks - opaque, no methods. (The closure returned by
     * part:chunks() is what the user holds; the userdata is just for
     * upvalue + GC anchoring.) */
    luaL_newmetatable(L, HL_MP_CHUNKS_MT);
    lua_pop(L, 1);
}
