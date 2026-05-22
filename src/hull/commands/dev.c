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
#include "hull/dev_state.h"
#include "hull/tool.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>

/* ── Agent sidecar file helpers ───────────────────────────────────── */

static void agent_ensure_dir(const char *app_dir)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.hull", app_dir);
    mkdir(path, 0755);
}

static void agent_write_dev_json(const char *app_dir, int port, pid_t pid)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.hull/dev.json", app_dir);

    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{\"port\":%d,\"pid\":%d,\"started_at\":%ld}\n",
            port, (int)pid, (long)time(NULL));
    fclose(f);
}

static void agent_remove_dev_json(const char *app_dir)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.hull/dev.json", app_dir);
    unlink(path);
}

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
    if (len > 5 && strcmp(name + len - 5, ".html") == 0) return 1;
    if (len > 5 && strcmp(name + len - 5, ".wgsl") == 0) return 1;
    if (len > 4 && strcmp(name + len - 4, ".sql") == 0) return 1;
    if (len > 5 && strcmp(name + len - 5, ".json") == 0) return 1;
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

/* ── Dev state singleton (used by `hull dev --tui`) ──────────────── */

static HlDevState g_dev_state;
static int        g_dev_state_inited = 0;

HlDevState *hl_dev_state(void) {
    return g_dev_state_inited ? &g_dev_state : NULL;
}

/* Append a complete log line to the ring buffer. `s`/`n` is one line
 * WITHOUT the trailing newline. Lines longer than the per-line
 * ceiling are truncated. */
static void dev_log_append(HlDevState *s, const char *line, size_t n)
{
    if (n >= HL_DEV_LOG_LINE_MAX) n = HL_DEV_LOG_LINE_MAX - 1;
    char *slot = s->log_lines[s->log_head];
    memcpy(slot, line, n);
    slot[n] = '\0';
    s->log_head = (s->log_head + 1) % HL_DEV_LOG_LINES;
    s->log_count++;
}

int hl_dev_state_drain(HlDevState *s)
{
    if (!s || s->pipe_fd < 0) return -1;
    int total = 0;
    for (;;) {
        struct pollfd pfd = { .fd = s->pipe_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 0);
        if (pr <= 0) break;

        char buf[1024];
        ssize_t n = read(s->pipe_fd, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) break;        /* EOF — child closed */
        total += (int)n;

        /* Walk the buffer, splitting on '\n'. */
        size_t start = 0;
        for (size_t i = 0; i < (size_t)n; i++) {
            if (buf[i] != '\n') continue;
            size_t llen = i - start;
            /* Concatenate accumulated partial + this segment. */
            if (s->partial_len > 0) {
                size_t need = s->partial_len + llen;
                if (need >= HL_DEV_LOG_LINE_MAX) need = HL_DEV_LOG_LINE_MAX - 1;
                /* Reuse partial buffer as a scratch — copy into a local
                 * buffer to keep things simple. */
                char merged[HL_DEV_LOG_LINE_MAX];
                size_t pcopy = s->partial_len;
                if (pcopy > sizeof merged) pcopy = sizeof merged;
                memcpy(merged, s->partial, pcopy);
                size_t rem = sizeof merged - pcopy;
                if (llen > rem) llen = rem;
                memcpy(merged + pcopy, buf + start, llen);
                dev_log_append(s, merged, pcopy + llen);
                s->partial_len = 0;
            } else {
                dev_log_append(s, buf + start, llen);
            }
            start = i + 1;
        }
        /* Anything past the last '\n' is a partial line — buffer it. */
        if (start < (size_t)n) {
            size_t plen = (size_t)n - start;
            size_t room = sizeof s->partial - s->partial_len;
            if (plen > room) plen = room;
            memcpy(s->partial + s->partial_len, buf + start, plen);
            s->partial_len += plen;
        }
    }
    return total;
}

int hl_dev_state_check_file_change(HlDevState *s)
{
    if (!s) return 0;
    time_t current = scan_mtime(s->app_dir);
    if (current > s->baseline_mtime) {
        s->baseline_mtime = current;
        return 1;
    }
    return 0;
}

/* monotonic_ms — bypass the async backend (the tool VM has none).
 * Wall-clock would work too; we use CLOCK_MONOTONIC for sanity. */
static int64_t dev_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

int hl_dev_state_reload(HlDevState *s)
{
    if (!s) return -1;
    if (s->child_pid > 0) {
        kill(s->child_pid, SIGTERM);
        waitpid(s->child_pid, NULL, 0);
        s->child_pid = 0;
    }
    /* Add a reload-marker line so the TUI shows where reloads happened. */
    char marker[64];
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int mlen = snprintf(marker, sizeof marker,
                        "── reload @ %02d:%02d:%02d ──",
                        lt.tm_hour, lt.tm_min, lt.tm_sec);
    if (mlen > 0) dev_log_append(s, marker, (size_t)mlen);

    /* Respawn. Child gets the pipe write end as fd 1+2. We keep the
     * read end open in the parent (re-used across reloads). */
    /* The pipe is preserved; only the child changes. */
    int pfd_for_child = -1;
    /* We need to recreate the write end. The original write end was
     * closed in the parent right after fork; here we recreate it by
     * dup()'ing the read fd's pair — but we don't have it stashed.
     * Easier: open a fresh pipe and replace pipe_fd. The cost is
     * losing whatever was in transit, which is fine right after kill. */
    int new_pipe[2];
    if (pipe(new_pipe) != 0) return -1;
    /* Make the read end non-blocking (drain uses poll, but defensive). */
    int flags = fcntl(new_pipe[0], F_GETFL, 0);
    if (flags >= 0) fcntl(new_pipe[0], F_SETFL, flags | O_NONBLOCK);

    /* Drain any final bytes from the old pipe, then close it. */
    if (s->pipe_fd >= 0) {
        hl_dev_state_drain(s);
        close(s->pipe_fd);
    }
    s->pipe_fd = new_pipe[0];
    pfd_for_child = new_pipe[1];

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd_for_child);
        return -1;
    }
    if (pid == 0) {
        /* Child: redirect stdout + stderr into the pipe, replace stdin
         * with /dev/null so the child can't read the controlling tty
         * (which the parent has put into raw mode for the TUI), then
         * exec the server binary. */
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(pfd_for_child, STDOUT_FILENO);
        dup2(pfd_for_child, STDERR_FILENO);
        close(pfd_for_child);
        close(s->pipe_fd);
        execvp(s->hull_exe, (char *const *)s->child_argv);
        _exit(127);
    }
    close(pfd_for_child);
    s->child_pid       = pid;
    s->reload_count   += 1;
    s->last_reload_ms  = dev_now_ms();
    s->baseline_mtime  = scan_mtime(s->app_dir);
    s->pending_reload  = 0;
    return 0;
}

/* ── TUI path ────────────────────────────────────────────────────── */

static int run_dev_tui(int argc, char **argv,
                       const char *hull_exe, const char **child_argv,
                       int child_argc, const char *app_dir)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr,
            "hull dev --tui: not attached to a terminal "
            "(stdin/stdout redirected). Run without --tui.\n");
        return 1;
    }

    /* Initialize singleton. */
    memset(&g_dev_state, 0, sizeof g_dev_state);
    g_dev_state.pipe_fd     = -1;
    g_dev_state.child_argv  = child_argv;
    g_dev_state.child_argc  = child_argc;
    g_dev_state.hull_exe    = hull_exe;
    strncpy(g_dev_state.app_dir, app_dir, sizeof g_dev_state.app_dir - 1);
    g_dev_state_inited = 1;

    /* First child spawn — uses the reload path so the pipe + bookkeeping
     * land in one place. */
    if (hl_dev_state_reload(&g_dev_state) != 0) {
        fprintf(stderr, "hull dev --tui: failed to spawn child\n");
        g_dev_state_inited = 0;
        return 1;
    }
    /* The reload marker is just noise for the first spawn; rewind. */
    g_dev_state.log_count = 0;
    g_dev_state.log_head  = 0;
    g_dev_state.reload_count = 0;

    int rc = hull_tool("hull.dev_tui", argc, argv, hull_exe);

    /* Cleanup: kill child if still alive. */
    if (g_dev_state.child_pid > 0) {
        kill(g_dev_state.child_pid, SIGTERM);
        waitpid(g_dev_state.child_pid, NULL, 0);
    }
    if (g_dev_state.pipe_fd >= 0) close(g_dev_state.pipe_fd);
    g_dev_state_inited = 0;
    return rc;
}

/* ── Main ─────────────────────────────────────────────────────────── */

int hl_cmd_dev(int argc, char **argv, const HlCommandEnv *env)
{
    const char *hull_exe = env->hull_exe;
    const char *entry_point = NULL;
    int agent_mode = 0;
    int tui_mode = 0;
    int port = 3000;

    /* Parse args: first positional is the entry point, rest are passthrough */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--agent") == 0) {
            agent_mode = 1;
            continue;
        }
        if (strcmp(argv[i], "--tui") == 0) {
            tui_mode = 1;
            continue;
        }
        if (argv[i][0] != '-') {
            entry_point = argv[i];
            break;
        }
        /* Pass through flags like -p, -b, -d etc. */
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "-b") == 0 ||
             strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "-m") == 0 ||
             strcmp(argv[i], "-M") == 0 || strcmp(argv[i], "-s") == 0 ||
             strcmp(argv[i], "-l") == 0 ||
             strcmp(argv[i], "--tls-cert") == 0 ||
             strcmp(argv[i], "--tls-key") == 0) && i + 1 < argc) {
            if (strcmp(argv[i], "-p") == 0) {
                char *end;
                long p = strtol(argv[i + 1], &end, 10);
                if (*end == '\0' && p > 0 && p <= 65535)
                    port = (int)p;
            }
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

    /* Pass through all original args except "dev" (argv[0]) and
     * dev-only flags. --tui is consumed by the parent (it triggers
     * the TUI render loop in this process); forwarding it to the
     * child would either get rejected by the server CLI parser or
     * be silently ignored, depending on the build. --agent is
     * already a server-mode flag, so pass that through. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tui") == 0) continue;
        child_argv[ci++] = argv[i];
    }

    child_argv[ci] = NULL;

#ifdef HL_ENABLE_TUI
    if (tui_mode) {
        int rc = run_dev_tui(argc, argv, hull_exe,
                              child_argv, ci, app_dir);
        free(child_argv);
        return rc;
    }
#else
    if (tui_mode) {
        fprintf(stderr, "hull dev --tui: this build was compiled without "
                        "HL_ENABLE_TUI.\n");
        free(child_argv);
        return 1;
    }
#endif

    /* Install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = dev_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "[hull:dev] watching %s for changes...\n", app_dir);

    if (agent_mode)
        agent_ensure_dir(app_dir);

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

        if (agent_mode)
            agent_write_dev_json(app_dir, port, pid);

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

    if (agent_mode)
        agent_remove_dev_json(app_dir);

    free(child_argv);
    return ret;
}
