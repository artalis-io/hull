/*
 * tool.c — Tool mode: unsandboxed Lua VM for hull build tools
 *
 * Provides hull_tool() for running Lua stdlib modules with controlled
 * process/filesystem access, and hull_keygen() for Ed25519 key generation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/tool.h"
#include "hull/compilers.h"
#include "hull/compiler.h"
#include "hull/cap/crypto.h"
#include "hull/cap/tool.h"
#include "hull/runtime/tool.h"
#include "hull/sandbox.h"
#include "hull/vfs.h"
#include "hull/entry.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#include "lua.h"
#include "lauxlib.h"
#endif

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

    /* Write hex-encoded secret key (mode 0600 — owner-only) */
    int sk_fd = open(sk_file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (sk_fd < 0) {
        fprintf(stderr, "hull keygen: cannot write %s\n", sk_file);
        return 1;
    }
    f = fdopen(sk_fd, "w");
    if (!f) {
        close(sk_fd);
        fprintf(stderr, "hull keygen: cannot write %s\n", sk_file);
        return 1;
    }
    for (int i = 0; i < 64; i++)
        fprintf(f, "%02x", sk[i]);
    fprintf(f, "\n");
    fclose(f);

    /* Zero secret key material before returning */
    volatile uint8_t *p = sk;
    for (size_t i = 0; i < sizeof(sk); i++)
        p[i] = 0;

    printf("wrote %s (public key)\n", pk_file);
    printf("wrote %s (secret key — keep safe!)\n", sk_file);
    return 0;
}

/* ── hull tool (Lua) ───────────────────────────────────────────────── */

#ifdef HL_ENABLE_LUA

/*
 * Parse --compiler from argv. Accepts both forms:
 *   --compiler tcc      (space-separated)
 *   --compiler=tcc      (equals-separated)
 * Returns compiler name (or NULL for default).
 */
static const char *parse_cc_option(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--compiler") == 0 && i + 1 < argc)
            return argv[i + 1];
        if (strncmp(argv[i], "--compiler=", 11) == 0)
            return argv[i] + 11;
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
        /* --flag=value: single token, no consumption */
        if (strncmp(argv[i], "--compiler=", 11) == 0 ||
            strncmp(argv[i], "--sign=", 7) == 0 ||
            strncmp(argv[i], "--runtime=", 10) == 0 ||
            strncmp(argv[i], "--output=", 9) == 0)
            continue;
        /* --flag value: skip value */
        if (strcmp(argv[i], "--compiler") == 0 ||
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

    /* Parse --cc/--compiler option for configurable compiler */
    const char *cc_explicit = parse_cc_option(argc, argv);
    const char *cc = cc_explicit ? cc_explicit : HL_DEFAULT_CC;

    /* Validate compiler against allowlist (skip for "tcc" and "system" sentinels) */
    if (strcmp(cc, "tcc") != 0 && strcmp(cc, "system") != 0 &&
        hl_tool_check_allowlist(cc) != 0) {
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

    /* Init VFS instances for module loading */
    extern const HlEntry hl_stdlib_entries[];
    extern const HlEntry hl_app_entries[];
    HlVfs platform_vfs, app_vfs;
    hl_vfs_init(&platform_vfs, hl_stdlib_entries, NULL);
    hl_vfs_init(&app_vfs, hl_app_entries, app_dir);

    HlLua lua;
    memset(&lua, 0, sizeof(lua));
    lua.tool_unveil_ctx = &unveil_ctx;
    lua.base.platform_vfs = &platform_vfs;
    lua.base.app_vfs = &app_vfs;

    if (hl_lua_init(&lua, &cfg) != 0) {
        fprintf(stderr, "hull: Lua init failed\n");
        return 1;
    }

    /* Set tool.cc and tool.default_cc in the tool global table */
    lua_State *L = lua.L;
    lua_getglobal(L, "tool");
    if (lua_istable(L, -1)) {
        lua_pushstring(L, cc);
        lua_setfield(L, -2, "cc");
        lua_pushstring(L, HL_DEFAULT_CC);
        lua_setfield(L, -2, "default_cc");
    }
    lua_pop(L, 1);

    /* Select compiler backend and expose as tool.compiler.
     * If no explicit cc was given, use auto-detection (NULL) rather than
     * requiring the default (cosmocc) to be installed. */
    HlCompiler *compiler = hl_compiler_select(cc_explicit);
    if (compiler)
        hl_lua_tool_expose_compiler(L, compiler);

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
    if (compiler)
        hl_compiler_destroy(compiler);
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
