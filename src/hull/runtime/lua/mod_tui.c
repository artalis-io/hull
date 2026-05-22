/* mod_tui.c — hull.tui module: terminal UI bindings.
 *
 * The cap layer enforces a single-ctx-per-process invariant, so the
 * binding mirrors that: one module-level ctx, acquired lazily on
 * first call. `tui.enter()` / `tui.leave()` are explicit but
 * idempotent; `tui.run` (Lua-side helper in stdlib) calls them.
 *
 * Async semantics: `tui.poll` integrates with the runtime's async
 * backend when a backend is wired. If no backend ctx is available
 * (tests, tool VMs), it falls back to the cap layer's blocking poll.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"

#ifdef HL_ENABLE_TUI

#include "hull/alloc.h"
#include "hull/async.h"
#include "hull/async_backend.h"
#include "hull/cap/tui.h"
#include "hull/runtime/lua.h"
#include "internal.h"
#include "log.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Cap for the periodic timer slice. The async tui.poll wakes every
 * HL_TUI_POLL_SLICE_MS to drain stdin into the parser; on each
 * wake-up, if the parser has been idle (no new bytes since the last
 * read) for at least HL_TUI_POLL_ESC_IDLE_MS, the binding asks the
 * cap layer to commit any in-flight ESC as a bare-escape event.
 *
 * Slice should be << ESC idle: it bounds the input latency, while
 * ESC idle bounds the lone-ESC commit delay. 25/50ms is the same
 * pair the cap layer's blocking poll uses internally — matching is
 * intentional. */
#define HL_TUI_POLL_SLICE_MS     25
#define HL_TUI_POLL_ESC_IDLE_MS  50

/* The cap layer enforces "at most one HlTuiCtx per process." Cache it
 * here so successive script calls reuse the same handle without
 * re-acquiring. */
static HlTuiCtx *g_ctx = NULL;

static int ensure_acquired(lua_State *L)
{
    if (g_ctx) return 0;
    int rc = hl_cap_tui_acquire(&g_ctx);
    if (rc != 0) {
        if (errno == EBUSY) {
            return luaL_error(L, "tui: another TUI session is already active in this process");
        }
        if (errno == ENOTTY) {
            return luaL_error(L, "tui: not attached to a terminal (stdin/stdout redirected?)");
        }
        return luaL_error(L, "tui: acquire failed (errno=%d)", errno);
    }
    return 0;
}

/* ── Lifecycle ─────────────────────────────────────────────────── */

static int lua_tui_enter(lua_State *L)
{
    ensure_acquired(L);
    return 0;
}

static int lua_tui_leave(lua_State *L)
{
    (void)L;
    if (g_ctx) {
        hl_cap_tui_release(g_ctx);
        g_ctx = NULL;
    }
    return 0;
}

/* ── Query ─────────────────────────────────────────────────────── */

static int lua_tui_size(lua_State *L)
{
    ensure_acquired(L);
    int cols = 0, rows = 0;
    hl_cap_tui_size(g_ctx, &cols, &rows);
    lua_pushinteger(L, cols);
    lua_pushinteger(L, rows);
    return 2;
}

static int lua_tui_caps(lua_State *L)
{
    ensure_acquired(L);
    uint32_t caps = hl_cap_tui_caps(g_ctx);
    lua_newtable(L);
    lua_pushboolean(L, (caps & HL_TUI_CAP_TRUECOLOR)  ? 1 : 0); lua_setfield(L, -2, "truecolor");
    lua_pushboolean(L, (caps & HL_TUI_CAP_256COLOR)   ? 1 : 0); lua_setfield(L, -2, "color256");
    lua_pushboolean(L, (caps & HL_TUI_CAP_BASIC_COLOR)? 1 : 0); lua_setfield(L, -2, "color");
    lua_pushboolean(L, (caps & HL_TUI_CAP_MOUSE)      ? 1 : 0); lua_setfield(L, -2, "mouse");
    lua_pushboolean(L, (caps & HL_TUI_CAP_FOCUS)      ? 1 : 0); lua_setfield(L, -2, "focus");
    lua_pushboolean(L, (caps & HL_TUI_CAP_KITTY_KBD)  ? 1 : 0); lua_setfield(L, -2, "kitty_kbd");
    lua_pushboolean(L, (caps & HL_TUI_CAP_OSC52)      ? 1 : 0); lua_setfield(L, -2, "clipboard");
    return 1;
}

static int lua_tui_theme(lua_State *L)
{
    ensure_acquired(L);
    lua_pushstring(L, hl_cap_tui_theme(g_ctx));
    return 1;
}

/* ── Rendering ─────────────────────────────────────────────────── */

static int lua_tui_clear(lua_State *L)
{
    ensure_acquired(L);
    hl_cap_tui_clear(g_ctx);
    return 0;
}

static int lua_tui_invalidate(lua_State *L)
{
    ensure_acquired(L);
    hl_cap_tui_invalidate(g_ctx);
    return 0;
}

static int lua_tui_move(lua_State *L)
{
    ensure_acquired(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    hl_cap_tui_move(g_ctx, x, y);
    return 0;
}

/* Decode the optional style argument table:
 *   { fg = 0xRRGGBB | "rgb(r,g,b)" | nil,
 *     bg = ..., bold, italic, underline, reverse, strike }
 * Returns fg, bg, attr packed into the call's parameters. */
static uint32_t color_from_lua(lua_State *L, int idx)
{
    if (lua_isnil(L, idx)) return HL_TUI_COLOR_DEFAULT;
    if (lua_isinteger(L, idx)) {
        lua_Integer v = lua_tointeger(L, idx);
        return (uint32_t)v & 0x00FFFFFFu;
    }
    return HL_TUI_COLOR_DEFAULT;
}

static int lua_tui_style(lua_State *L)
{
    ensure_acquired(L);
    uint32_t fg = HL_TUI_COLOR_DEFAULT, bg = HL_TUI_COLOR_DEFAULT, attr = 0;
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "fg"); fg = color_from_lua(L, -1); lua_pop(L, 1);
        lua_getfield(L, 1, "bg"); bg = color_from_lua(L, -1); lua_pop(L, 1);
        lua_getfield(L, 1, "bold");      if (lua_toboolean(L, -1)) attr |= HL_TUI_ATTR_BOLD;      lua_pop(L, 1);
        lua_getfield(L, 1, "dim");       if (lua_toboolean(L, -1)) attr |= HL_TUI_ATTR_DIM;       lua_pop(L, 1);
        lua_getfield(L, 1, "italic");    if (lua_toboolean(L, -1)) attr |= HL_TUI_ATTR_ITALIC;    lua_pop(L, 1);
        lua_getfield(L, 1, "underline"); if (lua_toboolean(L, -1)) attr |= HL_TUI_ATTR_UNDERLINE; lua_pop(L, 1);
        lua_getfield(L, 1, "reverse");   if (lua_toboolean(L, -1)) attr |= HL_TUI_ATTR_REVERSE;   lua_pop(L, 1);
        lua_getfield(L, 1, "strike");    if (lua_toboolean(L, -1)) attr |= HL_TUI_ATTR_STRIKE;    lua_pop(L, 1);
    }
    hl_cap_tui_style(g_ctx, fg, bg, attr);
    return 0;
}

static int lua_tui_print(lua_State *L)
{
    ensure_acquired(L);
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    size_t len = 0;
    const char *s = luaL_checklstring(L, 3, &len);
    hl_cap_tui_move(g_ctx, x, y);
    hl_cap_tui_write(g_ctx, s, len);
    return 0;
}

static int lua_tui_write(lua_State *L)
{
    ensure_acquired(L);
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    hl_cap_tui_write(g_ctx, s, len);
    return 0;
}

static int lua_tui_flush(lua_State *L)
{
    ensure_acquired(L);
    hl_cap_tui_flush(g_ctx);
    return 0;
}

/* ── Input ─────────────────────────────────────────────────────── */

/* Push the current cap-layer event onto Lua as a table. */
static void push_event(lua_State *L, const HlTuiEvent *ev)
{
    lua_newtable(L);
    switch (ev->kind) {
    case HL_TUI_EV_KEY:
        lua_pushstring(L, "key");       lua_setfield(L, -2, "kind");
        lua_pushstring(L, ev->key ? ev->key : "");
        lua_setfield(L, -2, "key");
        if (ev->codepoint) {
            lua_pushinteger(L, (lua_Integer)ev->codepoint);
            lua_setfield(L, -2, "codepoint");
        }
        lua_pushinteger(L, (lua_Integer)ev->mods);
        lua_setfield(L, -2, "mods");
        lua_pushboolean(L, (ev->mods & HL_TUI_MOD_SHIFT) ? 1 : 0); lua_setfield(L, -2, "shift");
        lua_pushboolean(L, (ev->mods & HL_TUI_MOD_ALT)   ? 1 : 0); lua_setfield(L, -2, "alt");
        lua_pushboolean(L, (ev->mods & HL_TUI_MOD_CTRL)  ? 1 : 0); lua_setfield(L, -2, "ctrl");
        lua_pushboolean(L, (ev->mods & HL_TUI_MOD_SUPER) ? 1 : 0); lua_setfield(L, -2, "super");
        break;
    case HL_TUI_EV_RESIZE:
        lua_pushstring(L, "resize");    lua_setfield(L, -2, "kind");
        lua_pushinteger(L, ev->cols);   lua_setfield(L, -2, "cols");
        lua_pushinteger(L, ev->rows);   lua_setfield(L, -2, "rows");
        break;
    case HL_TUI_EV_MOUSE:
        lua_pushstring(L, "mouse");     lua_setfield(L, -2, "kind");
        lua_pushinteger(L, ev->x);      lua_setfield(L, -2, "x");
        lua_pushinteger(L, ev->y);      lua_setfield(L, -2, "y");
        lua_pushinteger(L, ev->button); lua_setfield(L, -2, "button");
        break;
    case HL_TUI_EV_PASTE:
        lua_pushstring(L, "paste");     lua_setfield(L, -2, "kind");
        lua_pushlstring(L, ev->text ? ev->text : "", ev->text_len);
        lua_setfield(L, -2, "text");
        break;
    case HL_TUI_EV_FOCUS:
        lua_pushstring(L, "focus");     lua_setfield(L, -2, "kind");
        lua_pushboolean(L, ev->focus_in ? 1 : 0);
        lua_setfield(L, -2, "in");
        break;
    default:
        lua_pushstring(L, "none");      lua_setfield(L, -2, "kind");
        break;
    }
}

/* ── Async-integrated poll ─────────────────────────────────────
 *
 * tui.poll(timeout_ms) — yields the calling coroutine for up to
 * `timeout_ms` while the event loop continues to run timers, watchers,
 * and other coroutines. The runtime is a single-threaded cooperative
 * model: while one coroutine is suspended on tui.poll, others (e.g.
 * a tui.async background fetch) can make progress on the same loop.
 *
 * Mechanism: a periodic timer (HL_TUI_POLL_SLICE_MS) drains stdin
 * into the parser and tries to pop an event. The timer also tracks
 * how long the parser has been idle (no new bytes), and after
 * HL_TUI_POLL_ESC_IDLE_MS of silence asks the cap layer to commit
 * any in-flight ESC as a bare-escape event — resolving the standard
 * "lone ESC vs. start of CSI" ambiguity without blocking.
 */

typedef struct TuiPollOp {
    HlAsyncCtx *async_ctx;       /* owns this op's lifetime */
    HlLua      *lua;
    HlTuiEvent  ev;
    int         has_event;       /* 1 if op->ev is populated */
    int         remaining_ms;    /* counts down to zero */
    uint64_t    last_byte_ms;    /* monotonic_ms of most recent drained byte */
    uint64_t    timer_id;        /* current scheduled timer */
} TuiPollOp;

/* push_result: called by HlLuaAsyncCont::resume to deliver the
 * outcome to the Lua coroutine. Pushes either the event table or
 * nil (timeout). */
static void tui_poll_push_result(lua_State *co, void *driver)
{
    TuiPollOp *op = (TuiPollOp *)driver;
    if (op && op->has_event) push_event(co, &op->ev);
    else                     lua_pushnil(co);
}

/* free_driver: invoked after resume. */
static void tui_poll_free(void *driver) { free(driver); }

static void tui_poll_schedule(TuiPollOp *op);

static void tui_poll_complete(TuiPollOp *op)
{
    /* Resume the suspended coroutine. resume_detached frees the
     * driver (us) via free_driver, plus the cont + ctx. */
    hl_async_ctx_resume_detached(op->async_ctx);
}

static void tui_poll_tick(void *user)
{
    TuiPollOp *op = (TuiPollOp *)user;
    const HlAsyncBackend *be = hl_async_backend();
    uint64_t now = be->monotonic_ms();

    /* Drain everything currently readable into the parser. */
    int drained = 0;
    for (;;) {
        int n = hl_cap_tui_drain(g_ctx);
        if (n <= 0) break;
        drained += n;
    }
    if (drained > 0) op->last_byte_ms = now;

    /* Try to deliver an event. */
    HlTuiEvent ev = {0};
    if (hl_cap_tui_next_event(g_ctx, &ev) == 0) {
        op->ev = ev;
        op->has_event = 1;
        tui_poll_complete(op);
        return;
    }

    /* If the parser is pending and quiet for >= ESC idle window,
     * commit a bare ESC if applicable. */
    if (hl_cap_tui_parser_pending(g_ctx) &&
        (now - op->last_byte_ms) >= HL_TUI_POLL_ESC_IDLE_MS) {
        if (hl_cap_tui_commit_idle(g_ctx) &&
            hl_cap_tui_next_event(g_ctx, &ev) == 0) {
            op->ev = ev;
            op->has_event = 1;
            tui_poll_complete(op);
            return;
        }
    }

    /* No event yet. Decrement remaining and either reschedule or
     * resolve with timeout. */
    op->remaining_ms -= HL_TUI_POLL_SLICE_MS;
    if (op->remaining_ms <= 0) {
        op->has_event = 0;
        tui_poll_complete(op);
        return;
    }
    tui_poll_schedule(op);
}

static void tui_poll_schedule(TuiPollOp *op)
{
    const HlAsyncBackend *be = hl_async_backend();
    uint64_t slice = (op->remaining_ms < HL_TUI_POLL_SLICE_MS)
                     ? (uint64_t)op->remaining_ms
                     : HL_TUI_POLL_SLICE_MS;
    if (slice == 0) slice = 1;   /* never schedule a 0ms one-shot */
    op->timer_id = be->timer_add(op->lua->base.async_ctx, slice,
                                  tui_poll_tick, op);
}

static int lua_tui_poll(lua_State *L)
{
    ensure_acquired(L);
    int timeout_ms = 0;
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1))
        timeout_ms = (int)luaL_checkinteger(L, 1);

    /* Fast path: try non-blocking. Catches the common case where
     * an event is already queued in the parser (e.g., a prior
     * tui.poll feed left a decoded event behind, or stdin has
     * been readable since the last call). */
    HlTuiEvent ev = {0};
    int rc = hl_cap_tui_poll(g_ctx, 0, &ev);
    if (rc < 0) return luaL_error(L, "tui.poll failed (errno=%d)", errno);
    if (rc == 0) { push_event(L, &ev); return 1; }
    if (timeout_ms == 0) { lua_pushnil(L); return 1; }

    /* Slow path: yield to the event loop. */
    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);

    if (!lua || !lua->base.async_ctx) {
        /* No event loop wired — fall back to blocking. This happens
         * in test harnesses and tool VMs that don't init the async
         * backend. Behaviorally identical to the old API. */
        rc = hl_cap_tui_poll(g_ctx, timeout_ms, &ev);
        if (rc < 0) return luaL_error(L, "tui.poll failed (errno=%d)", errno);
        if (rc == 1) { lua_pushnil(L); return 1; }
        push_event(L, &ev);
        return 1;
    }

    if (timeout_ms < 0) timeout_ms = INT_MAX;

    TuiPollOp *op = calloc(1, sizeof *op);
    if (!op) return luaL_error(L, "tui.poll: out of memory");
    op->lua          = lua;
    op->remaining_ms = timeout_ms;
    op->last_byte_ms = hl_async_backend()->monotonic_ms();

    HlAsyncCtx *async = hl_async_ctx_create(lua->server,
                                             lua->base.net_ctx,
                                             lua->base.alloc);
    if (!async) { free(op); return luaL_error(L, "tui.poll: oom (ctx)"); }
    extern HlAsyncCont *hl_lua_async_cont_create(HlLua *, HlAllocator *,
                                                   HlLuaPushResultFn);
    HlAsyncCont *cont = hl_lua_async_cont_create(lua, lua->base.alloc,
                                                  tui_poll_push_result);
    if (!cont) {
        hl_async_ctx_free(async); free(op);
        return luaL_error(L, "tui.poll: oom (cont)");
    }
    async->cont        = cont;
    async->driver      = op;
    async->free_driver = tui_poll_free;
    async->detached    = 1;
    op->async_ctx      = async;

    tui_poll_schedule(op);
    return lua_yieldk(L, 0, 0, NULL);
}

/* ── Clipboard + modes ─────────────────────────────────────────── */

static int lua_tui_clipboard_set(lua_State *L)
{
    ensure_acquired(L);
    size_t len = 0;
    const char *s = luaL_checklstring(L, 1, &len);
    int rc = hl_cap_tui_clipboard_set(g_ctx, s, len);
    if (rc < 0) {
        if (errno == ENOTSUP)
            return luaL_error(L, "tui: clipboard not supported by this terminal");
        return luaL_error(L, "tui: clipboard_set failed (errno=%d)", errno);
    }
    return 0;
}

static int lua_tui_enable_mouse(lua_State *L)
{
    ensure_acquired(L);
    int on = lua_toboolean(L, 1);
    hl_cap_tui_enable_mouse(g_ctx, on);
    return 0;
}

static int lua_tui_enable_paste(lua_State *L)
{
    ensure_acquired(L);
    int on = lua_toboolean(L, 1);
    hl_cap_tui_enable_paste(g_ctx, on);
    return 0;
}

static int lua_tui_enable_focus(lua_State *L)
{
    ensure_acquired(L);
    int on = lua_toboolean(L, 1);
    hl_cap_tui_enable_focus(g_ctx, on);
    return 0;
}

static int lua_tui_enable_kitty_kbd(lua_State *L)
{
    ensure_acquired(L);
    int on = lua_toboolean(L, 1);
    hl_cap_tui_enable_kitty_kbd(g_ctx, on);
    return 0;
}

/* ── Async helper ──────────────────────────────────────────────── */

/* tui.async(fn) — spawn fn in a detached coroutine running on the
 * event loop. Lives under hull.tui (not hull.*) because TUI is its
 * only caller today; promote to hull.async if another consumer
 * shows up.
 *
 * The body might call async-yielding primitives (hull.sleep,
 * compute.async, http.fetch). Those primitives capture
 * `lua->active_co` at suspension time as the coroutine they should
 * resume — so we MUST set active_co (and the matching dispatch
 * bookkeeping) to the bg coroutine for the duration of its first
 * resume. Subsequent resumes happen via hl_lua_async_resume which
 * sets active_co correctly itself.
 *
 * We hold a registry ref so the new coroutine survives GC until it
 * either returns synchronously here, or yields and is later
 * cleaned up by hl_lua_async_resume's LUA_OK / error branches
 * (which already unref + decrement dispatch_depth). */
static int lua_tui_async(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);

    lua_getfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    HlLua *lua = (HlLua *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!lua)
        return luaL_error(L, "tui.async: no runtime context");

    /* Create + ref the new coroutine. */
    lua_State *co = lua_newthread(L);
    int co_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    /* Move the user function onto the new coroutine's stack. */
    lua_pushvalue(L, 1);
    lua_xmove(L, co, 1);

    /* Save caller's dispatch state so we can restore it after the
     * first resume (whether the bg yields or completes). */
    lua_State *saved_co         = lua->active_co;
    int        saved_thread_ref = lua->active_thread_ref;
    KlConn    *saved_conn       = lua->active_conn;

    lua->active_co         = co;
    lua->active_thread_ref = co_ref;
    lua->active_conn       = NULL;  /* bg is always detached */
    lua->dispatch_depth++;

    int nres = 0;
    int sr   = lua_resume(co, L, 0, &nres);

    if (sr == LUA_OK) {
        /* Completed synchronously — unref + balance dispatch. */
        luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
        lua->dispatch_depth--;
    } else if (sr == LUA_YIELD) {
        /* Bg yielded. The async machinery captured (co_ref, co) on
         * the suspension's cont; cleanup happens in
         * hl_lua_async_resume when the bg finally returns. We just
         * restore the caller's state below. */
    } else {
        /* Error on first dispatch. Log + unref + balance. */
        const char *msg = lua_tostring(co, -1);
        log_error("[hull:tui] tui.async coroutine error: %s",
                  msg ? msg : "(unknown)");
        luaL_unref(L, LUA_REGISTRYINDEX, co_ref);
        lua->dispatch_depth--;
    }

    /* Restore caller's dispatch state. */
    lua->active_co         = saved_co;
    lua->active_thread_ref = saved_thread_ref;
    lua->active_conn       = saved_conn;
    return 0;
}

/* ── Module table ──────────────────────────────────────────────── */

static const luaL_Reg tui_funcs[] = {
    {"enter",            lua_tui_enter},
    {"leave",            lua_tui_leave},
    {"size",             lua_tui_size},
    {"caps",             lua_tui_caps},
    {"theme",            lua_tui_theme},

    {"clear",            lua_tui_clear},
    {"invalidate",       lua_tui_invalidate},
    {"move",             lua_tui_move},
    {"style",            lua_tui_style},
    {"print",            lua_tui_print},
    {"write",            lua_tui_write},
    {"flush",            lua_tui_flush},

    {"poll",             lua_tui_poll},

    {"clipboard_set",    lua_tui_clipboard_set},

    {"enable_mouse",     lua_tui_enable_mouse},
    {"enable_paste",     lua_tui_enable_paste},
    {"enable_focus",     lua_tui_enable_focus},
    {"enable_kitty_kbd", lua_tui_enable_kitty_kbd},

    {"async",            lua_tui_async},

    {NULL, NULL}
};

int luaopen_hull_tui(lua_State *L)
{
    luaL_newlib(L, tui_funcs);
    /* Constant: mod-bit table for ev.mods comparisons. */
    lua_pushinteger(L, HL_TUI_MOD_SHIFT); lua_setfield(L, -2, "MOD_SHIFT");
    lua_pushinteger(L, HL_TUI_MOD_ALT);   lua_setfield(L, -2, "MOD_ALT");
    lua_pushinteger(L, HL_TUI_MOD_CTRL);  lua_setfield(L, -2, "MOD_CTRL");
    lua_pushinteger(L, HL_TUI_MOD_SUPER); lua_setfield(L, -2, "MOD_SUPER");
    return 1;
}

#endif /* HL_ENABLE_TUI */
