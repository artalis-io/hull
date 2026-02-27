/*
 * commands/dev.c — hull dev: hot-reload development server
 *
 * Forks a child process running the hull server, monitors app files
 * for changes (by polling max mtime), and restarts on change.
 *
 * Pure C — no Lua VM needed.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/dev.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

/* ── Signal handling ──────────────────────────────────────────────── */

static volatile sig_atomic_t dev_child_pid = 0;
static volatile sig_atomic_t dev_got_signal = 0;

static void dev_signal_handler(int sig)
{
    dev_got_signal = sig;
    if (dev_child_pid > 0)
        kill(dev_child_pid, SIGTERM);
}

/* ── File mtime scanning ──────────────────────────────────────────── */

static int should_skip(const char *name)
{
    if (name[0] == '.') return 1;
    if (strcmp(name, "node_modules") == 0) return 1;
    if (strcmp(name, "vendor") == 0) return 1;
    if (strcmp(name, "build") == 0) return 1;
    return 0;
}

static int is_app_file(const char *name)
{
    size_t len = strlen(name);
    if (len > 4 && strcmp(name + len - 4, ".lua") == 0) return 1;
    if (len > 3 && strcmp(name + len - 3, ".js") == 0) return 1;
    return 0;
}

/*
 * Recursively scan a directory for .lua/.js files and return the
 * maximum mtime found. Returns 0 if no files found.
 */
static time_t scan_mtime(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;

    time_t max_mt = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (should_skip(ent->d_name)) continue;

        char path[PATH_MAX];
        size_t dlen = strlen(dir);
        size_t nlen = strlen(ent->d_name);
        if (dlen + 1 + nlen + 1 > PATH_MAX) continue;
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (lstat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            time_t sub = scan_mtime(path);
            if (sub > max_mt) max_mt = sub;
        } else if (S_ISREG(st.st_mode) && is_app_file(ent->d_name)) {
            if (st.st_mtime > max_mt) max_mt = st.st_mtime;
        }
    }

    closedir(d);
    return max_mt;
}

/*
 * Derive app directory from an entry point path.
 * "examples/hello/app.lua" → "examples/hello"
 * "app.lua" → "."
 */
static void derive_app_dir(const char *entry_point, char *out, size_t out_size)
{
    const char *slash = strrchr(entry_point, '/');
    if (slash) {
        size_t len = (size_t)(slash - entry_point);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, entry_point, len);
        out[len] = '\0';
    } else {
        snprintf(out, out_size, ".");
    }
}

/* ── Auto-detect entry point (same as main.c) ────────────────────── */

static const char *dev_detect_entry(void)
{
    if (access("app.js", F_OK) == 0) return "app.js";
    if (access("app.lua", F_OK) == 0) return "app.lua";
    return NULL;
}

/* ── Main ─────────────────────────────────────────────────────────── */

int hl_cmd_dev(int argc, char **argv, const char *hull_exe)
{
    const char *entry_point = NULL;
    int passthrough_start = -1;

    /* Parse args: first positional is the entry point, rest are passthrough */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            entry_point = argv[i];
            passthrough_start = i;
            break;
        }
        /* Pass through flags like -p, -b, -d etc. */
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "-b") == 0 ||
             strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "-m") == 0 ||
             strcmp(argv[i], "-M") == 0 || strcmp(argv[i], "-s") == 0 ||
             strcmp(argv[i], "-l") == 0) && i + 1 < argc) {
            i++; /* skip value, will be collected below */
        }
    }

    if (!entry_point)
        entry_point = dev_detect_entry();

    if (!entry_point) {
        fprintf(stderr, "hull dev: no entry point found (app.js or app.lua)\n");
        return 1;
    }

    /* Derive app directory for file watching */
    char app_dir[PATH_MAX];
    derive_app_dir(entry_point, app_dir, sizeof(app_dir));

    /* Build child argv: hull_exe, entry_point, passthrough flags */
    /* Max: hull_exe + all original args + NULL */
    const char **child_argv = malloc(((size_t)argc + 2) * sizeof(const char *));
    if (!child_argv) {
        fprintf(stderr, "hull dev: allocation failed\n");
        return 1;
    }

    int ci = 0;
    child_argv[ci++] = hull_exe;

    /* Pass through all original args except "dev" (argv[0]) */
    for (int i = 1; i < argc; i++)
        child_argv[ci++] = argv[i];

    child_argv[ci] = NULL;

    /* Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = dev_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "[hull:dev] watching %s for changes...\n", app_dir);

    int ret = 0;

    for (;;) {
        if (dev_got_signal) {
            ret = 128 + dev_got_signal;
            break;
        }

        /* Fork child */
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "[hull:dev] fork failed: %s\n", strerror(errno));
            ret = 1;
            break;
        }

        if (pid == 0) {
            /* Child: restore default signal handlers and exec */
            signal(SIGINT, SIG_DFL);
            signal(SIGTERM, SIG_DFL);
            execvp(hull_exe, (char *const *)child_argv);
            _exit(127);
        }

        dev_child_pid = pid;

        /* Record baseline mtime */
        time_t baseline = scan_mtime(app_dir);

        /* Poll for changes or child exit */
        int child_exited = 0;
        int change_detected = 0;

        while (!dev_got_signal && !child_exited && !change_detected) {
            /* Check child status (non-blocking) */
            int status;
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w > 0) {
                child_exited = 1;
                if (WIFEXITED(status)) {
                    int code = WEXITSTATUS(status);
                    if (code != 0) {
                        fprintf(stderr, "[hull:dev] server exited with code %d, "
                                "restarting in 1s...\n", code);
                    } else {
                        fprintf(stderr, "[hull:dev] server exited, "
                                "restarting in 1s...\n");
                    }
                }
                break;
            }

            /* Sleep 1 second between polls */
            struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            nanosleep(&ts, NULL);

            /* Check for file changes */
            time_t current = scan_mtime(app_dir);
            if (current > baseline) {
                change_detected = 1;
                fprintf(stderr, "[hull:dev] change detected, reloading...\n");
                kill(pid, SIGTERM);
                waitpid(pid, NULL, 0);
            }
        }

        dev_child_pid = 0;

        if (dev_got_signal)
            break;

        if (child_exited) {
            /* Back-off before restart */
            struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            nanosleep(&ts, NULL);
        }

        /* Loop back and restart */
    }

    free(child_argv);
    return ret;
}
