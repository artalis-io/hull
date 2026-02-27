/*
 * tool.c — Tool mode: unsandboxed Lua VM for hull build tools
 *
 * Provides hull_tool() for running Lua stdlib modules with controlled
 * process/filesystem access, and hull_keygen() for Ed25519 key generation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/tool.h"
#include "hull/cap/crypto.h"
#include "hull/cap/tool.h"
#include "hull/sandbox.h"

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

/*
 * Parse --cc option from argv, return compiler name (or NULL for default).
 * Scans for "--cc" followed by a value.
 */
static const char *parse_cc_option(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--cc") == 0 && i + 1 < argc)
            return argv[i + 1];
    }
    return NULL;
}

/*
 * Extract app_dir from argv (first positional arg not starting with '-').
 * Returns "." if not found.
 */
static const char *parse_app_dir(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (!argv[i]) continue;
        if (argv[i][0] != '-')
            return argv[i];
        /* Skip --flag value pairs */
        if (strcmp(argv[i], "--cc") == 0 ||
            strcmp(argv[i], "--sign") == 0 ||
            strcmp(argv[i], "--runtime") == 0 ||
            strcmp(argv[i], "--output") == 0 ||
            strcmp(argv[i], "-o") == 0) {
            i++; /* skip value */
        }
    }
    return ".";
}

int hull_tool(const char *module, int argc, char **argv, const char *hull_exe)
{
    if (!module) {
        fprintf(stderr, "hull: no tool module specified\n");
        return 1;
    }

    /* Parse --cc option for configurable compiler */
    const char *cc = parse_cc_option(argc, argv);
    if (!cc) cc = "cosmocc";

    /* Validate compiler against allowlist */
    if (hl_tool_check_allowlist(cc) != 0) {
        fprintf(stderr, "hull: compiler '%s' not in allowlist\n", cc);
        return 1;
    }

    /* Set up tool-mode unveil context */
    const char *app_dir = parse_app_dir(argc, argv);
    HlToolUnveilCtx unveil_ctx;

    /* Derive platform directory from hull binary path */
    const char *platform_dir = NULL;
    char platform_buf[4096];
    if (hull_exe) {
        const char *slash = strrchr(hull_exe, '/');
        if (slash) {
            size_t len = (size_t)(slash - hull_exe + 1);
            if (len < sizeof(platform_buf)) {
                memcpy(platform_buf, hull_exe, len);
                platform_buf[len] = '\0';
                platform_dir = platform_buf;
            }
        }
    }

    hl_tool_sandbox_init(&unveil_ctx, app_dir, ".", platform_dir);

    /* Init unsandboxed Lua VM with tool unveil context */
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.sandbox = 0;

    HlLua lua;
    memset(&lua, 0, sizeof(lua));
    lua.tool_unveil_ctx = &unveil_ctx;

    if (hl_lua_init(&lua, &cfg) != 0) {
        fprintf(stderr, "hull: Lua init failed\n");
        return 1;
    }

    /* Set tool.cc in the tool global table */
    lua_State *L = lua.L;
    lua_getglobal(L, "tool");
    if (lua_istable(L, -1)) {
        lua_pushstring(L, cc);
        lua_setfield(L, -2, "cc");
    }
    lua_pop(L, 1);

    /* Pass CLI args as global `arg` table */
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
