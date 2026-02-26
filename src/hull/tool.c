/*
 * tool.c — Tool mode: unsandboxed Lua VM for hull build tools
 *
 * Provides hull_tool() for running Lua stdlib modules with full
 * filesystem access, and hull_keygen() for Ed25519 key generation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/tool.h"
#include "hull/cap/crypto.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#include "lua.h"
#include "lauxlib.h"
#endif

#include <stdio.h>
#include <string.h>

/* ── hull keygen ───────────────────────────────────────────────────── */

int hull_keygen(int argc, char **argv)
{
    const char *prefix = "developer";
    if (argc >= 2)
        prefix = argv[1];

    uint8_t pk[32], sk[64];
    if (hl_cap_crypto_ed25519_keypair(pk, sk) != 0) {
        fprintf(stderr, "hull keygen: keypair generation failed\n");
        return 1;
    }

    /* Build filenames */
    char pk_file[256], sk_file[256];
    snprintf(pk_file, sizeof(pk_file), "%s.pub", prefix);
    snprintf(sk_file, sizeof(sk_file), "%s.key", prefix);

    /* Write hex-encoded public key */
    FILE *f = fopen(pk_file, "w");
    if (!f) {
        fprintf(stderr, "hull keygen: cannot write %s\n", pk_file);
        return 1;
    }
    for (int i = 0; i < 32; i++)
        fprintf(f, "%02x", pk[i]);
    fprintf(f, "\n");
    fclose(f);

    /* Write hex-encoded secret key */
    f = fopen(sk_file, "w");
    if (!f) {
        fprintf(stderr, "hull keygen: cannot write %s\n", sk_file);
        return 1;
    }
    for (int i = 0; i < 64; i++)
        fprintf(f, "%02x", sk[i]);
    fprintf(f, "\n");
    fclose(f);

    printf("wrote %s (public key)\n", pk_file);
    printf("wrote %s (secret key — keep safe!)\n", sk_file);
    return 0;
}

/* ── hull tool (Lua) ───────────────────────────────────────────────── */

#ifdef HL_ENABLE_LUA

int hull_tool(const char *module, int argc, char **argv, const char *hull_exe)
{
    if (!module) {
        fprintf(stderr, "hull: no tool module specified\n");
        return 1;
    }

    /* Init unsandboxed Lua VM */
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.sandbox = 0;

    HlLua lua;
    memset(&lua, 0, sizeof(lua));
    if (hl_lua_init(&lua, &cfg) != 0) {
        fprintf(stderr, "hull: Lua init failed\n");
        return 1;
    }

    /* Pass CLI args as global `arg` table */
    lua_State *L = lua.L;
    lua_newtable(L);
    for (int i = 0; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i);
    }
    lua_setglobal(L, "arg");

    /* Expose hull binary path for locating platform assets */
    if (hull_exe) {
        lua_pushstring(L, hull_exe);
        lua_setglobal(L, "__hull_exe");
    }

    /* Load and run the stdlib module */
    char code[256];
    snprintf(code, sizeof(code), "require('%s')", module);
    int rc = luaL_dostring(L, code);
    if (rc != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        fprintf(stderr, "hull %s: %s\n", module, err ? err : "unknown error");
        lua_pop(L, 1);
    }

    hl_lua_free(&lua);
    return (rc == LUA_OK) ? 0 : 1;
}

#else /* !HL_ENABLE_LUA */

int hull_tool(const char *module, int argc, char **argv, const char *hull_exe)
{
    (void)module; (void)argc; (void)argv; (void)hull_exe;
    fprintf(stderr, "hull: Lua runtime not enabled in this build\n");
    return 1;
}

#endif /* HL_ENABLE_LUA */
