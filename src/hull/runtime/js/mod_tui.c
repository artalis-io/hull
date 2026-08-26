/*
 * mod_tui.c - hull:tui module (terminal UI bindings).
 *
 * Mirrors src/hull/runtime/lua/mod_tui.c - same cap-layer entry
 * points, JS naming convention (camelCase).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"

#ifdef HL_ENABLE_TUI

#include "hull/utils/alloc.h"
#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/cap/tui.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#define HL_TUI_POLL_SLICE_MS     25
#define HL_TUI_POLL_ESC_IDLE_MS  50

static HlTuiCtx *g_ctx = NULL;

static int ensure_acquired(JSContext *ctx)
{
    if (g_ctx) return 0;
    int rc = hl_cap_tui_acquire(&g_ctx);
    if (rc != 0) {
        if (errno == EBUSY) {
            JS_ThrowInternalError(ctx,
                "tui: another TUI session is already active in this process");
            return -1;
        }
        if (errno == ENOTTY) {
            JS_ThrowInternalError(ctx,
                "tui: not attached to a terminal (stdin/stdout redirected?)");
            return -1;
        }
        JS_ThrowInternalError(ctx, "tui: acquire failed (errno=%d)", errno);
        return -1;
    }
    return 0;
}

/* ── Lifecycle ─────────────────────────────────────────────────── */

static JSValue js_tui_enter(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue js_tui_leave(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)ctx; (void)this_val; (void)argc; (void)argv;
    if (g_ctx) {
        hl_cap_tui_release(g_ctx);
        g_ctx = NULL;
    }
    return JS_UNDEFINED;
}

/* ── Query ─────────────────────────────────────────────────────── */

static JSValue js_tui_size(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    int cols = 0, rows = 0;
    hl_cap_tui_size(g_ctx, &cols, &rows);
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "cols", JS_NewInt32(ctx, cols));
    JS_SetPropertyStr(ctx, obj, "rows", JS_NewInt32(ctx, rows));
    return obj;
}

static JSValue js_tui_caps(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    uint32_t caps = hl_cap_tui_caps(g_ctx);
    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "truecolor", JS_NewBool(ctx, !!(caps & HL_TUI_CAP_TRUECOLOR)));
    JS_SetPropertyStr(ctx, o, "color256",  JS_NewBool(ctx, !!(caps & HL_TUI_CAP_256COLOR)));
    JS_SetPropertyStr(ctx, o, "color",     JS_NewBool(ctx, !!(caps & HL_TUI_CAP_BASIC_COLOR)));
    JS_SetPropertyStr(ctx, o, "mouse",     JS_NewBool(ctx, !!(caps & HL_TUI_CAP_MOUSE)));
    JS_SetPropertyStr(ctx, o, "focus",     JS_NewBool(ctx, !!(caps & HL_TUI_CAP_FOCUS)));
    JS_SetPropertyStr(ctx, o, "kittyKbd",  JS_NewBool(ctx, !!(caps & HL_TUI_CAP_KITTY_KBD)));
    JS_SetPropertyStr(ctx, o, "clipboard", JS_NewBool(ctx, !!(caps & HL_TUI_CAP_OSC52)));
    return o;
}

static JSValue js_tui_theme(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    return JS_NewString(ctx, hl_cap_tui_theme(g_ctx));
}

/* ── Rendering ─────────────────────────────────────────────────── */

static JSValue js_tui_clear(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    hl_cap_tui_clear(g_ctx);
    return JS_UNDEFINED;
}

static JSValue js_tui_invalidate(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    hl_cap_tui_invalidate(g_ctx);
    return JS_UNDEFINED;
}

static JSValue js_tui_move(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    int x = 1, y = 1;
    if (argc >= 1) JS_ToInt32(ctx, &x, argv[0]);
    if (argc >= 2) JS_ToInt32(ctx, &y, argv[1]);
    hl_cap_tui_move(g_ctx, x, y);
    return JS_UNDEFINED;
}

static uint32_t color_from_js(JSContext *ctx, JSValue v)
{
    if (JS_IsUndefined(v) || JS_IsNull(v)) return HL_TUI_COLOR_DEFAULT;
    if (JS_IsNumber(v)) {
        int32_t n = 0;
        JS_ToInt32(ctx, &n, v);
        return (uint32_t)n & 0x00FFFFFFu;
    }
    return HL_TUI_COLOR_DEFAULT;
}

static JSValue js_tui_style(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    uint32_t fg = HL_TUI_COLOR_DEFAULT, bg = HL_TUI_COLOR_DEFAULT, attr = 0;
    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[0], "fg"); fg = color_from_js(ctx, v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "bg"); bg = color_from_js(ctx, v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "bold");      if (JS_ToBool(ctx, v) > 0) attr |= HL_TUI_ATTR_BOLD;      JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "dim");       if (JS_ToBool(ctx, v) > 0) attr |= HL_TUI_ATTR_DIM;       JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "italic");    if (JS_ToBool(ctx, v) > 0) attr |= HL_TUI_ATTR_ITALIC;    JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "underline"); if (JS_ToBool(ctx, v) > 0) attr |= HL_TUI_ATTR_UNDERLINE; JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "reverse");   if (JS_ToBool(ctx, v) > 0) attr |= HL_TUI_ATTR_REVERSE;   JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "strike");    if (JS_ToBool(ctx, v) > 0) attr |= HL_TUI_ATTR_STRIKE;    JS_FreeValue(ctx, v);
    }
    hl_cap_tui_style(g_ctx, fg, bg, attr);
    return JS_UNDEFINED;
}

static JSValue js_tui_print(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    if (argc < 3) return JS_ThrowTypeError(ctx, "tui.print(x, y, str)");
    int32_t x = 1, y = 1;
    JS_ToInt32(ctx, &x, argv[0]);
    JS_ToInt32(ctx, &y, argv[1]);
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[2]);
    if (!s) return JS_EXCEPTION;
    hl_cap_tui_move(g_ctx, x, y);
    hl_cap_tui_write(g_ctx, s, len);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue js_tui_write(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    if (argc < 1) return JS_UNDEFINED;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_EXCEPTION;
    hl_cap_tui_write(g_ctx, s, len);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue js_tui_flush(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    hl_cap_tui_flush(g_ctx);
    return JS_UNDEFINED;
}

/* ── Input ─────────────────────────────────────────────────────── */

static JSValue event_to_js(JSContext *ctx, const HlTuiEvent *ev)
{
    JSValue o = JS_NewObject(ctx);
    switch (ev->kind) {
    case HL_TUI_EV_KEY:
        JS_SetPropertyStr(ctx, o, "kind", JS_NewString(ctx, "key"));
        JS_SetPropertyStr(ctx, o, "key",
            JS_NewString(ctx, ev->key ? ev->key : ""));
        if (ev->codepoint)
            JS_SetPropertyStr(ctx, o, "codepoint",
                JS_NewUint32(ctx, ev->codepoint));
        JS_SetPropertyStr(ctx, o, "mods",  JS_NewUint32(ctx, ev->mods));
        JS_SetPropertyStr(ctx, o, "shift", JS_NewBool(ctx, !!(ev->mods & HL_TUI_MOD_SHIFT)));
        JS_SetPropertyStr(ctx, o, "alt",   JS_NewBool(ctx, !!(ev->mods & HL_TUI_MOD_ALT)));
        JS_SetPropertyStr(ctx, o, "ctrl",  JS_NewBool(ctx, !!(ev->mods & HL_TUI_MOD_CTRL)));
        JS_SetPropertyStr(ctx, o, "super", JS_NewBool(ctx, !!(ev->mods & HL_TUI_MOD_SUPER)));
        break;
    case HL_TUI_EV_RESIZE:
        JS_SetPropertyStr(ctx, o, "kind", JS_NewString(ctx, "resize"));
        JS_SetPropertyStr(ctx, o, "cols", JS_NewInt32(ctx, ev->cols));
        JS_SetPropertyStr(ctx, o, "rows", JS_NewInt32(ctx, ev->rows));
        break;
    case HL_TUI_EV_MOUSE:
        JS_SetPropertyStr(ctx, o, "kind", JS_NewString(ctx, "mouse"));
        JS_SetPropertyStr(ctx, o, "x", JS_NewInt32(ctx, ev->x));
        JS_SetPropertyStr(ctx, o, "y", JS_NewInt32(ctx, ev->y));
        JS_SetPropertyStr(ctx, o, "button", JS_NewInt32(ctx, ev->button));
        break;
    case HL_TUI_EV_PASTE:
        JS_SetPropertyStr(ctx, o, "kind", JS_NewString(ctx, "paste"));
        JS_SetPropertyStr(ctx, o, "text",
            JS_NewStringLen(ctx, ev->text ? ev->text : "", ev->text_len));
        break;
    case HL_TUI_EV_FOCUS:
        JS_SetPropertyStr(ctx, o, "kind", JS_NewString(ctx, "focus"));
        JS_SetPropertyStr(ctx, o, "in",   JS_NewBool(ctx, ev->focus_in));
        break;
    default:
        JS_SetPropertyStr(ctx, o, "kind", JS_NewString(ctx, "none"));
        break;
    }
    return o;
}

/* ── Async-integrated poll ─────────────────────────────────────
 *
 * Returns a Promise that resolves with an event object on key /
 * resize / mouse / etc., or with null on timeout. While the Promise
 * is pending, the JS event loop keeps running - other Promises
 * (background fetches, timers, tui.async tasks) progress.
 *
 * Mirrors the Lua side's logic in src/hull/runtime/lua/mod_tui.c.
 */

typedef struct TuiPollOpJs {
    HlAsyncCtx *async_ctx;
    HlJS       *js;
    HlTuiEvent  ev;
    int         has_event;
    int         remaining_ms;
    uint64_t    last_byte_ms;
} TuiPollOpJs;

/* push_result for JS conts: returns the JSValue to resolve with. */
static JSValue js_tui_poll_push_result(JSContext *ctx, void *driver)
{
    TuiPollOpJs *op = (TuiPollOpJs *)driver;
    if (!op || !op->has_event) return JS_NULL;
    return event_to_js(ctx, &op->ev);
}

static void js_tui_poll_free(void *driver) { free(driver); }

static void js_tui_poll_schedule(TuiPollOpJs *op);

static void js_tui_poll_tick(void *user)
{
    TuiPollOpJs *op = (TuiPollOpJs *)user;
    const HlAsyncBackend *be = hl_async_backend();
    uint64_t now = be->monotonic_ms();

    int drained = 0;
    for (;;) {
        int n = hl_cap_tui_drain(g_ctx);
        if (n <= 0) break;
        drained += n;
    }
    if (drained > 0) op->last_byte_ms = now;

    HlTuiEvent ev = {0};
    if (hl_cap_tui_next_event(g_ctx, &ev) == 0) {
        op->ev = ev;
        op->has_event = 1;
        hl_async_ctx_resume_detached(op->async_ctx);
        return;
    }

    if (hl_cap_tui_parser_pending(g_ctx) &&
        (now - op->last_byte_ms) >= HL_TUI_POLL_ESC_IDLE_MS) {
        if (hl_cap_tui_commit_idle(g_ctx) &&
            hl_cap_tui_next_event(g_ctx, &ev) == 0) {
            op->ev = ev;
            op->has_event = 1;
            hl_async_ctx_resume_detached(op->async_ctx);
            return;
        }
    }

    op->remaining_ms -= HL_TUI_POLL_SLICE_MS;
    if (op->remaining_ms <= 0) {
        op->has_event = 0;
        hl_async_ctx_resume_detached(op->async_ctx);
        return;
    }
    js_tui_poll_schedule(op);
}

static void js_tui_poll_schedule(TuiPollOpJs *op)
{
    const HlAsyncBackend *be = hl_async_backend();
    uint64_t slice = (op->remaining_ms < HL_TUI_POLL_SLICE_MS)
                     ? (uint64_t)op->remaining_ms
                     : HL_TUI_POLL_SLICE_MS;
    if (slice == 0) slice = 1;
    be->timer_add(op->js->base.async_ctx, slice, js_tui_poll_tick, op);
}

static JSValue js_tui_poll(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    int32_t timeout_ms = 0;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]))
        JS_ToInt32(ctx, &timeout_ms, argv[0]);

    /* Fast path: try non-blocking. */
    HlTuiEvent ev = {0};
    int rc = hl_cap_tui_poll(g_ctx, 0, &ev);
    if (rc < 0) return JS_ThrowInternalError(ctx, "tui.poll failed");
    if (rc == 0) return event_to_js(ctx, &ev);
    if (timeout_ms == 0) return JS_NULL;

    /* Slow path: schedule timer and return a Promise. */
    HlJS *js = JS_GetContextOpaque(ctx);
    if (!js || !js->base.async_ctx) {
        /* No event loop - fall back to blocking. */
        rc = hl_cap_tui_poll(g_ctx, timeout_ms, &ev);
        if (rc < 0) return JS_ThrowInternalError(ctx, "tui.poll failed");
        if (rc == 1) return JS_NULL;
        return event_to_js(ctx, &ev);
    }

    if (timeout_ms < 0) timeout_ms = INT_MAX;

    TuiPollOpJs *op = calloc(1, sizeof *op);
    if (!op) return JS_ThrowOutOfMemory(ctx);
    op->js           = js;
    op->remaining_ms = timeout_ms;
    op->last_byte_ms = hl_async_backend()->monotonic_ms();

    HlAsyncCtx *async = hl_async_ctx_create(js->server,
                                             js->base.net_ctx,
                                             js->base.alloc);
    if (!async) { free(op); return JS_ThrowOutOfMemory(ctx); }

    JSValue resolving_funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, resolving_funcs);
    if (JS_IsException(promise)) {
        hl_async_ctx_free(async); free(op);
        return JS_EXCEPTION;
    }

    extern HlAsyncCont *hl_js_async_cont_create(HlJS *js,
        JSValue resolve, JSValue reject, HlAllocator *alloc,
        JSValue (*push_result)(JSContext *, void *));
    HlAsyncCont *cont = hl_js_async_cont_create(js,
                                                  resolving_funcs[0],
                                                  resolving_funcs[1],
                                                  js->base.alloc,
                                                  js_tui_poll_push_result);
    if (!cont) {
        JS_FreeValue(ctx, resolving_funcs[0]);
        JS_FreeValue(ctx, resolving_funcs[1]);
        JS_FreeValue(ctx, promise);
        hl_async_ctx_free(async); free(op);
        return JS_ThrowOutOfMemory(ctx);
    }
    async->cont        = cont;
    async->driver      = op;
    async->free_driver = js_tui_poll_free;
    async->detached    = 1;
    op->async_ctx      = async;

    js_tui_poll_schedule(op);
    return promise;
}

/* tui.pollSync(timeoutMs) - fully synchronous, blocking variant. Used
 * by tui.run inside its synchronous render loop where no `await` is
 * available (and where the async poll's Promise would leak because
 * sync JS code never yields the thread for the microtask drain to
 * resolve it). Returns the event object directly or null on timeout.
 * tui.poll above remains the public async-aware entry point for
 * callers that own an async context. */
static JSValue js_tui_poll_sync(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    int32_t timeout_ms = -1;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]))
        JS_ToInt32(ctx, &timeout_ms, argv[0]);
    HlTuiEvent ev = {0};
    int rc = hl_cap_tui_poll(g_ctx, timeout_ms, &ev);
    if (rc < 0) return JS_ThrowInternalError(ctx, "tui.pollSync failed");
    if (rc == 1) return JS_NULL;
    return event_to_js(ctx, &ev);
}

/* ── Clipboard + modes ─────────────────────────────────────────── */

static JSValue js_tui_clipboard_set(JSContext *ctx, JSValueConst this_val,
                                     int argc, JSValueConst *argv)
{
    (void)this_val;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    if (argc < 1) return JS_UNDEFINED;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!s) return JS_EXCEPTION;
    int rc = hl_cap_tui_clipboard_set(g_ctx, s, len);
    JS_FreeCString(ctx, s);
    if (rc < 0) {
        if (errno == ENOTSUP)
            return JS_ThrowInternalError(ctx,
                "tui: clipboard not supported by this terminal");
        return JS_ThrowInternalError(ctx, "tui: clipboardSet failed");
    }
    return JS_UNDEFINED;
}

#define MODE_TOGGLE(NAME, CFUNC)                                       \
    static JSValue js_tui_enable_ ## NAME(JSContext *ctx,              \
                                          JSValueConst this_val,        \
                                          int argc, JSValueConst *argv) \
    {                                                                   \
        (void)this_val;                                                 \
        if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;             \
        int on = (argc >= 1) ? JS_ToBool(ctx, argv[0]) : 0;             \
        CFUNC(g_ctx, on);                                               \
        return JS_UNDEFINED;                                            \
    }

MODE_TOGGLE(mouse,     hl_cap_tui_enable_mouse)
MODE_TOGGLE(paste,     hl_cap_tui_enable_paste)
MODE_TOGGLE(focus,     hl_cap_tui_enable_focus)
MODE_TOGGLE(kitty_kbd, hl_cap_tui_enable_kitty_kbd)

/* ── Async helper ──────────────────────────────────────────────── */

/* tui.async(fn) - fire-and-forget background work, used by tui.run's
 * event handlers. JS already has Promises for this; we just need a
 * top-level wrapper that swallows the result and routes errors to
 * stderr (so a buggy background task doesn't crash the run loop). */
static JSValue js_tui_async(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "tui.async(fn): expected a function");
    JSValue ret = JS_Call(ctx, argv[0], JS_UNDEFINED, 0, NULL);
    /* If the function returned a Promise, we just let the engine run
     * it - rejections will propagate to QuickJS's unhandled-rejection
     * handler, which logs to stderr. We deliberately don't await. */
    JS_FreeValue(ctx, ret);
    return JS_UNDEFINED;
}

/* ── Module init ───────────────────────────────────────────────── */

static int js_tui_module_init(JSContext *ctx, JSModuleDef *m)
{
    /* hull:_tui is the private native bridge. The public hull:tui
     * module is the stdlib JS file that re-exports + augments these
     * primitives. Gate uses the canonical name `hull/tui` so the
     * resolver still enforces the declaration on the bridge - bypass
     * would require requiring `hull:_tui` directly, which is private.
     * Cheaper than threading a second registry slot. */
    if (hl_js_check_module_declared(ctx, "hull/tui", "hull:_tui") != 0)
        return -1;

    JSValue o = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, o, "enter",         JS_NewCFunction(ctx, js_tui_enter,         "enter",         0));
    JS_SetPropertyStr(ctx, o, "leave",         JS_NewCFunction(ctx, js_tui_leave,         "leave",         0));
    JS_SetPropertyStr(ctx, o, "size",          JS_NewCFunction(ctx, js_tui_size,          "size",          0));
    JS_SetPropertyStr(ctx, o, "caps",          JS_NewCFunction(ctx, js_tui_caps,          "caps",          0));
    JS_SetPropertyStr(ctx, o, "theme",         JS_NewCFunction(ctx, js_tui_theme,         "theme",         0));
    JS_SetPropertyStr(ctx, o, "clear",         JS_NewCFunction(ctx, js_tui_clear,         "clear",         0));
    JS_SetPropertyStr(ctx, o, "invalidate",    JS_NewCFunction(ctx, js_tui_invalidate,    "invalidate",    0));
    JS_SetPropertyStr(ctx, o, "move",          JS_NewCFunction(ctx, js_tui_move,          "move",          2));
    JS_SetPropertyStr(ctx, o, "style",         JS_NewCFunction(ctx, js_tui_style,         "style",         1));
    JS_SetPropertyStr(ctx, o, "print",         JS_NewCFunction(ctx, js_tui_print,         "print",         3));
    JS_SetPropertyStr(ctx, o, "write",         JS_NewCFunction(ctx, js_tui_write,         "write",         1));
    JS_SetPropertyStr(ctx, o, "flush",         JS_NewCFunction(ctx, js_tui_flush,         "flush",         0));
    JS_SetPropertyStr(ctx, o, "poll",          JS_NewCFunction(ctx, js_tui_poll,          "poll",          1));
    JS_SetPropertyStr(ctx, o, "pollSync",      JS_NewCFunction(ctx, js_tui_poll_sync,     "pollSync",      1));
    JS_SetPropertyStr(ctx, o, "clipboardSet",  JS_NewCFunction(ctx, js_tui_clipboard_set, "clipboardSet",  1));
    JS_SetPropertyStr(ctx, o, "enableMouse",   JS_NewCFunction(ctx, js_tui_enable_mouse,    "enableMouse",   1));
    JS_SetPropertyStr(ctx, o, "enablePaste",   JS_NewCFunction(ctx, js_tui_enable_paste,    "enablePaste",   1));
    JS_SetPropertyStr(ctx, o, "enableFocus",   JS_NewCFunction(ctx, js_tui_enable_focus,    "enableFocus",   1));
    JS_SetPropertyStr(ctx, o, "enableKittyKbd",JS_NewCFunction(ctx, js_tui_enable_kitty_kbd,"enableKittyKbd",1));
    JS_SetPropertyStr(ctx, o, "async",         JS_NewCFunction(ctx, js_tui_async,         "async",         1));

    JS_SetPropertyStr(ctx, o, "MOD_SHIFT", JS_NewUint32(ctx, HL_TUI_MOD_SHIFT));
    JS_SetPropertyStr(ctx, o, "MOD_ALT",   JS_NewUint32(ctx, HL_TUI_MOD_ALT));
    JS_SetPropertyStr(ctx, o, "MOD_CTRL",  JS_NewUint32(ctx, HL_TUI_MOD_CTRL));
    JS_SetPropertyStr(ctx, o, "MOD_SUPER", JS_NewUint32(ctx, HL_TUI_MOD_SUPER));

    JS_SetModuleExport(ctx, m, "tui", o);
    return 0;
}

int hl_js_init_tui_module(JSContext *ctx, HlJS *js)
{
    (void)js;
    /* Register as `hull:_tui` (private). The public `hull:tui` module
     * is the stdlib JS file at stdlib/js/hull/tui.js, which imports
     * the bridge here and layers tui.run / tui.list / etc. */
    JSModuleDef *m = JS_NewCModule(ctx, "hull:_tui", js_tui_module_init);
    if (!m) return -1;
    JS_AddModuleExport(ctx, m, "tui");
    return 0;
}

/* Strong override of the base weak hl_tui_feature_register_js
 * (cap/tui_feature.c): register the hull:_tui native module. Called
 * unconditionally from js/modules.c; in a base build with no tui feature the
 * weak no-op runs. void* params keep JS types out of tui.h. */
int hl_tui_feature_register_js(void *js_ctx, void *hl_js)
{
    return hl_js_init_tui_module((JSContext *)js_ctx, (HlJS *)hl_js);
}

#endif /* HL_ENABLE_TUI */
