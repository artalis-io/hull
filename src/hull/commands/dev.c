/*
 * commands/dev.c - hull dev: hot-reload development server
 *
 * Forks a child process running the hull server, monitors app files
 * for changes (by polling max mtime), and restarts on change.
 *
 * Pure C - no Lua VM needed.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/crypto.h"   /* hl_cap_crypto_random */
#include "hull/commands/dev.h"
#include "hull/dev_state.h"
#include "hull/release_io.h"  /* hl_release_io_self_path */
#include "hull/shared/host.h" /* hl_host_find_in_path */
#include "hull/shared/dev_session.h" /* HL_DEV_SESSION_ENV, sidecar MAC (audit 36/37) */
#include "hull/shared/generation.h"
#include "hull/shared/app_entry.h"
#include "hull/shared/json_term_safe.h"  /* hl_json_write_terminal_safe */
#include "hull/shared/term_escape.h"  /* hl_eprintf_line */  /* the pipeline generation handed to the child */
#include "hull/shared/type_check.h"
#include "hull/cap/tool.h"   /* hl_tool_spawn */
#include "hull/tool.h"
#include "sh_json.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#ifdef __COSMOPOLITAN__
#include <spawn.h>
#endif
#include <stdint.h>
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
    int n = snprintf(path, sizeof(path), "%s/.hull", app_dir);
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    mkdir(path, 0755);
}

/* <app_dir>/.hull may be written to and unlinked in only when it is a real
 * directory (not a symlink) owned by this user. The app tree is a cloned
 * repo: a committed `.hull -> <dir>` symlink redirected the sidecar writes
 * and the dev.json / discovery.json unlinks into any directory. */
static int agent_dir_safe(const char *app_dir)
{
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/.hull", app_dir);
    if (n < 0 || (size_t)n >= sizeof(path)) return 0;
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
#ifndef _WIN32
    if (st.st_uid != geteuid()) return 0;
#endif
    return 1;
}

/* The session's nonce, and where it is kept outside the app tree:
 * $HOME/.hull/dev-sessions/<supervisor pid>. It is the key of the MAC every
 * sidecar of the session carries (include/hull/shared/dev_session.h) and is
 * written to no file in the app tree: `hull agent inspect` / `errors` /
 * `status` serve a sidecar only when its MAC verifies under the $HOME copy -
 * a repo that ships its own .hull/ files, or an app that writes them, cannot
 * compute one (audit 36 / 37). */
static char g_dev_nonce[65];

static int dev_session_path(char *out, size_t out_sz, int mkdirs)
{
    /* hl_host_home: a relative $HOME put the sessions under the working
     * directory (audit 20). */
    const char *home = hl_host_home();
    if (!home) return -1;
    char dir[PATH_MAX];
    int n = snprintf(dir, sizeof dir, "%s/.hull/dev-sessions", home);
    if (n < 0 || (size_t)n >= sizeof dir) return -1;
    if (mkdirs) {
        char hd[PATH_MAX];
        snprintf(hd, sizeof hd, "%s/.hull", home);
        (void)mkdir(hd, 0700);
        (void)mkdir(dir, 0700);
    }
    n = snprintf(out, out_sz, "%s/%d", dir, (int)getpid());
    return (n < 0 || (size_t)n >= out_sz) ? -1 : 0;
}

static void dev_session_remove(void)
{
    char p[PATH_MAX];
    if (g_dev_nonce[0] && dev_session_path(p, sizeof p, 0) == 0) unlink(p);
}

static void agent_remove_last_error(const char *app_dir)
{
    if (!agent_dir_safe(app_dir)) return;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/.hull/last_error.json", app_dir);
    if (n > 0 && (size_t)n < sizeof(path)) unlink(path);
}

/* Start the agent session: make its nonce, keep the copy under $HOME, and
 * hand it to every served child through HL_DEV_SESSION_ENV (the child stamps
 * it into .hull/last_error.json and unsets it before app code runs). Done
 * before the first spawn, so the first load failure is already stamped. A
 * last_error.json already in the tree is not this session's: removed (audit
 * 36). */
/* Remove the session records of supervisors that are gone (a crash or a
 * SIGKILL leaves its record behind). A record keys the MACs of its session's
 * sidecars; once its pid is reused by another process, the record would
 * still verify that dead session's (genuine, but stale) sidecars, so it is
 * pruned whenever a new session starts. hl_host_pid_alive, not kill(pid, 0),
 * which under Cosmopolitan on Windows terminates the process. */
static void dev_session_prune(void)
{
    const char *home = hl_host_home();
    if (!home) return;
    char dir[PATH_MAX];
    int n = snprintf(dir, sizeof dir, "%s/.hull/dev-sessions", home);
    if (n < 0 || (size_t)n >= sizeof dir) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *nm = de->d_name;
        if (nm[0] < '1' || nm[0] > '9') continue;
        char *end = NULL;
        errno = 0;
        long pid = strtol(nm, &end, 10);
        if (errno || !end || *end || pid <= 0 || pid > INT_MAX) continue;
        if (pid == (long)getpid()) continue;
        if (hl_host_pid_alive(pid) != 0) continue;   /* running, or unknown */
        (void)unlinkat(dirfd(d), nm, 0);
    }
    closedir(d);
}

static void agent_session_begin(const char *app_dir)
{
    agent_remove_last_error(app_dir);
    hl_dev_types_remove(app_dir);
    if (g_dev_nonce[0]) return;
    dev_session_prune();
    unsigned char r[32];
    if (hl_cap_crypto_random(r, sizeof r) != 0) return;
    static const char hx[] = "0123456789abcdef";
    char nonce[65];
    for (int i = 0; i < 32; i++) {
        nonce[2 * i]     = hx[r[i] >> 4];
        nonce[2 * i + 1] = hx[r[i] & 15];
    }
    nonce[64] = '\0';
    char sp[PATH_MAX];
    if (dev_session_path(sp, sizeof sp, 1) != 0) return;
    (void)unlink(sp);
    int fd = open(sp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    ssize_t w = write(fd, nonce, 64);
    close(fd);
    if (w != 64) { unlink(sp); return; }
    memcpy(g_dev_nonce, nonce, sizeof nonce);
    (void)setenv(HL_DEV_SESSION_ENV, g_dev_nonce, 1);
}

static void agent_write_dev_json(const char *app_dir, int port, pid_t pid)
{
    char path[PATH_MAX], tmp[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/.hull/dev.json", app_dir);
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    int m = snprintf(tmp, sizeof(tmp), "%s/.hull/dev.json.tmp", app_dir);
    if (m < 0 || (size_t)m >= sizeof(tmp)) return;
    if (!agent_dir_safe(app_dir)) return;

    /* A fresh file, never through a link: fopen(tmp, "w") followed a
     * committed .hull/dev.json.tmp -> ~/.ssh/authorized_keys, truncated the
     * target and wrote the session JSON into it (the rename then moved only
     * the link). unlink first, then O_EXCL|O_NOFOLLOW. */
    (void)unlink(tmp);
    int tfd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (tfd < 0) return;
    FILE *f = fdopen(tfd, "w");
    if (!f) { close(tfd); unlink(tmp); return; }
    /* session_pid = the dev SUPERVISOR pid (stable across reloads); pid = the served
     * child (changes each reload). The supervisor identity lets `hull agent inspect` bind
     * a published discovery generation to THIS live dev session.
     *
     * No secret here: the app reads this tree, and an app granted .hull/ read
     * the nonce dev.json used to carry and forged sidecars with it (audit
     * 37). The fields are public; the MAC, keyed by the nonce kept under
     * $HOME, says this supervisor wrote them for this app directory. */
    long started = (long)time(NULL);
    char content[128], mac[HL_DEV_SIDECAR_MAC_LEN + 1] = "";
    snprintf(content, sizeof content, "port=%d pid=%d started_at=%ld",
             port, (int)pid, started);
    if (g_dev_nonce[0])
        (void)hl_dev_sidecar_mac(g_dev_nonce, HL_DEV_SIDECAR_DEV_JSON, app_dir,
                                 (long)getpid(), content, strlen(content), mac);
    fprintf(f, "{\"port\":%d,\"pid\":%d,\"session_pid\":%d,\"started_at\":%ld,"
               "\"mac\":\"%s\"}\n",
            port, (int)pid, (int)getpid(), started, mac);
    fclose(f);
    if (rename(tmp, path) != 0) unlink(tmp);   /* atomic publish (tmp + rename) */
}

static void agent_remove_discovery(const char *app_dir)
{
    if (!agent_dir_safe(app_dir)) return;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/.hull/discovery.json", app_dir);
    if (n > 0 && (size_t)n < sizeof(path)) unlink(path);
}

/* Publish a fresh discovery generation for the current dev session by spawning
 * `hull agent inspect <app_dir> --generation=N --session-pid=<supervisor>` (the C
 * dispatcher routes the publish flags to the internal hull.project.publish module, which
 * writes the canonical <app_dir>/.hull/discovery.json atomically). Synchronous (analysis
 * is fast, reloads are human-paced); child output silenced. Agent-mode only.
 *
 * On ANY failure (fork / exec / non-zero exit) the previous same-session discovery sidecar
 * is REMOVED so `hull agent inspect` falls back to a standalone analysis rather than
 * serving a stale generation as current. */
#ifdef __COSMOPOLITAN__
/* Start a hull child with posix_spawn - never fork()+execv() - on cosmo. On
 * Windows cosmo's fork() is EMULATED (a CreateProcess plus a copy of this
 * process's memory), and that child can fault before it reaches exec; it then
 * sits in cosmo's crash handler holding whatever pipe it inherited. See
 * cosmo_spawn_start in cap/tool.c, which measured it. posix_spawn is a direct
 * CreateProcess.
 *
 * The child gets SIGINT/SIGTERM at their defaults (what each fork child here
 * set before exec) and, when @p mask is non-NULL, that signal mask. Each
 * DEV_IO_* names what becomes the child's stdin / stdout / stderr: inherit,
 * /dev/null, or @p pipe_fd; @p pipe_fd and @p close_fd (when >= 0) are then
 * closed in the child. Returns the pid, or -1 with errno set. */
enum { DEV_IO_INHERIT, DEV_IO_NULL, DEV_IO_PIPE };

static pid_t dev_spawn(const char *exe, const char *const argv[],
                       const sigset_t *mask, int in, int out, int err,
                       int pipe_fd, int close_fd)
{
    extern char **environ;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    int rc = posix_spawn_file_actions_init(&fa);
    if (rc != 0) { errno = rc; return -1; }
    rc = posix_spawnattr_init(&at);
    if (rc != 0) { posix_spawn_file_actions_destroy(&fa); errno = rc; return -1; }

    sigset_t def;
    sigemptyset(&def);
    sigaddset(&def, SIGINT);
    sigaddset(&def, SIGTERM);
    short flags = POSIX_SPAWN_SETSIGDEF;
    rc = posix_spawnattr_setsigdefault(&at, &def);
    if (rc == 0 && mask) {
        flags |= POSIX_SPAWN_SETSIGMASK;
        rc = posix_spawnattr_setsigmask(&at, mask);
    }
    if (rc == 0) rc = posix_spawnattr_setflags(&at, flags);

    const int which[3] = { in, out, err };
    for (int fd = 0; rc == 0 && fd < 3; fd++) {
        if (which[fd] == DEV_IO_NULL)
            rc = posix_spawn_file_actions_addopen(&fa, fd, "/dev/null",
                                                  fd == 0 ? O_RDONLY : O_WRONLY, 0);
        else if (which[fd] == DEV_IO_PIPE)
            rc = posix_spawn_file_actions_adddup2(&fa, pipe_fd, fd);
    }
    if (rc == 0 && pipe_fd > 2)
        rc = posix_spawn_file_actions_addclose(&fa, pipe_fd);
    if (rc == 0 && close_fd > 2)
        rc = posix_spawn_file_actions_addclose(&fa, close_fd);

    pid_t pid = -1;
    if (rc == 0)
        rc = posix_spawn(&pid, exe, &fa, &at,
                         (char *const *)(uintptr_t)argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) { errno = rc; return -1; }
    return pid;
}
#endif

static void agent_publish_discovery(const char *hull_exe, const char *app_dir, long generation)
{
    /* The publisher writes .hull/discovery.json: through a committed
     * `.hull -> <dir>` link, as every other sidecar write here refuses to
     * (audit 23). */
    if (!agent_dir_safe(app_dir)) return;
    char gen_arg[64], sp_arg[64];
    snprintf(gen_arg, sizeof(gen_arg), "--generation=%ld", generation);
    snprintf(sp_arg, sizeof(sp_arg), "--session-pid=%d", (int)getpid());

    const char *pargv[] = { hull_exe, "agent", "inspect",
                            app_dir, gen_arg, sp_arg, NULL };
#ifdef __COSMOPOLITAN__
    pid_t pid = dev_spawn(hull_exe, pargv, NULL, DEV_IO_INHERIT, DEV_IO_NULL,
                          DEV_IO_NULL, -1, -1);
    if (pid < 0) { agent_remove_discovery(app_dir); return; }
#else
    pid_t pid = fork();
    if (pid < 0) { agent_remove_discovery(app_dir); return; }
    if (pid == 0) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > 2) close(devnull);
        }
        execv(hull_exe, (char *const *)(uintptr_t)pargv);   /* POSIX execv does not modify argv */
        _exit(127);
    }
#endif
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        agent_remove_discovery(app_dir);
}

static void agent_remove_sidecars(const char *app_dir)
{
    dev_session_remove();
    agent_remove_last_error(app_dir);
    if (!agent_dir_safe(app_dir)) return;
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/.hull/dev.json", app_dir);
    if (n > 0 && (size_t)n < sizeof(path)) unlink(path);
    n = snprintf(path, sizeof(path), "%s/.hull/discovery.json", app_dir);
    if (n > 0 && (size_t)n < sizeof(path)) unlink(path);
    hl_dev_types_remove(app_dir);
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
    /* .mjs / .cjs and TypeScript sources are discovered (hull dev --agent publishes them), so an
     * edit reloads. */
    if (len > 4 && strcmp(name + len - 4, ".mjs") == 0) return 1;
    if (len > 4 && strcmp(name + len - 4, ".cjs") == 0) return 1;
    if (len > 3 && strcmp(name + len - 3, ".ts") == 0) return 1;
    if (len > 4 && strcmp(name + len - 4, ".mts") == 0) return 1;
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
        int pn = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (pn < 0 || (size_t)pn >= sizeof(path)) continue;

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

/* ── The pipeline generation ─────────────────────────────────────────
 *
 * With build plugins (a hull.build.json), every (re)spawn first runs the
 * build pipeline (docs/dev_pipeline_generations_design.md §4.1): a good run
 * becomes the generation the new child is handed, on a descriptor named by
 * HULL_GENERATION_FD; a failed one replaces nothing - a running child keeps
 * its last good generation, and with none running nothing starts until the
 * next change. The paths the plugins observed are watched too (§4.2). */

typedef struct {
    char  **paths;
    size_t  n;
} DevInputs;

static void dev_inputs_clear(DevInputs *in)
{
    for (size_t i = 0; i < in->n; i++) free(in->paths[i]);
    free(in->paths);
    in->paths = NULL;
    in->n = 0;
}

static void dev_inputs_add(const char *path, void *ud)
{
    DevInputs *in = ud;
    char **np = realloc(in->paths, (in->n + 1) * sizeof *np);
    if (!np) return;
    in->paths = np;
    if ((in->paths[in->n] = strdup(path)) != NULL) in->n++;
}

/* A signature of the observed inputs: changes when one is created, removed
 * or modified (mtime or size). 0 with none. */
static uint64_t dev_inputs_sig(const char *app_dir, const DevInputs *in)
{
    uint64_t sig = 0;
    for (size_t i = 0; i < in->n; i++) {
        char path[PATH_MAX];
        int n = in->paths[i][0]
              ? snprintf(path, sizeof path, "%s/%s", app_dir, in->paths[i])
              : snprintf(path, sizeof path, "%s", app_dir);
        if (n < 0 || (size_t)n >= sizeof path) continue;
        struct stat st;
        uint64_t v = 1;
        if (lstat(path, &st) == 0)
            v = ((uint64_t)st.st_mtime << 20) ^ (uint64_t)st.st_size ^
                ((uint64_t)st.st_mode << 40) ^ 2u;
        sig = sig * 1099511628211ull + v + i;
    }
    return sig;
}

/* The generation's bytes on a descriptor the child inherits (an unlinked
 * temporary file, so nothing is left in the tree or in $TMPDIR). -1 on
 * failure. */
/* Run the pipeline when the app has build plugins. 1 = a generation in
 * *gen_fd (and *inputs), 0 = no pipeline work, -1 = it failed. */
static int dev_run_pipeline(const char *app_dir, const char *entry, int agent_mode,
                            int *gen_fd, DevInputs *inputs)
{
    *gen_fd = -1;
    if (!hl_generation_has_pipeline(app_dir)) {
        dev_inputs_clear(inputs);
        return 0;
    }
    uint8_t *b = NULL;
    size_t len = 0;
    if (hl_generation_produce(app_dir, &b, &len) != 0) {
        hl_eprintf_line("[hull:dev] %s", hl_generation_error());
        if (agent_mode) {
            char msg[512];
            snprintf(msg, sizeof msg, "failed to load %s: the build pipeline "
                     "failed", entry);
            hl_dev_last_error_write(app_dir, g_dev_nonce, (long)getpid(), msg);
        }
        return -1;
    }
    DevInputs fresh = { 0 };
    if (hl_generation_inputs(b, len, dev_inputs_add, &fresh) != 0) {
        fprintf(stderr, "[hull:dev] the build pipeline wrote no generation\n");
        free(b);
        dev_inputs_clear(&fresh);
        return -1;
    }
    int fd = hl_generation_fd(b, len);
    free(b);
    if (fd < 0) {
        hl_eprintf_line("[hull:dev] cannot hand over the generation: %s",
                        strerror(errno));
        dev_inputs_clear(&fresh);
        return -1;
    }
    dev_inputs_clear(inputs);
    *inputs = fresh;
    *gen_fd = fd;
    return 1;
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

/* Set when the working directory's entry is ambiguous (already reported). */
static int g_dev_entry_ambiguous;

static const char *dev_detect_entry(void)
{
    /* A TypeScript entry, refused beside another (shared/app_entry.h). */
    static char ts_entry[64];
    char err[512];
    int ts = hl_app_entry_ts(NULL, ts_entry, sizeof ts_entry, err, sizeof err);
    if (ts < 0) {
        hl_eprintf_line("hull dev: %s", err);
        g_dev_entry_ambiguous = 1;
        return NULL;
    }
    if (ts > 0) return ts_entry + 2;   /* "./app.ts" -> "app.ts" */
    if (access("app.js", F_OK) == 0) return "app.js";
    if (access("app.lua", F_OK) == 0) return "app.lua";
    return NULL;
}

/* ── Dev state singleton (used by `hull dev --tui`) ──────────────── */
/* The whole dev-state machinery exists solely for `hull dev --tui`: the
 * singleton is only ever initialized by run_dev_tui, and its sole consumers are
 * the tool.dev_* bindings that back the dev_tui Lua tool. So the entire block is
 * TUI-only - the tool.dev_* bindings in tool_orchestration.c carry the matching
 * HL_TUI_LINKED gate. HL_TUI_LINKED is set when the hull binary carries TUI
 * symbols, i.e. a TUI-compiled base OR a TUI-free base that force-loads the
 * feature archive (the default native toolchain). On a fully TUI-less build it's
 * all excluded, avoiding both unused-function (-Werror) and the
 * "g_dev_state_inited always false" tautology (cppcheck). */
#ifdef HL_TUI_LINKED

#ifndef __COSMOPOLITAN__
static pid_t dev_types_runner_start(const char *checker, const char *app_dir,
                                    int *cmd_fd, int *out_fd);
#endif

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

/* A line of the type check's report: logged, and its summary kept for the
 * status line. */
static void dev_types_line(HlDevState *s, const char *line, size_t n)
{
    static const char pre[] = "[hull:dev] types: ";
    if (n > sizeof pre - 1 && memcmp(line, pre, sizeof pre - 1) == 0) {
        size_t k = n - (sizeof pre - 1);
        if (k >= sizeof s->types_status) k = sizeof s->types_status - 1;
        memcpy(s->types_status, line + sizeof pre - 1, k);
        s->types_status[k] = '\0';
    }
    dev_log_append(s, line, n);
}

/* Read what @p fd has into the ring, one line at a time; bytes after the
 * last newline wait in @p partial (capacity @p cap). Bytes read, or -1. */
static int dev_drain_fd(HlDevState *s, int fd, char *partial, size_t *plen,
                        size_t cap, int types)
{
    int total = 0;
    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 0);
        if (pr <= 0) break;

        char buf[1024];
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) break;        /* EOF - writer closed */
        total += (int)n;

        /* Walk the buffer, splitting on '\n'. */
        size_t start = 0;
        for (size_t i = 0; i < (size_t)n; i++) {
            if (buf[i] != '\n') continue;
            size_t llen = i - start;
            /* The accumulated partial + this segment, clamped to one line. */
            char merged[HL_DEV_LOG_LINE_MAX];
            size_t pcopy = *plen;
            if (pcopy > sizeof merged) pcopy = sizeof merged;
            memcpy(merged, partial, pcopy);
            size_t rem = sizeof merged - pcopy;
            if (llen > rem) llen = rem;
            memcpy(merged + pcopy, buf + start, llen);
            if (types) dev_types_line(s, merged, pcopy + llen);
            else       dev_log_append(s, merged, pcopy + llen);
            *plen = 0;
            start = i + 1;
        }
        /* Anything past the last '\n' is a partial line - buffer it. */
        if (start < (size_t)n) {
            size_t take = (size_t)n - start;
            size_t room = cap - *plen;
            if (take > room) take = room;
            memcpy(partial + *plen, buf + start, take);
            *plen += take;
        }
    }
    return total;
}

int hl_dev_state_drain(HlDevState *s)
{
    if (!s || s->pipe_fd < 0) return -1;
    int total = dev_drain_fd(s, s->pipe_fd, s->partial, &s->partial_len,
                             sizeof s->partial, 0);
    if (s->types_fd >= 0) {
        int t = dev_drain_fd(s, s->types_fd, s->types_partial,
                             &s->types_partial_len, sizeof s->types_partial, 1);
        if (t > 0 && total >= 0) total += t;
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

/* monotonic_ms - bypass the async backend (the tool VM has none).
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
     * dup()'ing the read fd's pair - but we don't have it stashed.
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

#ifdef __COSMOPOLITAN__
    pid_t pid = dev_spawn(s->hull_exe, (const char *const *)s->child_argv, NULL,
                          DEV_IO_NULL, DEV_IO_PIPE, DEV_IO_PIPE,
                          pfd_for_child, s->pipe_fd);
    if (pid < 0) {
        close(pfd_for_child);
        return -1;
    }
#else
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
        execv(s->hull_exe, (char *const *)(uintptr_t)s->child_argv);  /* POSIX execv does not modify argv */
        _exit(127);
    }
#endif
    close(pfd_for_child);
    s->child_pid       = pid;
    s->reload_count   += 1;
    s->last_reload_ms  = dev_now_ms();
    s->baseline_mtime  = scan_mtime(s->app_dir);
    s->pending_reload  = 0;
    if (s->types_cmd_fd >= 0) {
        /* A check of the code before this reload is stopped by the runner. */
        char c = 'S';
        if (write(s->types_cmd_fd, &c, 1) == 1)
            snprintf(s->types_status, sizeof s->types_status, "checking");
    }
    return 0;
}

/* ── TUI path ────────────────────────────────────────────────────── */

static int run_dev_tui(int argc, char **argv,
                       const char *hull_exe, const char **child_argv,
                       int child_argc, const char *app_dir, int no_types,
                       int app_types)
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
    g_dev_state.types_cmd_fd = -1;
    g_dev_state.types_fd    = -1;
    g_dev_state.child_argv  = child_argv;
    g_dev_state.child_argc  = child_argc;
    g_dev_state.hull_exe    = hull_exe;
    int adn = snprintf(g_dev_state.app_dir, sizeof g_dev_state.app_dir,
                       "%s", app_dir);
    if (adn < 0 || (size_t)adn >= sizeof g_dev_state.app_dir) {
        fprintf(stderr, "hull dev --tui: app_dir too long\n");
        return -1;
    }
    g_dev_state_inited = 1;

    /* The type check, as in the plain loop, but run by a process of its own
     * started here: the TUI runs in the tool VM, whose sandbox (applied by
     * hull_tool below) grants the app directory no execute, so the app's
     * node_modules/.bin compiler could not run from inside it. */
    char types_note[PATH_MAX + 96] = "";
    if (!no_types && hl_type_check_configured(app_dir)) {
#ifdef __COSMOPOLITAN__
        snprintf(types_note, sizeof types_note, "[hull:dev] types are not "
                 "checked by this build; run `hull check --types`");
#else
        char checker[PATH_MAX], local[PATH_MAX];
        if (hl_type_check_find(app_dir, app_types, checker, sizeof checker) != 0) {
            if (hl_type_check_app_local(app_dir, local, sizeof local) == 0)
                /* The repository's own code: run only when asked (audit 49). */
                snprintf(types_note, sizeof types_note, "[hull:dev] types are not "
                         "checked: the app's %s is run only with --types (it is the "
                         "repository's own code)", local);
            else
                snprintf(types_note, sizeof types_note, "[hull:dev] tsconfig.json "
                         "but no TypeScript compiler (tsgo or tsc) installed: types "
                         "are not checked");
        } else {
            g_dev_state.types_pid = dev_types_runner_start(
                checker, app_dir, &g_dev_state.types_cmd_fd, &g_dev_state.types_fd);
            if (g_dev_state.types_pid > 0)
                snprintf(types_note, sizeof types_note, "[hull:dev] type-checking "
                         "with %s after each reload (--no-types to turn off)", checker);
            else
                snprintf(types_note, sizeof types_note, "[hull:dev] could not "
                         "start the type check");
        }
#endif
    }

    /* First child spawn - uses the reload path so the pipe + bookkeeping
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
    if (types_note[0])   /* tui.print drops control characters */
        dev_log_append(&g_dev_state, types_note, strlen(types_note));

    int rc = hull_tool("hull.dev_tui", argc, argv, hull_exe);

    /* Cleanup: kill child if still alive. */
    if (g_dev_state.child_pid > 0) {
        kill(g_dev_state.child_pid, SIGTERM);
        waitpid(g_dev_state.child_pid, NULL, 0);
    }
    if (g_dev_state.pipe_fd >= 0) close(g_dev_state.pipe_fd);
    if (g_dev_state.types_pid > 0) {
        /* EOF on its command pipe: the runner stops a check and exits. */
        close(g_dev_state.types_cmd_fd);
        (void)waitpid(g_dev_state.types_pid, NULL, 0);
        close(g_dev_state.types_fd);
    }
    g_dev_state_inited = 0;
    return rc;
}

#endif /* HL_TUI_LINKED */

/* ── Main ─────────────────────────────────────────────────────────── */

/* The hull binary the dev supervisor re-runs, as a path (audit 17). argv[0]
 * is a bare "hull" when hull was found on PATH, and an exec*p() of a bare
 * name searches PATH again with an empty or "." entry meaning the working
 * directory - a cloned app shipping ./hull would run as the child. Take the
 * running binary's own path, else an argv[0] that already names a path, else
 * the PATH walker's hit (absolute entries only); the children are started
 * with execv / posix_spawn, which do not search. */
static int dev_self_exe(const char *argv0, char *out, size_t out_sz)
{
    if (hl_release_io_self_path(out, out_sz) == 0 && out[0])
        return 0;
    if (!argv0 || !*argv0) return -1;
    if (strchr(argv0, '/') || strchr(argv0, '\\')) {
        int n = snprintf(out, out_sz, "%s", argv0);
        return (n < 0 || (size_t)n >= out_sz) ? -1 : 0;
    }
    return hl_host_find_in_path(argv0, out, out_sz) == 1 ? 0 : -1;
}

/* ── Type check after each reload ─────────────────────────────────── */
/*
 * When the app has a tsconfig.json and a TypeScript compiler is installed
 * (shared/type_check.h), each successful (re)spawn starts one
 * `HL_TYPE_CHECK_ARGS --pretty false -p <app>` in the background; an edit before it
 * ends stops it. It never holds up or decides a reload: when it ends its
 * diagnostics are summarised on stderr and, under --agent, written to
 * .hull/types.json for `hull agent errors`. `--no-types` turns it off. The
 * compiler is tsgo or tsc on PATH; the app's own node_modules/.bin one is the
 * repository's code and runs only with `--types` (audit 49).
 */

#define DEV_TYPES_OUT_MAX   (1024 * 1024)   /* compiler output kept */
#define DEV_TYPES_SHOW      10              /* diagnostics printed per run */
#define DEV_TYPES_SIDECAR   100             /* diagnostics in types.json */

typedef struct {
    char   checker[PATH_MAX];   /* "" = off */
    pid_t  pid;                 /* the running check's process group, 0 = none */
    int    fd;                  /* its output, -1 = none */
    char  *out;
    size_t len;
    int    truncated;
} DevTypes;

static void dev_types_stop(DevTypes *t)
{
    if (t->pid > 0) {
        kill(-t->pid, SIGTERM);
        (void)waitpid(t->pid, NULL, 0);
        /* The compiler is the helper's child, in the same group: whatever
         * of it is left once the helper is reaped goes too. */
        kill(-t->pid, SIGKILL);
        t->pid = 0;
    }
    if (t->fd >= 0) { close(t->fd); t->fd = -1; }
    free(t->out);
    t->out = NULL;
    t->len = 0;
    t->truncated = 0;
}

static void dev_types_start(DevTypes *t, const char *app_dir)
{
    dev_types_stop(t);
    if (!t->checker[0]) return;
    int p[2];
    if (pipe(p) != 0) return;
    /* SIGINT / SIGTERM blocked across the fork, as for the served child: one
     * landing before the child reset its handlers ran the supervisor's in it
     * (which only records the signal), and the compiler it then spawned
     * outlived the stop. */
    sigset_t blk, prev;
    sigemptyset(&blk);
    sigaddset(&blk, SIGINT);
    sigaddset(&blk, SIGTERM);
    sigprocmask(SIG_BLOCK, &blk, &prev);
    pid_t pid = fork();
    if (pid < 0) {
        sigprocmask(SIG_SETMASK, &prev, NULL);
        close(p[0]);
        close(p[1]);
        return;
    }
    if (pid == 0) {
        /* Its own process group, so a stop reaches the compiler too; the
         * dev-session key is the supervisor's, never the compiler's (it may
         * be the app's own node_modules). */
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        sigprocmask(SIG_SETMASK, &prev, NULL);
        (void)unsetenv(HL_DEV_SESSION_ENV);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, STDIN_FILENO); close(nul); }
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        close(p[0]);
        close(p[1]);
        const char *argv[] = { t->checker, HL_TYPE_CHECK_ARGS, "--pretty", "false",
                               "-p", app_dir, NULL };
        int rc = hl_tool_spawn(argv);
        _exit(rc < 0 ? 127 : rc);
    }
    setpgid(pid, pid);
    sigprocmask(SIG_SETMASK, &prev, NULL);
    close(p[1]);
    (void)fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    t->pid = pid;
    t->fd = p[0];
}

/* Read what the check has written so far. */
static void dev_types_drain(DevTypes *t)
{
    if (t->fd < 0) return;
    char buf[8192];
    for (;;) {
        ssize_t r = read(t->fd, buf, sizeof buf);
        if (r <= 0) return;
        /* Past the cap the rest is read and dropped, so the compiler never
         * blocks on a full pipe. */
        size_t take = (size_t)r;
        if (take > DEV_TYPES_OUT_MAX - t->len) {
            take = DEV_TYPES_OUT_MAX - t->len;
            t->truncated = 1;
        }
        if (take == 0) continue;
        char *grown = realloc(t->out, t->len + take + 1);
        if (!grown) { t->truncated = 1; continue; }
        t->out = grown;
        memcpy(t->out + t->len, buf, take);
        t->len += take;
        t->out[t->len] = '\0';
    }
}

/* The check ended with @p status: report it. */
static void dev_types_report(DevTypes *t, int status, const char *app_dir,
                             int agent_mode)
{
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    ShJsonBuf jb;
    sh_json_buf_init(&jb);
    ShJsonWriter w;
    sh_json_writer_init(&w, sh_json_buf_write, &jb);
    sh_json_write_object_start(&w);
    sh_json_write_key(&w, "diagnostics");
    sh_json_write_array_start(&w);

    int count = 0, errors = 0, shown = 0;
    const char *s = t->out ? t->out : "";
    while (*s) {
        const char *nl = strchr(s, '\n');
        size_t n = nl ? (size_t)(nl - s) : strlen(s);
        HlTypeDiag d;
        /* A line that starts with a space continues the diagnostic before
         * it (related information); it is part of the printed output only. */
        if (n > 0 && s[0] != ' ' && hl_type_check_parse_line(s, n, &d)) {
            count++;
            if (d.parsed && strcmp(d.severity, "error") == 0) errors++;
            if (shown < DEV_TYPES_SHOW) {
                if (d.parsed)
                    hl_eprintf_line("[hull:dev]   %s:%ld:%ld %s %s", d.file, d.line,
                                    d.col, d.code, d.message);
                else
                    hl_eprintf_line("[hull:dev]   %s", d.message);
                shown++;
            }
            if (count <= DEV_TYPES_SIDECAR) {
                sh_json_write_object_start(&w);
                if (d.parsed) {
                    sh_json_write_kv_string(&w, "file", d.file);
                    sh_json_write_kv_int(&w, "line", d.line);
                    sh_json_write_kv_int(&w, "col", d.col);
                    sh_json_write_kv_string(&w, "severity", d.severity);
                    sh_json_write_kv_string(&w, "code", d.code);
                }
                sh_json_write_kv_string(&w, "message", d.message);
                sh_json_write_object_end(&w);
            }
        }
        s = nl ? nl + 1 : s + n;
    }
    sh_json_write_array_end(&w);

    const char *verdict = code == 0 ? "ok" : (count > 0 ? "errors" : "failed");
    if (code == 0)
        fprintf(stderr, "[hull:dev] types: no errors\n");
    else if (count > 0)
        hl_eprintf_line("[hull:dev] types: %d error%s%s", errors ? errors : count,
                        (errors ? errors : count) == 1 ? "" : "s",
                        count > shown ? " (first shown; `hull check --types` for all)" : "");
    else
        fprintf(stderr, "[hull:dev] types: the compiler failed (exit %d)\n", code);

    if (agent_mode) {
        sh_json_write_kv_string(&w, "status", verdict);
        sh_json_write_kv_int(&w, "count", count);
        sh_json_write_kv_int(&w, "errors", errors);
        sh_json_write_kv_bool(&w, "truncated", t->truncated || count > DEV_TYPES_SIDECAR);
        sh_json_write_kv_int(&w, "exit", code);
        sh_json_write_kv_int(&w, "timestamp", (int64_t)time(NULL));
        sh_json_write_kv_int(&w, "session_pid", (int64_t)getpid());
        sh_json_write_object_end(&w);
        /* DEL and C1 as \u00NN, as every agent output is written: the
         * compiler quotes source text, and `hull agent errors` refuses a
         * document that is not terminal-safe (audit 49). */
        ShJsonBuf safe;
        sh_json_buf_init(&safe);
        if (hl_json_write_terminal_safe(sh_json_buf_write, &safe, jb.buf, jb.len) == 0)
            hl_dev_types_write(app_dir, g_dev_nonce, (long)getpid(), safe.buf, safe.len);
        sh_json_buf_free(&safe);
    }
    sh_json_buf_free(&jb);
}

/* Call from the poll loop: drains the output and reports a check that
 * ended. */
static void dev_types_poll(DevTypes *t, const char *app_dir, int agent_mode)
{
    if (t->pid <= 0) return;
    dev_types_drain(t);
    int status;
    pid_t w = waitpid(t->pid, &status, WNOHANG);
    if (w != t->pid) return;
    t->pid = 0;
    dev_types_drain(t);
    dev_types_report(t, status, app_dir, agent_mode);
    dev_types_stop(t);
}

#if defined(HL_TUI_LINKED) && !defined(__COSMOPOLITAN__)
/* `hull dev --tui`'s type check: a process forked before the tool sandbox,
 * so the compiler is run as the plain loop runs it. Each byte 'S' on
 * @p cmd starts a check (stopping one still running); EOF stops it and
 * exits. The report the plain loop prints on stderr goes to the pipe whose
 * read end is *@p out_fd. The pid, or -1. */
static pid_t dev_types_runner_start(const char *checker, const char *app_dir,
                                    int *cmd_fd, int *out_fd)
{
    int cp[2], op[2];
    if (pipe(cp) != 0) return -1;
    if (pipe(op) != 0) { close(cp[0]); close(cp[1]); return -1; }
    pid_t pid = fork();
    if (pid < 0) {
        close(cp[0]); close(cp[1]); close(op[0]); close(op[1]);
        return -1;
    }
    if (pid == 0) {
        /* The TUI puts the terminal in raw mode; the runner never reads it,
         * and a SIGINT meant for the TUI must not leave a compiler behind. */
        signal(SIGINT, SIG_IGN);
        signal(SIGTERM, SIG_DFL);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) { dup2(nul, STDIN_FILENO); close(nul); }
        dup2(op[1], STDOUT_FILENO);
        dup2(op[1], STDERR_FILENO);
        close(op[0]);
        close(op[1]);
        close(cp[1]);
        DevTypes t;
        memset(&t, 0, sizeof t);
        t.fd = -1;
        snprintf(t.checker, sizeof t.checker, "%s", checker);
        for (;;) {
            struct pollfd pf = { .fd = cp[0], .events = POLLIN };
            int pr = poll(&pf, 1, 200);
            if (pr < 0 && errno != EINTR) break;
            if (pr > 0) {
                char c;
                ssize_t r = read(cp[0], &c, 1);
                if (r == 0 || (r < 0 && errno != EINTR)) break;
                if (r == 1 && c == 'S') dev_types_start(&t, app_dir);
            }
            dev_types_poll(&t, app_dir, 0);
        }
        dev_types_stop(&t);
        _exit(0);
    }
    close(cp[0]);
    close(op[1]);
    (void)fcntl(cp[1], F_SETFD, FD_CLOEXEC);
    (void)fcntl(op[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(op[0], F_SETFL, fcntl(op[0], F_GETFL) | O_NONBLOCK);
    *cmd_fd = cp[1];
    *out_fd = op[0];
    return pid;
}
#endif

int hl_cmd_dev(int argc, char **argv, const HlCommandEnv *env)
{
    static char hull_exe_buf[PATH_MAX];
    if (dev_self_exe(env->hull_exe, hull_exe_buf, sizeof(hull_exe_buf)) != 0) {
        fprintf(stderr, "hull dev: cannot find the hull binary to run (%s)\n",
                env->hull_exe ? env->hull_exe : "?");
        return 1;
    }
    const char *hull_exe = hull_exe_buf;
    const char *entry_point = NULL;
    int agent_mode = 0;
    int tui_mode = 0;
    int no_types = 0;
    int app_types = 0;     /* --types: the app's own node_modules compiler too */
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
        if (strcmp(argv[i], "--no-types") == 0) {
            no_types = 1;
            continue;
        }
        if (strcmp(argv[i], "--types") == 0) {
            app_types = 1;
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

    if (!entry_point && g_dev_entry_ambiguous)
        return 1;
    if (!entry_point) {
        fprintf(stderr, "hull dev: no entry point found (app.js, app.ts or app.lua)\n");
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
        if (strcmp(argv[i], "--tui") == 0 || strcmp(argv[i], "--no-types") == 0 ||
            strcmp(argv[i], "--types") == 0)
            continue;
        child_argv[ci++] = argv[i];
    }

    child_argv[ci] = NULL;

#ifdef HL_TUI_LINKED
    if (tui_mode) {
        int rc = run_dev_tui(argc, argv, hull_exe,
                              child_argv, ci, app_dir, no_types, app_types);
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

    /* The type check: on when the app has a tsconfig.json and a compiler is
     * installed (a cosmo build has no fork to run it in the background). */
    DevTypes types;
    memset(&types, 0, sizeof types);
    types.fd = -1;
    if (!no_types && hl_type_check_configured(app_dir)) {
#ifdef __COSMOPOLITAN__
        fprintf(stderr, "[hull:dev] types are not checked by this build; "
                "run `hull check --types`\n");
#else
        char local[PATH_MAX];
        if (hl_type_check_find(app_dir, app_types, types.checker, sizeof types.checker) == 0)
            hl_eprintf_line("[hull:dev] type-checking with %s after each reload "
                            "(--no-types to turn off)", types.checker);
        else if (hl_type_check_app_local(app_dir, local, sizeof local) == 0)
            /* The repository's own code: run only when asked (audit 49). */
            hl_eprintf_line("[hull:dev] types are not checked: the app's %s is run "
                            "only with --types (it is the repository's own code)", local);
        else
            fprintf(stderr, "[hull:dev] tsconfig.json but no TypeScript compiler "
                    "(tsgo or tsc) installed: types are not checked\n");
#endif
    }

    if (agent_mode) {
        agent_ensure_dir(app_dir);
        agent_session_begin(app_dir);
    }

    int ret = 0;
    long generation = 0;   /* incremented per (re)spawn; published to .hull/discovery.json */
    DevInputs inputs = { 0 };
    pid_t pid = 0;         /* the running child, 0 for none */
    int need_build = 1;    /* run the pipeline and (re)spawn */

    for (;;) {
        if (dev_got_signal) {
            ret = 128 + dev_got_signal;
            break;
        }

        if (need_build) {
            need_build = 0;
            dev_types_stop(&types);   /* a check of the code before this edit */
            int gen_fd = -1;
            int built = dev_run_pipeline(app_dir, entry_point, agent_mode,
                                         &gen_fd, &inputs);
            if (built < 0 && pid > 0) {
                fprintf(stderr, "[hull:dev] the app keeps running its last good "
                        "build; fix the error to reload\n");
            } else if (built < 0) {
                fprintf(stderr, "[hull:dev] nothing is running; waiting for a "
                        "change\n");
            } else {
                if (pid > 0) {
                    kill(pid, SIGTERM);
                    waitpid(pid, NULL, 0);
                    dev_child_pid = 0;
                }
                char fdv[16];
                if (gen_fd >= 0) {
                    snprintf(fdv, sizeof fdv, "%d", gen_fd);
                    (void)setenv(HL_GENERATION_ENV, fdv, 1);
                }

                /* Fork child, with SIGINT/SIGTERM blocked across the fork: a
                 * signal landing in the child before it reset its handlers ran
                 * the parent's (which only records it), so it was swallowed -
                 * the child exec'd anyway and the parent then sat in waitpid.
                 * Blocked, it stays pending: the child resets to SIG_DFL
                 * before unblocking (and dies of it), the parent unblocks once
                 * dev_child_pid is set (and forwards it). */
                sigset_t blk, prev;
                sigemptyset(&blk);
                sigaddset(&blk, SIGINT);
                sigaddset(&blk, SIGTERM);
                sigprocmask(SIG_BLOCK, &blk, &prev);
#ifdef __COSMOPOLITAN__
                pid = dev_spawn(hull_exe, (const char *const *)child_argv, &prev,
                                DEV_IO_INHERIT, DEV_IO_INHERIT, DEV_IO_INHERIT,
                                -1, -1);
#else
                pid = fork();
                if (pid == 0) {
                    /* Child: restore default signal handlers, unblock, and exec */
                    signal(SIGINT, SIG_DFL);
                    signal(SIGTERM, SIG_DFL);
                    sigprocmask(SIG_SETMASK, &prev, NULL);
                    execv(hull_exe, (char *const *)(uintptr_t)child_argv);  /* POSIX execv does not modify argv */
                    _exit(127);
                }
#endif
                (void)unsetenv(HL_GENERATION_ENV);
                if (gen_fd >= 0) close(gen_fd);   /* the child has its copy */
                if (pid < 0) {
                    sigprocmask(SIG_SETMASK, &prev, NULL);
                    fprintf(stderr, "[hull:dev] spawn failed: %s\n", strerror(errno));
                    pid = 0;
                    ret = 1;
                    break;
                }

                dev_child_pid = pid;
                sigprocmask(SIG_SETMASK, &prev, NULL);
                /* A signal between fork() and the line above found no child
                 * to forward to; the loop then broke out and left this one
                 * running, holding the port. Forward it now. */
                if (dev_got_signal) {
                    kill(pid, dev_got_signal);
                    (void)waitpid(pid, NULL, 0);
                    dev_child_pid = 0;
                    pid = 0;
                    ret = 128 + dev_got_signal;
                    break;
                }

                if (agent_mode) {
                    agent_write_dev_json(app_dir, port, pid);
                    /* Publish a new discovery generation for this (re)spawn.
                     * A full deterministic reanalysis each time (no
                     * incremental in this story). */
                    generation++;
                    agent_publish_discovery(hull_exe, app_dir, generation);
                }
                dev_types_start(&types, app_dir);
            }
        }

        /* Record baseline: the app's files and what the plugins observed */
        time_t baseline = scan_mtime(app_dir);
        uint64_t in_baseline = dev_inputs_sig(app_dir, &inputs);

        /* Poll for changes or child exit */
        while (!dev_got_signal && !need_build) {
            /* Check child status (non-blocking) */
            if (pid > 0) {
                int status;
                pid_t w = waitpid(pid, &status, WNOHANG);
                if (w > 0) {
                    dev_child_pid = 0;
                    pid = 0;
                    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                        fprintf(stderr, "[hull:dev] server exited with code %d, "
                                "restarting in 1s...\n", WEXITSTATUS(status));
                    } else {
                        fprintf(stderr, "[hull:dev] server exited, "
                                "restarting in 1s...\n");
                    }
                    /* Back-off before restart */
                    struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
                    nanosleep(&ts, NULL);
                    need_build = 1;
                    break;
                }
            }

            /* Sleep 1 second between polls */
            struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            nanosleep(&ts, NULL);
            dev_types_poll(&types, app_dir, agent_mode);

            /* Check for file changes */
            if (scan_mtime(app_dir) > baseline ||
                dev_inputs_sig(app_dir, &inputs) != in_baseline) {
                fprintf(stderr, "[hull:dev] change detected, reloading...\n");
                need_build = 1;
            }
        }

        if (dev_got_signal)
            break;
        /* Loop back: run the pipeline, then restart */
    }

    if (pid > 0) {
        kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);
        dev_child_pid = 0;
    }
    dev_types_stop(&types);
    dev_inputs_clear(&inputs);

    if (agent_mode)
        agent_remove_sidecars(app_dir);

    free(child_argv);
    return ret;
}
