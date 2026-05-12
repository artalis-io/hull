/*
 * commands/doctor.c — hull doctor subcommand
 *
 * Checks the local environment and reports whether hull is ready to
 * build, develop, and deploy applications.
 *
 *   hull doctor          — human-readable output
 *   hull doctor --json   — machine-readable JSON
 *
 * Checks performed:
 *   1. Hull binary metadata (version, runtime, platform, build mode)
 *   2. Platform library embedding (none / single-arch / multi-arch)
 *   3. C compiler availability in PATH (cc, gcc, clang, cosmocc)
 *   4. Overall hull build readiness summary
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/doctor.h"
#include "hull/build_assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <limits.h>

#ifndef HL_VERSION
#define HL_VERSION "dev"
#endif

/* ── Runtime / platform / build labels (mirrors version.c) ──────── */

static const char *doctor_runtime(void)
{
#if defined(HL_ENABLE_JS) && defined(HL_ENABLE_LUA)
    return "lua+js";
#elif defined(HL_ENABLE_JS)
    return "js";
#elif defined(HL_ENABLE_LUA)
    return "lua";
#else
    return "none";
#endif
}

static const char *doctor_platform(void)
{
#if defined(__COSMOPOLITAN__)
    return "cosmo";
#elif defined(__APPLE__)
#  if defined(__aarch64__)
    return "darwin-arm64";
#  else
    return "darwin-x86_64";
#  endif
#elif defined(__linux__)
#  if defined(__aarch64__)
    return "linux-arm64";
#  else
    return "linux-x86_64";
#  endif
#else
    return "unknown";
#endif
}

static const char *doctor_build(void)
{
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_MEMORY__)
    return "asan";
#elif defined(DEBUG)
    return "debug";
#else
    return "release";
#endif
}

/* ── Compiler discovery ─────────────────────────────────────────── */

#define MAX_COMPILERS 4

typedef struct {
    const char *name;
    char        path[PATH_MAX];  /* empty string = not found */
} CompilerInfo;

/* Walk PATH and return the first directory containing `name`. */
static int find_in_path(const char *name, char *out, size_t out_sz)
{
    const char *path_env = getenv("PATH");
    if (!path_env || !name || !out || out_sz == 0)
        return 0;

    char buf[4096];
    size_t plen = strlen(path_env);
    if (plen >= sizeof(buf))
        return 0;
    memcpy(buf, path_env, plen + 1);

    char *dir = buf;
    while (dir && *dir) {
        char *colon = strchr(dir, ':');
        if (colon) *colon = '\0';

        char candidate[PATH_MAX];
        int n = snprintf(candidate, sizeof(candidate), "%s/%s", dir, name);
        if (n > 0 && (size_t)n < sizeof(candidate)) {
            if (access(candidate, X_OK) == 0) {
                snprintf(out, out_sz, "%s", candidate);
                return 1;
            }
        }

        dir = colon ? colon + 1 : NULL;
    }
    return 0;
}

static void discover_compilers(CompilerInfo *ci, int count)
{
    for (int i = 0; i < count; i++)
        ci[i].path[0] = '\0';

    for (int i = 0; i < count; i++)
        find_in_path(ci[i].name, ci[i].path, sizeof(ci[i].path));
}

/* ── Platform embedding detection ──────────────────────────────── */

typedef enum {
    PLATFORM_NONE,
    PLATFORM_SINGLE,
    PLATFORM_MULTI
} PlatformEmbed;

static PlatformEmbed detect_platform(void)
{
    if (hl_build_get_platforms(NULL) > 0)
        return PLATFORM_MULTI;

    const char *data = NULL;
    size_t len = 0;
    if (hl_build_get_template(&data, &len) == 0)
        return PLATFORM_SINGLE;

    return PLATFORM_NONE;
}

/* ── Human-readable output ──────────────────────────────────────── */

static void print_check(const char *label, int ok, const char *detail)
{
    if (ok)
        fprintf(stdout, "  %-12s \xe2\x9c\x93  %s\n", label, detail ? detail : "");
    else
        fprintf(stdout, "  %-12s \xe2\x9c\x97  %s\n", label, detail ? detail : "not found");
}

static void print_human(CompilerInfo *ci, int nci,
                        PlatformEmbed embed, int any_compiler)
{
    /* ── Binary info ── */
    fprintf(stdout, "hull %s  %s  %s  %s\n\n",
            HL_VERSION, doctor_runtime(), doctor_platform(), doctor_build());

    /* ── Platform library ── */
    fprintf(stdout, "Platform library\n");
    switch (embed) {
    case PLATFORM_MULTI:
        fprintf(stdout, "  embedded      multi-arch (x86_64 + aarch64)\n");
        break;
    case PLATFORM_SINGLE:
        fprintf(stdout, "  embedded      yes (single-arch)\n");
        break;
    case PLATFORM_NONE:
        fprintf(stdout, "  embedded      no\n");
        fprintf(stdout, "  hint          rebuild hull with EMBED_PLATFORM=1\n");
        break;
    }
    fprintf(stdout, "\n");

    /* ── Compilers ── */
    fprintf(stdout, "Compilers  (required for hull build)\n");
    for (int i = 0; i < nci; i++) {
        int found = ci[i].path[0] != '\0';
        print_check(ci[i].name, found, found ? ci[i].path : NULL);
    }
    fprintf(stdout, "\n");

    /* ── Summary ── */
    fprintf(stdout, "hull build    ");
    if (embed == PLATFORM_NONE) {
        fprintf(stdout, "not ready — platform library not embedded\n");
        fprintf(stdout, "              hint: make platform && make EMBED_PLATFORM=1\n");
    } else if (!any_compiler) {
        fprintf(stdout, "not ready — no C compiler found in PATH\n");
        fprintf(stdout, "              hint: install gcc, clang, or cosmocc\n");
    } else {
        fprintf(stdout, "ready\n");
    }
}

/* ── JSON output ────────────────────────────────────────────────── */

/* Minimal JSON escaping for string values. */
static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"')       fputs("\\\"", f);
        else if (*s == '\\') fputs("\\\\", f);
        else if (*s == '\n') fputs("\\n",  f);
        else                 fputc(*s, f);
    }
    fputc('"', f);
}

static void print_json(CompilerInfo *ci, int nci,
                       PlatformEmbed embed, int any_compiler)
{
    const char *embed_str =
        embed == PLATFORM_MULTI  ? "multi-arch" :
        embed == PLATFORM_SINGLE ? "single-arch" : "none";

    const char *ready_str =
        (embed == PLATFORM_NONE)  ? "no-platform" :
        (!any_compiler)           ? "no-compiler"  : "ready";

    fprintf(stdout, "{\n");
    fprintf(stdout, "  \"version\": \"%s\",\n", HL_VERSION);
    fprintf(stdout, "  \"runtime\": \"%s\",\n", doctor_runtime());
    fprintf(stdout, "  \"platform\": \"%s\",\n", doctor_platform());
    fprintf(stdout, "  \"build\": \"%s\",\n", doctor_build());
    fprintf(stdout, "  \"platform_embedded\": \"%s\",\n", embed_str);
    fprintf(stdout, "  \"compilers\": [");
    int first = 1;
    for (int i = 0; i < nci; i++) {
        if (!first) fprintf(stdout, ", ");
        first = 0;
        fprintf(stdout, "{\"name\": ");
        json_str(stdout, ci[i].name);
        fprintf(stdout, ", \"path\": ");
        if (ci[i].path[0])
            json_str(stdout, ci[i].path);
        else
            fprintf(stdout, "null");
        fprintf(stdout, "}");
    }
    fprintf(stdout, "],\n");
    fprintf(stdout, "  \"hull_build\": \"%s\"\n", ready_str);
    fprintf(stdout, "}\n");
}

/* ── Handler ─────────────────────────────────────────────────────── */

int hl_cmd_doctor(int argc, char **argv, const HlCommandEnv *env)
{
    (void)env;

    int json = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0)
            json = 1;
    }

    CompilerInfo ci[MAX_COMPILERS] = {
        { "cc",      {0} },
        { "gcc",     {0} },
        { "clang",   {0} },
        { "cosmocc", {0} },
    };
    discover_compilers(ci, MAX_COMPILERS);

    int any_compiler = 0;
    for (int i = 0; i < MAX_COMPILERS; i++) {
        if (ci[i].path[0]) { any_compiler = 1; break; }
    }

    PlatformEmbed embed = detect_platform();

    if (json)
        print_json(ci, MAX_COMPILERS, embed, any_compiler);
    else
        print_human(ci, MAX_COMPILERS, embed, any_compiler);

    /* Exit 1 if hull build cannot work, so scripts can check: hull doctor || ... */
    return (embed != PLATFORM_NONE && any_compiler) ? 0 : 1;
}
