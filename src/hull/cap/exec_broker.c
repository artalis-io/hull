/*
 * cap/exec_broker.c - the command broker and the server's side of it
 * (docs/design/exec_broker.md §5 - §7; header: hull/cap/exec_broker.h)
 *
 * Frames (child_channel.h, magic "HXEC"):
 *
 *   server -> broker   POLICY  u32 flags, the encoded policy (cap/exec.h)
 *                      RUN     id; u16 name_len, name, u16 n,
 *                              (u32 len, bytes) * n, u32 stdin_len, stdin
 *                      KILL    id
 *   broker -> server   READY   text: one report line per '\n'
 *                      REFUSED id (0 = the policy); text
 *                      BUSY    id; text
 *                      DONE    id; u32 status, u32 signo, u8 timed_out,
 *                              u8 out_cut, u8 err_cut, u8 0, u32 duration_ms,
 *                              u32 out_len, out, u32 err_len, err
 *                      PROBE   id; u32 pid            (test builds only)
 *
 * The broker trusts nothing the server sends after READY: every RUN is
 * matched again, admission (in-flight calls, per-command concurrency, live
 * ids, buffered output) is decided here, and a second POLICY ends it.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* memmem (glibc); before any header. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "hull/cap/exec_broker.h"
#include "hull/cap/exec.h"
#include "hull/cap/exec_child.h"   /* the child manifest's grants */
#include "hull/cap/fs_policy.h"    /* hl_fs_grant_is_state */
#include "hull/shared/fs_util.h"   /* hl_dsn_file_path */
#ifdef HL_ENABLE_DB
#include "hull/cap/db_registry.h"  /* hl_db_registry_file_paths */
#endif
#include "hull/cap/crypto.h"   /* the sha256 pin */
#include "hull/shared/async.h"
#include "hull/shared/async_backend.h"
#include "hull/net_backend.h"
#include "hull/shared/child_channel.h"
#include "hull/shared/host.h"     /* hl_host_is_windows */
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HX_MAGIC   0x43455848u   /* "HXEC" little-endian */
/* The POLICY frame's head before the policy: u32 flags, two roots and up to
 * HL_EXEC_MAX_PROTECT database paths (u16 length + bytes each), u16 count.
 * The sender sizes its buffer and the broker its read limit from this one
 * value (audit 53: the read limit had stayed at the policy + 4). */
#define HX_POLICY_HEAD_MAX (4 + 2 + (2 + HL_EXEC_MAX_PROTECT) * (2 + PATH_MAX))
#define HX_POLICY  1u
#define HX_READY   2u
#define HX_RUN     3u
#define HX_KILL    4u
#define HX_DONE    5u
#define HX_BUSY    6u
#define HX_REFUSED 7u
#define HX_PROBE   8u

#define HX_RUN_MAX    (HL_EXEC_MAX_ARG_BYTES + 4u * HL_EXEC_MAX_CALL_ARGS + \
                       HL_EXEC_STDIN_CEIL + 128u)
#define HX_REPLY_MAX  (HL_EXEC_STDOUT_CEIL + HL_EXEC_STDERR_CEIL + 64u)

/* Log a report, one record per line. */
static void log_report(const char *report, int as_error)
{
    const char *p = report;
    while (*p) {
        const char *e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p);
        if (n > 0) {
            if (as_error) log_error("[hull:c] %.*s", n, p);
            else if (strncmp(p, "exec: degraded", 14) == 0 ||
                     strncmp(p, "exec: limit", 11) == 0) log_warn("[hull:c] %.*s", n, p);
            else log_info("[hull:c] %.*s", n, p);
        }
        p += n + (e ? 1 : 0);
    }
}

/* Does any command have an app-relative (not "@state/") manifest.fs grant? */
static int has_app_relative_grant(const uint8_t *policy, size_t len)
{
    int n = hl_exec_policy_count(policy, len);
    for (int i = 0; i < n; i++) {
        HlExecCmd c;
        if (hl_exec_policy_at(policy, len, i, &c) != 0) return 1;
        const uint8_t *cur = c.app_read;
        for (unsigned k = 0; k < c.n_app_read; k++)
            if (!hl_fs_grant_is_state(hl_exec_next_str(&cur, NULL))) return 1;
        cur = c.app_write;
        for (unsigned k = 0; k < c.n_app_write; k++)
            if (!hl_fs_grant_is_state(hl_exec_next_str(&cur, NULL))) return 1;
    }
    return 0;
}

/* Does any command have a manifest.fs write grant? */
static int has_app_write_grant(const uint8_t *policy, size_t len)
{
    int n = hl_exec_policy_count(policy, len);
    for (int i = 0; i < n; i++) {
        HlExecCmd c;
        if (hl_exec_policy_at(policy, len, i, &c) != 0 || c.n_app_write > 0) return 1;
    }
    return 0;
}

int hl_exec_broker_apply(const uint8_t *policy, size_t len, int allow_degraded,
                         int no_sandbox, const char *app_dir, const char *state_dir,
                         HlDbRegistry *dbreg, const char *default_db)
{
    if (!policy || hl_exec_policy_count(policy, len) <= 0) {
        hl_exec_broker_stop();          /* no commands: nothing to run */
        return 0;
    }
    if (hl_host_is_windows()) {
        /* CreateProcess re-splits one command line: a value that matched as
         * one argument can become several (§9). No flag changes that. */
        log_error("[hull:c] exec: declared commands cannot run on Windows (its "
                  "processes re-split their command line); refusing to start");
        hl_exec_broker_stop();
        return -1;
    }
    /* "/" or the home is not an app directory (a built tool run from
     * there): a relative child grant would name the user's own files. Judged
     * here, with this process's $HOME (the broker has only LANG and TZ), and
     * before the broker creates any directory. */
    char rapp[PATH_MAX];
    if (has_app_relative_grant(policy, len)) {
        if (!app_dir || !realpath(app_dir, rapp)) {
            log_error("[hull:c] exec: a command's manifest.fs names a path under the "
                      "app directory, which cannot be resolved (%s); refusing to start",
                      app_dir ? strerror(errno) : "none");
            hl_exec_broker_stop();
            return -1;
        }
        if (strcmp(rapp, "/") == 0 || hl_host_path_too_broad(rapp)) {
            log_error("[hull:c] exec: a command's manifest.fs names a path under the "
                      "app directory, but the app directory is %s (/ or the home); "
                      "use \"@state/\" for a tool run from anywhere; refusing to "
                      "start", rapp);
            hl_exec_broker_stop();
            return -1;
        }
    }
    /* The app's SQLite files: no command may write them (the -shm truncated
     * under the app is a SIGBUS). */
    /* Every database must be protected before the first command runs; one
     * that cannot be (unresolvable, or past the limit) refuses the start. */
    static char dbs[HL_EXEC_MAX_PROTECT][PATH_MAX];
    int n_dbs = 0;
    int writes = has_app_write_grant(policy, len);
#ifdef HL_ENABLE_DB
    if (dbreg && writes) n_dbs = hl_db_registry_file_paths(dbreg, dbs, HL_EXEC_MAX_PROTECT);
#else
    (void)dbreg;
#endif
    /* n_dbs is 0 here on a build without HL_ENABLE_DB. */
    // cppcheck-suppress knownConditionTrueFalse
    if (writes && n_dbs >= 0 && default_db && default_db[0]) {
        const char *f = hl_dsn_file_path(default_db);
        if (f && f[0] && strcmp(f, ":memory:") != 0) {
            char real[PATH_MAX];
            if (hl_path_resolve_partial(f, real) != 0) {
                n_dbs = -1;
            } else {
                int dup = 0;
                // cppcheck-suppress knownConditionTrueFalse
                for (int k = 0; k < n_dbs && k < HL_EXEC_MAX_PROTECT; k++)
                    dup |= strcmp(dbs[k], real) == 0;
                if (!dup) {
                    if (n_dbs < HL_EXEC_MAX_PROTECT) snprintf(dbs[n_dbs], PATH_MAX, "%s", real);
                    n_dbs++;
                }
            }
        }
    }
    if (n_dbs < 0) {
        log_error("[hull:c] exec: a database's path cannot be resolved, so the "
                  "commands' write grants cannot be kept off it; refusing to start");
        hl_exec_broker_stop();
        return -1;
    }
    if (n_dbs > HL_EXEC_MAX_PROTECT) {
        log_error("[hull:c] exec: the app has %d database files; the command broker "
                  "protects at most %d from the commands' write grants; refusing to "
                  "start", n_dbs, HL_EXEC_MAX_PROTECT);
        hl_exec_broker_stop();
        return -1;
    }
    /* Constant only in a build with no broker (the stubs below). */
    // cppcheck-suppress knownConditionTrueFalse
    if (!hl_exec_broker_spawned()) {
        if (allow_degraded) {
            log_warn("[hull:c] exec: no command broker on this host "
                     "(--allow-degraded-sandbox): exec.run fails");
            return 0;
        }
        log_error("[hull:c] exec: this host has no command broker, so the declared "
                  "commands cannot be confined; refusing to start (pass "
                  "--allow-degraded-sandbox to start with exec.run failing)");
        return -1;
    }
    unsigned flags = (allow_degraded ? HL_EXEC_POLICY_DEGRADED_OK : 0) |
                     (no_sandbox ? HL_EXEC_POLICY_NO_SANDBOX : 0);
    char report[4096];
    int rc = hl_exec_broker_policy(policy, len, flags, app_dir, state_dir,
                                   (const char (*)[PATH_MAX])dbs, n_dbs,
                                   report, sizeof report);
    // cppcheck-suppress knownConditionTrueFalse
    log_report(report, rc != 0);
    // cppcheck-suppress knownConditionTrueFalse
    if (rc != 0) log_error("[hull:c] exec: refusing to start");
    return rc;
}

void hl_exec_reply_free(HlExecReply *r)
{
    if (!r) return;
    free(r->out);
    free(r->err);
    free(r);
}

#ifdef HL_EXEC_BROKER_SUPPORTED

#include "hull/cap/exec_platform.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0      /* macOS: the broker ignores SIGPIPE */
#endif

/* ═════════════════════════════════════════════════════════════════════
 * The broker process
 * ═════════════════════════════════════════════════════════════════════ */

/* The host's part (Landlock + seccomp, Seatbelt, ...) is behind
 * hull/cap/exec_platform.h: cap/exec_broker_linux.c, cap/exec_broker_macos.c. */

/* ── The report ────────────────────────────────────────────────────── */

void hl_exec_report_line(HlExecReport *r, const char *fmt, ...)
{
    if (r->at >= r->size) return;
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (w < 0) return;
    int k = snprintf(r->buf + r->at, r->size - r->at, "%s\n", line);
    if (k > 0 && (size_t)k < r->size - r->at) r->at += (size_t)k;
    else r->buf[r->at] = '\0';           /* a line that does not fit is dropped */
}

void hl_exec_report_set(HlExecReport *r, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(r->buf, r->size, fmt, ap);
    va_end(ap);
    r->at = strlen(r->buf);
}

/* ── Commands ──────────────────────────────────────────────────────── */

typedef HlExecBrokerCmd Cmd;

static uint8_t *g_policy;
static size_t   g_policy_len;
static Cmd      g_cmds[HL_EXEC_MAX_COMMANDS];
static int      g_ncmds;
static unsigned g_flags;
static char     g_app_dir[PATH_MAX];     /* the roots of the child manifests */
static char     g_state_dir[PATH_MAX];   /* "" = the app declares no state */
static char     g_dbs[HL_EXEC_MAX_PROTECT][PATH_MAX];   /* the app's databases */
static int      g_n_dbs;
static int      g_devnull = -1;

/* Is @p st owned and only writable by root (or, in a test build, by this
 * user)? */
static int owner_ok(const struct stat *st)
{
    if (st->st_mode & (S_IWGRP | S_IWOTH)) return 0;
    if (st->st_uid == 0) return 1;
#ifdef HL_EXEC_TEST_HOOKS
    if (st->st_uid == geteuid()) return 1;
#endif
    return 0;
}

/* Every directory of @p real (an absolute, resolved path) passes owner_ok:
 * only root can replace the file or anything above it (sshd's safe_path). */
static int path_dirs_ok(const char *real, char *why, size_t why_size)
{
    char buf[PATH_MAX];
    size_t n = strlen(real);
    if (n >= sizeof buf) return -1;
    memcpy(buf, real, n + 1);
    for (char *s = strrchr(buf, '/'); s; s = strrchr(buf, '/')) {
        if (s == buf) { buf[1] = '\0'; }
        else *s = '\0';
        struct stat st;
        if (stat(buf, &st) != 0 || !S_ISDIR(st.st_mode) || !owner_ok(&st)) {
            snprintf(why, why_size, "%s is not a directory only root can write", buf);
            return -1;
        }
        if (s == buf) break;
    }
    return 0;
}

/* The leaf checks, on an open descriptor. */
static int leaf_ok(int fd, const char *real, struct stat *st, char *why,
                   size_t why_size)
{
    if (fstat(fd, st) != 0 || !S_ISREG(st->st_mode)) {
        snprintf(why, why_size, "%s is not a regular file", real);
        return -1;
    }
    if (!owner_ok(st)) {
        snprintf(why, why_size, "%s is not owned and only writable by root", real);
        return -1;
    }
    if (st->st_mode & (S_ISUID | S_ISGID)) {
        snprintf(why, why_size, "%s is set-user-ID or set-group-ID", real);
        return -1;
    }
    if (!(st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
        snprintf(why, why_size, "%s is not executable", real);
        return -1;
    }
    return hl_exec_plat_leaf_check(fd, real, why, why_size);
}

/* The sha256 pin (§6.1): the bytes of the main executable, read from the
 * very descriptor that will be exec'd. A file over HL_EXEC_PIN_MAX_BYTES
 * is refused rather than hashed for ever. 0 / -1 (why says). */
static int pin_check(const Cmd *cmd, int fd, char *why, size_t why_size)
{
    if (!cmd->c.sha256) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 0 ||
        (uint64_t)st.st_size > HL_EXEC_PIN_MAX_BYTES) {
        snprintf(why, why_size, "%s is too large to check its sha256 pin "
                 "(over %u MiB)", cmd->real, (unsigned)(HL_EXEC_PIN_MAX_BYTES >> 20));
        return -1;
    }
    static uint8_t buf[64 * 1024];
    HlSha256Ctx h;
    hl_cap_crypto_sha256_init(&h);
    off_t off = 0;
    for (;;) {
        ssize_t n = pread(fd, buf, sizeof buf, off);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            snprintf(why, why_size, "%s: reading it for its sha256 pin: %s",
                     cmd->real, strerror(errno));
            return -1;
        }
        if (n == 0) break;
        (void)hl_cap_crypto_sha256_update(&h, buf, (size_t)n);
        off += n;
        if ((uint64_t)off > HL_EXEC_PIN_MAX_BYTES) {
            snprintf(why, why_size, "%s grew past %u MiB while its sha256 pin "
                     "was checked", cmd->real, (unsigned)(HL_EXEC_PIN_MAX_BYTES >> 20));
            return -1;
        }
    }
    uint8_t got[32];
    (void)hl_cap_crypto_sha256_final(&h, got);
    if (memcmp(got, cmd->c.sha256, sizeof got) == 0) return 0;
    char hex[65];
    static const char HEX[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex[2 * i] = HEX[got[i] >> 4];
        hex[2 * i + 1] = HEX[got[i] & 15];
    }
    hex[64] = '\0';
    snprintf(why, why_size, "%s does not match its sha256 pin (its sha256 is "
             "%s); restore the binary or update the pin", cmd->real, hex);
    return -1;
}

/* Open @p real and check it; with @p want_dev set, it must still be the
 * file found at policy load. The descriptor, or -1 (why says). */
static int open_checked(const Cmd *cmd, char *why, size_t why_size)
{
    int fd = open(cmd->real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        int e = errno;
        /* Landlock binds the read right to the inode found at load, so a
         * file renamed over it cannot even be opened: stat (which Landlock
         * does not mediate) tells the replacement from any other refusal. */
        struct stat now;
        if (e == EACCES && stat(cmd->real, &now) == 0 &&
            (now.st_dev != cmd->dev || now.st_ino != cmd->ino))
            snprintf(why, why_size, "%s was replaced since the app started; "
                     "restart the app (ESTALE)", cmd->real);
        else
            snprintf(why, why_size, "%s: %s", cmd->real, strerror(e));
        return -1;
    }
    struct stat st;
    if (leaf_ok(fd, cmd->real, &st, why, why_size) != 0) { close(fd); return -1; }
    if (st.st_dev != cmd->dev || st.st_ino != cmd->ino) {
        snprintf(why, why_size, "%s was replaced since the app started; restart "
                 "the app (ESTALE)", cmd->real);
        close(fd);
        return -1;
    }
    if (pin_check(cmd, fd, why, why_size) != 0) { close(fd); return -1; }
    return fd;
}

/* Resolve and check one command at policy load. */
/* Resolve the command's child manifest (manifest.fs) against the app's
 * roots, exactly as named: the kernel grant is the whole boundary for a
 * command (exec_child.h). Refused here, at policy load, so the app does not
 * start with a grant that means something else. */
static int app_grants_load(Cmd *cmd, char *why, size_t why_size)
{
    unsigned total = (unsigned)cmd->c.n_app_read + cmd->c.n_app_write;
    if (total == 0) return 0;
    /* Every grant's descriptor stays open until the lock is applied: up to
     * 32 commands x 32 grants, past macOS's default soft limit of 256. */
    static int nofile_raised;
    if (!nofile_raised) {
        struct rlimit nl;
        if (getrlimit(RLIMIT_NOFILE, &nl) == 0 && nl.rlim_cur < 4096) {
            nl.rlim_cur = nl.rlim_max < 4096 ? nl.rlim_max : 4096;
            (void)setrlimit(RLIMIT_NOFILE, &nl);
        }
        nofile_raised = 1;
    }
    char *buf = malloc((size_t)total * PATH_MAX);
    int *fds = malloc((size_t)total * sizeof *fds);
    if (!buf || !fds) {
        free(buf);
        free(fds);
        snprintf(why, why_size, "exec: out of memory");
        return -1;
    }
    const char *protect[HL_EXEC_MAX_PROTECT];
    for (int i = 0; i < g_n_dbs; i++) protect[i] = g_dbs[i];
    HlExecChildRoots roots = { g_app_dir, g_state_dir[0] ? g_state_dir : NULL,
                               protect, g_n_dbs };
    size_t at = 0;
    unsigned k = 0;
    for (int w = 0; w < 2; w++) {
        const uint8_t *cur = w ? cmd->c.app_write : cmd->c.app_read;
        unsigned n = w ? cmd->c.n_app_write : cmd->c.n_app_read;
        for (unsigned i = 0; i < n; i++) {
            const char *raw = hl_exec_next_str(&cur, NULL);
            char r[192];
            HlExecChildGrant gr;
            if (hl_exec_child_grant_open(&roots, raw, w, &gr, r, sizeof r) != 0) {
                snprintf(why, why_size, "exec.%s.manifest.fs.%s[%u] '%s' %s",
                         cmd->c.name, w ? "write" : "read", i + 1, raw, r);
                while (k > 0) close(fds[--k]);
                free(buf);
                free(fds);
                return -1;
            }
            size_t l = strlen(gr.path) + 1;
            memcpy(buf + at, gr.path, l);
            at += l;
            fds[k++] = gr.fd;
        }
    }
    cmd->app_paths = buf;
    cmd->app_fds = fds;
    cmd->n_app_read = cmd->c.n_app_read;
    cmd->n_app_write = cmd->c.n_app_write;
    return 0;
}

static int cmd_load(Cmd *cmd, char *why, size_t why_size)
{
    if (!realpath(cmd->c.path, cmd->real)) {
        snprintf(why, why_size, "exec.%s: %s: %s", cmd->c.name, cmd->c.path,
                 strerror(errno));
        return -1;
    }
    if (path_dirs_ok(cmd->real, why, why_size) != 0) return -1;
    int fd = open(cmd->real, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        snprintf(why, why_size, "%s: %s", cmd->real, strerror(errno));
        return -1;
    }
    struct stat st;
    char raw_interp[PATH_MAX];
    int rc = leaf_ok(fd, cmd->real, &st, why, why_size);
    if (rc == 0) rc = hl_exec_plat_binary_check(fd, cmd->real, raw_interp,
                                                sizeof raw_interp, why, why_size);
    if (rc == 0) rc = pin_check(cmd, fd, why, why_size);
    close(fd);
    if (rc != 0) return -1;
    cmd->dev = st.st_dev;
    cmd->ino = st.st_ino;
    cmd->interp[0] = '\0';
    if (raw_interp[0]) {
        if (!realpath(raw_interp, cmd->interp)) {
            snprintf(why, why_size, "%s: its loader %s: %s", cmd->real,
                     raw_interp, strerror(errno));
            return -1;
        }
        if (path_dirs_ok(cmd->interp, why, why_size) != 0) return -1;
        int ifd = open(cmd->interp, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat ist;
        if (ifd < 0 || fstat(ifd, &ist) != 0 || !S_ISREG(ist.st_mode) ||
            !owner_ok(&ist) || (ist.st_mode & (S_ISUID | S_ISGID))) {
            snprintf(why, why_size, "%s: its loader %s is not a root-owned "
                     "regular file", cmd->real, cmd->interp);
            if (ifd >= 0) close(ifd);
            return -1;
        }
        close(ifd);
    }
    return app_grants_load(cmd, why, why_size);
}

/* ── Jobs ──────────────────────────────────────────────────────────── */

typedef struct {
    int      used;
    uint32_t id;
    int      ci;
    pid_t    pid;               /* 0 once reaped */
    int      pidfd;             /* readable once it exits; -1 = none */
    int      out_fd, err_fd;    /* -1 once closed */
    int      in_fd;             /* the child's stdin pipe, -1 once written */
    uint8_t *in;   size_t in_len, in_off;
    uint8_t *out;  size_t out_len;  int out_cut;
    uint8_t *err;  size_t err_len;  int err_cut;
    uint64_t start_ms, deadline;
    int      timed_out;
    int      status, signo;
    int      discard;           /* KILLed: no reply */
    uint8_t *reply; size_t reply_len, reply_off;
    int      queued;
    size_t   reserved;          /* stdout_max + stderr_max, held until freed */
    int      routed;            /* run by its command's own broker (the router) */
#ifdef HL_EXEC_TEST_HOOKS
    int      probe_sent;
#endif
} Job;

static Job    g_jobs[HL_EXEC_MAX_INFLIGHT];
static size_t g_reserved;
static int    g_chan = 3;

/* The router (macOS, two or more commands; exec_platform.h
 * hl_exec_plat_router): command i runs in broker g_subs[i], a fresh copy of
 * this binary holding that command's policy alone. The router admits and
 * matches each call, forwards it, and relays the reply
 * (docs/design/exec_broker_macos_isolation.md §4). */
typedef struct {
    int         fd;             /* -1 once lost */
    pid_t       pid;
    HlChanFrame rd;
} Sub;
static int    g_router;
static Sub    g_subs[HL_EXEC_MAX_COMMANDS];

/* Small frames (BUSY / REFUSED / PROBE) waiting to go out. */
typedef struct { uint8_t *buf; size_t len, off; } Out;
#define HX_OUTQ 64
static Out    g_outq[HX_OUTQ];
static int    g_outq_n;
static int    g_order[HX_OUTQ + HL_EXEC_MAX_INFLIGHT];  /* -1-k = small k, j = job j */
static int    g_order_n;

static uint8_t *frame_make(uint32_t type, uint32_t id, const void *a, size_t alen,
                           const void *b, size_t blen, size_t *len)
{
    size_t n = HL_CHAN_HDR + alen + blen;
    uint8_t *f = malloc(n);
    if (!f) return NULL;
    hl_chan_put32(f, HX_MAGIC);
    hl_chan_put32(f + 4, type);
    hl_chan_put32(f + 8, id);
    hl_chan_put32(f + 12, (uint32_t)(alen + blen));
    if (alen) memcpy(f + HL_CHAN_HDR, a, alen);
    if (blen) memcpy(f + HL_CHAN_HDR + alen, b, blen);
    *len = n;
    return f;
}

static void queue_frame(uint32_t type, uint32_t id, const void *data, size_t dlen)
{
    if (g_outq_n >= HX_OUTQ) return;   /* reading stops before this (backpressure) */
    size_t len;
    uint8_t *f = frame_make(type, id, data, dlen, NULL, 0, &len);
    if (!f) return;
    int k = -1;
    for (int i = 0; i < HX_OUTQ; i++)
        if (!g_outq[i].buf) { k = i; break; }
    if (k < 0) { free(f); return; }
    g_outq[k] = (Out){ f, len, 0 };
    g_outq_n++;
    g_order[g_order_n++] = -1 - k;
}

static void queue_small(uint32_t type, uint32_t id, const char *text)
{
    queue_frame(type, id, text, text ? strlen(text) : 0);
}

/* The reply of routed job @p j: a frame its command's broker sent, or one
 * the router makes when that broker is gone. */
static void job_reply_frame(Job *j, uint32_t type, const void *body, size_t len)
{
    j->reply = frame_make(type, j->id, body, len, NULL, 0, &j->reply_len);
    if (!j->reply) { j->discard = 1; return; }
    j->queued = 1;
    g_order[g_order_n++] = (int)(j - g_jobs);
}

static void job_free(Job *j)
{
    if (j->pidfd >= 0) close(j->pidfd);
    if (j->in_fd >= 0) close(j->in_fd);
    free(j->in);
    if (j->out_fd >= 0) close(j->out_fd);
    if (j->err_fd >= 0) close(j->err_fd);
    free(j->out);
    free(j->err);
    free(j->reply);
    if (j->ci >= 0 && g_cmds[j->ci].running > 0) g_cmds[j->ci].running--;
    g_reserved -= j->reserved;
    memset(j, 0, sizeof *j);
    j->out_fd = j->err_fd = j->pidfd = j->in_fd = -1;
    j->ci = -1;
}

static void put_u32(uint8_t *p, uint32_t v) { hl_chan_put32(p, v); }

static void job_reply(Job *j)
{
    uint8_t head[24];
    put_u32(head, (uint32_t)j->status);
    put_u32(head + 4, (uint32_t)j->signo);
    head[8] = (uint8_t)j->timed_out;
    head[9] = (uint8_t)j->out_cut;
    head[10] = (uint8_t)j->err_cut;
    head[11] = 0;
    put_u32(head + 12, (uint32_t)(hl_chan_mono_ms() - j->start_ms));
    put_u32(head + 16, (uint32_t)j->out_len);
    size_t n = 20 + j->out_len + 4 + j->err_len;
    uint8_t *body = malloc(n ? n : 1);
    if (!body) { j->discard = 1; return; }
    memcpy(body, head, 20);
    if (j->out_len) memcpy(body + 20, j->out, j->out_len);
    put_u32(body + 20 + j->out_len, (uint32_t)j->err_len);
    if (j->err_len) memcpy(body + 24 + j->out_len, j->err, j->err_len);
    j->reply = frame_make(HX_DONE, j->id, body, n, NULL, 0, &j->reply_len);
    free(body);
    if (!j->reply) { j->discard = 1; return; }
    j->queued = 1;
    g_order[g_order_n++] = (int)(j - g_jobs);
}

/* Write what the channel takes. -1 when the server is gone. */
static int flush_out(void)
{
    while (g_order_n > 0) {
        int o = g_order[0];
        uint8_t *buf; size_t len, *off;
        if (o < 0) { Out *q = &g_outq[-1 - o]; buf = q->buf; len = q->len; off = &q->off; }
        else { Job *j = &g_jobs[o]; buf = j->reply; len = j->reply_len; off = &j->reply_off; }
        while (*off < len) {
            ssize_t w = send(g_chan, buf + *off, len - *off, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (w < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                if (errno == EINTR) continue;
                return -1;
            }
            *off += (size_t)w;
        }
        if (o < 0) { free(g_outq[-1 - o].buf); g_outq[-1 - o].buf = NULL; g_outq_n--; }
        else job_free(&g_jobs[o]);   /* delivered: its admission slot frees */
        memmove(g_order, g_order + 1, (size_t)(--g_order_n) * sizeof g_order[0]);
    }
    return 0;
}

static void kill_all(void)
{
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++)
        if (g_jobs[i].used && g_jobs[i].pid > 0) kill(-g_jobs[i].pid, SIGKILL);
}

/* ── The router's side of a call ───────────────────────────────────── */

/* Command @p ci's broker is gone (EOF, a write it did not take, a frame it
 * should not send): every call routed to it that has no reply yet fails,
 * and later calls to it are refused. Its running calls are its own to end:
 * on SIGKILL they run on (the documented limit; it cannot be reaped from
 * here without signalling grandchildren, §4.4). */
static void sub_lost(int ci)
{
    Sub *s = &g_subs[ci];
    if (s->fd < 0) return;
    close(s->fd);
    s->fd = -1;
    hl_chan_frame_reset(&s->rd);
    char msg[160];
    snprintf(msg, sizeof msg, "exec.%s: the command broker is gone", g_cmds[ci].c.name);
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) {
        Job *j = &g_jobs[i];
        if (!j->used || !j->routed || j->ci != ci || j->queued) continue;
        if (j->discard) job_free(j);
        else job_reply_frame(j, HX_REFUSED, msg, strlen(msg));
    }
}

/* A frame from command @p ci's broker: a reply ends the call routed under
 * its id; a test probe goes straight on to the server. */
static void sub_frame(int ci, uint32_t type, uint32_t id, const uint8_t *p, size_t len)
{
    if (type == HX_PROBE) { queue_frame(HX_PROBE, id, p, len); return; }
    if (type != HX_DONE && type != HX_BUSY && type != HX_REFUSED) { sub_lost(ci); return; }
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) {
        Job *j = &g_jobs[i];
        if (!j->used || !j->routed || j->id != id) continue;
        if (j->ci != ci) { sub_lost(ci); return; }   /* not its call */
        if (j->queued) return;
        if (j->discard) { job_free(j); return; }
        job_reply_frame(j, type, p, len);
        return;
    }
    /* No such call: it was KILLed and freed meanwhile. */
}

/* Forward an admitted, matched RUN to its command's broker, as one job.
 * The write is bounded: the broker reads every RUN as it comes (it holds at
 * most HL_EXEC_MAX_INFLIGHT jobs, so its own reading never stops). */
static void route_call(int slot, int ci, uint32_t id, const uint8_t *p, size_t len,
                       size_t reserve)
{
    Sub *s = &g_subs[ci];
    char msg[160];
    if (s->fd < 0 ||
        hl_chan_write_frame(s->fd, HX_MAGIC, HX_RUN, id, p, len, NULL, 0,
                            hl_chan_mono_ms() + 5000) != 0) {
        sub_lost(ci);
        snprintf(msg, sizeof msg, "exec.%s: the command broker is gone", g_cmds[ci].c.name);
        queue_small(HX_REFUSED, id, msg);
        return;
    }
    Job *j = &g_jobs[slot];
    memset(j, 0, sizeof *j);
    j->out_fd = j->err_fd = j->pidfd = j->in_fd = -1;
    j->used = 1;
    j->routed = 1;
    j->id = id;
    j->ci = ci;
    j->reserved = reserve;
    g_reserved += reserve;
    g_cmds[ci].running++;
}

/* ── One call ──────────────────────────────────────────────────────── */

/* The child, between fork and exec. Allocates nothing; reports a failure
 * through @p ep (stage byte + errno) and _exit(126). Its stdin is @p in_r
 * (the call's bytes), or /dev/null when it is -1. The host's steps are
 * hl_exec_plat_child_* (exec_platform.h); the order matters: die with the
 * broker before anything else, confine before stdio moves, the syscall
 * filter last. */
static void child_run(const Cmd *cmd, int bfd, int in_r, int out_w, int err_w,
                      int ep, char *const argv[], char *const envp[], pid_t broker)
{
    unsigned char stage = 0;
    int e = 0, st;
#define FAIL(s) do { stage = (s); e = errno; goto fail; } while (0)
    if (hl_exec_plat_child_death() != 0) FAIL(HL_EXEC_STAGE_DEATH);
    if (getppid() != broker) _exit(127);           /* the broker died already */
    if (setsid() < 0) FAIL(HL_EXEC_STAGE_SETSID);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);
    for (int s = 1; s < 32; s++)
        if (s != SIGKILL && s != SIGSTOP) signal(s, SIG_DFL);
    if ((st = hl_exec_plat_child_confine(cmd, g_flags)) != 0) FAIL(st);
    /* stdio: move every source above 10 first, so no dup2 clobbers one. */
    int hi_null = fcntl(in_r >= 0 ? in_r : g_devnull, F_DUPFD, 10), hi_out = fcntl(out_w, F_DUPFD, 10),
        hi_err = fcntl(err_w, F_DUPFD, 10), hi_bin = fcntl(bfd, F_DUPFD, 10),
        hi_ep = fcntl(ep, F_DUPFD_CLOEXEC, 10);
    if (hi_null < 0 || hi_out < 0 || hi_err < 0 || hi_bin < 0 || hi_ep < 0)
        FAIL(HL_EXEC_STAGE_STDIO);
    ep = hi_ep;
    if (dup2(hi_null, 0) < 0 || dup2(hi_out, 1) < 0 || dup2(hi_err, 2) < 0 ||
        dup2(hi_bin, 3) < 0) FAIL(HL_EXEC_STAGE_STDIO);
    (void)fcntl(3, F_SETFD, FD_CLOEXEC);           /* the binary is not inherited */
    for (int fd = 4; fd < 1024; fd++) if (fd != ep) close(fd);
    if (chdir("/") != 0) FAIL(HL_EXEC_STAGE_CHDIR);
    umask(077);
    struct rlimit core = { 0, 0 };
    setrlimit(RLIMIT_CORE, &core);
    rlim_t cpu = (cmd->c.timeout_ms + 999) / 1000 + 1;
    struct rlimit rcpu = { cpu, cpu };
    setrlimit(RLIMIT_CPU, &rcpu);
    struct rlimit nofile = { 256, 256 };
    setrlimit(RLIMIT_NOFILE, &nofile);
    if (cmd->c.n_fs_write == 0 && cmd->n_app_write == 0) {
        struct rlimit fsz = { 0, 0 };
        setrlimit(RLIMIT_FSIZE, &fsz);
    }
    if (hl_exec_plat_child_filter(cmd, g_flags) != 0) FAIL(HL_EXEC_STAGE_FILTER);
    hl_exec_plat_child_exec(cmd, 3, argv, envp);
    FAIL(HL_EXEC_STAGE_EXEC);
fail:;
    unsigned char msg[5] = { stage, (unsigned char)e, (unsigned char)(e >> 8),
                             (unsigned char)(e >> 16), (unsigned char)(e >> 24) };
    if (write(ep, msg, sizeof msg) < 0) { /* nothing to do */ }
    _exit(126);
#undef FAIL
}

static const char *const STAGE[] = { "", "parent-death signal", "setsid",
                                     "no_new_privs", "sandbox", "stdio", "chdir",
                                     "syscall filter", "exec" };

/* A close-on-exec pipe (pipe2 is not on every host; the broker is single-
 * threaded, so nothing can fork between the two calls). */
static int pipe_cloexec(int fds[2])
{
    if (pipe(fds) != 0) return -1;
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return 0;
}

/* RUN: admit, match, check, fork. A reply is queued when it is refused. */
static void run_call(uint32_t id, const uint8_t *p, size_t len)
{
    /* Admission first: the server may skip its own checks (§5.2). */
    int used = 0, free_slot = -1;
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) {
        if (g_jobs[i].used) {
            used++;
            if (g_jobs[i].id == id) {
                queue_small(HX_BUSY, id, "exec: that request id is already in use");
                return;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0 || used >= HL_EXEC_MAX_INFLIGHT) {
        queue_small(HX_BUSY, id, "exec: too many calls in flight");
        return;
    }

    /* Decode: u16 name_len, name, u16 n, (u32 len, bytes) * n,
     * u32 stdin_len, stdin */
    const char *args[HL_EXEC_MAX_CALL_ARGS];
    size_t lens[HL_EXEC_MAX_CALL_ARGS];
    if (len < 4) { queue_small(HX_REFUSED, id, "exec: malformed RUN"); return; }
    size_t nl = (size_t)(p[0] | (p[1] << 8)), off = 2;
    if (nl > HL_EXEC_NAME_MAX || off + nl + 2 > len) {
        queue_small(HX_REFUSED, id, "exec: malformed RUN");
        return;
    }
    const char *name = (const char *)p + off;
    off += nl;
    size_t n = (size_t)(p[off] | (p[off + 1] << 8));
    off += 2;
    if (n > HL_EXEC_MAX_CALL_ARGS) { queue_small(HX_REFUSED, id, "exec: too many arguments"); return; }
    for (size_t i = 0; i < n; i++) {
        if (off + 4 > len) { queue_small(HX_REFUSED, id, "exec: malformed RUN"); return; }
        size_t al = hl_chan_get32(p + off);
        off += 4;
        if (al > len - off) { queue_small(HX_REFUSED, id, "exec: malformed RUN"); return; }
        args[i] = (const char *)p + off;
        lens[i] = al;
        off += al;
    }
    if (off + 4 > len) { queue_small(HX_REFUSED, id, "exec: malformed RUN"); return; }
    size_t in_len = hl_chan_get32(p + off);
    off += 4;
    if (in_len != len - off) { queue_small(HX_REFUSED, id, "exec: malformed RUN"); return; }
    const uint8_t *in = p + off;

    HlExecCmd c;
    char err[256], msg[320];
    if (hl_exec_check_call(g_policy, g_policy_len, name, nl, args, lens, n, &c,
                           err, sizeof err) != HL_EXEC_OK) {
        snprintf(msg, sizeof msg, "exec: %s", err);
        queue_small(HX_REFUSED, id, msg);
        return;
    }
    int ci = -1;
    for (int i = 0; i < g_ncmds; i++)
        if (strcmp(g_cmds[i].c.name, c.name) == 0) { ci = i; break; }
    if (ci < 0) { queue_small(HX_REFUSED, id, "exec: unknown command"); return; }
    Cmd *cmd = &g_cmds[ci];
    /* stdin: the command's own cap, checked here too (the binding's check is
     * only a fast path). */
    if (in_len > cmd->c.stdin_max) {
        snprintf(msg, sizeof msg, cmd->c.stdin_max
                 ? "exec.%s: stdin is longer than its stdin_max"
                 : "exec.%s: takes no stdin (it declares no stdin_max)", cmd->c.name);
        queue_small(HX_REFUSED, id, msg);
        return;
    }
    if (cmd->running >= cmd->c.concurrency) {
        queue_small(HX_BUSY, id, "exec: the command's concurrency is reached");
        return;
    }
    /* Its output caps and its stdin, held until the reply is delivered. */
    size_t reserve = (size_t)cmd->c.stdout_max + cmd->c.stderr_max + in_len;
    if (reserve > HL_EXEC_MAX_BUFFERED - g_reserved) {
        queue_small(HX_BUSY, id, "exec: too much output held for delivery");
        return;
    }
    if (g_router) {                      /* its own broker runs it */
        route_call(free_slot, ci, id, p, len, reserve);
        return;
    }

    int bfd = open_checked(cmd, err, sizeof err);
    if (bfd < 0) {
        snprintf(msg, sizeof msg, "exec.%s: %s", cmd->c.name, err);
        queue_small(HX_REFUSED, id, msg);
        return;
    }

    /* argv: the resolved path, every fixed entry, the values in order. */
    char *argv[HL_EXEC_MAX_ARGV + HL_EXEC_MAX_CALL_ARGS + 2];
    char *envp[HL_EXEC_MAX_ENV + 4];
    char *owned[HL_EXEC_MAX_CALL_ARGS + 1];
    size_t no = 0, na = 0, ne = 0;
    argv[na++] = cmd->real;
    const uint8_t *cur = c.argv;
    size_t ai = 0;
    for (uint16_t e = 0; e < c.n_argv; e++) {
        HlExecArg a;
        hl_exec_next_arg(&cur, c.end, &a);
        if (!a.is_slot) { argv[na++] = (char *)(uintptr_t)a.fixed; continue; }
        size_t take = (a.slot.flags & HL_EXEC_SLOT_REPEAT) ? n - ai : 1;
        for (size_t k = 0; k < take; k++, ai++) {
            char *v = malloc(lens[ai] + 1);
            if (!v) break;
            memcpy(v, args[ai], lens[ai]);
            v[lens[ai]] = '\0';
            owned[no++] = v;
            argv[na++] = v;
        }
    }
    argv[na] = NULL;
    static char path_env[] = "PATH=/usr/bin:/bin";
    static char lang_env[] = "LANG=C";
    static char lc_env[] = "LC_ALL=C";
    int has_lang = 0, has_lc = 0;
    char *envbufs[HL_EXEC_MAX_ENV];
    size_t nenv = 0;
    cur = c.env;
    for (unsigned i = 0; i < c.n_env; i++) {
        size_t kl, vl;
        const char *k = hl_exec_next_str(&cur, &kl);
        const char *v = hl_exec_next_str(&cur, &vl);
        char *kv = malloc(kl + vl + 2);
        if (!kv) continue;
        memcpy(kv, k, kl);
        kv[kl] = '=';
        memcpy(kv + kl + 1, v, vl + 1);
        envbufs[nenv++] = kv;
        if (kl == 4 && memcmp(k, "LANG", 4) == 0) has_lang = 1;
        if (kl == 6 && memcmp(k, "LC_ALL", 6) == 0) has_lc = 1;
    }
    envp[ne++] = path_env;
    if (!has_lang) envp[ne++] = lang_env;
    if (!has_lc) envp[ne++] = lc_env;
    for (size_t i = 0; i < nenv; i++) envp[ne++] = envbufs[i];
    envp[ne] = NULL;

    int op[2] = { -1, -1 }, epp[2] = { -1, -1 }, ep[2] = { -1, -1 };
    int ip[2] = { -1, -1 };              /* stdin, only when the call sends some */
    pid_t pid = -1;
    if (pipe_cloexec(op) == 0 && pipe_cloexec(epp) == 0 && pipe_cloexec(ep) == 0 &&
        (in_len == 0 || pipe_cloexec(ip) == 0))
        pid = fork();
    if (pid == 0)
        child_run(cmd, bfd, ip[0], op[1], epp[1], ep[1], argv, envp, getppid());
    int fork_errno = errno;
    close(bfd);
    for (size_t i = 0; i < no; i++) free(owned[i]);
    for (size_t i = 0; i < nenv; i++) free(envbufs[i]);
    if (op[1] >= 0) close(op[1]);
    if (epp[1] >= 0) close(epp[1]);
    if (ep[1] >= 0) close(ep[1]);
    if (ip[0] >= 0) close(ip[0]);
    if (pid < 0) {
        if (ip[1] >= 0) close(ip[1]);
        if (op[0] >= 0) close(op[0]);
        if (epp[0] >= 0) close(epp[0]);
        if (ep[0] >= 0) close(ep[0]);
        snprintf(msg, sizeof msg, "exec.%s: could not start: %s", cmd->c.name,
                 strerror(fork_errno));
        queue_small(HX_REFUSED, id, msg);
        return;
    }
    /* Did it reach exec? The error pipe closes on a successful exec. */
    unsigned char em[5];
    struct pollfd epf = { ep[0], POLLIN, 0 };
    ssize_t got = 0;
    if (poll(&epf, 1, 2000) > 0) got = read(ep[0], em, sizeof em);
    close(ep[0]);
    if (got == (ssize_t)sizeof em) {
        int e = em[1] | (em[2] << 8) | (em[3] << 16) | (em[4] << 24);
        waitpid(pid, NULL, 0);
        if (ip[1] >= 0) close(ip[1]);
        close(op[0]);
        close(epp[0]);
        snprintf(msg, sizeof msg, "exec.%s: could not start (%s): %s", cmd->c.name,
                 em[0] < sizeof STAGE / sizeof STAGE[0] ? STAGE[em[0]] : "?",
                 strerror(e));
        queue_small(HX_REFUSED, id, msg);
        return;
    }

    Job *j = &g_jobs[free_slot];
    memset(j, 0, sizeof *j);
    j->pidfd = -1;
    j->used = 1;
    j->id = id;
    j->ci = ci;
    j->pid = pid;
    j->out_fd = op[0];
    j->err_fd = epp[0];
    /* Readable the moment it exits, so the reply goes out then rather than
     * at the next reap tick (else the 50 ms tick). */
    j->pidfd = hl_exec_plat_exit_watch(pid);
    hl_chan_set_nonblock(j->out_fd);
    hl_chan_set_nonblock(j->err_fd);
    j->out = malloc(cmd->c.stdout_max ? cmd->c.stdout_max : 1);
    j->err = malloc(cmd->c.stderr_max ? cmd->c.stderr_max : 1);
    j->in_fd = ip[1];
    if (in_len) {
        /* Written as the pipe takes it (the main loop); a command that exits
         * or closes its stdin first just leaves the rest unwritten. */
        hl_chan_set_nonblock(j->in_fd);
        j->in = malloc(in_len);
        if (j->in) { memcpy(j->in, in, in_len); j->in_len = in_len; }
    }
    j->start_ms = hl_chan_mono_ms();
    j->deadline = j->start_ms + cmd->c.timeout_ms;
    j->reserved = reserve;
    g_reserved += reserve;
    cmd->running++;
    if (!j->out || !j->err || (in_len && !j->in)) { kill(-pid, SIGKILL); j->discard = 1; }
}

static void kill_call(uint32_t id)
{
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) {
        Job *j = &g_jobs[i];
        if (!j->used || j->id != id) continue;
        if (j->routed && !j->queued) {
            /* Its broker kills the child and sends no reply; the slot frees
             * now (that broker holds its own caps meanwhile). */
            if (g_subs[j->ci].fd >= 0 &&
                hl_chan_write_frame(g_subs[j->ci].fd, HX_MAGIC, HX_KILL, id, NULL, 0,
                                    NULL, 0, hl_chan_mono_ms() + 1000) != 0)
                sub_lost(j->ci);
            job_free(j);
            return;
        }
        if (j->pid > 0) kill(-j->pid, SIGKILL);
        j->discard = 1;
        if (j->queued && j->reply_off == 0) {
            /* not started on the wire: drop it */
            for (int k = 0; k < g_order_n; k++)
                if (g_order[k] == i) {
                    memmove(g_order + k, g_order + k + 1,
                            (size_t)(g_order_n - k - 1) * sizeof g_order[0]);
                    g_order_n--;
                    break;
                }
            job_free(j);
        }
        return;
    }
}

/* Read what a pipe has into its buffer, keeping only up to @p cap. */
static void drain(int *fd, uint8_t *buf, size_t *len, size_t cap, int *cut)
{
    uint8_t tmp[16384];
    for (;;) {
        ssize_t r = read(*fd, tmp, sizeof tmp);
        if (r > 0) {
            size_t take = (size_t)r;
            if (*len + take > cap) { take = cap - *len; *cut = 1; }
            if (take) { memcpy(buf + *len, tmp, take); *len += take; }
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (r < 0 && errno == EINTR) continue;
        close(*fd);
        *fd = -1;
        return;
    }
}

/* Write what the child's stdin pipe takes; close it once everything is
 * written, or when the child stopped reading (EPIPE: SIGPIPE is ignored). */
static void feed(Job *j)
{
    while (j->in_off < j->in_len) {
        ssize_t w = write(j->in_fd, j->in + j->in_off, j->in_len - j->in_off);
        if (w > 0) { j->in_off += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        break;
    }
    close(j->in_fd);
    j->in_fd = -1;
    free(j->in);
    j->in = NULL;
}

static void reap(void)
{
    for (;;) {
        int st;
        pid_t p = waitpid(-1, &st, WNOHANG);
        if (p <= 0) return;
        for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) {
            Job *j = &g_jobs[i];
            if (!j->used || j->pid != p) continue;
            j->pid = 0;
            if (j->in_fd >= 0) { close(j->in_fd); j->in_fd = -1; }   /* nobody reads it */
            if (WIFEXITED(st)) { j->status = WEXITSTATUS(st); j->signo = 0; }
            else if (WIFSIGNALED(st)) { j->status = 0; j->signo = WTERMSIG(st); }
        }
    }
}

/* One root of the POLICY frame (u16 length + bytes) into @p out. */
static int root_take(const uint8_t **p, const uint8_t *end, char *out)
{
    if (end - *p < 2) return -1;
    size_t n = (size_t)(*p)[0] | ((size_t)(*p)[1] << 8);
    *p += 2;
    if (n >= PATH_MAX || (size_t)(end - *p) < n || memchr(*p, 0, n)) return -1;
    memcpy(out, *p, n);
    out[n] = '\0';
    *p += n;
    return 0;
}

static int env_allowed(const char *kv);

/* The router's start (docs/design/exec_broker_macos_isolation.md §4.2): one
 * broker per command, this same binary started afresh (a clean image: every
 * descriptor here is close-on-exec), each sent the POLICY head it got and its
 * command's entry alone, all ready before the router locks itself - it can
 * start nothing after. Any broker that refuses or does not answer refuses
 * the start, naming its command. */
static int router_start(const uint8_t *head, size_t head_len, const char *self,
                        HlExecReport *r)
{
    uint64_t deadline = hl_chan_mono_ms() + 8000;   /* the server waits 10 s */
    for (int i = 0; i < g_ncmds; i++) g_subs[i] = (Sub){ .fd = -1, .pid = -1 };
    /* Start them all first, so their loading overlaps. */
    for (int i = 0; i < g_ncmds; i++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            hl_exec_report_set(r, "exec.%s: its broker: %s", g_cmds[i].c.name,
                               strerror(errno));
            return -1;
        }
        (void)fcntl(sv[0], F_SETFD, FD_CLOEXEC);
        (void)fcntl(sv[1], F_SETFD, FD_CLOEXEC);
        char a0[PATH_MAX], a1[] = HL_EXEC_BROKER_ARGV1;
        snprintf(a0, sizeof a0, "%s", self);
        char *argv[] = { a0, a1, NULL };
        pid_t pid = hl_chan_spawn(self, argv, sv[1], env_allowed);
        int e = errno;
        close(sv[1]);
        if (pid < 0) {
            close(sv[0]);
            hl_exec_report_set(r, "exec.%s: its broker could not start: %s",
                               g_cmds[i].c.name, strerror(e));
            return -1;
        }
        g_subs[i].fd = sv[0];
        g_subs[i].pid = pid;
    }
    for (int i = 0; i < g_ncmds; i++) {
        uint8_t *one = NULL;
        size_t one_len = 0;
        if (hl_exec_policy_slice(g_policy, g_policy_len, i, &one, &one_len) != 0) {
            hl_exec_report_set(r, "exec.%s: out of memory", g_cmds[i].c.name);
            return -1;
        }
        int wr = hl_chan_write_frame(g_subs[i].fd, HX_MAGIC, HX_POLICY, 0, head, head_len,
                                     one, one_len, deadline);
        free(one);
        if (wr != 0) {
            hl_exec_report_set(r, "exec.%s: its broker did not take its policy",
                               g_cmds[i].c.name);
            return -1;
        }
    }
    for (int i = 0; i < g_ncmds; i++) {
        HlChanFrame rd = { .magic = HX_MAGIC, .max = 8192 };
        int rc = hl_chan_frame_read(g_subs[i].fd, &rd, deadline);
        if (rc != HL_CHAN_DONE || (rd.type != HX_READY && rd.type != HX_REFUSED)) {
            hl_chan_frame_reset(&rd);
            hl_exec_report_set(r, "exec.%s: its broker did not start", g_cmds[i].c.name);
            return -1;
        }
        if (rd.type == HX_REFUSED) {
            /* Its own report: one command's checks, the same messages. */
            hl_exec_report_set(r, "%.*s", (int)(rd.len < 1024 ? rd.len : 1024),
                               (const char *)rd.buf);
            hl_chan_frame_reset(&rd);
            return -1;
        }
        hl_chan_frame_reset(&rd);
        hl_chan_set_nonblock(g_subs[i].fd);
        g_subs[i].rd = (HlChanFrame){ .magic = HX_MAGIC, .max = HX_REPLY_MAX };
    }
    return hl_exec_plat_router_lock(r);
}

static int policy_load(const uint8_t *p, size_t len, char *report, size_t rsz)
{
    report[0] = '\0';
    if (len < 4) { snprintf(report, rsz, "exec: malformed POLICY"); return -1; }
    g_flags = hl_chan_get32(p);
    const uint8_t *q = p + 4, *end = p + len;
    int bad = root_take(&q, end, g_app_dir) != 0 || root_take(&q, end, g_state_dir) != 0;
    if (!bad) {
        if (end - q < 2) bad = 1;
        else {
            int nd = q[0] | (q[1] << 8);
            q += 2;
            bad = nd > HL_EXEC_MAX_PROTECT;
            g_n_dbs = bad ? 0 : nd;
            for (int i = 0; !bad && i < g_n_dbs; i++) bad = root_take(&q, end, g_dbs[i]) != 0;
        }
    }
    if (bad) {
        snprintf(report, rsz, "exec: malformed POLICY");
        return -1;
    }
    size_t head_len = (size_t)(q - p);   /* flags, roots, databases */
    size_t plen = (size_t)(end - q);
    char err[256];
    if (hl_exec_policy_validate(q, plen, err, sizeof err) != 0) {
        snprintf(report, rsz, "%s", err);
        return -1;
    }
    g_policy = malloc(plen);
    if (!g_policy) { snprintf(report, rsz, "exec: out of memory"); return -1; }
    memcpy(g_policy, q, plen);
    g_policy_len = plen;
    g_ncmds = hl_exec_policy_count(g_policy, g_policy_len);
    size_t at = 0;
    for (int i = 0; i < g_ncmds; i++) {
        Cmd *cmd = &g_cmds[i];
        memset(cmd, 0, sizeof *cmd);
        cmd->plat = -1;
        (void)hl_exec_policy_at(g_policy, g_policy_len, i, &cmd->c);
        if (cmd_load(cmd, err, sizeof err) != 0) {
            snprintf(report, rsz, "%s", err);
            return -1;
        }
        int w = snprintf(report + at, rsz - at, "exec: %s -> %s\n", cmd->c.name,
                         cmd->real);
        if (w > 0 && (size_t)w < rsz - at) at += (size_t)w;
    }

    g_devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (g_devnull < 0) { snprintf(report, rsz, "exec: /dev/null: %s", strerror(errno)); return -1; }

    /* The host's lock (§6.2 - §6.5): each child's confinement prepared, the
     * broker confined to the union of the commands' grants. */
    char self_raw[PATH_MAX], self[PATH_MAX];
    if (hl_chan_self_exe(self_raw, sizeof self_raw) != 0 || !realpath(self_raw, self)) {
        snprintf(report, rsz, "exec: cannot resolve the broker's own path");
        return -1;
    }
    HlExecReport r = { report, rsz, at };
    int lrc;
    if (hl_exec_plat_router() && g_ncmds >= 2 && !(g_flags & HL_EXEC_POLICY_NO_SANDBOX)) {
        /* One broker per command, then the router locks itself. The checks
         * above ran here too, so a refusal reads the same either way. */
        g_router = 1;
        lrc = 0;
        for (int i = 0; i < g_ncmds && lrc == 0; i++) {
            if (strcmp(g_cmds[i].real, self) == 0) {
                hl_exec_report_set(&r, "exec.%s: %s is this hull binary itself, "
                                   "which macOS lets fork; declare another command",
                                   g_cmds[i].c.name, g_cmds[i].real);
                lrc = -1;
            }
        }
        if (lrc == 0) lrc = router_start(p, head_len, self, &r);
    } else {
        lrc = hl_exec_plat_lock(g_cmds, g_ncmds, g_flags, self, &r);
    }
    /* The rules are made: the grants' descriptors are no longer needed. */
    for (int i = 0; i < g_ncmds; i++) {
        Cmd *c = &g_cmds[i];
        for (unsigned j = 0; c->app_fds && j < (unsigned)c->n_app_read + c->n_app_write; j++)
            if (c->app_fds[j] >= 0) { close(c->app_fds[j]); c->app_fds[j] = -1; }
    }
    return lrc;
}

int hl_exec_broker_main(void)
{
    /* Go when the server does: the kernel ends this process (Linux), or a
     * descriptor turns readable (macOS), on top of the channel's EOF. */
    int server_gone = -1;
    if (hl_exec_plat_broker_init(&server_gone) != 0) return 1;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);
    (void)fcntl(g_chan, F_SETFD, FD_CLOEXEC);
    hl_chan_set_nonblock(g_chan);
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) {
        g_jobs[i].out_fd = g_jobs[i].err_fd = g_jobs[i].pidfd = g_jobs[i].in_fd = -1;
        g_jobs[i].ci = -1;
    }

    HlChanFrame rd = { .magic = HX_MAGIC, .max = HL_EXEC_POLICY_MAX_BYTES + HX_POLICY_HEAD_MAX };
    int rc = hl_chan_frame_read(g_chan, &rd, 0);
    if (rc != HL_CHAN_DONE) return rc == HL_CHAN_EOF ? 0 : 1;   /* no exec section */
    if (rd.type != HX_POLICY) return 1;
    char report[4096];
    int ok = policy_load(rd.buf, rd.len, report, sizeof report);
    hl_chan_frame_reset(&rd);
    if (hl_chan_write_frame(g_chan, HX_MAGIC, ok == 0 ? HX_READY : HX_REFUSED, 0,
                            report, strlen(report), NULL, 0,
                            hl_chan_mono_ms() + 5000) != 0 || ok != 0)
        return 1;

    rd.max = HX_RUN_MAX;
    for (;;) {
        struct pollfd pf[2 + 4 * HL_EXEC_MAX_INFLIGHT + HL_EXEC_MAX_COMMANDS];
        int map[2 + 4 * HL_EXEC_MAX_INFLIGHT + HL_EXEC_MAX_COMMANDS];
        int np = 0;
        short cev = (g_outq_n < HX_OUTQ - 2) ? POLLIN : 0;   /* backpressure */
        if (g_order_n > 0) cev |= POLLOUT;
        pf[np] = (struct pollfd){ g_chan, cev, 0 };
        map[np++] = -1;
        if (server_gone >= 0) {
            pf[np] = (struct pollfd){ server_gone, POLLIN, 0 };
            map[np++] = -2;
        }
        /* The command brokers (router only): map -3 - i. Not read while the
         * server is not reading us (backpressure, as above). */
        for (int i = 0; g_router && i < g_ncmds; i++)
            if (g_subs[i].fd >= 0) {
                pf[np] = (struct pollfd){ g_subs[i].fd, (short)(cev & POLLIN), 0 };
                map[np++] = -3 - i;
            }
        uint64_t now = hl_chan_mono_ms();
        int timeout = -1;
        for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) {
            Job *j = &g_jobs[i];
            if (!j->used || j->routed) continue;   /* its broker times it */
            if (j->out_fd >= 0) { pf[np] = (struct pollfd){ j->out_fd, POLLIN, 0 }; map[np++] = i; }
            if (j->err_fd >= 0) { pf[np] = (struct pollfd){ j->err_fd, POLLIN, 0 }; map[np++] = i; }
            if (j->pidfd >= 0 && j->pid > 0) { pf[np] = (struct pollfd){ j->pidfd, POLLIN, 0 }; map[np++] = i; }
            if (j->in_fd >= 0) { pf[np] = (struct pollfd){ j->in_fd, POLLOUT, 0 }; map[np++] = i; }
            int t = -1;
            if (j->pid > 0 && !j->timed_out) t = j->deadline > now ? (int)(j->deadline - now) : 0;
            else if (j->pid > 0) t = 50;                   /* killed: reap soon */
            else if (j->out_fd < 0 && j->err_fd < 0) t = 0;
            if (j->pid > 0 && j->pidfd < 0 && j->out_fd < 0 && j->err_fd < 0 &&
                (t < 0 || t > 50)) t = 50;
            if (t >= 0 && (timeout < 0 || t < timeout)) timeout = t;
        }
        int pr = poll(pf, (nfds_t)np, timeout);
        if (pr < 0 && errno != EINTR) break;

        if (pf[0].revents & (POLLERR | POLLNVAL)) break;
        if (pf[0].revents & (POLLIN | POLLHUP)) {
            for (;;) {
                rc = hl_chan_frame_read_some(g_chan, &rd);
                if (rc == HL_CHAN_MORE) break;
                if (rc != HL_CHAN_DONE) goto out;   /* EOF or a protocol error */
                if (rd.type == HX_RUN) run_call(rd.id, rd.buf, rd.len);
                else if (rd.type == HX_KILL) kill_call(rd.id);
                else { hl_chan_frame_reset(&rd); goto out; }   /* a second POLICY ends it */
                hl_chan_frame_reset(&rd);
                if (g_outq_n >= HX_OUTQ - 2) break;
            }
        }
        for (int k = 1; k < np; k++) {
            if (!(pf[k].revents & (POLLIN | POLLOUT | POLLHUP | POLLERR))) continue;
            if (map[k] == -2) goto out;       /* the server is gone */
            if (map[k] <= -3) {
                int ci = -3 - map[k];
                Sub *s = &g_subs[ci];
                while (s->fd >= 0 && g_outq_n < HX_OUTQ - 2) {
                    int src = hl_chan_frame_read_some(s->fd, &s->rd);
                    if (src == HL_CHAN_MORE) break;
                    if (src != HL_CHAN_DONE) { sub_lost(ci); break; }
                    sub_frame(ci, s->rd.type, s->rd.id, s->rd.buf, s->rd.len);
                    hl_chan_frame_reset(&s->rd);
                }
                continue;
            }
            Job *j = &g_jobs[map[k]];
            Cmd *cmd = &g_cmds[j->ci];
            if (pf[k].fd == j->pidfd) continue;   /* reap() below */
            if (pf[k].fd == j->in_fd) { feed(j); continue; }
            if (pf[k].fd == j->out_fd)
                drain(&j->out_fd, j->out, &j->out_len, cmd->c.stdout_max, &j->out_cut);
            else if (pf[k].fd == j->err_fd)
                drain(&j->err_fd, j->err, &j->err_len, cmd->c.stderr_max, &j->err_cut);
#ifdef HL_EXEC_TEST_HOOKS
            static const char mark[] = "HULL_EXEC_PROBE_READY\n";
            if (!j->probe_sent && j->out_len >= sizeof mark - 1 &&
                memmem(j->out, j->out_len, mark, sizeof mark - 1)) {
                j->probe_sent = 1;
                uint8_t pid4[4];
                put_u32(pid4, (uint32_t)j->pid);
                size_t fl;
                uint8_t *f = frame_make(HX_PROBE, j->id, pid4, 4, NULL, 0, &fl);
                if (f) {
                    for (int q = 0; q < HX_OUTQ; q++)
                        if (!g_outq[q].buf) {
                            g_outq[q] = (Out){ f, fl, 0 };
                            g_outq_n++;
                            g_order[g_order_n++] = -1 - q;
                            f = NULL;
                            break;
                        }
                    free(f);
                }
            }
#endif
        }
        reap();
        now = hl_chan_mono_ms();
        for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) {
            Job *j = &g_jobs[i];
            if (!j->used || j->routed) continue;   /* replied to by sub_frame */
            if (j->pid > 0 && !j->timed_out && now >= j->deadline) {
                kill(-j->pid, SIGKILL);
                j->timed_out = 1;
            }
            if (j->pid == 0 && j->out_fd < 0 && j->err_fd < 0 && !j->queued) {
                if (j->discard) job_free(j);
                else job_reply(j);
                if (j->used && j->discard && !j->queued) job_free(j);
            }
        }
        if (flush_out() != 0) break;
    }
out:
    kill_all();
    return 0;
}

/* ═════════════════════════════════════════════════════════════════════
 * The server's side
 * ═════════════════════════════════════════════════════════════════════ */

typedef struct {
    int          used;
    uint32_t     id;
    char         name[HL_EXEC_NAME_MAX + 1];
    HlExecDoneFn fn;
    void        *ud;
} Pending;

static int       g_fd = -1;
static pid_t     g_pid = -1;
static uint32_t  g_next_id = 1;
static int       g_ready;
static HlAsyncBackendCtx *g_loop;
static HlChanFrame g_rd = { .magic = HX_MAGIC, .max = HX_REPLY_MAX };
static Pending   g_pend[HL_EXEC_MAX_INFLIGHT];

static int env_allowed(const char *kv)
{
    static const char *const names[] = { "LANG", "TZ", NULL };
    for (int i = 0; names[i]; i++) {
        size_t n = strlen(names[i]);
        if (strncmp(kv, names[i], n) == 0 && kv[n] == '=') return 1;
    }
    return 0;
}

int hl_exec_broker_start(void)
{
    if (g_fd >= 0) return 0;
    const char *off = getenv("HULL_NO_EXEC_BROKER");
    if (off && off[0] && strcmp(off, "0") != 0) return -1;
    char exe[PATH_MAX];
    if (hl_chan_self_exe(exe, sizeof exe) != 0) return -1;
    int sv[2];
    /* No SOCK_CLOEXEC on macOS; this runs before any thread exists. */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    (void)fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(sv[1], F_SETFD, FD_CLOEXEC);
    char a1[] = HL_EXEC_BROKER_ARGV1;
    char *argv[] = { exe, a1, NULL };
    pid_t pid = hl_chan_spawn(exe, argv, sv[1], env_allowed);
    close(sv[1]);
    if (pid < 0) { close(sv[0]); return -1; }
    hl_chan_set_nonblock(sv[0]);
    hl_chan_set_nosigpipe(sv[0]);
    g_fd = sv[0];
    g_pid = pid;
#ifdef HL_EXEC_TEST_HOOKS
    log_info("[hull:c] exec broker pid %d", (int)pid);
#endif
    return 0;
}

int hl_exec_broker_running(void) { return g_fd >= 0 && g_ready; }
int hl_exec_broker_spawned(void) { return g_fd >= 0; }

static void fail_all(const char *why)
{
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) {
        Pending *p = &g_pend[i];
        if (!p->used) continue;
        HlExecDoneFn fn = p->fn;
        void *ud = p->ud;
        p->used = 0;
        HlExecReply *r = calloc(1, sizeof *r);
        if (!r) continue;
        r->kind = HL_EXEC_R_LOST;
        snprintf(r->msg, sizeof r->msg, "%s", why);
        fn(ud, r);
    }
}

void hl_exec_broker_stop(void)
{
    if (g_loop && g_fd >= 0) hl_async_backend()->watcher_del(g_loop, g_fd);
    g_loop = NULL;
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    g_ready = 0;
    hl_chan_frame_reset(&g_rd);
    if (g_pid > 0) {
        int st;
        /* It exits on the EOF, killing its children first. */
        if (!hl_chan_reap(g_pid, &st, 500, NULL)) {
            kill(g_pid, SIGKILL);
            (void)hl_chan_reap(g_pid, &st, 500, NULL);
        }
        g_pid = -1;
    }
    /* A stop is a teardown: every pending call is cancelled by its own path
     * (the server's connections, the runtime's detached ops), and its
     * cancel finds nothing to send now. Nothing is resumed from here. */
    memset(g_pend, 0, sizeof g_pend);
}

int hl_exec_broker_policy(const uint8_t *policy, size_t len, unsigned flags,
                          const char *app_dir, const char *state_dir,
                          const char (*dbs)[PATH_MAX], int n_dbs,
                          char *report, size_t report_size)
{
    report[0] = '\0';
    if (g_fd < 0) {
        snprintf(report, report_size, "exec: the command broker is not running");
        return -1;
    }
    /* u32 flags, the two roots (u16 length + bytes, "" = none), u16 count
     * and that many protected databases (u16 length + bytes), the policy. */
    size_t al = app_dir ? strlen(app_dir) : 0, sl = state_dir ? strlen(state_dir) : 0;
    if (al >= PATH_MAX || sl >= PATH_MAX || n_dbs < 0 || n_dbs > HL_EXEC_MAX_PROTECT) {
        snprintf(report, report_size, "exec: the app or state path is too long");
        return -1;
    }
    static uint8_t head[HX_POLICY_HEAD_MAX];
    size_t hn = 0;
    hl_chan_put32(head, flags);
    hn = 4;
    head[hn++] = (uint8_t)al; head[hn++] = (uint8_t)(al >> 8);
    if (al) memcpy(head + hn, app_dir, al);
    hn += al;
    head[hn++] = (uint8_t)sl; head[hn++] = (uint8_t)(sl >> 8);
    if (sl) memcpy(head + hn, state_dir, sl);
    hn += sl;
    head[hn++] = (uint8_t)n_dbs; head[hn++] = 0;
    for (int i = 0; i < n_dbs; i++) {
        size_t dl = strnlen(dbs[i], PATH_MAX - 1);
        head[hn++] = (uint8_t)dl; head[hn++] = (uint8_t)(dl >> 8);
        memcpy(head + hn, dbs[i], dl);
        hn += dl;
    }
    uint64_t deadline = hl_chan_mono_ms() + 10000;
    if (hl_chan_write_frame(g_fd, HX_MAGIC, HX_POLICY, 0, head, hn, policy, len,
                            deadline) != 0) {
        snprintf(report, report_size, "exec: the command broker did not take its policy");
        hl_exec_broker_stop();
        return -1;
    }
    HlChanFrame r = { .magic = HX_MAGIC, .max = 8192 };
    int rc = hl_chan_frame_read(g_fd, &r, deadline);
    if (rc != HL_CHAN_DONE || (r.type != HX_READY && r.type != HX_REFUSED)) {
        hl_chan_frame_reset(&r);
        snprintf(report, report_size, "exec: the command broker did not answer");
        hl_exec_broker_stop();
        return -1;
    }
    size_t n = r.len < report_size - 1 ? r.len : report_size - 1;
    if (n) memcpy(report, r.buf, n);
    report[n] = '\0';
    int ok = r.type == HX_READY;
    hl_chan_frame_reset(&r);
    if (!ok) { hl_exec_broker_stop(); return -1; }
    g_ready = 1;
    return 0;
}

static Pending *pend_find(uint32_t id)
{
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++)
        if (g_pend[i].used && g_pend[i].id == id) return &g_pend[i];
    return NULL;
}

static const char *sig_name(int s, char *buf, size_t n)
{
    switch (s) {
    case SIGKILL: return "SIGKILL";
    case SIGTERM: return "SIGTERM";
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS:  return "SIGBUS";
    case SIGXCPU: return "SIGXCPU";
    case SIGXFSZ: return "SIGXFSZ";
    case SIGSYS:  return "SIGSYS";
    case SIGPIPE: return "SIGPIPE";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    case SIGINT:  return "SIGINT";
    case SIGHUP:  return "SIGHUP";
    default: snprintf(buf, n, "SIG%d", s); return buf;
    }
}

static void deliver(uint32_t type, uint32_t id, const uint8_t *p, size_t len)
{
    if (type == HX_PROBE) {
#ifdef HL_EXEC_TEST_HOOKS
        if (len == 4) log_info("[hull:c] exec probe ready pid=%u", hl_chan_get32(p));
#endif
        return;
    }
    Pending *pd = pend_find(id);
    if (!pd) return;                       /* cancelled meanwhile */
    HlExecReply *r = calloc(1, sizeof *r);
    HlExecDoneFn fn = pd->fn;
    void *ud = pd->ud;
    pd->used = 0;
    if (!r) return;
    if (type == HX_DONE && len >= 24) {
        r->kind = HL_EXEC_R_DONE;
        r->status = (int32_t)hl_chan_get32(p);
        int signo = (int)hl_chan_get32(p + 4);
        if (signo) {
            char b[16];
            snprintf(r->signal, sizeof r->signal, "%s", sig_name(signo, b, sizeof b));
        }
        r->timed_out = p[8];
        r->out_cut = p[9];
        r->err_cut = p[10];
        r->duration_ms = hl_chan_get32(p + 12);
        size_t ol = hl_chan_get32(p + 16);
        if (ol <= len - 24) {
            size_t el = hl_chan_get32(p + 20 + ol);
            if (20 + ol + 4 + el == len) {
                r->out = malloc(ol ? ol : 1);
                r->err = malloc(el ? el : 1);
                if (r->out && r->err) {
                    if (ol) memcpy(r->out, p + 20, ol);
                    if (el) memcpy(r->err, p + 24 + ol, el);
                    r->out_len = ol;
                    r->err_len = el;
                    fn(ud, r);
                    return;
                }
            }
        }
        free(r->out); free(r->err);
        r->out = r->err = NULL;
        r->kind = HL_EXEC_R_LOST;
        snprintf(r->msg, sizeof r->msg, "exec: a malformed reply from the broker");
    } else {
        r->kind = type == HX_BUSY ? HL_EXEC_R_BUSY : HL_EXEC_R_REFUSED;
        size_t n = len < sizeof r->msg - 1 ? len : sizeof r->msg - 1;
        if (n) memcpy(r->msg, p, n);
        r->msg[n] = '\0';
    }
    fn(ud, r);
}

static void on_readable(int fd, unsigned ready, void *user)
{
    (void)ready; (void)user;
    for (;;) {
        int rc = hl_chan_frame_read_some(fd, &g_rd);
        if (rc == HL_CHAN_MORE) return;
        if (rc != HL_CHAN_DONE) {
            log_error("[hull:c] exec: the command broker is gone; exec.run fails "
                      "from now on");
            /* Lost while serving: the calls waiting on it fail now. */
            HlAsyncBackendCtx *loop = g_loop;
            hl_async_backend()->watcher_del(loop, fd);
            g_loop = NULL;
            fail_all("exec: the command broker is gone");
            hl_exec_broker_stop();
            return;
        }
        deliver(g_rd.type, g_rd.id, g_rd.buf, g_rd.len);
        hl_chan_frame_reset(&g_rd);
    }
}

int hl_exec_submit(HlAsyncBackendCtx *loop, const char *name, size_t name_len,
                   const char *const *args, const size_t *lens, size_t n,
                   const void *in, size_t in_len,
                   HlExecDoneFn fn, void *ud, uint32_t *id_out,
                   char *why, size_t why_size)
{
    if (g_fd < 0 || !g_ready) {
        snprintf(why, why_size, "exec: the command broker is not running");
        return -1;
    }
    if (!g_loop) {
        if (!loop || hl_async_backend()->watcher_add(loop, g_fd, HL_ASYNC_READ,
                                                     on_readable, NULL) != 0) {
            snprintf(why, why_size, "exec: cannot watch the command broker");
            return -1;
        }
        g_loop = loop;
    }
    Pending *slot = NULL;
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++)
        if (!g_pend[i].used) { slot = &g_pend[i]; break; }
    if (!slot || name_len > HL_EXEC_NAME_MAX || n > HL_EXEC_MAX_CALL_ARGS) {
        snprintf(why, why_size, "exec: too many calls in flight");
        return -1;
    }
    if (in_len > HL_EXEC_STDIN_CEIL) {
        snprintf(why, why_size, "exec: stdin is longer than %u bytes",
                 (unsigned)HL_EXEC_STDIN_CEIL);
        return -1;
    }
    size_t total = 4 + name_len + 4;
    for (size_t i = 0; i < n; i++) total += 4 + lens[i];
    uint8_t *buf = malloc(total);
    if (!buf) { snprintf(why, why_size, "exec: out of memory"); return -1; }
    size_t off = 0;
    buf[off++] = (uint8_t)name_len;
    buf[off++] = (uint8_t)(name_len >> 8);
    memcpy(buf + off, name, name_len);
    off += name_len;
    buf[off++] = (uint8_t)n;
    buf[off++] = (uint8_t)(n >> 8);
    for (size_t i = 0; i < n; i++) {
        hl_chan_put32(buf + off, (uint32_t)lens[i]);
        off += 4;
        if (lens[i]) memcpy(buf + off, args[i], lens[i]);
        off += lens[i];
    }
    hl_chan_put32(buf + off, (uint32_t)in_len);   /* the stdin follows, uncopied */
    off += 4;
    uint32_t id = g_next_id++;
    if (g_next_id == 0) g_next_id = 1;
    int rc = hl_chan_write_frame(g_fd, HX_MAGIC, HX_RUN, id, buf, off,
                                 in_len ? in : NULL, in_len, hl_chan_mono_ms() + 1000);
    free(buf);
    if (rc != 0) {
        snprintf(why, why_size, "exec: the command broker is not answering");
        return -1;
    }
    slot->used = 1;
    slot->id = id;
    memcpy(slot->name, name, name_len);
    slot->name[name_len] = '\0';
    slot->fn = fn;
    slot->ud = ud;
    *id_out = id;
    return 0;
}

void hl_exec_cancel(uint32_t id)
{
    Pending *p = pend_find(id);
    if (!p) return;
    p->used = 0;
    if (g_fd >= 0)
        (void)hl_chan_write_frame(g_fd, HX_MAGIC, HX_KILL, id, NULL, 0, NULL, 0,
                                  hl_chan_mono_ms() + 200);
}


/* ── One exec.run as an async op ───────────────────────────────────── */

static void exec_op_free(void *driver)
{
    HlExecOp *op = driver;
    if (!op) return;
    if (op->pending) hl_exec_cancel(op->id);   /* the broker kills the child */
    hl_exec_reply_free(op->reply);
    free(op);
}

static void exec_op_done(void *ud, HlExecReply *r)
{
    HlAsyncCtx *ctx = ud;
    HlExecOp *op = ctx->driver;
    op->pending = 0;
    op->reply = r;
    if (ctx->detached) hl_async_ctx_resume_detached(ctx);
    else hl_net_op_complete(ctx->net_ctx, (HlSuspendOp *)&ctx->op);
}

/* The broker never answered an attached call (it enforces each command's
 * timeout itself, so this is a wedged broker). */
static void exec_op_deadline(KlAsyncOp *kop, void *user_data)
{
    HlAsyncCtx *ctx = user_data;
    HlExecOp *op = ctx->driver;
    if (op && op->pending) {
        hl_exec_cancel(op->id);
        op->pending = 0;
        op->reply = calloc(1, sizeof *op->reply);
        if (op->reply) {
            op->reply->kind = HL_EXEC_R_LOST;
            snprintf(op->reply->msg, sizeof op->reply->msg,
                     "exec: the command broker did not answer");
        }
    }
    hl_net_op_complete(ctx->net_ctx, (HlSuspendOp *)kop);
}

int hl_exec_op_start(HlAsyncCtx *ctx, HlAsyncBackendCtx *loop,
                     const char *name, size_t name_len,
                     const char *const *args, const size_t *lens, size_t n,
                     const void *in, size_t in_len,
                     uint32_t timeout_ms, char *why, size_t why_size)
{
    HlExecOp *op = calloc(1, sizeof *op);
    if (!op) { snprintf(why, why_size, "exec: out of memory"); return -1; }
    if (hl_exec_submit(loop, name, name_len, args, lens, n, in, in_len, exec_op_done, ctx,
                       &op->id, why, why_size) != 0) {
        free(op);
        return -1;
    }
    op->pending = 1;
    ctx->driver = op;
    ctx->free_driver = exec_op_free;
    if (!ctx->detached) {
        ctx->op.deadline_ms = hl_async_backend()->monotonic_ms() + timeout_ms + 5000;
        ctx->op.on_deadline = exec_op_deadline;
    }
    return 0;
}

int hl_exec_inflight(void)
{
    int n = 0;
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) n += g_pend[i].used;
    return n;
}

int hl_exec_inflight_cmd(const char *name, size_t name_len)
{
    int n = 0;
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++)
        if (g_pend[i].used && strlen(g_pend[i].name) == name_len &&
            memcmp(g_pend[i].name, name, name_len) == 0) n++;
    return n;
}

#else /* !HL_EXEC_BROKER_SUPPORTED */

int hl_exec_broker_main(void) { return 2; }
int hl_exec_broker_start(void) { return -1; }
int hl_exec_broker_running(void) { return 0; }
int hl_exec_broker_spawned(void) { return 0; }
void hl_exec_broker_stop(void) {}
int hl_exec_broker_policy(const uint8_t *policy, size_t len, unsigned flags,
                          const char *app_dir, const char *state_dir,
                          const char (*dbs)[PATH_MAX], int n_dbs,
                          char *report, size_t report_size)
{
    (void)policy; (void)len; (void)flags; (void)app_dir; (void)state_dir; (void)dbs;
    (void)n_dbs;
    snprintf(report, report_size, "exec: this host has no command broker");
    return -1;
}
int hl_exec_submit(HlAsyncBackendCtx *loop, const char *name, size_t name_len,
                   const char *const *args, const size_t *lens, size_t n,
                   const void *in, size_t in_len,
                   HlExecDoneFn fn, void *ud, uint32_t *id_out,
                   char *why, size_t why_size)
{
    (void)loop; (void)name; (void)name_len; (void)args; (void)lens; (void)n;
    (void)in; (void)in_len; (void)fn; (void)ud; (void)id_out;
    snprintf(why, why_size, "exec: this host has no command broker");
    return -1;
}
void hl_exec_cancel(uint32_t id) { (void)id; }
int hl_exec_op_start(HlAsyncCtx *ctx, HlAsyncBackendCtx *loop,
                     const char *name, size_t name_len,
                     const char *const *args, const size_t *lens, size_t n,
                     const void *in, size_t in_len,
                     uint32_t timeout_ms, char *why, size_t why_size)
{
    (void)ctx; (void)loop; (void)name; (void)name_len; (void)args; (void)lens;
    (void)n; (void)in; (void)in_len; (void)timeout_ms;
    snprintf(why, why_size, "exec: this host has no command broker");
    return -1;
}
int hl_exec_inflight(void) { return 0; }
int hl_exec_inflight_cmd(const char *name, size_t name_len)
{
    (void)name; (void)name_len;
    return 0;
}

#endif /* HL_EXEC_BROKER_SUPPORTED */
