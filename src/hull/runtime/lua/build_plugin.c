/*
 * build_plugin.c - `app.transform` for a Lua build plugin
 * (docs/buildcontext_design.md §9.1, include/hull/shared/build_plugin.h).
 *
 * The decoration hands the stdlib (hull._build_plugin) the live `app` and a
 * channel table of three calls over the inherited socket; the loop and the
 * ctx API are Lua. The channel calls are made by the stdlib only: the table
 * stays inside its closure.
 *
 *   chan.next()            -> the next action (a table), or nil when the
 *                             host is done; app.main's own run only, and a
 *                             new run (budget and deadline) each time
 *   chan.call(op, args)    -> the host's reply: { ok = v } or { error = t }
 *   chan.done(err?)        -> the transform ended (err: its message)
 *   chan.bytes(s)          -> a bytes value (a string is text otherwise)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "plain_value.h"

#include "hull/cap/run_watchdog.h"
#include "hull/shared/build_plugin.h"
#include "hull/shared/dispatch_meter.h"
#include "hull/shared/work_charge.h"

#include "lauxlib.h"

#include <stdlib.h>

static uint32_t g_call_id;

static int not_a_plugin(lua_State *L)
{
    return luaL_error(L, "build plugin: this process has no channel to a "
                         "build host (run it through `hull build`)");
}

/* Read one frame; the wait is the host's, not the run's work. */
static int recv_frame(HlChanFrame *f, uint64_t deadline)
{
    uint64_t tok = hl_meter_exclude_begin();
    int rc = hl_build_plugin_recv(f, deadline);
    hl_meter_exclude_end(tok);
    return rc;
}

static int chan_next(lua_State *L)
{
    HlLua *lua = get_hl_lua_from_L(L);
    if (hl_build_plugin_fd() < 0) return not_a_plugin(L);
    if (!lua || L != lua->cli_main_co)
        return luaL_error(L, "build plugin: actions are read by app.main's run");
    HlChanFrame f;
    int rc = recv_frame(&f, 0);
    if (rc != HL_CHAN_DONE || f.type == HL_BP_STOP) {
        int stop = rc == HL_CHAN_DONE || rc == HL_CHAN_EOF;
        hl_chan_frame_reset(&f);
        if (!stop) hl_build_plugin_end();
        if (stop) {
            lua_pushnil(L);
            return 1;
        }
        return luaL_error(L, "build plugin: the channel to the host broke");
    }
    if (f.type != HL_BP_ACTION) {
        hl_chan_frame_reset(&f);
        hl_build_plugin_end();
        return luaL_error(L, "build plugin: the host sent a frame out of turn");
    }
    /* An action is a run of its own, as a tui.poll event is. */
    HL_LUA_ARM_KIND(lua, L, HL_RUN_MAIN);
    int err = hl_lua_pv_decode_protected(L, f.buf, f.len);
    size_t len = f.len;
    hl_chan_frame_reset(&f);
    if (err == HL_LUA_PV_RAISED) return lua_error(L);
    if (err != HL_PV_OK) {
        hl_build_plugin_end();
        return luaL_error(L, "build plugin: the host sent a %s action",
                          hl_pv_error_name(err));
    }
    lua_hlcharge(L, len / 8, 0);
    return 1;
}

static uint64_t reply_deadline(lua_State *L)
{
    int64_t left = hl_run_deadline_left_ms();
    if (left == 0) {
        luaL_error(L, "build plugin: the run's time is up");
        return 0;
    }
    uint64_t ms = (left < 0 || left > (int64_t)HL_BP_REPLY_TIMEOUT_MS)
                      ? HL_BP_REPLY_TIMEOUT_MS : (uint64_t)left;
    return hl_chan_mono_ms() + ms;
}

static int chan_call(lua_State *L)
{
    size_t oplen;
    const char *op = luaL_checklstring(L, 1, &oplen);
    lua_settop(L, 2);
    if (hl_build_plugin_fd() < 0) return not_a_plugin(L);
    /* The round trip's floor before anything is allocated (it may raise). */
    lua_hlwork(L, HL_WORK_PLUGIN_CALL_UNITS, 0);
    uint64_t deadline = reply_deadline(L);

    HlPvWriter w;
    hl_pv_writer_init(&w, HL_BP_MAX_PAYLOAD);
    hl_pv_put_map(&w, 2);
    hl_pv_put_key(&w, "args", 4);
    w.depth = 1;   /* inside the envelope, as the host's check counts it */
    hl_lua_pv_encode(L, 2, &w);
    w.depth = 0;
    hl_pv_put_key(&w, "op", 2);
    hl_pv_put_text(&w, op, oplen);
    if (w.err != HL_PV_OK) {
        int e = w.err;
        hl_pv_writer_free(&w);
        return luaL_error(L, "build plugin: %s: arguments are %s", op,
                          hl_pv_error_name(e));
    }
    uint32_t id = ++g_call_id;
    size_t sent = w.len;
    int rc = hl_build_plugin_send(HL_BP_CALL, id, w.buf, w.len, deadline);
    hl_pv_writer_free(&w);
    if (rc != 0) return luaL_error(L, "build plugin: the channel to the host broke");
    lua_hlcharge(L, sent / 8, 0);

    HlChanFrame f;
    rc = recv_frame(&f, deadline);
    if (rc != HL_CHAN_DONE || f.type != HL_BP_REPLY || f.id != id) {
        hl_chan_frame_reset(&f);
        hl_build_plugin_end();
        return luaL_error(L, rc == HL_CHAN_MORE
                                 ? "build plugin: %s: the host did not answer"
                                 : "build plugin: %s: the channel to the host broke",
                          op);
    }
    int err = hl_lua_pv_decode_protected(L, f.buf, f.len);
    size_t len = f.len;
    hl_chan_frame_reset(&f);
    if (err == HL_LUA_PV_RAISED) return lua_error(L);
    if (err != HL_PV_OK) {
        hl_build_plugin_end();
        return luaL_error(L, "build plugin: %s: the host sent a %s reply", op,
                          hl_pv_error_name(err));
    }
    lua_hlcharge(L, len / 8, 0);
    return 1;
}

static int chan_done(lua_State *L)
{
    if (hl_build_plugin_fd() < 0) return not_a_plugin(L);
    size_t n = 0;
    const char *msg = lua_isnoneornil(L, 1) ? NULL : luaL_tolstring(L, 1, &n);
    HlPvWriter w;
    hl_pv_writer_init(&w, 64 * 1024);
    hl_build_plugin_put_done(&w, msg, n);
    int rc = w.err ? -1 : hl_build_plugin_send(HL_BP_DONE, 0, w.buf, w.len,
                                              hl_chan_mono_ms() + HL_BP_REPLY_TIMEOUT_MS);
    hl_pv_writer_free(&w);
    lua_pushboolean(L, rc == 0);
    return 1;
}

static int chan_bytes(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TSTRING);
    hl_lua_pv_push_bytes(L, 1);
    return 1;
}

static const luaL_Reg chan_funcs[] = {
    {"next",  chan_next},
    {"call",  chan_call},
    {"done",  chan_done},
    {"bytes", chan_bytes},
    {NULL, NULL}
};

/* The stub runner test.run_transform calls, keyed by this address. */
static const char STUB_KEY = 0;

void hl_lua_install_app_transform(lua_State *L)
{
    lua_pushcfunction(L, hl_lua_require_trusted);   /* as mod_fs.c does */
    lua_pushliteral(L, "hull._build_plugin");
    lua_call(L, 1, 1);                 /* function(app, chan) */
    lua_getglobal(L, "app");
    lua_createtable(L, 0, 4);
    hl_lua_setfuncs(L, chan_funcs, 0);
    lua_call(L, 2, 1);                 /* the stub runner */
    lua_rawsetp(L, LUA_REGISTRYINDEX, &STUB_KEY);
}

int hl_lua_push_transform_stub(lua_State *L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &STUB_KEY);
    if (lua_isfunction(L, -1)) return 1;
    lua_pop(L, 1);
    return 0;
}
