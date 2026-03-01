/*
 * cap/tool.c — Controlled process/filesystem access for tool scripts
 *
 * All process execution uses fork/execvp with an allowlisted set of
 * compiler binaries. All filesystem operations validate paths against
 * an unveil table. No shell invocation (system/popen) anywhere.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/tool.h"
#include "hull/build_assets.h"

#ifdef HL_ENABLE_LUA
#include "lua.h"
#include "lauxlib.h"
#endif

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HL_ENABLE_LUA
/* Registry key for the unveil context pointer */
#define TOOL_UNVEIL_KEY "__hull_tool_unveil"
#endif

/* ── Unveil path table ─────────────────────────────────────────────── */

void hl_tool_unveil_init(HlToolUnveilCtx *ctx)
{
    if (!ctx) return;
    memset(ctx, 0, sizeof(*ctx));
}

int hl_tool_unveil_add(HlToolUnveilCtx *ctx, const char *path, const char *perms)
{
    if (!ctx || !path || !perms) return -1;
    if (ctx->sealed) return -1;
    if (ctx->count >= HL_TOOL_MAX_UNVEILED) return -1;

    /* Resolve to absolute path if possible */
    char resolved[PATH_MAX];
    const char *use_path = path;
    int resolved_differs = 0;
    if (realpath(path, resolved) != NULL) {
        if (strcmp(resolved, path) != 0)
            resolved_differs = 1;
        use_path = resolved;
    }

    /* Store the resolved copy */
    char *dup = strdup(use_path);
    if (!dup) return -1;

    ctx->entries[ctx->count].path = dup;
    ctx->entries[ctx->count].perms = perms;
    ctx->count++;

    /* If resolved path differs (e.g. /tmp → /private/tmp on macOS),
     * also store the original path for prefix matching */
    if (resolved_differs && ctx->count < HL_TOOL_MAX_UNVEILED) {
        char *dup_orig = strdup(path);
        if (dup_orig) {
            ctx->entries[ctx->count].path = dup_orig;
            ctx->entries[ctx->count].perms = perms;
            ctx->count++;
        }
    }

    return 0;
}

void hl_tool_unveil_free(HlToolUnveilCtx *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < ctx->count; i++)
        free((void *)ctx->entries[i].path);
    memset(ctx, 0, sizeof(*ctx));
}

void hl_tool_unveil_seal(HlToolUnveilCtx *ctx)
{
    if (ctx) ctx->sealed = 1;
}

int hl_tool_unveil_check(const HlToolUnveilCtx *ctx, const char *path, char needed)
{
    if (!ctx || !path) return -1;

    /* Resolve the path being checked */
    char resolved[PATH_MAX];
    const char *check_path = path;
    if (realpath(path, resolved) != NULL)
        check_path = resolved;

    for (int i = 0; i < ctx->count; i++) {
        const char *unveiled = ctx->entries[i].path;
        size_t ulen = strlen(unveiled);

        /* Check if path is under unveiled prefix */
        if (strncmp(check_path, unveiled, ulen) != 0)
            continue;

        /* Must be exact match or have a / separator */
        if (check_path[ulen] != '\0' && check_path[ulen] != '/')
            continue;

        /* Check permission */
        if (strchr(ctx->entries[i].perms, needed) != NULL)
            return 0;
    }

    return -1;
}

/* ── Compiler allowlist ────────────────────────────────────────────── */

static const char *allowed_prefixes[] = {
    "cc", "gcc", "clang", "cosmocc", "cosmoar", "ar", NULL
};

int hl_tool_check_allowlist(const char *binary)
{
    if (!binary) return -1;

    /* Extract basename */
    const char *base = strrchr(binary, '/');
    base = base ? base + 1 : binary;

    for (const char **p = allowed_prefixes; *p; p++) {
        size_t plen = strlen(*p);
        if (strncmp(base, *p, plen) == 0) {
            /* Exact match or versioned variant (e.g. clang-18, gcc-12) */
            char next = base[plen];
            if (next == '\0' || next == '-')
                return 0;
        }
    }

    return -1;
}

/* ── Process spawning ──────────────────────────────────────────────── */

int hl_tool_spawn(const char *const argv[])
{
    if (!argv || !argv[0]) return -1;
    if (hl_tool_check_allowlist(argv[0]) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* Child: exec */
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* Parent: wait */
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

char *hl_tool_spawn_read(const char *const argv[], size_t *out_len)
{
    if (!argv || !argv[0]) return NULL;
    if (hl_tool_check_allowlist(argv[0]) != 0) return NULL;

    int pipefd[2];
    if (pipe(pipefd) < 0) return NULL;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    if (pid == 0) {
        /* Child: redirect stdout to pipe */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    /* Parent: read from pipe */
    close(pipefd[1]);

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return NULL;
    }

    ssize_t n;
    while ((n = read(pipefd[0], buf + len, cap - len)) > 0) {
        len += (size_t)n;
        if (len >= cap) {
            if (cap > SIZE_MAX / 2) { free(buf); close(pipefd[0]); waitpid(pid, NULL, 0); return NULL; }
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(pipefd[0]); waitpid(pid, NULL, 0); return NULL; }
            buf = nb;
        }
    }
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);

    /* Null-terminate */
    char *result = realloc(buf, len + 1);
    if (!result) result = buf;
    result[len] = '\0';

    if (out_len) *out_len = len;
    return result;
}

/* ── File discovery ────────────────────────────────────────────────── */

/* Skip list for directory names */
static int should_skip_dir(const char *name)
{
    if (name[0] == '.') return 1;
    if (strcmp(name, "node_modules") == 0) return 1;
    if (strcmp(name, "vendor") == 0) return 1;
    return 0;
}

/* Recursive helper for find_files */
static int find_files_recurse(const char *dir, const char *pattern,
                               char ***results, size_t *count, size_t *cap)
{
    DIR *d = opendir(dir);
    if (!d) return 0; /* skip unreadable dirs */

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (strcmp(ent->d_name, "node_modules") == 0) continue;
        if (strcmp(ent->d_name, "vendor") == 0) continue;

        /* Build full path */
        size_t dlen = strlen(dir);
        size_t nlen = strlen(ent->d_name);
        if (dlen + 1 + nlen + 1 > PATH_MAX) continue;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (lstat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            if (!should_skip_dir(ent->d_name))
                find_files_recurse(path, pattern, results, count, cap);
        } else if (S_ISREG(st.st_mode)) {
            if (fnmatch(pattern, ent->d_name, 0) == 0) {
                /* Add to results */
                if (*count >= *cap) {
                    if (*cap > SIZE_MAX / (2 * sizeof(char *))) { closedir(d); return -1; }
                    size_t newcap = *cap * 2;
                    char **nr = realloc(*results, newcap * sizeof(char *));
                    if (!nr) { closedir(d); return -1; }
                    *results = nr;
                    *cap = newcap;
                }
                (*results)[*count] = strdup(path);
                if ((*results)[*count])
                    (*count)++;
            }
        }
    }

    closedir(d);
    return 0;
}

/* String comparison for qsort */
static int str_compare(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

char **hl_tool_find_files(const char *dir, const char *pattern,
                          const HlToolUnveilCtx *ctx)
{
    if (!dir || !pattern) return NULL;

    /* Validate path against unveil if context provided */
    if (ctx && hl_tool_unveil_check(ctx, dir, 'r') != 0)
        return NULL;

    size_t cap = 64, count = 0;
    char **results = malloc(cap * sizeof(char *));
    if (!results) return NULL;

    find_files_recurse(dir, pattern, &results, &count, &cap);

    /* Sort alphabetically for deterministic ordering */
    if (count > 1)
        qsort(results, count, sizeof(char *), str_compare);

    /* NULL-terminate */
    char **final = realloc(results, (count + 1) * sizeof(char *));
    if (!final) final = results;
    final[count] = NULL;

    return final;
}

/* ── File copy ─────────────────────────────────────────────────────── */

int hl_tool_copy(const char *src, const char *dst,
                 const HlToolUnveilCtx *ctx)
{
    if (!src || !dst) return -1;

    if (ctx) {
        if (hl_tool_unveil_check(ctx, src, 'r') != 0) return -1;
        if (hl_tool_unveil_check(ctx, dst, 'w') != 0) return -1;
    }

    FILE *in = fopen(src, "rb");
    if (!in) return -1;

    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }

    char buf[8192];
    size_t n;
    int ok = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = -1; break; }
    }
    if (ferror(in)) ok = -1;

    fclose(in);
    fclose(out);
    return ok;
}

/* ── Recursive directory creation ──────────────────────────────────── */

int hl_tool_mkdir(const char *path, const HlToolUnveilCtx *ctx)
{
    if (!path) return -1;

    if (ctx && hl_tool_unveil_check(ctx, path, 'w') != 0)
        return -1;

    /* Walk the path creating each component */
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);

    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
        return -1;

    return 0;
}

/* ── Recursive directory removal ───────────────────────────────────── */

static int rmdir_recurse(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return -1;

    struct dirent *ent;
    int ret = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        size_t plen = strlen(path);
        size_t nlen = strlen(ent->d_name);
        if (plen + 1 + nlen + 1 > PATH_MAX) { ret = -1; continue; }

        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);

        struct stat st;
        if (lstat(child, &st) != 0) { ret = -1; continue; }

        if (S_ISDIR(st.st_mode)) {
            if (rmdir_recurse(child) != 0) ret = -1;
        } else {
            if (unlink(child) != 0) ret = -1;
        }
    }

    closedir(d);
    if (rmdir(path) != 0) ret = -1;
    return ret;
}

int hl_tool_rmdir(const char *path, const HlToolUnveilCtx *ctx)
{
    if (!path) return -1;

    if (ctx && hl_tool_unveil_check(ctx, path, 'w') != 0)
        return -1;

    return rmdir_recurse(path);
}

#ifdef HL_ENABLE_LUA

/* ── Lua helper: get unveil context from registry ──────────────────── */

static HlToolUnveilCtx *get_unveil_ctx(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, TOOL_UNVEIL_KEY);
    HlToolUnveilCtx *ctx = (HlToolUnveilCtx *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ctx;
}

/* ── tool.spawn(argv_table) → (bool, int) ─────────────────────────── */

static int l_tool_spawn(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    /* Count elements */
    int n = (int)luaL_len(L, 1);
    if (n <= 0) {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, -1);
        return 2;
    }

    /* Build argv array */
    const char **argv = malloc(((size_t)n + 1) * sizeof(const char *));
    if (!argv) {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, -1);
        return 2;
    }

    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        argv[i - 1] = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    argv[n] = NULL;

    int rc = hl_tool_spawn(argv);
    free(argv);

    if (rc == 0) {
        lua_pushboolean(L, 1);
        lua_pushinteger(L, 0);
    } else {
        lua_pushboolean(L, 0);
        lua_pushinteger(L, rc);
    }
    return 2;
}

/* ── tool.spawn_read(argv_table) → string | nil ──────────────────── */

static int l_tool_spawn_read(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    int n = (int)luaL_len(L, 1);
    if (n <= 0) {
        lua_pushnil(L);
        return 1;
    }

    const char **argv = malloc(((size_t)n + 1) * sizeof(const char *));
    if (!argv) {
        lua_pushnil(L);
        return 1;
    }

    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        argv[i - 1] = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    argv[n] = NULL;

    size_t out_len = 0;
    char *output = hl_tool_spawn_read(argv, &out_len);
    free(argv);

    if (output) {
        lua_pushlstring(L, output, out_len);
        free(output);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/* ── tool.find_files(dir, pattern) → table ────────────────────────── */

static int l_tool_find_files(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    const char *pattern = luaL_checkstring(L, 2);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    char **files = hl_tool_find_files(dir, pattern, ctx);
    if (!files) {
        lua_newtable(L);
        return 1;
    }

    lua_newtable(L);
    int idx = 1;
    for (char **p = files; *p; p++) {
        lua_pushstring(L, *p);
        lua_rawseti(L, -2, idx++);
        free(*p);
    }
    free(files);

    return 1;
}

/* ── tool.copy(src, dst) → bool ───────────────────────────────────── */

static int l_tool_copy(lua_State *L)
{
    const char *src = luaL_checkstring(L, 1);
    const char *dst = luaL_checkstring(L, 2);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    int rc = hl_tool_copy(src, dst, ctx);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.mkdir(path) → bool ──────────────────────────────────────── */

static int l_tool_mkdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    int rc = hl_tool_mkdir(path, ctx);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.rmdir(path) → bool ──────────────────────────────────────── */

static int l_tool_rmdir(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    int rc = hl_tool_rmdir(path, ctx);
    lua_pushboolean(L, rc == 0);
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
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    if (ctx && hl_tool_unveil_check(ctx, path, 'r') != 0) {
        lua_pushnil(L);
        return 1;
    }

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
    int read_err = ferror(f);
    fclose(f);
    if (read_err) {
        lua_pushnil(L);
        return 1;
    }
    luaL_pushresult(&b);
    return 1;
}

/* ── tool.write_file(path, data) ───────────────────────────────────── */

static int l_tool_write_file(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t len;
    const char *data = luaL_checklstring(L, 2, &len);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    if (ctx && hl_tool_unveil_check(ctx, path, 'w') != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

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
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    if (ctx && hl_tool_unveil_check(ctx, path, 'r') != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

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

/* ── tool.extract_platform(dir) → bool ─────────────────────────────── */

static int l_tool_extract_platform(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    int rc = hl_build_extract_platform(dir);
    lua_pushboolean(L, rc == 0);
    return 1;
}

/* ── tool.extract_platform_cosmo(dir) → bool ───────────────────────── */
/*
 * Extract both arch-specific archives and set up the .aarch64/ directory
 * layout that cosmocc expects:
 *   dir/libhull_platform.a          ← x86_64
 *   dir/.aarch64/libhull_platform.a ← aarch64
 */

static int l_tool_extract_platform_cosmo(lua_State *L)
{
    const char *dir = luaL_checkstring(L, 1);
    HlToolUnveilCtx *ctx = get_unveil_ctx(L);

    const HlEmbeddedPlatform *platforms = NULL;
    int count = hl_build_get_platforms(&platforms);
    if (count < 2 || !platforms) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Extract x86_64 as dir/libhull_platform.a */
    char path[1024];
    snprintf(path, sizeof(path), "%s/libhull_platform.a", dir);
    const HlEmbeddedPlatform *x86 = NULL;
    const HlEmbeddedPlatform *arm = NULL;

    for (int i = 0; i < count; i++) {
        if (strstr(platforms[i].arch, "x86_64"))
            x86 = &platforms[i];
        else if (strstr(platforms[i].arch, "aarch64"))
            arm = &platforms[i];
    }

    if (!x86 || !arm) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Write x86_64 archive */
    FILE *f = fopen(path, "wb");
    if (!f) { lua_pushboolean(L, 0); return 1; }
    size_t w = fwrite(x86->data, 1, x86->len, f);
    fclose(f);
    if (w != x86->len) { lua_pushboolean(L, 0); return 1; }

    /* Create .aarch64/ subdir */
    char aarch64_dir[1024];
    snprintf(aarch64_dir, sizeof(aarch64_dir), "%s/.aarch64", dir);
    if (hl_tool_mkdir(aarch64_dir, ctx) != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    /* Write aarch64 archive */
    snprintf(path, sizeof(path), "%s/.aarch64/libhull_platform.a", dir);
    f = fopen(path, "wb");
    if (!f) { lua_pushboolean(L, 0); return 1; }
    w = fwrite(arm->data, 1, arm->len, f);
    fclose(f);
    if (w != arm->len) { lua_pushboolean(L, 0); return 1; }

    lua_pushboolean(L, 1);
    return 1;
}

/* ── tool.platform_archs() → table | nil ───────────────────────────── */

static int l_tool_platform_archs(lua_State *L)
{
    const HlEmbeddedPlatform *platforms = NULL;
    int count = hl_build_get_platforms(&platforms);
    if (count == 0 || !platforms) {
        lua_pushnil(L);
        return 1;
    }

    lua_newtable(L);
    for (int i = 0; i < count; i++) {
        lua_pushstring(L, platforms[i].arch);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/* ── Registration ──────────────────────────────────────────────────── */

static const luaL_Reg tool_funcs[] = {
    { "spawn",                  l_tool_spawn },
    { "spawn_read",             l_tool_spawn_read },
    { "find_files",             l_tool_find_files },
    { "copy",                   l_tool_copy },
    { "mkdir",                  l_tool_mkdir },
    { "rmdir",                  l_tool_rmdir },
    { "tmpdir",                 l_tool_tmpdir },
    { "exit",                   l_tool_exit },
    { "read_file",              l_tool_read_file },
    { "write_file",             l_tool_write_file },
    { "file_exists",            l_tool_file_exists },
    { "stderr",                 l_tool_stderr },
    { "loadfile",               l_tool_loadfile },
    { "extract_platform",       l_tool_extract_platform },
    { "extract_platform_cosmo", l_tool_extract_platform_cosmo },
    { "platform_archs",         l_tool_platform_archs },
    { NULL, NULL }
};

void hl_cap_tool_register(lua_State *L, HlToolUnveilCtx *ctx)
{
    /* Store unveil context in registry */
    if (ctx)
        lua_pushlightuserdata(L, ctx);
    else
        lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, TOOL_UNVEIL_KEY);

    /* Register tool table */
    luaL_newlib(L, tool_funcs);
    lua_setglobal(L, "tool");
}

#endif /* HL_ENABLE_LUA */
