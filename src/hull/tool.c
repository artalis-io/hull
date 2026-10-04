/*
 * tool.c - Tool mode: unsandboxed Lua VM for hull build tools
 *
 * Provides hull_tool() for running Lua stdlib modules with controlled
 * process/filesystem access, and hull_keygen() for Ed25519 key generation.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/tool.h"
#include "hull/stdlib_feature.h"
#include "hull/compilers.h"
#include "hull/module_registry.h"
#include "hull/compiler.h"
#include "hull/linker.h"
#include "hull/cap/crypto.h"
#include "hull/cap/tool.h"
#include "hull/runtime/tool.h"
#include "hull/runtime/cache_common.h"   /* the cache seal keys */
#include "hull/sandbox.h"
#include "hull/vfs.h"
#include "hull/entry.h"

#ifdef HL_ENABLE_LUA
#include "hull/runtime/lua.h"
#include "lua.h"
#include "lauxlib.h"
#endif

#ifdef HL_FRONTEND_JS
#include "hull/frontend/js_generation.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── hull keygen ───────────────────────────────────────────────────── */

/* Write @p text to @p path as a NEW file of mode @p mode: O_EXCL (an existing
 * file is refused unless @p force, which replaces it), O_NOFOLLOW (a symlink
 * planted at the path is never written through), the mode forced with fchmod
 * (O_CREAT's mode is masked by the umask, and a replaced file kept its old,
 * possibly world-readable, mode), and every write checked. 0 / -1. */
static int keygen_write(const char *path, const char *text, size_t len,
                        mode_t mode, int force)
{
    if (force) {
        struct stat st;
        if (lstat(path, &st) == 0) {
            if (!S_ISREG(st.st_mode)) {
                fprintf(stderr, "hull keygen: %s exists and is not a regular file\n", path);
                return -1;
            }
            if (unlink(path) != 0) {
                fprintf(stderr, "hull keygen: cannot replace %s\n", path);
                return -1;
            }
        }
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode);
    if (fd < 0) {
        if (errno == EEXIST)
            fprintf(stderr, "hull keygen: %s already exists; refusing to overwrite "
                            "it (pass --force to replace the key)\n", path);
        else
            fprintf(stderr, "hull keygen: cannot create %s: %s\n", path, strerror(errno));
        return -1;
    }
    int bad = fchmod(fd, mode) != 0;
    size_t off = 0;
    while (!bad && off < len) {
        ssize_t n = write(fd, text + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { bad = 1; break; }
        off += (size_t)n;
    }
    if (!bad && fsync(fd) != 0) bad = 1;
    if (close(fd) != 0) bad = 1;
    if (bad) {
        unlink(path);                       /* never leave half a key */
        fprintf(stderr, "hull keygen: failed writing %s\n", path);
        return -1;
    }
    return 0;
}

int hull_keygen(int argc, char **argv)
{
    const char *prefix = "developer";
    int force = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--force") == 0) force = 1;
        else if (argv[i][0] == '-') {
            fprintf(stderr, "hull keygen: unknown option %s\n"
                            "usage: hull keygen [prefix] [--force]\n", argv[i]);
            return 1;
        } else prefix = argv[i];
    }

    /* Build filenames */
    char pk_file[256], sk_file[256];
    if ((size_t)snprintf(pk_file, sizeof(pk_file), "%s.pub", prefix) >= sizeof pk_file ||
        (size_t)snprintf(sk_file, sizeof(sk_file), "%s.key", prefix) >= sizeof sk_file) {
        fprintf(stderr, "hull keygen: prefix too long\n");
        return 1;
    }

    uint8_t pk[32], sk[64];
    if (hl_cap_crypto_ed25519_keypair(pk, sk) != 0) {
        fprintf(stderr, "hull keygen: keypair generation failed\n");
        return 1;
    }

    char pk_hex[65 + 1], sk_hex[129 + 1];
    for (int i = 0; i < 32; i++) snprintf(pk_hex + 2 * i, 3, "%02x", pk[i]);
    pk_hex[64] = '\n'; pk_hex[65] = '\0';
    for (int i = 0; i < 64; i++) snprintf(sk_hex + 2 * i, 3, "%02x", sk[i]);
    sk_hex[128] = '\n'; sk_hex[129] = '\0';

    /* The secret first: if it cannot be written, no public half is left
     * behind naming a key that does not exist. */
    int rc = keygen_write(sk_file, sk_hex, 129, 0600, force);
    if (rc == 0 && keygen_write(pk_file, pk_hex, 65, 0644, force) != 0) {
        unlink(sk_file);
        rc = -1;
    }

    /* Zero secret key material on every path */
    volatile uint8_t *p = sk;
    for (size_t i = 0; i < sizeof(sk); i++) p[i] = 0;
    volatile char *q = sk_hex;
    for (size_t i = 0; i < sizeof(sk_hex); i++) q[i] = 0;
    if (rc != 0) return 1;

    printf("wrote %s (public key)\n", pk_file);
    printf("wrote %s (secret key - keep safe!)\n", sk_file);
    return 0;
}

/* ── hull tool (Lua) ───────────────────────────────────────────────── */

#ifdef HL_ENABLE_LUA

/*
 * Parse --compiler from argv. Accepts both forms:
 *   --compiler system   (space-separated)
 *   --compiler=system   (equals-separated)
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

/* Parse --linker (`--linker lld` / `--linker=lld`) for the link backend
 * (system | lld | <path>). NULL = auto-select. */
static const char *parse_linker_option(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--linker") == 0 && i + 1 < argc)
            return argv[i + 1];
        if (strncmp(argv[i], "--linker=", 9) == 0)
            return argv[i] + 9;
    }
    return NULL;
}

/*
 * Extract app_dir from argv (first positional arg not starting with '-').
 * Returns "." if not found.
 */
static const char *parse_app_dir(int argc, char **argv)
{
    /* From i=1: argv[0] is the SUBCOMMAND ("build"), which
     * hl_command_dispatch passes through as argv[0] of the handler.
     * Scanning from 0 returned "build" as the app dir, so the
     * read-only app unveil pointed at a relative path that does not
     * exist. It went unnoticed because the temp dir and the
     * invocation dir are unveiled separately, and apps normally live
     * under one of them. */
    /* Options that take their value as the NEXT argument (a --flag=value
     * form is one token and needs nothing). Their values are not the app
     * directory: `hull verify --developer-key k.pub app` took "k.pub". */
    static const char *const value_flags[] = {
        "--compiler", "--sign", "--runtime", "--output", "-o", "--linker",
        "--target", "--flavor", "--with", "--platform-sig",
        "--platform-key", "--developer-key", "--gethull-key", "--binary", NULL
    };
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (argv[i][0] != '-')
            return argv[i];
        for (int k = 0; value_flags[k]; k++) {
            if (strcmp(argv[i], value_flags[k]) == 0) {
                i++; /* skip value */
                break;
            }
        }
    }
    return ".";
}

/*
 * Extract the output DIRECTORY from argv: dirname of `-o` / `--output`, else
 * the app dir (a bare `hull build <dir>` writes <dir>/app).
 *
 * The tool sandbox unveils app_dir READ-ONLY, so writes under it depend
 * entirely on the output dir being unveiled "rwc". That used to be the literal
 * ".", i.e. whatever directory hull happened to be INVOKED from - so
 * `hull build /path/to/app` from anywhere else could not create
 * <app>/.hull/build, and the post-link tidy-up that moves cosmocc's debug
 * sidecars (app.com.dbg, app.aarch64.elf) out of the app root silently
 * degraded to "leaving debug artifacts in place". Building from INSIDE the app
 * dir worked, which is why it went unnoticed. Not Windows-specific.
 *
 * Writes into `buf` and returns it, or NULL when the caller should fall back.
 */
static const char *parse_output_dir(int argc, char **argv, char *buf, size_t bufsz)
{
    const char *out = NULL;
    for (int i = 1; i < argc; i++) {   /* i=1: skip the subcommand */
        if (!argv[i]) continue;
        if (strncmp(argv[i], "--output=", 9) == 0) { out = argv[i] + 9; continue; }
        if ((strcmp(argv[i], "--output") == 0 || strcmp(argv[i], "-o") == 0)
            && i + 1 < argc && argv[i + 1]) {
            out = argv[++i];
        }
    }
    if (!out) return NULL;                 /* caller falls back to app_dir */

    /* dirname(out); a bare filename means the current directory. */
    const char *slash = strrchr(out, '/');
    const char *bslash = strrchr(out, '\\');
    if (bslash > slash) slash = bslash;
    if (!slash) {                          /* a bare name: the cwd */
        if (bufsz < 2) return NULL;
        buf[0] = '.';
        buf[1] = '\0';
        return buf;
    }

    size_t len = (size_t)(slash - out);
    if (len == 0) len = 1;                 /* "/x" -> "/" */
    if (len >= bufsz) return NULL;
    memcpy(buf, out, len);
    buf[len] = '\0';
    return buf;
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

    /* Validate compiler against allowlist (skip for the "system" sentinel) */
    if (strcmp(cc, "system") != 0 &&
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

    /* Where `hull build` actually writes. This used to be the literal ".",
     * i.e. the invocation directory, which is only the app dir when you
     * happen to build from inside it - see parse_output_dir. The
     * invocation dir is unveiled unconditionally inside
     * hl_tool_sandbox_init now, so nothing is lost by naming the real
     * one here. (It must be passed IN: the context is sealed on return,
     * and hl_tool_unveil_add then silently drops adds.) */
    char out_buf[4096];
    const char *output_dir = parse_output_dir(argc, argv, out_buf, sizeof(out_buf));

    /* Load the cache sealing keys now: they live in $HOME/.hull, which the
     * tool sandbox does not grant, and were read lazily after it applied -
     * so the tool VM's AOT cache was silently off under a kernel sandbox. */
    hl_runtime_cache_seal_prepare();
    hl_tool_cache_seal_prepare();

    int scaffold = strcmp(module, "hull.new") == 0 ||
                   strcmp(module, "hull.init") == 0;
    if (hl_tool_sandbox_init(&unveil_ctx, app_dir,
                             output_dir ? output_dir : app_dir,
                             platform_dir, scaffold) != 0) {
        fprintf(stderr, "hull: the tool sandbox could not be applied\n");
        return 1;
    }

    /* Init unsandboxed Lua VM with tool unveil context */
    HlLuaConfig cfg = HL_LUA_CONFIG_DEFAULT;
    cfg.sandbox = 0;

    /* Init VFS instances for module loading. The platform VFS unions the base
     * stdlib with each composed runtime's stdlib (tool mode is Lua-only, so it
     * pulls the Lua feature entries when the runtime is composed as a feature). */
    extern const HlEntry hl_app_entries[];
    HlVfs platform_vfs, app_vfs;
    void    *platform_vfs_owned = NULL;
    hl_platform_vfs_init(&platform_vfs, &platform_vfs_owned);
    hl_vfs_init(&app_vfs, hl_app_entries, app_dir);

    HlLua lua;
    memset(&lua, 0, sizeof(lua));
    lua.tool_unveil_ctx = &unveil_ctx;
    lua.base.platform_vfs = &platform_vfs;
    lua.base.app_vfs = &app_vfs;

    if (hl_lua_init(&lua, &cfg) != 0) {
        fprintf(stderr, "hull: Lua init failed\n");
        hl_platform_vfs_dispose(platform_vfs_owned);
        return 1;
    }

    /* Manifest extraction resolves an app's relative local modules the SAME way
     * the runtime does: extraction (`hull build`, `hull manifest`, ...) executes
     * the app entry via tool.loadfile + pcall, and a modular app's top-level
     * require("./routes/users") (+ nested ./../models/user) only reaches the
     * filesystem-fallback resolver in hl_lua_require when lua->app_dir is set,
     * which then applies requiring-module-relative resolution + canonical ./..
     * collapse + app-root containment. That root is set on demand by
     * tool.set_app_dir(dir) from the extraction code, which knows the real app
     * directory; parse_app_dir() above is a sandbox-unveil heuristic that
     * returns the first positional (the SUBCOMMAND, e.g. "build", for a
     * `hull build <dir>` invocation) and is NOT a reliable module root. See
     * l_tool_set_app_dir in mod_tool.c and fcompose.extract_manifest. */

    /* Tool-mode native-module exposure.
     *
     * The tool VM is used to load app code for manifest extraction
     * (`hull manifest`, `hull modules list`, `hull deploy`, …) AND to
     * run hull's own tool modules (`hull.build`, `hull.sign_platform`,
     * `hull.inspect`, etc.). Both code paths assume:
     *
     *   1. `require("hull.X")` works for every native (apps after
     *      phase 2b top-level-require their deps).
     *   2. `crypto.X`, `db.X`, … are reachable as globals - tool .lua
     *      modules are trusted hull internals shipped with hull and
     *      pre-date the import-only refactor.
     *
     * Two stubs together cover both:
     *
     *   (a) For conditional natives whose backing handle isn't wired up
     *       in tool mode (hull.db, hull.worker, hull.compute, hull.gpu),
     *       install a chain-friendly nop table into LUA_LOADED_TABLE so
     *       `local db = require("hull.db")` succeeds and `db.exec(...)`
     *       silently no-ops. Tool-mode never invokes route handlers, so
     *       the no-op stub is safe - only top-level code runs to set
     *       the manifest.
     *
     *   (b) Promote every entry in LUA_LOADED_TABLE to a global of the
     *       same short name (hull.crypto → crypto, hull.web.middleware.session
     *       → session, …). Restores phase-2a tool-mode globals. */
    hl_lua_stub_unbacked_modules(lua.L);   /* (a), shared with manifest extraction */

    /* (b) Promote registry-known modules in _LOADED to short-name globals
     * for tool-mode convenience. Required because every shipped tool
     * .lua (hull.build, hull.sign_platform, hull.inspect, …) references
     * `crypto`, `db`, `time`, etc. as globals, predating the phase-2b
     * import-only refactor of the runtime. Only registry-known names
     * are promoted; private bridges (hull._template) and user files are
     * left alone. */
    {
        lua_State *L = lua.L;
        size_t total = 0;
        const HlModuleSpec *all = hl_module_registry_all(&total);
        lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
        for (size_t i = 0; i < total; i++) {
            const char *cname = all[i].name;
            if (strncmp(cname, "hull/", 5) != 0) continue;
            const char *short_name = cname + 5;
            /* Skip nested names like "middleware/session" - those are
             * stdlib .lua modules accessed via require, never as a
             * global (no Lua identifier could spell that). */
            if (strchr(short_name, '/')) continue;
            char lua_name[HL_MODULE_NAME_MAX];
            size_t cname_len = strlen(cname);
            if (cname_len + 1 > sizeof(lua_name)) continue;
            memcpy(lua_name, cname, cname_len + 1);
            for (char *p = lua_name; *p; p++)
                if (*p == '/') *p = '.';
            lua_getfield(L, -1, lua_name);
            if (!lua_isnil(L, -1))
                lua_setglobal(L, short_name);
            else
                lua_pop(L, 1);
        }
        lua_pop(L, 1);  /* pop _LOADED */
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

    /* Select a linker backend and expose as tool.linker (the compiler-free
     * build's link step; independent of the compiler choice). */
    HlLinker *linker = hl_linker_select(parse_linker_option(argc, argv), hull_exe);
    if (linker)
        hl_lua_tool_expose_linker(L, linker);

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

    /* Load the command module and run its entry. Each command script RETURNS
     * its main() function; the dispatcher invokes it here rather than letting
     * the script self-run at load time. This is the mechanism that keeps a
     * command's main() from firing when the module is merely require()'d as a
     * dependency: an app that legitimately does require("hull.compute") during
     * manifest extraction pulls in the CLI compute.lua and gets its main
     * function back, but only THIS dispatcher ever calls it. (Previously the
     * script self-dispatched at the bottom of the file, so such a require ran
     * main() against the *build* argv: `hull compute: unknown command '.'`.) */
    char code[256];
    snprintf(code, sizeof(code), "return require('%s')", module);
    int rc = luaL_dostring(L, code);   /* leaves the module's return on the stack */
    if (rc == LUA_OK) {
        if (lua_isfunction(L, -1)) {
            rc = lua_pcall(L, 0, 0, 0);   /* run main(); errors propagate below */
        } else {
            lua_pop(L, 1);                /* module returned no entry; nothing to run */
        }
    }
    if (rc != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        fprintf(stderr, "hull %s: %s\n", module, err ? err : "unknown error");
        lua_pop(L, 1);
    }

    hl_lua_free(&lua);
#ifdef HL_FRONTEND_JS
    /* The Lua tool VM is the sole caller of the JS generation manager
     * (hl_js_gen_open/_close via the frontend proxy). Now that
     * it is gone, tear the manager down unconditionally -- this destroys any
     * generation session left live by a defective/interrupted analysis and must
     * NOT reset the monotonic next_token. A JS-less build compiles without this. */
    hl_js_gen_shutdown();
#endif
    if (compiler)
        hl_compiler_destroy(compiler);
    hl_platform_vfs_dispose(platform_vfs_owned);
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
