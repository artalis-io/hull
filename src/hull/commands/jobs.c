/*
 * commands/jobs.c — hull jobs subcommand
 *
 * Dispatch:
 *   hull jobs worker [entry|dir] [-d DSN] [args...]
 *       Run an app as a dedicated job-worker process. The app's app.main
 *       drives jobs.run_worker (the blocking claim loop); run K copies for
 *       horizontal scale - each claims disjoint jobs via the atomic claim.
 *       This is a thin launcher over the normal app-run path (hull_serve):
 *       the entry is resolved (a file as-is, a directory -> app.lua|app.js),
 *       and every remaining argument (-d DSN, app args) is forwarded
 *       unchanged, so worker config lives in the app's run_worker(opts) call.
 *
 * Ops subcommands (dead / retry / cleanup) land with jobs Phase 5.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/jobs.h"
#include "hull/serve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Usage ─────────────────────────────────────────────────────────── */

static void jobs_usage(void)
{
    fprintf(stderr,
        "Usage: hull jobs <subcommand> [options]\n"
        "\n"
        "Subcommands:\n"
        "  worker [entry|dir] [-d DSN] [args...]\n"
        "        Run an app as a dedicated job-worker process. The app's\n"
        "        app.main should call jobs.run_worker(). Run K copies for\n"
        "        horizontal scale; each claims disjoint jobs. All arguments\n"
        "        after the entry are forwarded to the app unchanged.\n"
        "\n"
        "Examples:\n"
        "  hull jobs worker worker.lua -d ./app.db\n"
        "  hull jobs worker .            # resolve app.lua|app.js in this dir\n");
}

/* Resolve an entry point into `out` (size `outsz`).
 *   - a path to an existing regular file -> used as-is
 *   - a directory (or ".")               -> <dir>/app.lua, else <dir>/app.js
 * Returns 0 on success, -1 if nothing runnable was found. */
static int resolve_entry(const char *cand, char *out, size_t outsz)
{
    struct stat st;
    if (stat(cand, &st) == 0 && S_ISREG(st.st_mode)) {
        if ((size_t)snprintf(out, outsz, "%s", cand) >= outsz) return -1;
        return 0;
    }
    /* Treat as a directory (existing dir, or a bare name we probe under). */
    const char *names[] = { "app.lua", "app.js" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if ((size_t)snprintf(out, outsz, "%s/%s", cand, names[i]) >= outsz)
            return -1;
        if (access(out, F_OK) == 0) return 0;
    }
    return -1;
}

/* ── Entry point ───────────────────────────────────────────────────── */

int hl_cmd_jobs(int argc, char **argv, const HlCommandEnv *env)
{
    if (argc < 2) {
        jobs_usage();
        return 1;
    }

    if (strcmp(argv[1], "worker") != 0) {
        fprintf(stderr, "hull jobs: unknown subcommand '%s'\n\n", argv[1]);
        jobs_usage();
        return 1;
    }

    /* Everything after `worker`. The first non-flag token (if any) is the
     * entry candidate; the rest is forwarded verbatim. A leading flag (or no
     * token) means "resolve the entry from the app dir" and forward all. */
    int rest = 2;                       /* first arg after `worker` */
    const char *cand = (env && env->app_dir) ? env->app_dir : ".";
    int passthrough = rest;             /* index where forwarded args begin */
    if (rest < argc && argv[rest][0] != '-') {
        cand = argv[rest];
        passthrough = rest + 1;
    }

    char entry[4096];
    if (resolve_entry(cand, entry, sizeof(entry)) != 0) {
        fprintf(stderr,
            "hull jobs worker: no runnable app found at '%s' "
            "(expected a file, or app.lua|app.js in a directory)\n", cand);
        return 1;
    }

    /* Build argv for hull_serve: { hull_exe, entry, <forwarded args...> }.
     * hull_serve owns the full app lifecycle (load -> manifest -> sandbox ->
     * app.main), and app.main is where jobs.run_worker blocks. */
    int n_forward = argc - passthrough;
    int nargc = 2 + n_forward;
    char **nargv = calloc((size_t)nargc + 1, sizeof(char *));
    /* argv[0] is cosmetic (hull_serve runs in-process, no re-exec) but must be
     * a mutable char* like the rest; hull_exe is const, so copy it. */
    char *prog = strdup((env && env->hull_exe) ? env->hull_exe : "hull");
    if (!nargv || !prog) {
        fprintf(stderr, "hull jobs worker: out of memory\n");
        free(nargv);
        free(prog);
        return 1;
    }
    nargv[0] = prog;
    nargv[1] = entry;
    for (int i = 0; i < n_forward; i++)
        nargv[2 + i] = argv[passthrough + i];
    nargv[nargc] = NULL;

    int rc = hull_serve(nargc, nargv);
    free(nargv);
    free(prog);
    return rc;
}
