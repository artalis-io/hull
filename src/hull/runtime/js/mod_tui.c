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
#include "db_wait.h"   /* hl_js_db_refuse_wait */
#include "internal.h"  /* hl_js_async_gate */
#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/cap/tui.h"
#include "hull/shared/work_charge.h"   /* hl_work_bytes_units */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#define HL_TUI_POLL_SLICE_MS     25
#define HL_TUI_POLL_ESC_IDLE_MS  50

static HlTuiCtx *g_ctx = NULL;

static int ensure_acquired(JSContext *ctx)
{
    /* TUI is CLI mode only (docs/tui_mode.md): an HTTP request's handler or
     * middleware may not drive the terminal. tui.poll there made a
     * continuation counted as the op holding the request's connection that
     * never suspended it: the client got an empty 200 at once, and the
     * handler's res.json later wrote into the recycled slot (audit 8 M9). */
    const HlJS *js = (const HlJS *)JS_GetContextOpaque(ctx);
    if (js && js->active_conn) {
        JS_ThrowTypeError(ctx,
            "tui: the terminal UI is for app.main (CLI mode); it cannot be "
            "used while serving an HTTP request");
        return -1;
    }
    if (g_ctx) return 0;
    int rc = hl_cap_tui_acquire(&g_ctx);
    if (rc != 0) {
        if (errno == EBUSY) {
            JS_ThrowInternalError(ctx,
                "tui: another TUI session is already active in this process");
            return -1;
        }
        if (errno == EPERM) {   /* the manifest has not admitted TUI (audit 31) */
            JS_ThrowInternalError(ctx,
                "tui: not admitted - the app must declare `tui: true` and "
                "\"hull/tui@1\" in app.manifest, and use the terminal from "
                "app.main, not at top level");
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

/* Define @p v on @p o, once nothing has failed yet (*bad == 0): a value
 * that could not be made (out of memory) was stored as JS_EXCEPTION in
 * the event / size / caps objects (audit 27). The define consumes @p v. */
static void tui_put(JSContext *ctx, JSValue o, const char *k, JSValue v,
                    int *bad)
{
    if (*bad) { JS_FreeValue(ctx, v); return; }
    if (JS_IsException(v) ||
        JS_DefinePropertyValueStr(ctx, o, k, v, JS_PROP_C_W_E) < 0)
        *bad = 1;
}

/* @p o, or JS_EXCEPTION (freeing it) when a tui_put failed. */
static JSValue tui_obj_done(JSContext *ctx, JSValue o, int bad)
{
    if (!bad) return o;
    JS_FreeValue(ctx, o);
    return JS_EXCEPTION;
}

static JSValue js_tui_size(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    int cols = 0, rows = 0;
    hl_cap_tui_size(g_ctx, &cols, &rows);
    JSValue obj = JS_NewObject(ctx);
    if (JS_IsException(obj)) return JS_EXCEPTION;
    int bad = 0;
    tui_put(ctx, obj, "cols", JS_NewInt32(ctx, cols), &bad);
    tui_put(ctx, obj, "rows", JS_NewInt32(ctx, rows), &bad);
    return tui_obj_done(ctx, obj, bad);
}

static JSValue js_tui_caps(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    uint32_t caps = hl_cap_tui_caps(g_ctx);
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o)) return JS_EXCEPTION;
    int bad = 0;
    tui_put(ctx, o, "truecolor", JS_NewBool(ctx, !!(caps & HL_TUI_CAP_TRUECOLOR)), &bad);
    tui_put(ctx, o, "color256",  JS_NewBool(ctx, !!(caps & HL_TUI_CAP_256COLOR)), &bad);
    tui_put(ctx, o, "color",     JS_NewBool(ctx, !!(caps & HL_TUI_CAP_BASIC_COLOR)), &bad);
    tui_put(ctx, o, "mouse",     JS_NewBool(ctx, !!(caps & HL_TUI_CAP_MOUSE)), &bad);
    tui_put(ctx, o, "focus",     JS_NewBool(ctx, !!(caps & HL_TUI_CAP_FOCUS)), &bad);
    tui_put(ctx, o, "kittyKbd",  JS_NewBool(ctx, !!(caps & HL_TUI_CAP_KITTY_KBD)), &bad);
    tui_put(ctx, o, "clipboard", JS_NewBool(ctx, !!(caps & HL_TUI_CAP_OSC52)), &bad);
    return tui_obj_done(ctx, o, bad);
}

static JSValue js_tui_theme(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    return JS_NewString(ctx, hl_cap_tui_theme(g_ctx));
}

/* ── Rendering ─────────────────────────────────────────────────── */

/* clear / invalidate / flush walk every cell of the grid whatever was drawn:
 * 1 unit per 4 cells before the work (audit 39; the Lua twin is
 * tui_charge_cells). 0, or -1 with the uncatchable interrupt pending. */
static int js_tui_charge_cells(JSContext *ctx)
{
    int cols = 0, rows = 0;
    hl_cap_tui_size(g_ctx, &cols, &rows);
    return hl_js_budget_charge(ctx,
        hl_work_tui_cells_units(cols > 0 ? (uint64_t)cols : 0,
                                rows > 0 ? (uint64_t)rows : 0)) ? -1 : 0;
}

static JSValue js_tui_clear(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    if (js_tui_charge_cells(ctx) != 0) return JS_EXCEPTION;
    hl_cap_tui_clear(g_ctx);
    return JS_UNDEFINED;
}

static JSValue js_tui_invalidate(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    if (js_tui_charge_cells(ctx) != 0) return JS_EXCEPTION;
    hl_cap_tui_invalidate(g_ctx);
    return JS_UNDEFINED;
}

static JSValue js_tui_move(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    int x = 1, y = 1;
    /* A valueOf that throws fails the call (audit 23). */
    if (argc >= 1 && JS_ToInt32(ctx, &x, argv[0]) < 0) return JS_EXCEPTION;
    if (argc >= 2 && JS_ToInt32(ctx, &y, argv[1]) < 0) return JS_EXCEPTION;
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
        /* A getter that throws fails the call (audit 23: it was left
         * pending and the style applied without it). */
        static const struct { const char *key; uint32_t bit; } flags[] = {
            { "bold", HL_TUI_ATTR_BOLD }, { "dim", HL_TUI_ATTR_DIM },
            { "italic", HL_TUI_ATTR_ITALIC },
            { "underline", HL_TUI_ATTR_UNDERLINE },
            { "reverse", HL_TUI_ATTR_REVERSE },
            { "strike", HL_TUI_ATTR_STRIKE },
        };
        JSValue v;
        v = JS_GetPropertyStr(ctx, argv[0], "fg");
        if (JS_IsException(v)) return JS_EXCEPTION;
        fg = color_from_js(ctx, v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, argv[0], "bg");
        if (JS_IsException(v)) return JS_EXCEPTION;
        bg = color_from_js(ctx, v); JS_FreeValue(ctx, v);
        for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
            int on = hl_js_opt_bool(ctx, argv[0], flags[i].key);
            if (on < 0) return JS_EXCEPTION;
            if (on) attr |= flags[i].bit;
        }
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
    if (JS_ToInt32(ctx, &x, argv[0]) < 0 ||   /* audit 23 */
        JS_ToInt32(ctx, &y, argv[1]) < 0)
        return JS_EXCEPTION;
    size_t len = 0;
    const char *s = JS_ToCStringLen(ctx, &len, argv[2]);
    if (!s) return JS_EXCEPTION;
    /* 1 unit per 8 bytes, as Lua's tui.print (audit 27). */
    if (hl_js_budget_charge(ctx, hl_work_bytes_units(len)) < 0) {
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
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
    if (hl_js_budget_charge(ctx, hl_work_bytes_units(len)) < 0) {   /* audit 27 */
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
    hl_cap_tui_write(g_ctx, s, len);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

static JSValue js_tui_flush(JSContext *ctx, JSValueConst this_val,
                             int argc, JSValueConst *argv)
{
    (void)this_val; (void)argc; (void)argv;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    if (js_tui_charge_cells(ctx) != 0) return JS_EXCEPTION;
    hl_cap_tui_flush(g_ctx);
    /* The frame it wrote, 1 unit per 8 bytes (known once it is built). */
    if (hl_js_budget_charge(ctx,
            hl_work_bytes_units(hl_cap_tui_last_flush_bytes(g_ctx))))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

/* ── Input ─────────────────────────────────────────────────────── */

static JSValue event_to_js(JSContext *ctx, const HlTuiEvent *ev)
{
    JSValue o = JS_NewObject(ctx);
    if (JS_IsException(o)) return JS_EXCEPTION;
    int bad = 0;
    switch (ev->kind) {
    case HL_TUI_EV_KEY:
        tui_put(ctx, o, "kind", JS_NewString(ctx, "key"), &bad);
        tui_put(ctx, o, "key",
            JS_NewString(ctx, ev->key ? ev->key : ""), &bad);
        if (ev->codepoint)
            tui_put(ctx, o, "codepoint",
                JS_NewUint32(ctx, ev->codepoint), &bad);
        tui_put(ctx, o, "mods",  JS_NewUint32(ctx, ev->mods), &bad);
        tui_put(ctx, o, "shift", JS_NewBool(ctx, !!(ev->mods & HL_TUI_MOD_SHIFT)), &bad);
        tui_put(ctx, o, "alt",   JS_NewBool(ctx, !!(ev->mods & HL_TUI_MOD_ALT)), &bad);
        tui_put(ctx, o, "ctrl",  JS_NewBool(ctx, !!(ev->mods & HL_TUI_MOD_CTRL)), &bad);
        tui_put(ctx, o, "super", JS_NewBool(ctx, !!(ev->mods & HL_TUI_MOD_SUPER)), &bad);
        break;
    case HL_TUI_EV_RESIZE:
        tui_put(ctx, o, "kind", JS_NewString(ctx, "resize"), &bad);
        tui_put(ctx, o, "cols", JS_NewInt32(ctx, ev->cols), &bad);
        tui_put(ctx, o, "rows", JS_NewInt32(ctx, ev->rows), &bad);
        break;
    case HL_TUI_EV_MOUSE:
        tui_put(ctx, o, "kind", JS_NewString(ctx, "mouse"), &bad);
        tui_put(ctx, o, "x", JS_NewInt32(ctx, ev->x), &bad);
        tui_put(ctx, o, "y", JS_NewInt32(ctx, ev->y), &bad);
        tui_put(ctx, o, "button", JS_NewInt32(ctx, ev->button), &bad);
        break;
    case HL_TUI_EV_PASTE:
        tui_put(ctx, o, "kind", JS_NewString(ctx, "paste"), &bad);
        tui_put(ctx, o, "text",
            JS_NewStringLen(ctx, ev->text ? ev->text : "", ev->text_len), &bad);
        break;
    case HL_TUI_EV_FOCUS:
        tui_put(ctx, o, "kind", JS_NewString(ctx, "focus"), &bad);
        tui_put(ctx, o, "in",   JS_NewBool(ctx, ev->focus_in), &bad);
        break;
    case HL_TUI_EV_HANGUP:
        /* the terminal went away (stdin EOF); sticky - audit 10 */
        tui_put(ctx, o, "kind", JS_NewString(ctx, "hangup"), &bad);
        break;
    default:
        tui_put(ctx, o, "kind", JS_NewString(ctx, "none"), &bad);
        break;
    }
    return tui_obj_done(ctx, o, bad);
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
    uint64_t    timer_id;       /* the scheduled slice; 0 = none */
} TuiPollOpJs;

/* push_result for JS conts: returns the JSValue to resolve with. */
static JSValue js_tui_poll_push_result(JSContext *ctx, void *driver)
{
    TuiPollOpJs *op = (TuiPollOpJs *)driver;
    if (!op || !op->has_event) return JS_NULL;
    return event_to_js(ctx, &op->ev);
}

static void js_tui_poll_free(void *driver) { free(driver); }

static int js_tui_poll_schedule(TuiPollOpJs *op);

static void js_tui_poll_tick(void *user)
{
    TuiPollOpJs *op = (TuiPollOpJs *)user;
    op->timer_id = 0;                   /* this one has fired */
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
    /* Match Lua: a failed reschedule completes as a timeout. */
    if (op->remaining_ms <= 0 || js_tui_poll_schedule(op) != 0) {
        op->has_event = 0;
        hl_async_ctx_resume_detached(op->async_ctx);
        return;
    }
}

static int js_tui_poll_schedule(TuiPollOpJs *op)
{
    const HlAsyncBackend *be = hl_async_backend();
    uint64_t slice = (op->remaining_ms < HL_TUI_POLL_SLICE_MS)
                     ? (uint64_t)op->remaining_ms
                     : HL_TUI_POLL_SLICE_MS;
    if (slice == 0) slice = 1;
    op->timer_id = be->timer_add(op->js->base.async_ctx, slice,
                                 js_tui_poll_tick, op);
    return op->timer_id ? 0 : -1;
}

/* A poll still pending at teardown (HlAsyncCtx.on_shutdown, audit 14). */
static void js_tui_poll_shutdown(HlAsyncCtx *ctx, HlAsyncBackendCtx *loop)
{
    TuiPollOpJs *op = (TuiPollOpJs *)ctx->driver;
    if (op && op->timer_id && loop)
        hl_async_backend()->timer_cancel(loop, op->timer_id);
    if (op) op->timer_id = 0;
}

static JSValue js_tui_poll(JSContext *ctx, JSValueConst this_val,
                            int argc, JSValueConst *argv)
{
    (void)this_val;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    int32_t timeout_ms = 0;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]))
        if (JS_ToInt32(ctx, &timeout_ms, argv[0])) return JS_EXCEPTION;

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

    if (hl_js_async_gate(ctx, js, "tui.poll()") != 0) return JS_EXCEPTION;
    if (hl_js_db_refuse_wait(ctx, "tui.poll()")) return JS_EXCEPTION;
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

    if (js_tui_poll_schedule(op) != 0) {
        /* No timer can settle the promise. The continuation owns the
         * resolving functions; destroy it before freeing the context. */
        async->cont = NULL;
        async->driver = NULL;
        cont->destroy(cont);
        hl_async_ctx_free(async);
        free(op);
        JS_FreeValue(ctx, promise);
        return JS_ThrowInternalError(ctx, "tui.poll: could not arm a timer");
    }
    async->on_shutdown = js_tui_poll_shutdown;
    hl_async_ctx_track(async, &js->base.async_live);
    return promise;
}

/* tui.pollSync(timeoutMs) - fully synchronous, blocking variant. Used
 * by tui.run inside its synchronous render loop where no `await` is
 * available (and where the async poll's Promise would leak because
 * sync JS code never yields the thread for the microtask drain to
 * resolve it). Returns the event object directly or null on timeout.
 * tui.poll above remains the public async-aware entry point for
 * callers that own an async context.
 *
 * Each event starts a new run, as each tui.poll resume does in Lua: the
 * instruction budget and the run deadline are re-armed when the wait
 * returns. tui.run never yields, so its whole session was ONE app.main run -
 * killed by the 10-minute deadline (or the 100M-instruction budget) however
 * the user paced it (audit 21). A run already over its budget is not
 * re-armed: its trip is raised instead. A poll that returns no event re-arms
 * only once the run has really waited HL_JS_TUI_REARM_MIN_MS in all: the
 * measured idle time of every no-event poll is added up on HlJS (tui_idle_ms,
 * reset by every arm; a pollSync(0) adds nothing). A pollSync(0) - audit 25 -
 * or a pollSync(1) loop reset its own budget and deadline for ever (audit 26);
 * a pollSync(1) loop now gets a new run at most once per 50 ms of real
 * waiting, a pollSync(0) loop never. Judged per call, as audit 26 did, a
 * tickMs clock shorter than 50 ms (tui.run({tickMs: 16})) never re-armed, so
 * its idle ticks piled into one run that died on the budget (audit 28) - Lua
 * re-arms every tui.poll resume. A hangup returns at once on every later
 * poll, so it never re-arms. */
#define HL_JS_TUI_REARM_MIN_MS 50
static JSValue js_tui_poll_sync(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv)
{
    (void)this_val;
    if (ensure_acquired(ctx) != 0) return JS_EXCEPTION;
    int32_t timeout_ms = -1;
    if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]))
        if (JS_ToInt32(ctx, &timeout_ms, argv[0])) return JS_EXCEPTION;
    HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
    if (js && js->budget_tripped) return hl_js_budget_throw(ctx);
    HlTuiEvent ev = {0};
    const HlAsyncBackend *be = hl_async_backend();
    uint64_t t0 = be->monotonic_ms();
    int rc = hl_cap_tui_poll(g_ctx, timeout_ms, &ev);
    uint64_t waited = be->monotonic_ms() - t0;
    /* Only a user event, or real waiting adding up to the minimum since the
     * last arm, starts a new run. A hangup adds nothing, and neither does a
     * pollSync(0): it asked not to wait, and the few microseconds its poll
     * takes would otherwise add up, a millisecond tick at a time, to a new
     * run for a busy loop. */
    int rearm = 0;
    if (rc == 0 && ev.kind != HL_TUI_EV_HANGUP) {
        rearm = 1;
    } else if (rc == 1 && js && timeout_ms != 0) {
        js->tui_idle_ms += waited;
        rearm = js->tui_idle_ms >= HL_JS_TUI_REARM_MIN_MS;
    }
    /* Only app.main's own run starts a new one here (audit 32). Any other
     * entry that is not an HTTP request - a timer, a ws-server / ws-client
     * callback, a hull:_task task - could also sit in a synchronous tui.run
     * loop, and re-arming there renewed its deadline and budget for as long
     * as the terminal kept producing events or idle ticks, holding the event
     * loop past the run limit. Such an entry keeps the one run it was given
     * (Lua's tui.poll yields to the loop, so its resume is an ordinary new
     * run). */
    if (js && rearm && js->active_cli_main)
        hl_js_budget_arm_kind(js, HL_RUN_MAIN);
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
    if (hl_js_budget_charge(ctx, hl_work_bytes_units(len)) < 0) {   /* audit 27 */
        JS_FreeCString(ctx, s);
        return JS_EXCEPTION;
    }
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
    if (JS_IsException(ret)) {
        /* Audit 29: the exception was left pending with undefined
         * returned. A tripped budget stays the uncatchable trip; any other
         * error is logged and taken, as the comment above promises. */
        HlJS *js = (HlJS *)JS_GetContextOpaque(ctx);
        if (js && js->budget_tripped) {
            JS_FreeValue(ctx, JS_GetException(ctx));
            return hl_js_budget_throw(ctx);
        }
        if (js) hl_js_dump_error(js);
        else JS_FreeValue(ctx, JS_GetException(ctx));
        return JS_UNDEFINED;
    }
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

    hl_js_export_frozen(ctx, m, "tui", o);
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
