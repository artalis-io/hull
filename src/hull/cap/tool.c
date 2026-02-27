/*
 * cap/tool.c — Controlled process/filesystem access for tool scripts
 *
 * Replaces raw os/io in tool mode with an explicit, auditable C module.
 * Each function is a Lua C function registered in the `tool` global table.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/tool.h"

#include "lua.h"
#include "lauxlib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── tool.exec(cmd) ────────────────────────────────────────────────── */

static int l_tool_exec(lua_State *L)
{
    const char *cmd = luaL_checkstring(L, 1);
    int rc = system(cmd);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.read(cmd) ────────────────────────────────────────────────── */

static int l_tool_read(lua_State *L)
{
    const char *cmd = luaL_checkstring(L, 1);
    FILE *f = popen(cmd, "r");
    if (!f) {
        lua_pushnil(L);
        return 1;
    }

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        luaL_addlstring(&b, buf, n);
    pclose(f);
    luaL_pushresult(&b);
    return 1;
}

/* ── tool.tmpdir() ─────────────────────────────────────────────────── */

static int l_tool_tmpdir(lua_State *L)
{
    char tmpl[] = "/tmp/hull_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, dir);
    return 1;
}

/* ── tool.exit(code) ───────────────────────────────────────────────── */

static int l_tool_exit(lua_State *L)
{
    int code = (int)luaL_checkinteger(L, 1);
    exit(code);
    return 0; /* unreachable */
}

/* ── tool.read_file(path) ──────────────────────────────────────────── */

static int l_tool_read_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    FILE *f = fopen(path, "rb");
    if (!f) {
        lua_pushnil(L);
        return 1;
    }

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        luaL_addlstring(&b, buf, n);
    fclose(f);
    luaL_pushresult(&b);
    return 1;
}

/* ── tool.write_file(path, data) ───────────────────────────────────── */

static int l_tool_write_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);

    FILE *f = fopen(path, "wb");
    if (!f) {
        lua_pushboolean(L, 0);
        return 1;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    lua_pushboolean(L, written == len);
    return 1;
}

/* ── tool.file_exists(path) ────────────────────────────────────────── */

static int l_tool_file_exists(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    lua_pushboolean(L, access(path, F_OK) == 0);
    return 1;
}

/* ── tool.stderr(msg) ──────────────────────────────────────────────── */

static int l_tool_stderr(lua_State *L)
{
    const char *msg = luaL_checkstring(L, 1);
    fprintf(stderr, "%s", msg);
    return 0;
}

/* ── tool.loadfile(path) ───────────────────────────────────────────── */

static int l_tool_loadfile(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    int rc = luaL_loadfile(L, path);
    if (rc != LUA_OK) {
        /* Stack: error message. Return nil, errmsg. */
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    return 1; /* chunk function on stack */
}

/* ── Registration ──────────────────────────────────────────────────── */

static const luaL_Reg tool_funcs[] = {
    { "exec",        l_tool_exec },
    { "read",        l_tool_read },
    { "tmpdir",      l_tool_tmpdir },
    { "exit",        l_tool_exit },
    { "read_file",   l_tool_read_file },
    { "write_file",  l_tool_write_file },
    { "file_exists", l_tool_file_exists },
    { "stderr",      l_tool_stderr },
    { "loadfile",    l_tool_loadfile },
    { NULL, NULL }
};

void hl_cap_tool_register(lua_State *L)
{
    luaL_newlib(L, tool_funcs);
    lua_setglobal(L, "tool");
}
