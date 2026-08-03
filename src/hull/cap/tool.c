/*
 * cap/tool.c — Pure-C tool capability (controlled process + filesystem access)
 *
 * All process execution uses fork/execvp with an allowlisted set of
 * compiler binaries. All filesystem operations validate paths against
 * an unveil table. No shell invocation (system/popen) anywhere.
 *
 * The Lua bindings that expose this capability as the `tool` global live
 * in src/hull/runtime/lua/mod_tool.c (moved out as part of architectural
 * roadmap item F — restores the cap-layer "no runtime knowledge" invariant).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/tool.h"
#include "hull/cap/audit.h"
#include "hull/build_assets.h"
#include "hull/compiler.h"
#include "hull/utils/alloc.h"

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

    char *perms_dup = strdup(perms);
    if (!perms_dup) { free(dup); return -1; }

    ctx->entries[ctx->count].path = dup;
    ctx->entries[ctx->count].perms = perms_dup;
    ctx->count++;

    /* If resolved path differs (e.g. /tmp → /private/tmp on macOS),
     * also store the original path for prefix matching */
    if (resolved_differs && ctx->count < HL_TOOL_MAX_UNVEILED) {
        char *dup_orig = strdup(path);
        char *perms_dup2 = strdup(perms);
        if (dup_orig && perms_dup2) {
            ctx->entries[ctx->count].path = dup_orig;
            ctx->entries[ctx->count].perms = perms_dup2;
            ctx->count++;
        } else {
            free(dup_orig);
            free(perms_dup2);
        }
    }

    return 0;
}

void hl_tool_unveil_free(HlToolUnveilCtx *ctx)
{
    if (!ctx) return;
    for (int i = 0; i < ctx->count; i++) {
        hl_free_const(ctx->entries[i].path);
        hl_free_const(ctx->entries[i].perms);
    }
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
    "cc", "gcc", "clang", "cosmocc", "cosmoar", "ar", "wamrc", "hull",
    "ld",
    /* lld personalities spawned DIRECTLY (not via a cc driver) by
     * `hull build --linker=lld-static` (Tier B): `ld.lld` on ELF, `ld64.lld`
     * on Mach-O. The bare "ld" prefix above does not admit them - the match
     * requires the next char to be '\0' or a "-<digit>" version suffix, and
     * "ld.lld" has a '.'. docs/toolchain_free_build.md. */
    "ld.lld", "ld64.lld",
    /* zig: `hull build --linker=zig` drives `zig cc` as a toolchain-free +
     * cross-compiling link driver (docs/toolchain_free_build.md). */
    "zig",
    /* nm: read-only symbol lister. `hull build` probes the resolved platform lib
     * for hl_db_backend_sqlite to decide whether to auto-compose the SQLite
     * feature onto a SQLite-less base (docs/sqlite_feature.md, Phase C). */
    "nm",
    NULL
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
            if (next == '\0')
                return 0;
            if (next == '-' && base[plen + 1] >= '0' && base[plen + 1] <= '9')
                return 0;
        }
    }

    return -1;
}

/* ── Dangerous flag validation ─────────────────────────────────────── */

int hl_tool_validate_args(const char *const argv[])
{
    if (!argv) return -1;
    for (int i = 1; argv[i]; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-load") == 0)          return -1; /* Clang plugin */
        if (strcmp(a, "-fplugin") == 0)       return -1; /* GCC plugin */
        if (strncmp(a, "-fplugin=", 9) == 0)  return -1; /* GCC plugin= */
        if (strcmp(a, "-Xlinker") == 0)       return -1; /* linker pass */
        if (strncmp(a, "-Wl,", 4) == 0) {
            /* Only allow specific safe linker options after -Wl, */
            static const char *safe_linker_flags[] = {
                "--no-entry", "--export=", "--export-all", "--allow-undefined",
                "--initial-memory=", "--max-memory=", "--stack-first",
                "--import-memory", "--export-memory", "--shared-memory",
                /* Composable-feature whole-archive linking (`hull build
                 * --with=<feature>` for a whole_archive feature like tui). These
                 * only control WHICH members of an archive are pulled -- no
                 * plugin load, no codegen, no code execution (contrast the
                 * blocked -load / -Xlinker / @response). build.lua emits them
                 * solely for the trusted FEATURE_SPECS feature archive, whose
                 * path it controls; the build sandbox's unveil bounds the path. */
                "-force_load,",       /* macOS ld64: -Wl,-force_load,<archive> */
                "--whole-archive", "--no-whole-archive",  /* GNU ld / lld bracket */
                /* GNU ld archive-interdependency group, emitted by build.lua when
                 * composing a DB wire feature (postgres/mysql) whose archive
                 * references base symbols (tls_client) in the platform lib. Only
                 * affects archive resolution order -- no code execution. */
                "--start-group", "--end-group",
                /* Dead-code section stripping, emitted by the zig linker backend
                 * (linker_zig.c) per target format: --gc-sections on ELF,
                 * -dead_strip on Mach-O. Pure size optimization (drops unreferenced
                 * sections) -- no plugin, no codegen, no code execution. */
                "--gc-sections", "-dead_strip",
                NULL
            };
            const char *wl_arg = a + 4;
            int safe = 0;
            for (const char **sf = safe_linker_flags; *sf; sf++) {
                size_t sflen = strlen(*sf);
                if (strncmp(wl_arg, *sf, sflen) == 0) { safe = 1; break; }
            }
            if (!safe) return -1;
        }
        if (a[0] == '@')                       return -1; /* response file */
    }
    return 0;
}

/* ── Process spawning ──────────────────────────────────────────────── */

int hl_tool_spawn(const char *const argv[])
{
    return hl_tool_spawn_env(argv, NULL);
}

int hl_tool_spawn_env(const char *const argv[], const char *const envadd[])
{
    if (!argv || !argv[0]) return -1;
    if (hl_tool_check_allowlist(argv[0]) != 0 ||
        hl_tool_validate_args(argv) != 0) {
        ShJsonWriter w = hl_audit_begin("tool.spawn");
        sh_json_write_key(&w, "argv");
        sh_json_write_array_start(&w);
        for (int i = 0; argv[i]; i++)
            sh_json_write_string(&w, argv[i]);
        sh_json_write_array_end(&w);
        sh_json_write_kv_string(&w, "result", "denied");
        hl_audit_end(&w);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* Child: apply extra env (KEY=VALUE strings), then exec. putenv points
         * into envadd, which lives in the parent's memory the child shares
         * post-fork until execvp - fine for this immediate exec. */
        if (envadd)
            for (int i = 0; envadd[i]; i++)
                putenv((char *)(uintptr_t)envadd[i]);
        execvp(argv[0], (char *const *)(uintptr_t)argv);  /* POSIX execvp takes char*const[] but does not modify argv */
        _exit(127);
    }

    /* Parent: wait */
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;

    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    {
        ShJsonWriter w = hl_audit_begin("tool.spawn");
        sh_json_write_key(&w, "argv");
        sh_json_write_array_start(&w);
        for (int i = 0; argv[i]; i++)
            sh_json_write_string(&w, argv[i]);
        sh_json_write_array_end(&w);
        sh_json_write_kv_int(&w, "exit_code", exit_code);
        hl_audit_end(&w);
    }
    return exit_code;
}

char *hl_tool_spawn_read(const char *const argv[], size_t *out_len)
{
    if (!argv || !argv[0]) return NULL;
    if (hl_tool_check_allowlist(argv[0]) != 0 ||
        hl_tool_validate_args(argv) != 0) {
        ShJsonWriter w = hl_audit_begin("tool.spawn_read");
        sh_json_write_key(&w, "argv");
        sh_json_write_array_start(&w);
        for (int i = 0; argv[i]; i++)
            sh_json_write_string(&w, argv[i]);
        sh_json_write_array_end(&w);
        sh_json_write_kv_string(&w, "result", "denied");
        hl_audit_end(&w);
        return NULL;
    }

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
        execvp(argv[0], (char *const *)(uintptr_t)argv);  /* POSIX execvp takes char*const[] but does not modify argv */
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

    /* Return NULL if child failed (exec not found, non-zero exit) */
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(buf);
        return NULL;
    }

    /* Null-terminate */
    char *result = realloc(buf, len + 1);
    if (!result) result = buf;
    result[len] = '\0';

    if (out_len) *out_len = len;
    return result;
}

/* ── File discovery ────────────────────────────────────────────────── */

/* Skip list for directory names */
/* Whether a dir is unconditionally skipped during recursion.
 * `include_vendor` lets static-asset walks opt out of the
 * vendor-skip default (which is correct for source walks but
 * wrong for `static/vendor/`, where apps legitimately put
 * vendored CSS/JS that must be embedded). */
static int should_skip_dir(const char *name, int include_vendor)
{
    if (name[0] == '.') return 1;
    if (strcmp(name, "node_modules") == 0) return 1;
    if (!include_vendor && strcmp(name, "vendor") == 0) return 1;
    return 0;
}

/* Recursive helper for find_files */
static int find_files_recurse(const char *dir, const char *pattern,
                               char ***results, size_t *count, size_t *cap,
                               int include_vendor)
{
    DIR *d = opendir(dir);
    if (!d) return 0; /* skip unreadable dirs */

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (should_skip_dir(ent->d_name, include_vendor)) continue;

        /* Build full path */
        size_t dlen = strlen(dir);
        size_t nlen = strlen(ent->d_name);
        if (dlen + 1 + nlen + 1 > PATH_MAX) continue;

        char path[PATH_MAX];
        int pn = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (pn < 0 || (size_t)pn >= sizeof(path)) continue;

        struct stat st;
        if (lstat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            find_files_recurse(path, pattern, results, count, cap,
                               include_vendor);
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
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

char **hl_tool_find_files_ex(const char *dir, const char *pattern,
                             const HlToolUnveilCtx *ctx,
                             int include_vendor)
{
    if (!dir || !pattern) return NULL;

    /* Validate path against unveil if context provided */
    if (ctx && hl_tool_unveil_check(ctx, dir, 'r') != 0)
        return NULL;

    size_t cap = 64, count = 0;
    char **results = malloc(cap * sizeof(char *));
    if (!results) return NULL;

    find_files_recurse(dir, pattern, &results, &count, &cap, include_vendor);

    /* Sort alphabetically for deterministic ordering */
    if (count > 1)
        qsort(results, count, sizeof(char *), str_compare);

    /* NULL-terminate */
    char **final = realloc(results, (count + 1) * sizeof(char *));
    if (!final) final = results;
    final[count] = NULL;

    return final;
}

char **hl_tool_find_files(const char *dir, const char *pattern,
                          const HlToolUnveilCtx *ctx)
{
    /* Back-compat: default behavior skips vendor (correct for
     * source walks, where vendor/ is third-party noise). New
     * callers that need vendor included use hl_tool_find_files_ex
     * with include_vendor=1. */
    return hl_tool_find_files_ex(dir, pattern, ctx, 0);
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
        int cn = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (cn < 0 || (size_t)cn >= sizeof(child)) { ret = -1; continue; }

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

