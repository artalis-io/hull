/*
 * shared/cli_log.c - logging policy for `hull <subcommand>` (CLI mode).
 *
 * See include/hull/shared/cli_log.h for the policy and its rationale.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/cli_log.h"

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int g_installed  = 0;
static int g_verbose    = 0;
static int g_app_phase  = 0;
/* Set once the server's logging policy takes over (see hl_cli_log_suspend).
 * Read on every record; written only from the single-threaded boot path
 * BEFORE hl_log_make_threadsafe + pool_create, so no worker can observe a
 * torn value. */
static int g_suspended  = 0;

int hl_cli_log_verbose(void)   { return g_verbose; }
int hl_cli_log_app_phase(void) { return g_app_phase; }
int hl_cli_log_suspended(void) { return g_suspended; }

void hl_cli_log_suspend(void) { g_suspended = 1; }

#ifdef HL_CLI_LOG_TEST_HOOKS
/* See the header: one-way in production, resettable only for tests. */
void hl_cli_log_reset_for_test(void) { g_suspended = 0; g_app_phase = 0; }
#endif

void hl_cli_log_set_app_phase(int on) { g_app_phase = on ? 1 : 0; }

int hl_cli_log_is_native_source(const char *file)
{
    if (!file || !*file) return 0;
    size_t n = strlen(file);
    /* Hull C call sites pass __FILE__ (".../foo.c" or ".../foo.h"). Script
     * call sites pass a Lua short_src ("./app.lua", "hull.build") or a JS
     * module name - never a C extension. */
    if (n >= 2 && file[n - 2] == '.' &&
        (file[n - 1] == 'c' || file[n - 1] == 'h'))
        return 1;
    return 0;
}

/* Lower-case level word for the terse default line ("warn", "error"). */
static const char *level_word(int level)
{
    const char *s = log_level_string(level);
    if (!s) return "log";
    if (strcmp(s, "TRACE") == 0) return "trace";
    if (strcmp(s, "DEBUG") == 0) return "debug";
    if (strcmp(s, "INFO")  == 0) return "info";
    if (strcmp(s, "WARN")  == 0) return "warn";
    if (strcmp(s, "ERROR") == 0) return "error";
    if (strcmp(s, "FATAL") == 0) return "fatal";
    return s;
}

/* `fmt` comes from log.c's callback API, not a literal; vsnprintf is doing
 * exactly what it is meant to. Same suppression as serve.c::hl_log_callback. */
static void cli_log_callback(log_Event *ev)
{
    FILE *f = (FILE *)ev->udata;

    /* Ownership handed to the server policy: go inert rather than emit a
     * second copy of every record in a different format. log.c cannot
     * unregister a callback, so this flag IS the removal. */
    if (g_suspended) return;

    int native = hl_cli_log_is_native_source(ev->file);

    if (!g_verbose) {
        /* An app's own logging during build-time manifest extraction is an
         * artifact of the extraction mechanism - not build output. */
        if (g_app_phase) return;
        /* Hull-internal chatter below WARN is implementation detail. */
        if (native && ev->level < LOG_WARN) return;
        /* Script records keep INFO; TRACE/DEBUG stay internal either way. */
        if (!native && ev->level < LOG_INFO) return;
    }

    char msg[1024];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
    vsnprintf(msg, sizeof(msg), ev->fmt, ev->ap);
#pragma GCC diagnostic pop

    if (g_verbose) {
        /* Full diagnostics: level, source coordinates, and an explicit marker
         * when the record came from user app code executed by a build tool. */
        fprintf(f, "%-5s %s:%d: %s%s\n",
                log_level_string(ev->level), ev->file, ev->line,
                g_app_phase ? "[build-eval] " : "", msg);
    } else {
        fprintf(f, "hull: %s: %s\n", level_word(ev->level), msg);
    }
}

void hl_cli_log_init(int verbose)
{
    g_verbose = verbose ? 1 : 0;

    if (g_installed) return;   /* callbacks cannot be removed; register once */
    g_installed = 1;

    /* Filtering happens inside the callback (it depends on the record's
     * source domain and the app-phase flag, which log.c cannot express), so
     * register at TRACE and let the callback decide. */
    log_set_quiet(true);       /* silence log.c's built-in file:line stderr */
    log_set_level(LOG_TRACE);
    log_add_callback(cli_log_callback, stderr, LOG_TRACE);
}
