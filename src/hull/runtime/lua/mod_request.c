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
 * The iterator drives Keel's kl_http_multipart_next() against the parkable
 * wrapper (see hl_cap_multipart_factory in cap/body.c). When the parser
 * reports NEED_DATA the iterator parks a resume callback on the wrapper,
 * sets c->state = KL_HTTP_CONN_READING_BODY, and lua_yieldk's. Bytes off the
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
#include "hull/shared/req_life.h"

#include <keel/http_body_reader.h>
#include <keel/http_body_reader_multipart.h>
#include <keel/http_connection.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── Lua metatable names ────────────────────────────────────────────── */

#define HL_MP_ITER_MT   "HlMpIter"
#define HL_MP_PART_MT   "HlMpPart"

/* A Part userdata's uservalues: the iterator it anchors, and its own
 * metadata (audit 10). */
enum {
    MP_PART_UV_ITER = 1,
    MP_PART_UV_NAME,
    MP_PART_UV_FILENAME,
    MP_PART_UV_CTYPE,
    MP_PART_UV_COUNT = MP_PART_UV_CTYPE
};
#define HL_MP_CHUNKS_MT "HlMpChunks"
#define HL_MP_OWNER_MT  "HlMpOwner"

/* ── Internal state ─────────────────────────────────────────────────── */

/* Iterator state. Lifetime owned by Lua GC (it's a userdata).
 *
 * Holds heap-allocated COPIES of the current part's metadata strings:
 * kl_http_multipart_next() docs say the borrowed name/filename/content_type
 * pointers are valid only until the next mutation, which for us means
 * the next kl_http_multipart_next call OR the next on_data callback (which
 * fires inside our park-callback / lua_resume path). The coroutine can
 * yield between reading the meta and the chunks loop - so we always
 * snapshot meta the moment PART_BEGIN fires. The copies stay valid for
 * the lifetime of one Part.
 */
typedef struct {
    KlHttpBodyReader *wrapper;     /* parkable wrapper (from hl_cap_multipart_factory) */
    KlHttpBodyReader *inner;       /* inner kl_http_body_reader_multipart for kl_http_multipart_next */
    HlLua        *lua;         /* runtime - for allocator + async-cont creation */
    HlAllocator  *alloc;       /* shorthand for lua->base.alloc */
    HlReqLife    *life;        /* the request's life (a reference) */
    KlHttpConn   *conn;        /* the connection the request came on */

    int           done;        /* parser hit DONE; subsequent iter:step returns nil */
    int           errored;     /* parser/IO error - iter:step raises */
    int           in_part;     /* between PART_BEGIN..PART_END for the active Part */
    unsigned      gen;         /* bumped at every PART_BEGIN: the current Part's */

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
    /* The iterator's gen when this Part began (audit 9 L5): a Part the outer
     * iterator has moved past is not the current one, and its read() /
     * chunks() consumed the NEXT part's body. */
    unsigned  gen;
    /* part:read()'s accumulator, kept across parks: allocated through the
     * VM's allocator (so it counts against the Lua memory limit), grown
     * geometrically, freed once the value is pushed or by __gc. */
    char     *acc;
    size_t    acc_len;
    size_t    acc_cap;
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
static int mp_iter_copy_meta(HlMpIter *it, const KlHttpMultipartPartMeta *meta)
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

/* ── Ownership ──────────────────────────────────────────────────────────
 * The reader pointers belong to one request. The request table (and with it
 * the `multipart` closure), an iterator, a part or a chunks iterator can all
 * be kept past it - in a global, by another request's handler - and every
 * later call used a body reader Keel had freed. Each holds the request's
 * life, and is usable only while it is live and only from its own request's
 * handler (that connection is the active one): parking also suspends the
 * ACTIVE connection, which must be this request's. */

typedef struct {
    KlHttpBodyReader *wrapper;
    HlLua            *lua;
    HlReqLife        *life;     /* a reference */
    KlHttpConn       *conn;
} HlMpOwner;

static int mp_owner_gc(lua_State *L)
{
    HlMpOwner *o = (HlMpOwner *)luaL_checkudata(L, 1, HL_MP_OWNER_MT);
    hl_req_life_release(o->life);
    o->life = NULL;
    return 0;
}

static int mp_owned(HlLua *lua, HlReqLife *life, KlHttpConn *conn)
{
    return life && hl_req_life_live(life) && lua && lua->active_conn == conn;
}

static void mp_check_usable(lua_State *L, HlMpIter *it)
{
    if (!mp_owned(it->lua, it->life, it->conn))
        luaL_error(L, "req:multipart(): the request is over, or this is "
                      "not its handler");
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
    HlAsyncCont *cont = (HlAsyncCont *)ctx;
    if (!cont) return;
    if (reason == HL_MP_RESUME_CANCEL) {
        /* The connection is going away: the handler never resumes. */
        if (cont->cancel)  cont->cancel(cont);
    } else if (cont->resume) {
        /* iter's drive loop figures the rest out from the next parser event */
        cont->resume(cont, NULL);
    }
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
    hl_lua_check_can_wait(L, "req:multipart()");
    mp_check_usable(L, it);
    KlHttpConn *conn = it->lua->active_conn;
    if (!conn)
        return luaL_error(L,
            "req:multipart(): no active connection - streaming routes "
            "require a live server (hull dev / built binary)");

    /* The parser wants more, but none can come: the body ended (a client
     * that never sent the closing boundary) or failed. Parking would fire
     * its callback inline and resume this coroutine while it is running -
     * the 500 went out and the request ended mid-handler. */
    int st = hl_cap_multipart_state(it->wrapper);
    if (st != 0) {
        it->errored = 1;
        return luaL_error(L, st == 1
            ? "req:multipart(): the request body ended inside a part"
            : "req:multipart(): reading the request body failed");
    }

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

    /* Keel 3.x streaming-async: park and keep reading the request body; Keel
     * re-enters via the body reader's on_data. Replaces the pre-3.0 internal
     * conn->state = KL_HTTP_CONN_READING_BODY; kl_http_request_await_body is the
     * public "park, keep reading" call added in 3.0.0-rc.2. */
    kl_http_request_await_body(it->lua->active_req);

    return lua_yieldk(L, 0, kctx, kfunc);
}

/* ── Part: read + chunks ────────────────────────────────────────────── */

static HlMpPart *check_part(lua_State *L, int idx)
{
    return (HlMpPart *)luaL_checkudata(L, idx, HL_MP_PART_MT);
}

/* Refuse a Part that is no longer the iterator's current one (audit 9 L5):
 * the parser is on a later part, which its read() / chunks() would read. */
static void mp_part_check_current(lua_State *L, HlMpPart *p)
{
    if (!p->spent && p->gen != p->iter->gen) {
        luaL_error(L, "req:multipart(): this part is no longer current "
                      "(the iterator has moved to a later part)");
    }
}

/* part:read() - accumulate every PART_DATA event until PART_END, return
 * one big string. Convenience for small fields; large uploads should use
 * part:chunks() instead. */
static int mp_part_read(lua_State *L);
static int mp_part_read_continue(lua_State *L, int status, lua_KContext ctx);

static void mp_part_acc_free(lua_State *L, HlMpPart *p)
{
    if (p->acc) {
        void *ud;
        lua_Alloc f = lua_getallocf(L, &ud);
        f(ud, p->acc, p->acc_cap, 0);
    }
    p->acc = NULL;
    p->acc_len = 0;
    p->acc_cap = 0;
}

/* Append to the accumulator. Geometric growth, so a field read across many
 * parks is copied O(n) times in all: it used to be re-copied whole into a
 * new Lua string at every park, O(n^2) for a field sent slowly. The bytes
 * are charged to the instruction budget (one unit per 64). */
static void mp_part_acc_add(lua_State *L, HlMpPart *p, const char *data,
                            size_t n)
{
    if (n == 0) return;   /* nothing to copy (and acc may still be NULL) */
    if (n > p->acc_cap - p->acc_len) {
        if (n > SIZE_MAX / 2 - p->acc_len) {
            luaL_error(L, "req:multipart(): part too large");
            return;   /* not reached: luaL_error does not return */
        }
        size_t need = p->acc_len + n;
        size_t cap = p->acc_cap ? p->acc_cap : 4096;
        while (cap < need) cap *= 2;
        void *ud;
        lua_Alloc f = lua_getallocf(L, &ud);
        char *nb = (char *)f(ud, p->acc, p->acc ? p->acc_cap : 0, cap);
        if (!nb) {
            luaL_error(L, "req:multipart(): out of memory reading a part");
            return;   /* not reached */
        }
        p->acc = nb;
        p->acc_cap = cap;
    }
    memcpy(p->acc + p->acc_len, data, n);
    p->acc_len += n;
    lua_hlcharge(L, 0, n);
}

/* Push the accumulated value and release the accumulator (a failed push
 * leaves it to __gc). */
static int mp_part_acc_push(lua_State *L, HlMpPart *p)
{
    lua_pushlstring(L, p->acc ? p->acc : "", p->acc_len);
    mp_part_acc_free(L, p);
    return 1;
}

/* Shared drive loop for read(). Stack contract:
 *   in : Part at index 1 (a continuation may find resume values above it).
 *   out: pushes one result (the assembled Lua string) and returns 1, OR
 *        parks + yields with exactly [Part] on the stack (the continuation
 *        re-enters this function). The bytes read so far stay in the
 *        Part's accumulator across the park. */
static int mp_part_read_pump(lua_State *L)
{
    HlMpPart *p = check_part(L, 1);
    HlMpIter *it = p->iter;

    mp_check_usable(L, it);
    mp_part_check_current(L, p);
    lua_settop(L, 1);

    for (;;) {
        if (it->errored)
            return luaL_error(L, "req:multipart(): parser error");

        if (p->spent || !it->in_part) {
            /* Already drained for this part (e.g. called twice). */
            return mp_part_acc_push(L, p);
        }

        KlHttpMultipartPartMeta meta;
        const char *data = NULL;
        size_t data_len = 0;
        KlHttpMultipartEvent ev = kl_http_multipart_next(it->inner, &meta,
                                                  &data, &data_len);
        switch (ev) {
        case KL_HTTP_MP_EVT_PART_DATA:
            if (data && data_len > 0)
                mp_part_acc_add(L, p, data, data_len);
            continue;
        case KL_HTTP_MP_EVT_PART_END:
            it->in_part = 0;
            p->spent = 1;
            return mp_part_acc_push(L, p);
        case KL_HTTP_MP_EVT_NEED_DATA:
            /* The bytes so far stay in p->acc: a park leaves [Part]. */
            return mp_park_and_yield(L, it, mp_part_read_continue, 0);
        case KL_HTTP_MP_EVT_DONE:
            /* DONE mid-part is unexpected (the parser should emit
             * PART_END first), but treat it as end-of-part anyway. */
            it->in_part = 0;
            it->done = 1;
            p->spent = 1;
            return mp_part_acc_push(L, p);
        case KL_HTTP_MP_EVT_ERROR:
            it->errored = 1;
            return luaL_error(L, "req:multipart(): parser error (code %d)",
                              (int)kl_http_multipart_last_error(it->inner));
        case KL_HTTP_MP_EVT_PART_BEGIN:
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

static int mp_part_gc(lua_State *L)
{
    mp_part_acc_free(L, (HlMpPart *)luaL_checkudata(L, 1, HL_MP_PART_MT));
    return 0;
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

    if (c->ended || p->spent) {
        lua_pushnil(L);
        return 1;
    }
    mp_part_check_current(L, p);
    if (!it->in_part) {
        lua_pushnil(L);
        return 1;
    }
    mp_check_usable(L, it);

    for (;;) {
        if (it->errored)
            return luaL_error(L, "req:multipart(): parser error");

        KlHttpMultipartPartMeta meta;
        const char *data = NULL;
        size_t data_len = 0;
        KlHttpMultipartEvent ev = kl_http_multipart_next(it->inner, &meta,
                                                  &data, &data_len);
        switch (ev) {
        case KL_HTTP_MP_EVT_PART_DATA:
            if (data && data_len > 0) {
                lua_pushlstring(L, data, data_len);
                return 1;
            }
            /* Empty PART_DATA is unusual but harmless - loop. */
            continue;
        case KL_HTTP_MP_EVT_PART_END:
            it->in_part = 0;
            p->spent = 1;
            c->ended = 1;
            lua_pushnil(L);
            return 1;
        case KL_HTTP_MP_EVT_NEED_DATA:
            return mp_park_and_yield(L, it, mp_chunks_continue, 0);
        case KL_HTTP_MP_EVT_DONE:
            it->in_part = 0;
            it->done = 1;
            p->spent = 1;
            c->ended = 1;
            lua_pushnil(L);
            return 1;
        case KL_HTTP_MP_EVT_ERROR:
            it->errored = 1;
            return luaL_error(L, "req:multipart(): parser error (code %d)",
                              (int)kl_http_multipart_last_error(it->inner));
        case KL_HTTP_MP_EVT_PART_BEGIN:
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
    (void)check_part(L, 1);
    const char *key = luaL_checkstring(L, 2);

    /* This part's metadata, snapshotted into the userdata's uservalues when
     * the Part was made (MP_PART_UV_*; audit 10): the iterator's copy is the
     * CURRENT part's, so a Part kept past its loop step reported the next
     * part's name / filename / content_type. */
    if (strcmp(key, "name") == 0) {
        lua_getiuservalue(L, 1, MP_PART_UV_NAME);
        return 1;
    }
    if (strcmp(key, "filename") == 0) {
        lua_getiuservalue(L, 1, MP_PART_UV_FILENAME);
        return 1;
    }
    if (strcmp(key, "content_type") == 0) {
        lua_getiuservalue(L, 1, MP_PART_UV_CTYPE);
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
    mp_check_usable(L, it);

    for (;;) {
        KlHttpMultipartPartMeta meta;
        const char *data = NULL;
        size_t data_len = 0;
        KlHttpMultipartEvent ev = kl_http_multipart_next(it->inner, &meta,
                                                  &data, &data_len);
        switch (ev) {
        case KL_HTTP_MP_EVT_PART_BEGIN:
            if (mp_iter_copy_meta(it, &meta) != 0)
                return luaL_error(L,
                    "req:multipart(): out of memory copying part metadata");
            it->in_part = 1;
            it->gen++;

            HlMpPart *p = (HlMpPart *)lua_newuserdatauv(L, sizeof(*p),
                                                        MP_PART_UV_COUNT);
            memset(p, 0, sizeof(*p));
            p->iter = it;
            p->gen = it->gen;
            luaL_setmetatable(L, HL_MP_PART_MT);
            /* Anchor iter against GC for the Part's lifetime. */
            lua_pushvalue(L, lua_upvalueindex(1));
            lua_setiuservalue(L, -2, MP_PART_UV_ITER);
            /* Its own metadata (audit 10; see mp_part_index). */
            if (it->name) lua_pushlstring(L, it->name, it->name_len);
            else          lua_pushlstring(L, "", 0);
            lua_setiuservalue(L, -2, MP_PART_UV_NAME);
            if (it->filename) lua_pushlstring(L, it->filename, it->filename_len);
            else              lua_pushnil(L);
            lua_setiuservalue(L, -2, MP_PART_UV_FILENAME);
            if (it->content_type)
                lua_pushlstring(L, it->content_type, it->content_type_len);
            else
                lua_pushnil(L);
            lua_setiuservalue(L, -2, MP_PART_UV_CTYPE);
            return 1;
        case KL_HTTP_MP_EVT_PART_DATA:
            /* User skipped the previous part's body without iterating
             * chunks - auto-drain. */
            continue;
        case KL_HTTP_MP_EVT_PART_END:
            it->in_part = 0;
            continue;
        case KL_HTTP_MP_EVT_DONE:
            it->done = 1;
            it->in_part = 0;
            lua_pushnil(L);
            return 1;
        case KL_HTTP_MP_EVT_NEED_DATA:
            return mp_park_and_yield(L, it, mp_iter_continue, 0);
        case KL_HTTP_MP_EVT_ERROR:
            it->errored = 1;
            return luaL_error(L, "req:multipart(): parser error (code %d)",
                              (int)kl_http_multipart_last_error(it->inner));
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
    hl_req_life_release(it->life);
    it->life = NULL;
    return 0;
}

/* req:multipart() entry point. Upvalue 1 = the request's HlMpOwner. */
static int lua_req_multipart(lua_State *L)
{
    HlMpOwner *o = (HlMpOwner *)luaL_checkudata(L, lua_upvalueindex(1),
                                                 HL_MP_OWNER_MT);
    KlHttpBodyReader *wrapper = o->wrapper;
    HlLua *lua = o->lua;

    if (!wrapper || !lua)
        return luaL_error(L,
            "req:multipart(): no body reader (not a streaming-multipart route?)");
    if (!mp_owned(lua, o->life, o->conn))
        return luaL_error(L, "req:multipart(): the request is over, or this "
                             "is not its handler");

    KlHttpBodyReader *inner = hl_cap_multipart_inner(wrapper);
    if (!inner)
        return luaL_error(L,
            "req:multipart(): body reader is not a streaming-multipart wrapper");

    HlMpIter *it = (HlMpIter *)lua_newuserdatauv(L, sizeof(*it), 0);
    memset(it, 0, sizeof(*it));
    luaL_setmetatable(L, HL_MP_ITER_MT);
    it->wrapper = wrapper;
    it->inner   = inner;
    it->lua     = lua;
    it->alloc   = lua->base.alloc;
    it->conn    = o->conn;
    it->life    = o->life;
    hl_req_life_retain(it->life);     /* released by mp_iter_gc */

    /* Return the step closure capturing the iter userdata as upvalue 1. */
    lua_pushcclosure(L, mp_iter_drive, 1);
    return 1;
}

/* ── Public installer ───────────────────────────────────────────────── */

/*
 * Install the `multipart` method on the request table at stack top.
 * Called from hl_lua_make_request immediately after the table is built
 * - only for routes registered with kl_http_server_route_streaming, where
 * req->body_reader is hl_cap_multipart_factory's wrapper.
 *
 * No-op if body_reader isn't a streaming-multipart wrapper.
 */
void hl_lua_request_install_multipart(lua_State *L, HlLua *lua,
                                       KlHttpBodyReader *body_reader,
                                       HlReqLife *life, KlHttpConn *conn)
{
    if (!body_reader || !hl_cap_multipart_inner(body_reader))
        return; /* not a streaming-multipart route - skip silently */

    HlMpOwner *o = (HlMpOwner *)lua_newuserdatauv(L, sizeof *o, 0);
    memset(o, 0, sizeof *o);
    luaL_setmetatable(L, HL_MP_OWNER_MT);
    o->wrapper = body_reader;
    o->lua     = lua;
    o->conn    = conn;
    o->life    = life;
    hl_req_life_retain(life);          /* released by mp_owner_gc */
    lua_pushcclosure(L, lua_req_multipart, 1);
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

    luaL_newmetatable(L, HL_MP_OWNER_MT);
    lua_pushcfunction(L, mp_owner_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    /* HlMpPart - __index dispatches name/filename/content_type/read/chunks;
     * __gc frees a read() accumulator its request left behind (an error,
     * or a connection that went away mid-part). Locked, so app code
     * cannot take the __gc away. */
    luaL_newmetatable(L, HL_MP_PART_MT);
    lua_pushcfunction(L, mp_part_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, mp_part_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);

    /* HlMpChunks - opaque, no methods. (The closure returned by
     * part:chunks() is what the user holds; the userdata is just for
     * upvalue + GC anchoring.) */
    luaL_newmetatable(L, HL_MP_CHUNKS_MT);
    lua_pop(L, 1);
}
