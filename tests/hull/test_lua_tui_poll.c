/* The Lua tui.poll binding against fault-injected terminal IO and run
 * checks. Terminal IO, timers and the run's budget are isolated from the
 * host (the JS twin is test_js_tui_poll.c).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#ifndef HL_ENABLE_TUI
#define HL_ENABLE_TUI 1
#endif
#include "../../src/hull/runtime/lua/mod_tui.c"

#include "lualib.h"

/* ── Seams ─────────────────────────────────────────────────────────── */

/* The run: hl_lua_budget_charge says "tripped" once `run_stop_after`
 * checks have been asked; 0 = never. */
static int run_charges, run_stop_after, run_raised;
int hl_lua_budget_charge(lua_State *L, int64_t units)
{
    (void)L; (void)units;
    run_charges++;
    return run_stop_after && run_charges >= run_stop_after;
}
int hl_lua_budget_raise(lua_State *L)
{
    run_raised++;
    return luaL_error(L, "run exceeded its time limit");
}
int hl_lua_check_can_wait(lua_State *L, const char *what)
{ (void)L; (void)what; return 0; }

/* Every poll times out (no input), recording the longest wait asked for;
 * -1 is "wait for ever". */
static int poll_calls, poll_max_timeout, poll_unbounded;
int hl_cap_tui_poll(HlTuiCtx *ctx, int timeout, HlTuiEvent *out)
{
    (void)ctx; (void)out;
    poll_calls++;
    if (timeout < 0) { poll_unbounded++; return 1; }
    if (timeout > poll_max_timeout) poll_max_timeout = timeout;
    return 1;
}
int hl_cap_tui_acquire(HlTuiCtx **out) { (void)out; return -1; }
int hl_cap_tui_drain(HlTuiCtx *ctx) { (void)ctx; return 0; }
int hl_cap_tui_next_event(HlTuiCtx *ctx, HlTuiEvent *out)
{ (void)ctx; (void)out; return 1; }
int hl_cap_tui_parser_pending(HlTuiCtx *ctx) { (void)ctx; return 0; }
int hl_cap_tui_commit_idle(HlTuiCtx *ctx) { (void)ctx; return 0; }

/* Everything else the binding references (its function table is kept
 * whole - under ASan, or where the linker does not drop it - and the
 * native Linux base links no TUI cap): inert stubs. */
int hl_cap_tui_release(HlTuiCtx *ctx) { (void)ctx; return 0; }
int hl_cap_tui_size(HlTuiCtx *ctx, int *cols, int *rows)
{ (void)ctx; if (cols) *cols = 80; if (rows) *rows = 24; return 0; }
uint32_t hl_cap_tui_caps(HlTuiCtx *ctx) { (void)ctx; return 0; }
const char *hl_cap_tui_theme(HlTuiCtx *ctx) { (void)ctx; return "unknown"; }
int hl_cap_tui_clear(HlTuiCtx *ctx) { (void)ctx; return 0; }
int hl_cap_tui_invalidate(HlTuiCtx *ctx) { (void)ctx; return 0; }
int hl_cap_tui_move(HlTuiCtx *ctx, int x, int y)
{ (void)ctx; (void)x; (void)y; return 0; }
int hl_cap_tui_style(HlTuiCtx *ctx, uint32_t fg, uint32_t bg, uint32_t attr)
{ (void)ctx; (void)fg; (void)bg; (void)attr; return 0; }
int hl_cap_tui_write(HlTuiCtx *ctx, const char *s, size_t len)
{ (void)ctx; (void)s; (void)len; return 0; }
int hl_cap_tui_flush(HlTuiCtx *ctx) { (void)ctx; return 0; }
size_t hl_cap_tui_last_flush_bytes(const HlTuiCtx *ctx) { (void)ctx; return 0; }
int hl_cap_tui_clipboard_set(HlTuiCtx *ctx, const char *text, size_t len)
{ (void)ctx; (void)text; (void)len; return 0; }
int hl_cap_tui_enable_mouse(HlTuiCtx *ctx, int on) { (void)ctx; (void)on; return 0; }
int hl_cap_tui_enable_paste(HlTuiCtx *ctx, int on) { (void)ctx; (void)on; return 0; }
int hl_cap_tui_enable_focus(HlTuiCtx *ctx, int on) { (void)ctx; (void)on; return 0; }
int hl_cap_tui_enable_kitty_kbd(HlTuiCtx *ctx, int on) { (void)ctx; (void)on; return 0; }
int lua_hull_spawn(lua_State *L) { (void)L; return 0; }
void hl_lua_setfuncs(lua_State *L, const luaL_Reg *l, int nup)
{ luaL_setfuncs(L, l, nup); }

static uint64_t test_now(void) { return 100; }
static uint64_t test_timer(HlAsyncBackendCtx *ctx, uint64_t ms,
                           HlAsyncTimerFn fn, void *user)
{ (void)ctx; (void)ms; (void)fn; (void)user; return 0; }
static void test_timer_cancel(HlAsyncBackendCtx *ctx, uint64_t handle)
{ (void)ctx; (void)handle; }
static const HlAsyncBackend test_backend = {
    .monotonic_ms = test_now, .timer_add = test_timer,
    .timer_cancel = test_timer_cancel,
};
const HlAsyncBackend *hl_async_backend(void) { return &test_backend; }
HlAsyncCtx *hl_async_ctx_create(KlHttpServer *server, HlNetBackendCtx *net,
                               HlAllocator *alloc)
{ (void)server; (void)net; (void)alloc; return NULL; }
void hl_async_ctx_free(HlAsyncCtx *ctx) { (void)ctx; }
void hl_async_ctx_track(HlAsyncCtx *ctx, HlAsyncTeardown **list)
{ (void)ctx; (void)list; }
void hl_async_ctx_resume_detached(HlAsyncCtx *ctx) { (void)ctx; }
HlAsyncCont *hl_lua_async_cont_create(HlLua *lua, HlAllocator *alloc,
                                      HlLuaPushResultFn fn)
{ (void)lua; (void)alloc; (void)fn; return NULL; }

/* ── Harness ───────────────────────────────────────────────────────── */

static HlLua g_lua;

static lua_State *setup(void)
{
    memset(&g_lua, 0, sizeof g_lua);   /* base.async_ctx NULL: no event loop */
    lua_State *L = luaL_newstate();
    lua_pushlightuserdata(L, &g_lua);
    lua_setfield(L, LUA_REGISTRYINDEX, "__hull_lua");
    g_ctx = (HlTuiCtx *)(uintptr_t)1;
    run_charges = 0; run_stop_after = 0; run_raised = 0;
    poll_calls = 0; poll_max_timeout = 0; poll_unbounded = 0;
    return L;
}

static void teardown(lua_State *L)
{
    g_ctx = NULL;
    lua_close(L);
}

/* tui.poll(timeout) under lua_pcall: LUA_OK and the result left on the
 * stack, or the error. */
static int call_poll(lua_State *L, int timeout_ms)
{
    lua_pushcfunction(L, lua_tui_poll);
    lua_pushinteger(L, timeout_ms);
    return lua_pcall(L, 1, 1, 0);
}

/* Audit 44: with no event loop wired (hull test / hull agent build their app
 * with no async ctx) tui.poll fell back to ONE blocking cap poll with the
 * caller's timeout - a negative one waited for ever - and never asked the
 * run, so the run deadline could not end it. It now waits in slices, asking
 * the run after each, and raises the run's limit once it says stop. */
UTEST(tui_audit44, blocking_poll_honours_deadline)
{
    lua_State *L = setup();
    run_stop_after = 3;
    EXPECT_NE(call_poll(L, -1), LUA_OK);
    EXPECT_EQ(poll_unbounded, 0);
    EXPECT_GT(poll_calls, 1);
    EXPECT_LE(poll_max_timeout, 50);
    EXPECT_EQ(run_raised, 1);
    const char *e = lua_tostring(L, -1);
    EXPECT_NE(strstr(e ? e : "", "time limit"), NULL);
    lua_settop(L, 0);

    /* A bounded poll that runs out first is a timeout (nil). */
    run_charges = 0; run_stop_after = 0; poll_calls = 0;
    ASSERT_EQ(call_poll(L, 120), LUA_OK);
    EXPECT_TRUE(lua_isnil(L, -1));
    EXPECT_EQ(poll_unbounded, 0);
    teardown(L);
}

UTEST_MAIN();
