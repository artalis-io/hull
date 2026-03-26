/* mod_smtp.c — hull.smtp module: SMTP email sending
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/smtp.h"

#include <sh_arena.h>
#include <stdlib.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════
 * hull.smtp module
 *
 * smtp.send(opts) → { ok = true } or { ok = false, error = "..." }
 * ════════════════════════════════════════════════════════════════════ */

/* Helper: extract string array from Lua table at stack index */
static int lua_get_string_array(lua_State *L, int idx,
                                const char ***out, int *out_count)
{
    *out = NULL;
    *out_count = 0;

    if (!lua_istable(L, idx))
        return 0;

    int len = (int)luaL_len(L, idx);
    if (len <= 0)
        return 0;

    /* Allocate array on Lua stack (will be GC'd) — use scratch arena */
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->scratch)
        return -1;

    const char **arr = sh_arena_calloc(lua->scratch, (size_t)len,
                                       sizeof(const char *));
    if (!arr)
        return -1;

    int count = 0;
    for (int i = 1; i <= len; i++) {
        lua_rawgeti(L, idx, i);
        if (lua_isstring(L, -1)) {
            size_t slen;
            const char *s = lua_tolstring(L, -1, &slen);
            char *copy = sh_arena_alloc(lua->scratch, slen + 1);
            if (copy) {
                memcpy(copy, s, slen + 1);
                arr[count++] = copy;
            }
        }
        lua_pop(L, 1);
    }

    *out = arr;
    *out_count = count;
    return 0;
}

/* smtp.send(opts) */
static int lua_smtp_send(lua_State *L)
{
    HlLua *lua = get_hl_lua(L);
    if (!lua || !lua->base.smtp_cfg)
        return luaL_error(L, "smtp not configured (no hosts in manifest)");

    luaL_checktype(L, 1, LUA_TTABLE);

    /* Extract required fields */
    lua_getfield(L, 1, "host");
    const char *host = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    lua_pop(L, 1);

    lua_getfield(L, 1, "port");
    int port = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : 587;
    lua_pop(L, 1);

    lua_getfield(L, 1, "username");
    const char *username = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    lua_pop(L, 1);

    lua_getfield(L, 1, "password");
    const char *password = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    lua_pop(L, 1);

    lua_getfield(L, 1, "tls");
    int use_tls = 0;
    if (lua_isboolean(L, -1))
        use_tls = lua_toboolean(L, -1) ? 1 : 0;
    else if (lua_isinteger(L, -1))
        use_tls = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "from");
    const char *from = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    lua_pop(L, 1);

    lua_getfield(L, 1, "to");
    const char *to = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    lua_pop(L, 1);

    lua_getfield(L, 1, "subject");
    const char *subject = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    lua_pop(L, 1);

    lua_getfield(L, 1, "body");
    const char *body = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    lua_pop(L, 1);

    lua_getfield(L, 1, "content_type");
    const char *content_type = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    lua_pop(L, 1);

    lua_getfield(L, 1, "reply_to");
    const char *reply_to = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    lua_pop(L, 1);

    /* CC array (optional) */
    const char **cc = NULL;
    int cc_count = 0;
    lua_getfield(L, 1, "cc");
    if (lua_istable(L, -1)) {
        int cc_idx = lua_gettop(L);
        lua_get_string_array(L, cc_idx, &cc, &cc_count);
    }
    lua_pop(L, 1);

    /* Validate required fields */
    if (!host)    { lua_newtable(L); lua_pushboolean(L, 0); lua_setfield(L, -2, "ok"); lua_pushstring(L, "host required"); lua_setfield(L, -2, "error"); return 1; }
    if (!from)    { lua_newtable(L); lua_pushboolean(L, 0); lua_setfield(L, -2, "ok"); lua_pushstring(L, "from required"); lua_setfield(L, -2, "error"); return 1; }
    if (!to)      { lua_newtable(L); lua_pushboolean(L, 0); lua_setfield(L, -2, "ok"); lua_pushstring(L, "to required"); lua_setfield(L, -2, "error"); return 1; }
    if (!subject) { lua_newtable(L); lua_pushboolean(L, 0); lua_setfield(L, -2, "ok"); lua_pushstring(L, "subject required"); lua_setfield(L, -2, "error"); return 1; }
    if (!body)    { lua_newtable(L); lua_pushboolean(L, 0); lua_setfield(L, -2, "ok"); lua_pushstring(L, "body required"); lua_setfield(L, -2, "error"); return 1; }

    /* Build message struct */
    HlSmtpMessage msg = {
        .host = host,
        .port = port,
        .username = username,
        .password = password,
        .use_tls = use_tls,
        .from = from,
        .to = to,
        .cc = cc,
        .cc_count = cc_count,
        .reply_to = reply_to,
        .subject = subject,
        .body = body,
        .content_type = content_type,
    };

    const char *err_msg = NULL;
    int rc = hl_cap_smtp_send(lua->base.smtp_cfg, &msg, &err_msg);

    /* Return { ok = true/false, error = "..." } */
    lua_newtable(L);
    if (rc == 0) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "ok");
    } else {
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "ok");
        lua_pushstring(L, err_msg ? err_msg : "smtp send failed");
        lua_setfield(L, -2, "error");
    }

    return 1;
}

static const luaL_Reg smtp_funcs[] = {
    {"send", lua_smtp_send},
    {NULL, NULL}
};

int luaopen_hull_smtp(lua_State *L)
{
    luaL_newlib(L, smtp_funcs);
    return 1;
}
