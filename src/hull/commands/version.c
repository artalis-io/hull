/*
 * commands/version.c — hull version subcommand
 *
 *   hull version          — human-readable one-liner
 *   hull version --json   — machine-readable JSON object
 *
 * HL_VERSION is injected at compile time via -DHL_VERSION="...".
 * Falls back to "dev" if the define is absent (out-of-tree builds).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/version.h"
#include "sh_json.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int stdio_write_fn(void *ctx, const char *data, size_t len)
{
    FILE *fp = (FILE *)ctx;
    return fwrite(data, 1, len, fp) == len ? 0 : -1;
}

#ifndef HL_VERSION
#define HL_VERSION "dev"
#endif

static const char *hl_version_runtime(void)
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

static const char *hl_version_platform(void)
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

static const char *hl_version_build(void)
{
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_MEMORY__)
    return "asan";
#elif defined(DEBUG)
    return "debug";
#else
    return "release";
#endif
}

int hl_cmd_version(int argc, char **argv, const HlCommandEnv *env)
{
    (void)env;

    int json = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0)
            json = 1;
    }

    if (json) {
        /* Tiny output but structured emission removes a future
         * escape-vector if any field ever takes user input. */
        ShJsonWriter w;
        sh_json_writer_init(&w, stdio_write_fn, stdout);
        sh_json_write_object_start(&w);
        sh_json_write_kv_string(&w, "version",  HL_VERSION);
        sh_json_write_kv_string(&w, "runtime",  hl_version_runtime());
        sh_json_write_kv_string(&w, "platform", hl_version_platform());
        sh_json_write_kv_string(&w, "build",    hl_version_build());
        sh_json_write_object_end(&w);
        fputc('\n', stdout);
    } else {
        fprintf(stdout, "hull %s\n", HL_VERSION);
    }

    return 0;
}
