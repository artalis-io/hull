/*
 * cap/exec_broker.c - the command broker and the server's side of it
 * (docs/design/exec_broker.md §5 - §7; header: hull/cap/exec_broker.h)
 *
 * Frames (child_channel.h, magic "HXEC"):
 *
 *   server -> broker   POLICY  u32 flags, the encoded policy (cap/exec.h)
 *                      RUN     id; u16 name_len, name, u16 n,
 *                              (u32 len, bytes) * n, u32 stdin_len, stdin,
 *                              u8 flags (HX_RUN_STREAM, HX_RUN_STDIN_OPEN)
 *                      KILL    id
 *                      CREDIT  id; u8 fd (1 stdout, 2 stderr), u32 bytes
 *                      STDIN   id; u8 flags (0x1 EOF after), bytes
 *                      PING    0; u32 silence_ms, u32 wait_ms (the heartbeat
 *                              the sender keeps; the router holds its
 *                              brokers to the same)
 *   broker -> server   READY   text: one report line per '\n'
 *                      REFUSED id (0 = the policy); text
 *                      BUSY    id; text
 *                      DONE    id; u32 status, u32 signo, u8 timed_out
 *                              (1 its timeout, 2 a stream's idle limit),
 *                              u8 out_cut, u8 err_cut, u8 0, u32 duration_ms,
 *                              u32 out_len, out, u32 err_len, err
 *                              (a stream's DONE: byte 11 is 1 and the two
 *                              outputs are replaced by u64 out_bytes,
 *                              u64 err_bytes)
 *                      CHUNK   id; u8 fd, bytes      (a stream's output)
 *                      STDIN_CREDIT id; u8 flags (0x1 the child closed its
 *                              stdin), u32 bytes
 *                      PROBE   id; u32 pid            (test builds only)
 *                      PONG    0                      (the answer to PING)
 *
 * The broker trusts nothing the server sends after READY: every RUN is
 * matched again, admission (in-flight calls, per-command concurrency, live
 * ids, buffered output) is decided here, and a second POLICY ends it.
 *
 * A stream (docs/design/exec_stream.md) never holds more than
 * HL_EXEC_STREAM_WINDOW per pipe: output is read only while the server has
 * credit for it, and stdin is taken only within the credit granted. Credit or
 * stdin past those bounds is a protocol error that ends the call.
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
#include "hull/shared/offheap.h"  /* a stream's queued bytes on the VM's total */
#include "hull/cap/audit.h"        /* --audit records for exec.run / exec.stream */
#include "hull/shared/cli_log.h"   /* hl_log_escape_line */
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
#define HX_CHUNK   9u
#define HX_CREDIT 10u
#define HX_STDIN  11u
#define HX_STDIN_CREDIT 12u
#define HX_PING   13u
#define HX_PONG   14u

#define HX_RUN_STREAM     0x1u
#define HX_RUN_STDIN_OPEN 0x2u
#define HX_STDIN_EOF      0x1u
#define HX_STDIN_CLOSED   0x1u   /* STDIN_CREDIT: the child closed its stdin */

#define HX_RUN_MAX    (HL_EXEC_MAX_ARG_BYTES + 4u * HL_EXEC_MAX_CALL_ARGS + \
                       HL_EXEC_STDIN_CEIL + 129u)
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

/* A frame waiting to go to the server for one call. A call's frames go out
 * in order (its CHUNKs, then its DONE), interleaved with other calls'. */
typedef struct {
    uint8_t *buf; size_t len, cap, off;
    uint32_t type;
    uint8_t  fd;                /* CHUNK: 1 / 2; STDIN_CREDIT: its flags */
    uint8_t  final;             /* the call's last frame: delivered, the job frees */
} JobOut;
/* Per stream pipe, at most WINDOW / CHUNK_MAX full CHUNKs and one part-full
 * one wait (a new CHUNK is merged into the last one while it fits and has
 * not started), plus the one being sent; one STDIN_CREDIT (merged likewise);
 * the final frame. */
#define HX_JOBQ ((int)(2 * (HL_EXEC_STREAM_WINDOW / HL_EXEC_STREAM_CHUNK_MAX + 2) + 4))

/* One stream output pipe (index 0 stdout, 1 stderr). */
typedef struct {
    size_t   credit;            /* bytes the server will still take */
    size_t   wire;              /* bytes sent (whole CHUNKs) not yet credited back:
                                 * the server may credit only these */
    uint64_t total;             /* bytes read so far (within the cap) */
    uint64_t cap;               /* HL_EXEC_STREAM_NO_CAP = none */
    uint8_t *open;              /* bytes read, not yet a CHUNK (CHUNK_MAX) */
    size_t   open_len;
    uint64_t open_ms;           /* when its first byte was read */
} StreamOut;

typedef struct {
    int      used;
    uint32_t id;
    int      ci;
    pid_t    pid;               /* 0 once reaped (its keeper's, under one) */
    pid_t    job_pid;           /* the command itself (its keeper's child) */
    int      kill_fd;           /* under a keeper: closing it ends the call */
    int      pidfd;             /* readable once it exits; -1 = none */
    int      out_fd, err_fd;    /* -1 once closed */
    int      in_fd;             /* the child's stdin pipe, -1 once closed */
    uint8_t *in;   size_t in_len, in_off, in_cap;
    uint8_t *out;  size_t out_len;  int out_cut;
    uint8_t *err;  size_t err_len;  int err_cut;
    uint64_t start_ms, deadline;   /* deadline UINT64_MAX = none */
    int      timed_out;         /* 1 its time limit, 2 its idle limit */
    int      status, signo;
    int      discard;           /* KILLed: no reply */
    JobOut   q[HX_JOBQ];        /* FIFO, q[0] goes out first */
    int      qn;
    int      queued;            /* the final frame is queued */
    size_t   reserved;          /* what admission counted for it, held until freed */
    int      routed;            /* run by its command's own broker (the router) */
    /* A stream (HX_RUN_STREAM). The router keeps the same counts for a
     * routed one, to hold its command's broker to the windows too. */
    int       stream;
    StreamOut so[2];
    int       in_open;          /* HX_RUN_STDIN_OPEN: STDIN frames are taken */
    int       in_eof;           /* the server sent EOF: no more STDIN */
    int       in_closed;        /* the child closed its stdin (or exited) */
    int       in_closed_tell;   /* ...and the server has not been told yet */
    const char *fail_msg;       /* call_fail: REFUSED once the child is gone */
    size_t    in_granted;       /* stdin credit the server has not used */
    size_t    in_init_left;     /* bytes of the RUN's own stdin not yet written */
    size_t    in_ret;           /* stdin credit to give back */
    uint64_t  in_total;         /* streamed stdin taken so far */
    uint64_t  in_total_max;
    /* A stream's idle limit (HL_EXEC_CMD_IDLE; exec_stream.md §5a): the
     * command is ended once it has written nothing for idle_ms while Hull was
     * ready to read. The clock stands still while a pipe waits on the app's
     * credit (back pressure is the app's, not the command's silence). */
    uint32_t  idle_ms;          /* 0 = none */
    uint64_t  idle_last;        /* the last output, or the end of a wait on credit */
    int       idle_held;        /* a pipe was waiting on credit at the last look */
#ifdef HL_EXEC_TEST_HOOKS
    int      probe_sent;
#endif
} Job;

static Job    g_jobs[HL_EXEC_MAX_INFLIGHT];
static size_t g_reserved;
static int    g_chan = 3;

/* A frame waiting to go to a command's broker. The router never blocks on
 * one broker (#1234): a broker busy starting its calls (each RUN may wait up
 * to 2 s on its error pipe) stalled every other command's replies, and a
 * write that timed out lost the command for good. */
typedef struct {
    uint8_t *buf; size_t len, cap, off;
    uint32_t type, id;
    uint8_t  fd;                /* CREDIT: the pipe */
    size_t   hold;              /* bytes counted in g_reserved for it, if any */
} SubOut;
/* Per call in flight: its RUN, a KILL, one CREDIT per pipe (merged) and
 * its waiting STDIN (full frames, one part-full, one being sent). */
#define HX_SUBQ ((int)(HL_EXEC_MAX_INFLIGHT * \
                       (4 + HL_EXEC_STREAM_WINDOW / HL_EXEC_STREAM_CHUNK_MAX + 2)))
/* A broker that has taken no byte of a waiting frame for this long is
 * wedged (it reads every frame as it comes, after at most ~4 s of starting
 * each of its HL_EXEC_MAX_INFLIGHT calls). */
#define HX_SUB_STALL_MS 60000u

/* The router (macOS, two or more commands; exec_platform.h
 * hl_exec_plat_router): command i runs in broker g_subs[i], a fresh copy of
 * this binary holding that command's policy alone. The router admits and
 * matches each call, forwards it, and relays the reply
 * (docs/design/exec_broker_macos_isolation.md §4). */
typedef struct {
    int         fd;             /* -1 once lost */
    pid_t       pid;
    HlChanFrame rd;
    SubOut      q[HX_SUBQ];     /* FIFO, q[0] goes out first */
    int         qn;
    uint64_t    progress_ms;    /* when its queue last moved (or filled) */
    uint64_t    rx_ms;          /* when it last sent a frame (or was last idle) */
    uint64_t    ping_ms;        /* its unanswered PING, 0 = none */
} Sub;
static int    g_router;
static Sub    g_subs[HL_EXEC_MAX_COMMANDS];
/* The heartbeat the server keeps on this broker (its PING says), which the
 * router keeps on its own brokers. */
static uint32_t g_hb_ms = HL_EXEC_HEARTBEAT_MS;
static uint32_t g_hb_wait_ms = HL_EXEC_HEARTBEAT_WAIT_MS;

/* Small frames (BUSY / REFUSED / PROBE) waiting to go out. */
typedef struct { uint8_t *buf; size_t len, off; } Out;
#define HX_OUTQ 64
static Out    g_outq[HX_OUTQ];
static int    g_outq_n;
/* What goes out next: -1-k = small frame k, j = the next frame of job j
 * (one entry per frame in its queue). */
static int    g_order[HX_OUTQ + HL_EXEC_MAX_INFLIGHT * HX_JOBQ];
static int    g_order_n;

/* A frame with room for @p cap payload bytes (at least alen + blen). */
static uint8_t *frame_make_cap(uint32_t type, uint32_t id, const void *a, size_t alen,
                               const void *b, size_t blen, size_t cap, size_t *len)
{
    if (cap < alen + blen) cap = alen + blen;
    size_t n = HL_CHAN_HDR + alen + blen;
    uint8_t *f = malloc(HL_CHAN_HDR + cap);
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

static uint8_t *frame_make(uint32_t type, uint32_t id, const void *a, size_t alen,
                           const void *b, size_t blen, size_t *len)
{
    return frame_make_cap(type, id, a, alen, b, blen, 0, len);
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

/* PING from the server (or the router): answer from this loop, which is
 * what the heartbeat checks, and keep the sender's timing for our own
 * brokers (a router's). */
static void hb_answer(const uint8_t *p, size_t len)
{
    if (len == 8) {
        uint32_t ms = hl_chan_get32(p), wait = hl_chan_get32(p + 4);
        if (ms >= 50 && ms <= HL_EXEC_HEARTBEAT_MS) g_hb_ms = ms;
        if (wait >= 50 && wait <= HL_EXEC_HEARTBEAT_WAIT_MS) g_hb_wait_ms = wait;
    }
    queue_frame(HX_PONG, 0, NULL, 0);
}

static void job_free(Job *j);

/* Append @p f (its whole length @p len, room @p cap) to job @p j's queue.
 * -1 when the queue is full (the frame is freed). */
static int job_push(Job *j, uint8_t *f, size_t len, size_t cap, uint32_t type,
                    uint8_t fd, int final)
{
    if (j->qn >= HX_JOBQ) { free(f); return -1; }
    j->q[j->qn++] = (JobOut){ f, len, cap, 0, type, fd, (uint8_t)final };
    g_order[g_order_n++] = (int)(j - g_jobs);
    if (final) j->queued = 1;
    return 0;
}

/* Drop job @p j's frames that have not started on the wire (every one but a
 * part-sent q[0]), and their places in g_order (its last entries there). */
static void job_drop_unsent(Job *j)
{
    int keep = (j->qn > 0 && j->q[0].off > 0) ? 1 : 0;
    int drop = j->qn - keep;
    int idx = (int)(j - g_jobs);
    for (int k = g_order_n - 1; k >= 0 && drop > 0; k--)
        if (g_order[k] == idx) {
            memmove(g_order + k, g_order + k + 1, (size_t)(g_order_n - k - 1) * sizeof g_order[0]);
            g_order_n--;
            drop--;
        }
    for (int k = keep; k < j->qn; k++) {
        if (j->q[k].final) j->queued = 0;
        free(j->q[k].buf);
    }
    j->qn = keep;
}

/* The final frame of job @p j: its reply, or one the router makes. When it
 * cannot be held, the slot frees now (nothing else would) and a small frame
 * says so. */
static void job_reply_frame(Job *j, uint32_t type, const void *body, size_t len)
{
    size_t fl;
    uint8_t *f = frame_make(type, j->id, body, len, NULL, 0, &fl);
    if (!f || j->qn >= HX_JOBQ) {
        free(f);
        uint32_t id = j->id;
        job_drop_unsent(j);
        if (j->qn == 0) job_free(j);
        else j->discard = 1;
        queue_small(HX_REFUSED, id, "exec: out of memory");
        return;
    }
    (void)job_push(j, f, fl, fl - HL_CHAN_HDR, type, 0, 1);
}

/* Bytes for stream pipe @p fd (1 / 2) of job @p j, as CHUNK frames: merged
 * into its last CHUNK of that pipe while it fits and has not started, else
 * new frames. The bytes that did not fit (a full queue, out of memory), or 0. */
static size_t chunk_push(Job *j, uint8_t fd, const uint8_t *data, size_t n)
{
    for (int k = j->qn - 1; k >= 0 && n > 0; k--) {
        JobOut *o = &j->q[k];
        if (o->type != HX_CHUNK || o->fd != fd) continue;
        if (o->off == 0) {
            size_t have = o->len - HL_CHAN_HDR, room = o->cap - have;
            size_t take = n < room ? n : room;
            if (take) {
                memcpy(o->buf + o->len, data, take);
                o->len += take;
                hl_chan_put32(o->buf + 12, (uint32_t)(have + take));
                data += take;
                n -= take;
            }
        }
        break;                   /* only the last CHUNK of the pipe */
    }
    while (n > 0) {
        if (j->qn >= HX_JOBQ - 1) return n;   /* the final frame's slot stays free */
        size_t take = n < HL_EXEC_STREAM_CHUNK_MAX ? n : HL_EXEC_STREAM_CHUNK_MAX;
        size_t fl;
        uint8_t *f = frame_make_cap(HX_CHUNK, j->id, &fd, 1, data, take,
                                    1 + HL_EXEC_STREAM_CHUNK_MAX, &fl);
        if (!f) return n;
        (void)job_push(j, f, fl, 1 + HL_EXEC_STREAM_CHUNK_MAX, HX_CHUNK, fd, 0);
        data += take;
        n -= take;
    }
    return 0;
}

/* Give the server back @p bytes of stdin credit (and the CLOSED flag), merged
 * into a waiting STDIN_CREDIT of job @p j. 0, or -1 (queue full: kept for later). */
static int stdin_credit_push(Job *j, uint8_t flags, uint32_t bytes)
{
    for (int k = j->qn - 1; k >= 0; k--) {
        JobOut *o = &j->q[k];
        if (o->type != HX_STDIN_CREDIT || o->off != 0) continue;
        uint8_t *pl = o->buf + HL_CHAN_HDR;
        pl[0] |= flags;
        hl_chan_put32(pl + 1, hl_chan_get32(pl + 1) + bytes);
        return 0;
    }
    if (j->qn >= HX_JOBQ - 1) return -1;
    uint8_t pl[5];
    pl[0] = flags;
    hl_chan_put32(pl + 1, bytes);
    size_t fl;
    uint8_t *f = frame_make(HX_STDIN_CREDIT, j->id, pl, 5, NULL, 0, &fl);
    if (!f) return -1;
    return job_push(j, f, fl, 5, HX_STDIN_CREDIT, 0, 0);
}

static void job_free(Job *j)
{
    if (j->kill_fd >= 0) close(j->kill_fd);
    if (j->pidfd >= 0) close(j->pidfd);
    if (j->in_fd >= 0) close(j->in_fd);
    free(j->in);
    if (j->out_fd >= 0) close(j->out_fd);
    if (j->err_fd >= 0) close(j->err_fd);
    free(j->out);
    free(j->err);
    free(j->so[0].open);
    free(j->so[1].open);
    job_drop_unsent(j);
    /* A part-sent frame cannot be taken back: the caller frees a job only
     * once its queue is empty or the channel is gone. */
    for (int k = 0; k < j->qn; k++) free(j->q[k].buf);
    int idx = (int)(j - g_jobs);
    for (int k = g_order_n - 1; k >= 0; k--)
        if (g_order[k] == idx) {
            memmove(g_order + k, g_order + k + 1, (size_t)(g_order_n - k - 1) * sizeof g_order[0]);
            g_order_n--;
        }
    if (j->ci >= 0 && g_cmds[j->ci].running > 0) g_cmds[j->ci].running--;
    g_reserved -= j->reserved;
    memset(j, 0, sizeof *j);
    j->out_fd = j->err_fd = j->pidfd = j->in_fd = j->kill_fd = -1;
    j->ci = -1;
}

static void put_u32(uint8_t *p, uint32_t v) { hl_chan_put32(p, v); }

static void put_u64(uint8_t *p, uint64_t v)
{
    hl_chan_put32(p, (uint32_t)v);
    hl_chan_put32(p + 4, (uint32_t)(v >> 32));
}

static void job_reply(Job *j)
{
    uint8_t head[36];
    put_u32(head, (uint32_t)j->status);
    put_u32(head + 4, (uint32_t)j->signo);
    head[8] = (uint8_t)j->timed_out;
    head[9] = (uint8_t)j->out_cut;
    head[10] = (uint8_t)j->err_cut;
    head[11] = (uint8_t)(j->stream ? 1 : 0);
    put_u32(head + 12, (uint32_t)(hl_chan_mono_ms() - j->start_ms));
    if (j->stream) {
        put_u64(head + 16, j->so[0].total);
        put_u64(head + 24, j->so[1].total);
        job_reply_frame(j, HX_DONE, head, 32);
        return;
    }
    put_u32(head + 16, (uint32_t)j->out_len);
    size_t n = 20 + j->out_len + 4 + j->err_len;
    uint8_t *body = malloc(n ? n : 1);
    if (!body) { j->discard = 1; return; }
    memcpy(body, head, 20);
    if (j->out_len) memcpy(body + 20, j->out, j->out_len);
    put_u32(body + 20 + j->out_len, (uint32_t)j->err_len);
    if (j->err_len) memcpy(body + 24 + j->out_len, j->err, j->err_len);
    job_reply_frame(j, HX_DONE, body, n);
    free(body);
}

/* Is job @p j done with its command and its pipes (nothing more to read)? */
static int job_settled(const Job *j)
{
    return j->pid == 0 && j->out_fd < 0 && j->err_fd < 0 &&
           j->so[0].open_len == 0 && j->so[1].open_len == 0;
}

/* Write what the channel takes. -1 when the server is gone. */
static int flush_out(void)
{
    while (g_order_n > 0) {
        int o = g_order[0];
        uint8_t *buf; size_t len, *off;
        if (o < 0) { Out *q = &g_outq[-1 - o]; buf = q->buf; len = q->len; off = &q->off; }
        else { JobOut *q = &g_jobs[o].q[0]; buf = q->buf; len = q->len; off = &q->off; }
        while (*off < len) {
            ssize_t w = send(g_chan, buf + *off, len - *off, MSG_NOSIGNAL | MSG_DONTWAIT);
            if (w < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                if (errno == EINTR) continue;
                return -1;
            }
            *off += (size_t)w;
        }
        memmove(g_order, g_order + 1, (size_t)(--g_order_n) * sizeof g_order[0]);
        if (o < 0) { free(g_outq[-1 - o].buf); g_outq[-1 - o].buf = NULL; g_outq_n--; continue; }
        Job *j = &g_jobs[o];
        int final = j->q[0].final;
        if (j->q[0].type == HX_CHUNK && (j->q[0].fd == 1 || j->q[0].fd == 2))
            j->so[j->q[0].fd - 1].wire += j->q[0].len - HL_CHAN_HDR - 1;
        free(j->q[0].buf);
        memmove(j->q, j->q + 1, (size_t)(--j->qn) * sizeof j->q[0]);
        /* Delivered: its admission slot frees (a routed call's broker has
         * already ended it; a discarded one's command is gone). */
        if (final || (j->discard && j->qn == 0 && (j->routed || job_settled(j))))
            job_free(j);
    }
    return 0;
}

/* End job @p j's command: through its keeper (close the kill pipe: the
 * keeper kills and reaps it), else its process group. */
static void job_kill(Job *j)
{
    if (j->kill_fd >= 0) { close(j->kill_fd); j->kill_fd = -1; }
    else if (j->pid > 0) kill(-j->pid, SIGKILL);
}

static void kill_all(void)
{
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++)
        if (g_jobs[i].used && g_jobs[i].pid > 0) job_kill(&g_jobs[i]);
}

/* ── The router's side of a call ───────────────────────────────────── */

/* Command @p ci's broker is gone (EOF, a write it did not take, a frame it
 * should not send): every call routed to it that has no reply yet fails,
 * and later calls to it are refused. Its running calls are not the
 * router's to end: each one's keeper sees that broker go and ends it, however
 * it went (exec_broker_macos_isolation.md §4.6). */
static void sub_lost(int ci)
{
    Sub *s = &g_subs[ci];
    if (s->fd < 0) return;
    close(s->fd);
    s->fd = -1;
    hl_chan_frame_reset(&s->rd);
    for (int k = 0; k < s->qn; k++) {
        g_reserved -= s->q[k].hold;
        free(s->q[k].buf);
    }
    s->qn = 0;
    char msg[160];
    snprintf(msg, sizeof msg, "exec.%s: the command broker is gone", g_cmds[ci].c.name);
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) {
        Job *j = &g_jobs[i];
        if (!j->used || !j->routed || j->ci != ci || j->queued) continue;
        if (j->discard) {
            job_drop_unsent(j);
            if (j->qn == 0) job_free(j);
        } else {
            job_reply_frame(j, HX_REFUSED, msg, strlen(msg));
        }
    }
}

static int sub_queue(int ci, uint32_t type, uint32_t id, const void *p, size_t len);
static int sub_flush(int ci);

/* Command @p ci's broker has a call of this router in flight. */
static int sub_busy(int ci)
{
    if (g_subs[ci].fd < 0) return 0;
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++)
        if (g_jobs[i].used && g_jobs[i].routed && g_jobs[i].ci == ci && !g_jobs[i].queued)
            return 1;
    return 0;
}

/* The router's heartbeat on command @p ci's broker: PING after g_hb_ms of
 * silence while it has calls, lost when nothing comes g_hb_wait_ms after. An
 * idle broker is not pinged (its clock restarts with its next call). */
static void sub_heartbeat(int ci, uint64_t now)
{
    Sub *s = &g_subs[ci];
    if (!sub_busy(ci)) { s->rx_ms = now; s->ping_ms = 0; return; }
    if (s->ping_ms) {
        if (now - s->ping_ms >= g_hb_wait_ms) sub_lost(ci);   /* wedged */
        return;
    }
    if (now - s->rx_ms < g_hb_ms) return;
    uint8_t pl[8];
    hl_chan_put32(pl, g_hb_ms);
    hl_chan_put32(pl + 4, g_hb_wait_ms);
    /* A full queue is the stall check's (HX_SUB_STALL_MS), not ours. */
    if (sub_queue(ci, HX_PING, 0, pl, sizeof pl) != 0) return;
    s->ping_ms = now;
    if (sub_flush(ci) != 0) sub_lost(ci);
}

/* Queue a frame for command @p ci's broker, with room for @p cap payload
 * bytes. -1 when its queue is full or memory is short (neither happens with
 * the admission caps and the windows: see HX_SUBQ). */
static int sub_queue_cap(int ci, uint32_t type, uint32_t id, uint8_t fd,
                         const void *a, size_t alen, const void *b, size_t blen,
                         size_t cap)
{
    Sub *s = &g_subs[ci];
    if (s->fd < 0 || s->qn >= HX_SUBQ) return -1;
    size_t n;
    uint8_t *f = frame_make_cap(type, id, a, alen, b, blen, cap, &n);
    if (!f) return -1;
    if (s->qn == 0) s->progress_ms = hl_chan_mono_ms();
    if (cap < alen + blen) cap = alen + blen;
    s->q[s->qn++] = (SubOut){ f, n, cap, 0, type, id, fd, 0 };
    return 0;
}

static int sub_queue(int ci, uint32_t type, uint32_t id, const void *p, size_t len)
{
    return sub_queue_cap(ci, type, id, 0, p, len, NULL, 0, 0);
}

/* Forward @p bytes of credit for pipe @p fd of call @p id, merged into a
 * CREDIT for it that has not started. */
static int sub_credit(int ci, uint32_t id, uint8_t fd, uint32_t bytes)
{
    Sub *s = &g_subs[ci];
    for (int k = s->qn - 1; k >= 0; k--) {
        SubOut *o = &s->q[k];
        if (o->type != HX_CREDIT || o->id != id || o->fd != fd || o->off) continue;
        uint8_t *pl = o->buf + HL_CHAN_HDR;
        hl_chan_put32(pl + 1, hl_chan_get32(pl + 1) + bytes);
        return 0;
    }
    uint8_t pl[5];
    pl[0] = fd;
    hl_chan_put32(pl + 1, bytes);
    return sub_queue(ci, HX_CREDIT, id, pl, 5);
}

/* Forward stdin bytes (and EOF) for call @p id, merged into its last STDIN
 * while that has not started and fits. */
static int sub_stdin(int ci, uint32_t id, uint8_t flags, const uint8_t *data, size_t n)
{
    Sub *s = &g_subs[ci];
    for (int k = s->qn - 1; k >= 0; k--) {
        SubOut *o = &s->q[k];
        if (o->type != HX_STDIN || o->id != id) continue;
        uint8_t *pl = o->buf + HL_CHAN_HDR;
        if (o->off == 0 && !(pl[0] & HX_STDIN_EOF)) {
            size_t have = o->len - HL_CHAN_HDR, room = o->cap - have;
            size_t take = n < room ? n : room;
            memcpy(o->buf + o->len, data, take);
            o->len += take;
            hl_chan_put32(o->buf + 12, (uint32_t)(have + take));
            data += take;
            n -= take;
            if (n == 0) { pl[0] |= flags; return 0; }
        }
        break;
    }
    do {
        size_t take = n < HL_EXEC_STREAM_CHUNK_MAX ? n : HL_EXEC_STREAM_CHUNK_MAX;
        uint8_t fl = take == n ? flags : 0;
        if (sub_queue_cap(ci, HX_STDIN, id, 0, &fl, 1, data, take,
                          1 + HL_EXEC_STREAM_CHUNK_MAX) != 0) return -1;
        data += take;
        n -= take;
    } while (n > 0);
    return 0;
}

/* Write what command @p ci's broker takes now. -1 when it is gone. */
static int sub_flush(int ci)
{
    Sub *s = &g_subs[ci];
    while (s->fd >= 0 && s->qn > 0) {
        SubOut *o = &s->q[0];
        ssize_t w = send(s->fd, o->buf + o->off, o->len - o->off,
                         MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            if (errno == EINTR) continue;
            return -1;
        }
        if (w > 0) s->progress_ms = hl_chan_mono_ms();
        o->off += (size_t)w;
        if (o->off < o->len) continue;
        g_reserved -= o->hold;
        free(o->buf);
        memmove(s->q, s->q + 1, (size_t)(--s->qn) * sizeof s->q[0]);
    }
    return 0;
}

/* A frame from command @p ci's broker: a reply ends the call routed under
 * its id, a stream's CHUNK / STDIN_CREDIT goes on to the server within the
 * windows the router holds it to, a test probe goes straight on. */
static void sub_frame(int ci, uint32_t type, uint32_t id, const uint8_t *p, size_t len)
{
    g_subs[ci].rx_ms = hl_chan_mono_ms();
    g_subs[ci].ping_ms = 0;               /* any frame answers a PING */
    if (type == HX_PONG) { if (id != 0 || len != 0) sub_lost(ci); return; }
#ifdef HL_EXEC_TEST_HOOKS
    if (type == HX_PROBE) { queue_frame(HX_PROBE, id, p, len); return; }
#endif
    if (type != HX_DONE && type != HX_BUSY && type != HX_REFUSED &&
        type != HX_CHUNK && type != HX_STDIN_CREDIT) { sub_lost(ci); return; }
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) {
        Job *j = &g_jobs[i];
        if (!j->used || !j->routed || j->id != id) continue;
        if (j->ci != ci) { sub_lost(ci); return; }   /* not its call */
        if (j->queued || j->discard) return;         /* ended here already */
        if (type == HX_CHUNK) {
            size_t n = len - 1;
            int fd = len >= 2 ? p[0] : 0;
            if (!j->stream || (fd != 1 && fd != 2) || n > HL_EXEC_STREAM_CHUNK_MAX ||
                n > j->so[fd - 1].credit) { sub_lost(ci); return; }
            j->so[fd - 1].credit -= n;
            j->so[fd - 1].total += n;
            if (chunk_push(j, (uint8_t)fd, p + 1, n) != 0) sub_lost(ci);
            return;
        }
        if (type == HX_STDIN_CREDIT) {
            if (!j->stream || !j->in_open || len != 5) { sub_lost(ci); return; }
            uint32_t bytes = hl_chan_get32(p + 1);
            if (bytes > HL_EXEC_STREAM_WINDOW - j->in_granted) { sub_lost(ci); return; }
            j->in_granted += bytes;
            if (p[0] & HX_STDIN_CLOSED) j->in_closed = 1;
            if (stdin_credit_push(j, p[0] & HX_STDIN_CLOSED, bytes) != 0) sub_lost(ci);
            return;
        }
        /* A reply holds the call's output, at most what was reserved for it
         * (the DONE head is 24 bytes); more is a broker gone wrong. */
        if (len > j->reserved + 24 + 1024) { sub_lost(ci); return; }
        job_reply_frame(j, type, p, len);
        return;
    }
    /* No such call: it was KILLed and freed meanwhile. */
}

/* Set up job @p j as a stream when @p rflags asks for one: the windows, and
 * (unless @p routed: its broker grants that) the stdin credit. */
static void stream_init(Job *j, const Cmd *cmd, unsigned rflags, size_t in_len, int routed)
{
    if (!(rflags & HX_RUN_STREAM)) return;
    j->stream = 1;
    j->so[0].credit = j->so[1].credit = HL_EXEC_STREAM_WINDOW;
    j->so[0].cap = cmd->c.stream_stdout_max;
    j->so[1].cap = cmd->c.stream_stderr_max;
    j->in_open = (rflags & HX_RUN_STDIN_OPEN) ? 1 : 0;
    j->in_total = in_len;
    j->in_total_max = cmd->c.stream_stdin_max;
    j->in_granted = (j->in_open && !routed) ? HL_EXEC_STREAM_WINDOW : 0;
    j->idle_ms = routed ? 0 : cmd->c.stream_idle_ms;   /* a routed one: its broker's */
}

/* A stream pipe that is open and not read because the app has not given
 * credit for it (or its part-chunk is full): the command may be blocked on
 * the app, so its silence does not count against the idle limit. */
static int idle_held_by_app(const Job *j)
{
    for (int k = 0; k < 2; k++) {
        int fd = k ? j->err_fd : j->out_fd;
        if (fd < 0 || (k ? j->err_cut : j->out_cut)) continue;
        if (j->so[k].credit == 0 || j->so[k].open_len == HL_EXEC_STREAM_CHUNK_MAX)
            return 1;
    }
    return 0;
}

/* Forward an admitted, matched RUN to its command's broker, as one job: it
 * is queued and goes out as that broker takes it, never holding the router
 * (#1234). */
static void route_call(int slot, int ci, uint32_t id, const uint8_t *p, size_t len,
                       size_t reserve, unsigned rflags, size_t in_len)
{
    char msg[160];
    if (g_subs[ci].fd < 0) {
        snprintf(msg, sizeof msg, "exec.%s: the command broker is gone", g_cmds[ci].c.name);
        queue_small(HX_REFUSED, id, msg);
        return;
    }
    if (sub_queue(ci, HX_RUN, id, p, len) != 0) {
        queue_small(HX_BUSY, id, "exec: too many calls waiting for the command broker");
        return;
    }
    Job *j = &g_jobs[slot];
    memset(j, 0, sizeof *j);
    j->out_fd = j->err_fd = j->pidfd = j->in_fd = j->kill_fd = -1;
    j->used = 1;
    j->routed = 1;
    j->id = id;
    j->ci = ci;
    j->reserved = reserve;
    g_reserved += reserve;
    g_cmds[ci].running++;
    stream_init(j, &g_cmds[ci], rflags, in_len, 1);
    if (sub_flush(ci) != 0) sub_lost(ci);   /* replies REFUSED for this job */
}

/* KILL routed call @p id, run by command @p ci's broker. Its RUN, if not
 * started on the wire, is simply dropped (that broker never saw the call);
 * one partly written goes out whole first, its bytes counted until then.
 * The call's own waiting frames to that broker (CREDIT, STDIN) go too. */
static void sub_kill(int ci, uint32_t id)
{
    Sub *s = &g_subs[ci];
    if (s->fd < 0) return;
    int started = 1;
    for (int k = 0; k < s->qn; k++) {
        SubOut *o = &s->q[k];
        if (o->id != id || o->type == HX_KILL) continue;
        if (o->off == 0) {
            if (o->type == HX_RUN) started = 0;
            g_reserved -= o->hold;
            free(o->buf);
            memmove(s->q + k, s->q + k + 1, (size_t)(s->qn - k - 1) * sizeof s->q[0]);
            s->qn--;
            k--;
            continue;
        }
        if (o->type == HX_RUN) {
            o->hold = o->len;   /* its job is freed now; the frame still holds memory */
            g_reserved += o->hold;
        }
    }
    if (!started) return;
    if (sub_queue(ci, HX_KILL, id, NULL, 0) != 0 || sub_flush(ci) != 0) sub_lost(ci);
}

/* End call @p j for a protocol error the server made (credit or stdin past
 * its bounds): its command is killed, what it had waiting is dropped, and
 * the server gets REFUSED as its last frame. */
static void call_fail(Job *j, const char *msg)
{
    job_drop_unsent(j);
    j->discard = 0;
    if (j->routed) {
        sub_kill(j->ci, j->id);
        job_reply_frame(j, HX_REFUSED, msg, strlen(msg));
        return;
    }
    /* Its slot is held until the child is reaped: REFUSED goes out then
     * (the main loop), so concurrency counts the dying child. */
    job_kill(j);
    if (j->out_fd >= 0) { close(j->out_fd); j->out_fd = -1; }
    if (j->err_fd >= 0) { close(j->err_fd); j->err_fd = -1; }
    if (j->in_fd >= 0) { close(j->in_fd); j->in_fd = -1; }
    j->so[0].open_len = j->so[1].open_len = 0;
    j->fail_msg = msg;
}

/* ── One call ──────────────────────────────────────────────────────── */

/* The child, between fork and exec. Allocates nothing; reports a failure
 * through @p ep (stage byte + errno) and _exit(126). Its stdin is @p in_r
 * (the call's bytes), or /dev/null when it is -1. The host's steps are
 * hl_exec_plat_child_* (exec_platform.h); the order matters: die with the
 * broker before anything else, confine before stdio moves, the syscall
 * filter last. */
static void child_run(const Cmd *cmd, uint32_t timeout_ms, int bfd, int in_r, int out_w,
                      int err_w, int ep, char *const argv[], char *const envp[],
                      pid_t broker)
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
    if (timeout_ms) {                              /* 0: a stream with no time limit */
        rlim_t cpu = ((rlim_t)timeout_ms + 999) / 1000 + 1;
        struct rlimit rcpu = { cpu, cpu };
        setrlimit(RLIMIT_CPU, &rcpu);
    }
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
                                     "syscall filter", "exec", "keeper" };

/* The keeper (macOS, exec_platform.h hl_exec_plat_keeper): forked by the
 * broker, it keeps only what the job needs, forks the job, tells the broker
 * its pid on @p pid_w, drops everything, and waits (hl_exec_plat_keeper_run).
 * It is the job's parent, so it reaps it: the broker's death cannot leave
 * the job, nor a late kill hit a reused pid. */
static void keeper_start(const Cmd *cmd, uint32_t timeout_ms, int bfd, int in_r,
                         int out_w, int err_w, int ep, int kill_r, int pid_w,
                         char *const argv[], char *const envp[], pid_t broker)
{
    hl_exec_plat_keeper_signals();                 /* before anything can end it */
    if (getppid() != broker) _exit(127);           /* the broker died already */
    const int keep[] = { bfd, in_r, out_w, err_w, ep, kill_r, pid_w, g_devnull };
    for (int fd = 3; fd < 1024; fd++) {
        int k = 0;
        for (size_t i = 0; i < sizeof keep / sizeof keep[0]; i++)
            if (keep[i] == fd) { k = 1; break; }
        if (!k) close(fd);
    }
    pid_t job = fork();
    if (job == 0) {
        close(kill_r);
        close(pid_w);
        child_run(cmd, timeout_ms, bfd, in_r, out_w, err_w, ep, argv, envp, getppid());
    }
    if (job < 0) {
        int e = errno;
        unsigned char msg[5] = { HL_EXEC_STAGE_KEEPER, (unsigned char)e,
                                 (unsigned char)(e >> 8), (unsigned char)(e >> 16),
                                 (unsigned char)(e >> 24) };
        if (write(ep, msg, sizeof msg) < 0) { /* nothing to do */ }
        _exit(126);
    }
    /* The job holds its own copies: the broker sees the pipes close, and
     * the error pipe's EOF, only from the job. */
    close(bfd);
    if (in_r >= 0) close(in_r);
    close(out_w);
    close(err_w);
    close(ep);
    if (g_devnull >= 0) close(g_devnull);
    uint8_t p4[4];
    hl_chan_put32(p4, (uint32_t)job);
    if (write(pid_w, p4, sizeof p4) < 0) { /* the broker reads 0 */ }
    close(pid_w);
    hl_exec_plat_keeper_run(broker, job, kill_r);
}

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
     * u32 stdin_len, stdin, u8 flags */
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
    if (len - off < 1 || in_len != len - off - 1) {
        queue_small(HX_REFUSED, id, "exec: malformed RUN");
        return;
    }
    const uint8_t *in = p + off;
    unsigned rflags = p[len - 1];
    if ((rflags & ~(HX_RUN_STREAM | HX_RUN_STDIN_OPEN)) ||
        ((rflags & HX_RUN_STDIN_OPEN) && !(rflags & HX_RUN_STREAM))) {
        queue_small(HX_REFUSED, id, "exec: malformed RUN");
        return;
    }

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
    int stream = (rflags & HX_RUN_STREAM) != 0, in_open = (rflags & HX_RUN_STDIN_OPEN) != 0;
    if (stream && !(cmd->c.flags & HL_EXEC_CMD_STREAM)) {
        snprintf(msg, sizeof msg, "exec.%s: is not a stream command (it declares no "
                 "stream = true)", cmd->c.name);
        queue_small(HX_REFUSED, id, msg);
        return;
    }
    /* stdin: the command's own cap, checked here too (the binding's check is
     * only a fast path). A stream's is its total, of which the RUN may carry
     * up to the exec.run ceiling (more goes in STDIN frames). */
    uint64_t in_max = stream ? cmd->c.stream_stdin_max : cmd->c.stdin_max;
    if (in_len > in_max || (in_open && in_max == 0)) {
        snprintf(msg, sizeof msg, in_max
                 ? "exec.%s: stdin is longer than its stdin_max"
                 : "exec.%s: takes no stdin (it declares no stdin_max)", cmd->c.name);
        queue_small(HX_REFUSED, id, msg);
        return;
    }
    if (cmd->running >= cmd->c.concurrency) {
        queue_small(HX_BUSY, id, "exec: the command's concurrency is reached");
        return;
    }
    /* Its output caps and its stdin, held until the reply is delivered; a
     * stream holds its windows instead. */
    size_t reserve = stream
        ? 2u * HL_EXEC_STREAM_WINDOW + in_len + (in_open ? HL_EXEC_STREAM_WINDOW : 0u)
        : (size_t)cmd->c.stdout_max + cmd->c.stderr_max + in_len;
    if (reserve > HL_EXEC_MAX_BUFFERED - g_reserved) {
        queue_small(HX_BUSY, id, "exec: too much output held for delivery");
        return;
    }
    if (g_router) {                      /* its own broker runs it */
        route_call(free_slot, ci, id, p, len, reserve, rflags, in_len);
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
    int kp[2] = { -1, -1 }, pp[2] = { -1, -1 };   /* the keeper's kill / pid pipes */
    int keeper = hl_exec_plat_keeper();
    pid_t pid = -1;
    /* A keeper is forked with every signal blocked: until it has set its
     * dispositions (hl_exec_plat_keeper_signals), a signal to the process
     * group would end it and leave its job with nobody to end it. */
    sigset_t all, was;
    sigfillset(&all);
    if (keeper) sigprocmask(SIG_SETMASK, &all, &was);
    if (pipe_cloexec(op) == 0 && pipe_cloexec(epp) == 0 && pipe_cloexec(ep) == 0 &&
        ((in_len == 0 && !in_open) || pipe_cloexec(ip) == 0) &&
        (!keeper || (pipe_cloexec(kp) == 0 && pipe_cloexec(pp) == 0)))
        pid = fork();
    int fork_errno = errno;
    if (pid != 0 && keeper) sigprocmask(SIG_SETMASK, &was, NULL);
    uint32_t timeout = stream ? cmd->c.stream_timeout_ms : cmd->c.timeout_ms;
    if (pid == 0 && keeper)
        keeper_start(cmd, timeout, bfd, ip[0], op[1], epp[1], ep[1], kp[0], pp[1], argv,
                     envp, getppid());
    if (pid == 0)
        child_run(cmd, timeout, bfd, ip[0], op[1], epp[1], ep[1], argv, envp, getppid());
    if (kp[0] >= 0) close(kp[0]);
    if (pp[1] >= 0) close(pp[1]);
    close(bfd);
    for (size_t i = 0; i < no; i++) free(owned[i]);
    for (size_t i = 0; i < nenv; i++) free(envbufs[i]);
    if (op[1] >= 0) close(op[1]);
    if (epp[1] >= 0) close(epp[1]);
    if (ep[1] >= 0) close(ep[1]);
    if (ip[0] >= 0) close(ip[0]);
    if (pid < 0) {
        if (kp[1] >= 0) close(kp[1]);
        if (pp[0] >= 0) close(pp[0]);
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
    /* Under a keeper: the job's pid (written right after its fork). */
    pid_t job_pid = pid;
    if (pp[0] >= 0) {
        uint8_t p4[4];
        struct pollfd ppf = { pp[0], POLLIN, 0 };
        job_pid = 0;
        if (poll(&ppf, 1, 2000) > 0 && read(pp[0], p4, sizeof p4) == (ssize_t)sizeof p4)
            job_pid = (pid_t)hl_chan_get32(p4);
        close(pp[0]);
    }
    if (got == (ssize_t)sizeof em) {
        int e = em[1] | (em[2] << 8) | (em[3] << 16) | (em[4] << 24);
        if (kp[1] >= 0) close(kp[1]);    /* the keeper ends its job, then itself */
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
    j->job_pid = job_pid;
    j->kill_fd = kp[1];
    j->out_fd = op[0];
    j->err_fd = epp[0];
    /* Readable the moment it exits, so the reply goes out then rather than
     * at the next reap tick (else the 50 ms tick). */
    j->pidfd = hl_exec_plat_exit_watch(pid);
    hl_chan_set_nonblock(j->out_fd);
    hl_chan_set_nonblock(j->err_fd);
    stream_init(j, cmd, rflags, in_len, 0);
    int oom = 0;
    if (!stream) {
        j->out = malloc(cmd->c.stdout_max ? cmd->c.stdout_max : 1);
        j->err = malloc(cmd->c.stderr_max ? cmd->c.stderr_max : 1);
        oom = !j->out || !j->err;
    }
    j->in_fd = ip[1];
    if (in_len || in_open) {
        /* Written as the pipe takes it (the main loop); a command that exits
         * or closes its stdin first just leaves the rest unwritten. An open
         * stdin has room for one window more. */
        hl_chan_set_nonblock(j->in_fd);
        j->in_cap = in_len + (in_open ? HL_EXEC_STREAM_WINDOW : 0);
        j->in = malloc(j->in_cap);
        if (j->in) { memcpy(j->in, in, in_len); j->in_len = in_len; }
        else oom = 1;
        j->in_init_left = in_len;
    }
    j->start_ms = hl_chan_mono_ms();
    j->deadline = timeout ? j->start_ms + timeout : UINT64_MAX;
    j->idle_last = j->start_ms;
    j->reserved = reserve;
    g_reserved += reserve;
    cmd->running++;
    if (oom) { job_kill(j); j->discard = 1; return; }
    if (in_open && stdin_credit_push(j, 0, HL_EXEC_STREAM_WINDOW) != 0) {
        job_kill(j);
        j->discard = 1;
    }
}

static void kill_call(uint32_t id)
{
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) {
        Job *j = &g_jobs[i];
        if (!j->used || j->id != id) continue;
        /* What has not started on the wire is dropped; a part-sent frame
         * goes out whole, and the job frees once it has. */
        job_drop_unsent(j);
        if (j->routed) {
            /* Its broker kills the child and sends no reply; the slot frees
             * now (that broker holds its own caps meanwhile). Freed BEFORE a
             * failed write can mark the broker lost, which would otherwise
             * queue a reply for this slot (review: the freed slot stayed in
             * g_order). */
            int ci = j->ci;
            if (j->qn == 0) job_free(j);
            else j->discard = 1;
            sub_kill(ci, id);
            return;
        }
        if (j->pid > 0) job_kill(j);
        j->discard = 1;
        j->so[0].open_len = j->so[1].open_len = 0;   /* no more output for it */
        if (j->qn == 0 && job_settled(j)) job_free(j);
        return;
    }
}

/* A CREDIT or STDIN frame from the server for call @p id (§5, §5.1 of
 * exec_stream.md). A call that has ended, or is not known, takes nothing
 * (the server's frame crossed its end); one that is not a stream, or credit
 * or stdin past its bounds, ends the call. */
static void stream_frame(uint32_t type, uint32_t id, const uint8_t *p, size_t len)
{
    Job *j = NULL;
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT && !j; i++)
        if (g_jobs[i].used && g_jobs[i].id == id) j = &g_jobs[i];
    if (!j || j->discard || j->queued || j->fail_msg) return;
    if (!j->stream) { call_fail(j, "exec: stream frames for a call that is not a stream"); return; }
    if (type == HX_CREDIT) {
        int fd = len == 5 ? p[0] : 0;
        if (fd != 1 && fd != 2) { call_fail(j, "exec: a malformed CREDIT"); return; }
        uint32_t bytes = hl_chan_get32(p + 1);
        StreamOut *so = &j->so[fd - 1];
        /* Credit only for output the server has been sent: a server that
         * credited bytes it never read could fill the queue (review). */
        if (bytes > so->wire || bytes > HL_EXEC_STREAM_WINDOW - so->credit) {
            call_fail(j, "exec: credit past the stream's window");
            return;
        }
        so->wire -= bytes;
        so->credit += bytes;
        if (j->routed && sub_credit(j->ci, id, (uint8_t)fd, bytes) != 0) sub_lost(j->ci);
        return;
    }
    /* STDIN */
    if (len < 1 || len - 1 > HL_EXEC_STREAM_CHUNK_MAX) { call_fail(j, "exec: a malformed STDIN"); return; }
    uint8_t flags = p[0];
    size_t n = len - 1;
    if (!j->in_open || j->in_eof || (flags & ~HX_STDIN_EOF)) {
        call_fail(j, "exec: stdin the call did not open");
        return;
    }
    if (n > j->in_granted) { call_fail(j, "exec: stdin past its credit"); return; }
    if (n > j->in_total_max - j->in_total) {
        call_fail(j, "exec: stdin longer than its stdin_max");
        return;
    }
    j->in_granted -= n;
    j->in_total += n;
    if (flags & HX_STDIN_EOF) j->in_eof = 1;
    if (j->routed) {
        if (sub_stdin(j->ci, id, flags, p + 1, n) != 0) sub_lost(j->ci);
        return;
    }
    if (j->in_closed || j->in_fd < 0) return;   /* the child stopped reading */
    if (j->in_off > 0) {
        memmove(j->in, j->in + j->in_off, j->in_len - j->in_off);
        j->in_len -= j->in_off;
        j->in_off = 0;
    }
    if (n > j->in_cap - j->in_len) { call_fail(j, "exec: stdin past its credit"); return; }
    if (n) memcpy(j->in + j->in_len, p + 1, n);
    j->in_len += n;
    if (j->in_eof && j->in_len == 0) { close(j->in_fd); j->in_fd = -1; }
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
 * written (an open stream stdin: once the server sent EOF), or when the
 * child stopped reading (EPIPE: SIGPIPE is ignored). An open stdin gives its
 * credit back as the pipe takes the streamed bytes. */
static void stdin_closed(Job *j)
{
    if (j->in_fd >= 0) { close(j->in_fd); j->in_fd = -1; }
    if (!j->in_open || j->in_closed) return;
    j->in_closed = 1;
    j->in_len = j->in_off = 0;
    j->in_closed_tell = 1;           /* sent by the main loop (a full queue retries) */
}

static void feed(Job *j)
{
    int gone = 0;
    while (j->in_off < j->in_len) {
        ssize_t w = write(j->in_fd, j->in + j->in_off, j->in_len - j->in_off);
        if (w > 0) {
            size_t c = (size_t)w;
            size_t init = c < j->in_init_left ? c : j->in_init_left;
            j->in_init_left -= init;
            j->in_ret += c - init;
            j->in_off += c;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        gone = 1;
        break;
    }
    if (gone) { stdin_closed(j); return; }
    if (j->in_off == j->in_len) {
        j->in_off = j->in_len = 0;
        if ((!j->in_open || j->in_eof) && j->in_fd >= 0) {
            close(j->in_fd);
            j->in_fd = -1;
            if (!j->in_open) { free(j->in); j->in = NULL; }
        }
    }
    if (j->in_open && j->in_ret &&
        (j->in_len == 0 || j->in_ret >= HL_EXEC_STREAM_WINDOW / 4) &&
        stdin_credit_push(j, 0, (uint32_t)j->in_ret) == 0) {
        j->in_granted += j->in_ret;
        j->in_ret = 0;
    }
}

/* Move stream pipe @p k's (0 stdout, 1 stderr) waiting bytes into CHUNKs. */
static void stream_emit(Job *j, int k)
{
    StreamOut *so = &j->so[k];
    if (!so->open_len) return;
    size_t left = chunk_push(j, (uint8_t)(k + 1), so->open, so->open_len);
    if (left) {
        memmove(so->open, so->open + so->open_len - left, left);
        so->open_ms = hl_chan_mono_ms();    /* a full queue: try again later, not at once */
    }
    so->open_len = left;
}

/* Read stream pipe @p k while it has credit. Once its command is ended -
 * past its total cap, timed out, or KILLed - what credit does not cover is
 * read and dropped to EOF (marked cut), so the call ends whether or not the
 * app reads: back pressure holds a running command, never an ended one. */
static void stream_drain(Job *j, int k)
{
    int *fd = k ? &j->err_fd : &j->out_fd;
    int *cut = k ? &j->err_cut : &j->out_cut;
    StreamOut *so = &j->so[k];
    uint8_t tmp[16384];
    for (;;) {
        ssize_t r;
        size_t room = HL_EXEC_STREAM_CHUNK_MAX - so->open_len;
        if (so->credit < room) room = so->credit;
        if (!*cut && (j->discard || (room == 0 && j->timed_out))) *cut = 1;
        if (*cut) {
            r = read(*fd, tmp, sizeof tmp);
        } else {
            if (room == 0) return;
            if (!so->open && !(so->open = malloc(HL_EXEC_STREAM_CHUNK_MAX))) {
                call_fail(j, "exec: out of memory");
                return;
            }
            r = read(*fd, so->open + so->open_len, room);
        }
        if (r > 0) {
            j->idle_last = hl_chan_mono_ms();
            if (*cut) continue;
            size_t keep = (size_t)r;
            if (so->cap != HL_EXEC_STREAM_NO_CAP && keep > so->cap - so->total) {
                keep = (size_t)(so->cap - so->total);
                *cut = 1;
                if (j->pid > 0) job_kill(j);   /* past its declared total */
            }
            if (keep && so->open_len == 0) so->open_ms = hl_chan_mono_ms();
            so->credit -= keep;
            so->total += keep;
            so->open_len += keep;
            /* Out at once when the app has caught up (every byte before these
             * credited back): a reply to a request must not wait the
             * coalescing delay. Coalesced only while the app lags. */
            if (so->open_len == HL_EXEC_STREAM_CHUNK_MAX ||
                HL_EXEC_STREAM_WINDOW - so->credit == so->open_len)
                stream_emit(j, k);
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (r < 0 && errno == EINTR) continue;
        close(*fd);
        *fd = -1;
        stream_emit(j, k);
        return;
    }
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
            j->in_len = j->in_off = 0;
            if (j->in_open && !j->in_closed) j->in_closed_tell = 1;
            j->in_closed = 1;
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
        char kind[96] = "";
        if (cmd->c.flags & HL_EXEC_CMD_STREAM) {
            char idle[40] = "";
            if (cmd->c.stream_idle_ms)
                snprintf(idle, sizeof idle, ", idle %u ms", (unsigned)cmd->c.stream_idle_ms);
            if (cmd->c.stream_timeout_ms == 0)
                snprintf(kind, sizeof kind, " (stream, no time limit%s)", idle);
            else
                snprintf(kind, sizeof kind, " (stream, %u ms%s)",
                         (unsigned)cmd->c.stream_timeout_ms, idle);
        }
        int w = snprintf(report + at, rsz - at, "exec: %s -> %s%s\n", cmd->c.name,
                         cmd->real, kind);
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
        g_jobs[i].out_fd = g_jobs[i].err_fd = g_jobs[i].pidfd = g_jobs[i].in_fd =
            g_jobs[i].kill_fd = -1;
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
        /* The command brokers (router only): map -3 - i. Always read: a
         * reply goes to its job's own slot, not the small-frame queue the
         * backpressure above protects (and a broker that hung up must be
         * seen, or poll returns at once on its POLLHUP for ever). */
        uint64_t now = hl_chan_mono_ms();
        int timeout = -1;
        for (int i = 0; g_router && i < g_ncmds; i++)
            if (g_subs[i].fd >= 0) {
                short ev = POLLIN;
                if (g_subs[i].qn > 0) {
                    ev |= POLLOUT;
                    timeout = 1000;              /* look for a wedged broker */
                }
                if (sub_busy(i)) {               /* its heartbeat */
                    int tk = (int)(g_hb_ms / 2 < 1000 ? g_hb_ms / 2 : 1000);
                    if (timeout < 0 || tk < timeout) timeout = tk;
                }
                pf[np] = (struct pollfd){ g_subs[i].fd, ev, 0 };
                map[np++] = -3 - i;
            }
        for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) {
            Job *j = &g_jobs[i];
            if (!j->used || j->routed) continue;   /* its broker times it */
            /* A stream's pipe is read only while it has credit (or past its
             * cap, to drop the rest): the child blocks, Hull holds nothing more. */
            for (int k = 0; k < 2; k++) {
                int fd = k ? j->err_fd : j->out_fd;
                if (fd < 0) continue;
                if (j->stream && !(k ? j->err_cut : j->out_cut) && !j->timed_out &&
                    !j->discard &&
                    (j->so[k].credit == 0 || j->so[k].open_len == HL_EXEC_STREAM_CHUNK_MAX))
                    continue;
                pf[np] = (struct pollfd){ fd, POLLIN, 0 };
                map[np++] = i;
            }
            if (j->pidfd >= 0 && j->pid > 0) { pf[np] = (struct pollfd){ j->pidfd, POLLIN, 0 }; map[np++] = i; }
            if (j->in_fd >= 0 && j->in_off < j->in_len) {
                pf[np] = (struct pollfd){ j->in_fd, POLLOUT, 0 };
                map[np++] = i;
            }
            int t = -1;
            if (j->pid > 0 && !j->timed_out && j->deadline != UINT64_MAX)
                t = j->deadline > now ? (int)(j->deadline - now > 86400000u ? 86400000u
                                                                    : j->deadline - now) : 0;
            else if (j->pid > 0 && (j->timed_out || j->discard || j->fail_msg ||
                                    j->out_cut || j->err_cut))
                t = 50;                                    /* killed: reap soon */
            else if (j->out_fd < 0 && j->err_fd < 0) t = 0;
            if (j->pid > 0 && j->pidfd < 0 && j->out_fd < 0 && j->err_fd < 0 &&
                (t < 0 || t > 50)) t = 50;
            for (int k = 0; k < 2; k++)            /* a waiting part-chunk */
                if (j->so[k].open_len) {
                    uint64_t due = j->so[k].open_ms + HL_EXEC_STREAM_COALESCE_MS;
                    int tk = due > now ? (int)(due - now) : 0;
                    if (t < 0 || tk < t) t = tk;
                }
            if (j->idle_ms && j->pid > 0 && !j->timed_out && !idle_held_by_app(j)) {
                uint64_t due = j->idle_last + j->idle_ms;
                uint64_t left = due > now ? due - now : 0;
                int tk = (int)(left > 86400000u ? 86400000u : left);
                if (t < 0 || tk < t) t = tk;
            }
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
                else if (rd.type == HX_CREDIT || rd.type == HX_STDIN)
                    stream_frame(rd.type, rd.id, rd.buf, rd.len);
                else if (rd.type == HX_PING) hb_answer(rd.buf, rd.len);
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
                if ((pf[k].revents & POLLOUT) && sub_flush(ci) != 0) { sub_lost(ci); continue; }
                if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                while (s->fd >= 0) {
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
            if (!j->used) continue;                 /* ended by a frame above */
            if (pf[k].fd == j->in_fd) { feed(j); continue; }
            if (j->stream) {
                if (pf[k].fd == j->out_fd) stream_drain(j, 0);
                else if (pf[k].fd == j->err_fd) stream_drain(j, 1);
                continue;
            }
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
                put_u32(pid4, (uint32_t)(j->job_pid > 0 ? j->job_pid : j->pid));
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
        for (int i = 0; g_router && i < g_ncmds; i++)
            if (g_subs[i].fd >= 0 && g_subs[i].qn > 0 &&
                now - g_subs[i].progress_ms >= HX_SUB_STALL_MS)
                sub_lost(i);                     /* took nothing for a minute */
            else if (g_subs[i].fd >= 0) sub_heartbeat(i, now);
        for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++) {
            Job *j = &g_jobs[i];
            if (!j->used || j->routed) continue;   /* replied to by sub_frame */
            if (j->pid > 0 && !j->timed_out && now >= j->deadline) {
                job_kill(j);
                j->timed_out = 1;
            }
            if (j->idle_ms && j->pid > 0 && !j->timed_out) {
                if (idle_held_by_app(j)) {
                    j->idle_held = 1;
                    j->idle_last = now;
                } else if (j->idle_held) {
                    j->idle_held = 0;           /* credit came: the clock restarts */
                    j->idle_last = now;
                } else if (now - j->idle_last >= j->idle_ms) {
                    job_kill(j);
                    j->timed_out = 2;
                }
            }
            for (int k = 0; k < 2; k++)
                if (j->so[k].open_len && now - j->so[k].open_ms >= HL_EXEC_STREAM_COALESCE_MS)
                    stream_emit(j, k);
            if (j->in_open && j->in_ret && j->in_fd >= 0) feed(j);   /* credit kept back */
            if (j->in_closed_tell && !j->discard && !j->fail_msg && !j->queued &&
                stdin_credit_push(j, HX_STDIN_CLOSED, 0) == 0)
                j->in_closed_tell = 0;
            if (job_settled(j) && !j->queued && j->fail_msg && !j->discard) {
                job_drop_unsent(j);
                job_reply_frame(j, HX_REFUSED, j->fail_msg, strlen(j->fail_msg));
            } else if (job_settled(j) && !j->queued) {
                if (j->discard) { if (j->qn == 0) job_free(j); }
                else job_reply(j);
                if (j->used && j->discard && j->qn == 0) job_free(j);
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
    HlExecStream *stream;        /* a streamed call (fn unused) */
    int          is_stream;      /* for the audit record: exec.stream */
    uint64_t     t0_ms;          /* when it was submitted */
} Pending;

static void stream_lost(HlExecStream *s, const char *why, int wake);
static void stream_deliver(Pending *pd, uint32_t type, const uint8_t *p, size_t len);

static int       g_fd = -1;
static pid_t     g_pid = -1;
static uint32_t  g_next_id = 1;
static int       g_ready;
static HlAsyncBackendCtx *g_loop;
static HlChanFrame g_rd = { .magic = HX_MAGIC, .max = HX_REPLY_MAX };
static Pending   g_pend[HL_EXEC_MAX_INFLIGHT];
/* The heartbeat (HL_EXEC_HEARTBEAT_MS): a timer on g_loop while a call is
 * in flight, the time of the broker's last frame, and an unanswered PING. */
static uint64_t  g_hb_timer;
static uint64_t  g_rx_ms;
static uint64_t  g_ping_ms;
static pid_t     g_dropped_pid = -1;   /* a broker the heartbeat dropped, unreaped */

/* ── --audit (docs/design/exec_broker.md §12) ──────────────────────────
 * Two records per call: "start" once the broker has the RUN (name, id, the
 * call's arguments escaped, the stdin bytes it carries) and "end" when its
 * slot goes (the outcome; for "done" the status, signal, timed_out,
 * duration and output byte counts; else the reason). Output and stdin are
 * never logged. Written on the loop thread only. */

#define EXEC_AUDIT_ARG_MAX 1024u   /* bytes of one argument logged */

static uint64_t stream_audit_stdin(const HlExecStream *s);

static const char *exec_audit_cap(const Pending *pd)
{
    return pd->is_stream ? "exec.stream" : "exec.run";
}

static void exec_audit_start(const Pending *pd, const char *const *args,
                             const size_t *lens, size_t n, size_t in_len)
{
    if (!hl_audit_enabled) return;
    ShJsonWriter w = hl_audit_begin(exec_audit_cap(pd));
    sh_json_write_kv_string(&w, "event", "start");
    sh_json_write_kv_int(&w, "id", pd->id);
    sh_json_write_kv_string(&w, "name", pd->name);
    sh_json_write_key(&w, "args");
    sh_json_write_array_start(&w);
    int cut = 0;
    for (size_t i = 0; i < n; i++) {
        char raw[EXEC_AUDIT_ARG_MAX + 1];
        char esc[EXEC_AUDIT_ARG_MAX * 4 + 1];
        size_t k = lens[i] < EXEC_AUDIT_ARG_MAX ? lens[i] : EXEC_AUDIT_ARG_MAX;
        if (lens[i] > k) cut = 1;
        if (k) memcpy(raw, args[i], k);
        raw[k] = '\0';                    /* the matcher refused any NUL */
        hl_log_escape_line(raw, esc, sizeof esc);
        sh_json_write_string(&w, esc);
    }
    sh_json_write_array_end(&w);
    if (cut) sh_json_write_kv_bool(&w, "args_cut", 1);
    sh_json_write_kv_int(&w, "stdin_bytes", (int64_t)in_len);
    hl_audit_end(&w);
}

/* @p outcome: "done" (with @p r), "busy" / "refused" / "lost" (with
 * @p why), or "cancelled" (the app closed the stream, or the request or
 * op that waited for it went). */
static void exec_audit_end(const Pending *pd, const char *outcome, const HlExecReply *r,
                           const char *why)
{
    if (!hl_audit_enabled) return;
    ShJsonWriter w = hl_audit_begin(exec_audit_cap(pd));
    sh_json_write_kv_string(&w, "event", "end");
    sh_json_write_kv_int(&w, "id", pd->id);
    sh_json_write_kv_string(&w, "name", pd->name);
    sh_json_write_kv_string(&w, "outcome", outcome);
    if (r && r->kind == HL_EXEC_R_DONE) {
        sh_json_write_kv_int(&w, "status", r->status);
        if (r->signal[0]) sh_json_write_kv_string(&w, "signal", r->signal);
        else sh_json_write_kv_null(&w, "signal");
        sh_json_write_kv_bool(&w, "timed_out", r->timed_out != 0);
        if (r->idle_timed_out) sh_json_write_kv_bool(&w, "idle_timed_out", 1);
        sh_json_write_kv_int(&w, "duration_ms", r->duration_ms);
        sh_json_write_kv_int(&w, "stdout_bytes",
                             (int64_t)(r->stream ? r->out_bytes : r->out_len));
        sh_json_write_kv_int(&w, "stderr_bytes",
                             (int64_t)(r->stream ? r->err_bytes : r->err_len));
        sh_json_write_kv_bool(&w, "stdout_truncated", r->out_cut != 0);
        sh_json_write_kv_bool(&w, "stderr_truncated", r->err_cut != 0);
    } else {
        uint64_t now = hl_chan_mono_ms();
        sh_json_write_kv_int(&w, "duration_ms",
                             (int64_t)(now > pd->t0_ms ? now - pd->t0_ms : 0));
        const char *m = why ? why : (r ? r->msg : NULL);
        if (m && *m) {
            char esc[sizeof r->msg * 4 + 1];
            hl_log_escape_line(m, esc, sizeof esc);
            sh_json_write_kv_string(&w, "reason", esc);
        }
    }
    if (pd->stream) sh_json_write_kv_int(&w, "stdin_bytes",
                                         (int64_t)stream_audit_stdin(pd->stream));
    hl_audit_end(&w);
}

static const char *exec_audit_outcome(const HlExecReply *r)
{
    switch (r ? r->kind : HL_EXEC_R_LOST) {
    case HL_EXEC_R_DONE:    return "done";
    case HL_EXEC_R_BUSY:    return "busy";
    case HL_EXEC_R_REFUSED: return "refused";
    default:                return "lost";
    }
}

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
        exec_audit_end(p, "lost", NULL, why);
        if (p->stream) {
            HlExecStream *st = p->stream;
            p->used = 0;
            p->stream = NULL;
            stream_lost(st, why, 1);
            continue;
        }
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
    if (g_loop && g_hb_timer) hl_async_backend()->timer_cancel(g_loop, g_hb_timer);
    g_hb_timer = 0;
    g_ping_ms = 0;
    if (g_dropped_pid > 0) {               /* reaped if it has gone; never waited for */
        (void)waitpid(g_dropped_pid, NULL, WNOHANG);
        g_dropped_pid = -1;
    }
    if (g_loop && g_fd >= 0) hl_async_backend()->watcher_del(g_loop, g_fd);
    g_loop = NULL;
    if (g_fd >= 0) { close(g_fd); g_fd = -1; }
    g_ready = 0;
    hl_chan_frame_reset(&g_rd);
    if (g_pid > 0) {
        int st;
        /* It exits on the EOF, killing its children first. One that does not
         * within the wait is left, never killed: by now (the policy is sent
         * after sandbox phase 1, a teardown runs after phase 2) the sandbox
         * gives the server no kill(2) of another process - on Linux pledge
         * allows only a kill of itself and kills the SERVER for any other.
         * It dies with the server (PDEATHSIG on Linux, the exit watch on
         * macOS), with its commands; until then it is a broker with no
         * channel, which serves nothing. */
        (void)hl_chan_reap(g_pid, &st, 500, NULL);
        g_pid = -1;
    }
    /* A stop is a teardown: every pending call is cancelled by its own path
     * (the server's connections, the runtime's detached ops), and its
     * cancel finds nothing to send now. Nothing is resumed from here: a
     * stream only learns it is over (its waits are cancelled by their own
     * teardown). */
    for (int i = 0; i < HL_EXEC_MAX_INFLIGHT; i++)
        if (g_pend[i].used && g_pend[i].stream)
            stream_lost(g_pend[i].stream, "exec: the command broker was stopped", 0);
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
    if (pd->stream) { stream_deliver(pd, type, p, len); return; }
    if (type != HX_DONE && type != HX_BUSY && type != HX_REFUSED) return;
    HlExecReply *r = calloc(1, sizeof *r);
    HlExecDoneFn fn = pd->fn;
    void *ud = pd->ud;
    pd->used = 0;
    if (!r) { exec_audit_end(pd, "lost", NULL, "exec: out of memory"); return; }
    if (type == HX_DONE && len >= 24) {
        r->kind = HL_EXEC_R_DONE;
        r->status = (int32_t)hl_chan_get32(p);
        int signo = (int)hl_chan_get32(p + 4);
        if (signo) {
            char b[16];
            snprintf(r->signal, sizeof r->signal, "%s", sig_name(signo, b, sizeof b));
        }
        r->timed_out = p[8] != 0;
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
                    exec_audit_end(pd, "done", r, NULL);
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
    exec_audit_end(pd, exec_audit_outcome(r), r, NULL);
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
        g_rx_ms = hl_chan_mono_ms();
        g_ping_ms = 0;                     /* any frame answers a PING */
        if (g_rd.type != HX_PONG) deliver(g_rd.type, g_rd.id, g_rd.buf, g_rd.len);
        hl_chan_frame_reset(&g_rd);
    }
}

#ifdef HL_EXEC_TEST_HOOKS
/* HULL_TEST_EXEC_HEARTBEAT_MS=N: both heartbeat times N ms (50..), so a test
 * can see a wedged broker found in seconds. */
static uint32_t hb_test_ms(void)
{
    const char *v = getenv("HULL_TEST_EXEC_HEARTBEAT_MS");
    unsigned long n = v ? strtoul(v, NULL, 10) : 0;
    return n >= 50 && n <= HL_EXEC_HEARTBEAT_MS ? (uint32_t)n : 0;
}
#else
#define hb_test_ms() 0u
#endif
static uint32_t hb_ms(void) { return hb_test_ms() ? hb_test_ms() : HL_EXEC_HEARTBEAT_MS; }
static uint32_t hb_wait_ms(void)
{
    return hb_test_ms() ? hb_test_ms() : HL_EXEC_HEARTBEAT_WAIT_MS;
}

static void hb_fire(void *user);

static void hb_arm(void)
{
    if (!g_loop || g_hb_timer || g_fd < 0) return;
    uint32_t tick = hb_ms() / 2 < 1000 ? hb_ms() / 2 : 1000;
    g_hb_timer = hl_async_backend()->timer_add(g_loop, tick, hb_fire, NULL);
}

/* The broker has sent nothing for hb_ms() while calls are in flight: PING
 * it; nothing for hb_wait_ms() after that and it is wedged (alive, its loop
 * stuck): kill it and fail every call, as for a broker that died. It
 * enforces each command's limits itself, so silence past these is never a
 * command's doing. */
static void hb_fire(void *user)
{
    (void)user;
    g_hb_timer = 0;
    if (g_fd < 0 || !g_loop || hl_exec_inflight() == 0) return;
    uint64_t now = hl_chan_mono_ms();
    if (g_ping_ms && now - g_ping_ms >= hb_wait_ms()) {
        /* Dropped, not killed: the sandbox gives the server no kill(2) of
         * another process (Linux pledge allows only kill of itself, with a
         * kill-the-process penalty). With its channel closed, a broker that
         * moves again reads EOF, kills its commands and exits; one that never
         * does dies with the server (its death-with-the-server setup), as do
         * its commands. It is not reaped either (that waits, then kills). */
        log_error("[hull:c] exec: the command broker stopped answering; it is "
                  "dropped and exec.run fails from now on");
        hl_async_backend()->watcher_del(g_loop, g_fd);
        g_loop = NULL;
        close(g_fd);                       /* first: nothing new reaches it */
        g_fd = -1;
        g_ready = 0;
        g_dropped_pid = g_pid;
        g_pid = -1;
        g_ping_ms = 0;
        hl_chan_frame_reset(&g_rd);
        fail_all("exec: the command broker stopped answering");
        return;
    }
    if (!g_ping_ms && now - g_rx_ms >= hb_ms()) {
        uint8_t pl[8];
        hl_chan_put32(pl, hb_ms());
        hl_chan_put32(pl + 4, hb_wait_ms());
        g_ping_ms = now;   /* a write that does not go through is not an answer */
        (void)hl_chan_write_frame(g_fd, HX_MAGIC, HX_PING, 0, pl, sizeof pl, NULL, 0,
                                  now + 200);
    }
    hb_arm();
}

static int submit_ex(HlAsyncBackendCtx *loop, const char *name, size_t name_len,
                     const char *const *args, const size_t *lens, size_t n,
                     const void *in, size_t in_len, unsigned rflags,
                     HlExecDoneFn fn, void *ud, HlExecStream *stream,
                     uint32_t *id_out, char *why, size_t why_size);

int hl_exec_submit(HlAsyncBackendCtx *loop, const char *name, size_t name_len,
                   const char *const *args, const size_t *lens, size_t n,
                   const void *in, size_t in_len,
                   HlExecDoneFn fn, void *ud, uint32_t *id_out,
                   char *why, size_t why_size)
{
    return submit_ex(loop, name, name_len, args, lens, n, in, in_len, 0, fn, ud, NULL,
                     id_out, why, why_size);
}

static int submit_ex(HlAsyncBackendCtx *loop, const char *name, size_t name_len,
                     const char *const *args, const size_t *lens, size_t n,
                     const void *in, size_t in_len, unsigned rflags,
                     HlExecDoneFn fn, void *ud, HlExecStream *stream,
                     uint32_t *id_out, char *why, size_t why_size)
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
    size_t total = 4 + name_len + 4 + in_len + 1;
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
    hl_chan_put32(buf + off, (uint32_t)in_len);
    off += 4;
    if (in_len) memcpy(buf + off, in, in_len);
    off += in_len;
    buf[off++] = (uint8_t)rflags;                 /* RUN flags */
    uint32_t id = g_next_id++;
    if (g_next_id == 0) g_next_id = 1;
    int rc = hl_chan_write_frame(g_fd, HX_MAGIC, HX_RUN, id, buf, off, NULL, 0,
                                 hl_chan_mono_ms() + 1000);
    free(buf);
    if (rc != 0) {
        snprintf(why, why_size, "exec: the command broker is not answering");
        return -1;
    }
    if (hl_exec_inflight() == 0) {                /* idle time is not silence */
        g_rx_ms = hl_chan_mono_ms();
        g_ping_ms = 0;
    }
    slot->used = 1;
    slot->id = id;
    memcpy(slot->name, name, name_len);
    slot->name[name_len] = '\0';
    slot->fn = fn;
    slot->ud = ud;
    slot->stream = stream;
    slot->is_stream = stream != NULL;
    slot->t0_ms = hl_chan_mono_ms();
    *id_out = id;
    exec_audit_start(slot, args, lens, n, in_len);
    hb_arm();
    return 0;
}

static void exec_cancel_ex(uint32_t id, const char *outcome, const char *why)
{
    Pending *p = pend_find(id);
    if (!p) return;
    exec_audit_end(p, outcome, NULL, why);
    p->used = 0;
    p->stream = NULL;
    if (g_fd >= 0)
        (void)hl_chan_write_frame(g_fd, HX_MAGIC, HX_KILL, id, NULL, 0, NULL, 0,
                                  hl_chan_mono_ms() + 200);
}

void hl_exec_cancel(uint32_t id) { exec_cancel_ex(id, "cancelled", NULL); }

/* ── One exec.stream (docs/design/exec_stream.md) ──────────────────── */

typedef struct ExecWait {
    HlExecStream *s;
    HlAsyncCtx   *ctx;
    int           kind;
    int           done;          /* woken: the ctx is completing */
    uint64_t      timer;         /* the pending 0 ms wake, 0 = none */
    void        (*on_free)(void *ud);
    void         *ud;
} ExecWait;

#ifdef HL_EXEC_TEST_HOOKS
/* Test hooks (tests/e2e_exec_stream.sh): HULL_TEST_EXEC_FAIL_WRITE names a
 * file; each time a new one appears there (by inode and mtime: the sandbox
 * lets the app stat it, not remove it) the next stream write to the broker
 * fails. HULL_TEST_EXEC_WAKE_DELAY_MS delays every stream wake, widening
 * the window between a wait's wake and its resume. */
static int stream_test_fail_write(void)
{
    static ino_t last_ino;
    static time_t last_mtime;
    const char *f = getenv("HULL_TEST_EXEC_FAIL_WRITE");
    struct stat st;
    if (!f || !*f || stat(f, &st) != 0) return 0;
    if (st.st_ino == last_ino && st.st_mtime == last_mtime) return 0;
    last_ino = st.st_ino;
    last_mtime = st.st_mtime;
    return 1;
}
static uint32_t stream_test_wake_ms(void)
{
    const char *v = getenv("HULL_TEST_EXEC_WAKE_DELAY_MS");
    return v ? (uint32_t)strtoul(v, NULL, 10) : 0;
}
#else
#define stream_test_fail_write() 0
#define stream_test_wake_ms() 0u
#endif

struct HlExecStream {
    int          refs;
    uint32_t     id;
    int          live;           /* the broker has the call (a Pending holds it) */
    HlAsyncBackendCtx *loop;
    HlExecChunk *head, *tail;
    HlExecReply *result;
    int          closed;
    int          stdin_open, stdin_eof_want, stdin_eof_sent, stdin_closed;
    uint64_t     stdin_sent, stdin_max;
    uint32_t     credit;         /* stdin bytes the broker will take */
    uint8_t     *wbuf;
    size_t       wlen, woff;
    int          wfail;          /* a write's bytes were dropped unsent */
    ExecWait    *w[3];
    /* What it holds off the VM heap (its queued chunks, a write's copy
     * waiting for credit) on the VM's off-heap total (hl_exec_stream_hold_on;
     * exec_stream.md §5.1). */
    HlOffheapHold hold;
    size_t      *hold_total;
    size_t       hold_limit;
    /* A hull test stub (hl_exec_stream_stub): no broker, no slot. */
    int          stub, stub_killed;
    uint64_t     stub_cap[2], stub_total[2];
    uint8_t      stub_cut[2];
};

static uint64_t stream_audit_stdin(const HlExecStream *s) { return s->stdin_sent; }

/* Take @p n bytes on the VM's off-heap total; -1 (nothing taken) past its
 * ceiling. A stream with no total set holds nothing. */
static int stream_hold(HlExecStream *s, size_t n)
{
    return s->hold_total ? hl_offheap_take(&s->hold, s->hold_total, s->hold_limit, n) : 0;
}

static void stream_unhold(HlExecStream *s, size_t n) { hl_offheap_give(&s->hold, n); }

static void chunk_free(HlExecStream *s, HlExecChunk *c)
{
    stream_unhold(s, c->len);
    free(c);
}

/* The write copy is gone (sent, dropped or the stdin ended). */
static void stream_wbuf_free(HlExecStream *s)
{
    if (s->wbuf) stream_unhold(s, s->wlen);
    free(s->wbuf);
    s->wbuf = NULL;
    s->wlen = s->woff = 0;
}

static void wait_fire(void *user)
{
    ExecWait *w = user;
    w->timer = 0;
    w->done = 1;
    /* Off the stream before the resume: the resumed code may wait again
     * (the ctx itself is freed only after it has run). */
    if (w->s->w[w->kind] == w) w->s->w[w->kind] = NULL;
    HlAsyncCtx *ctx = w->ctx;
    if (ctx->detached) hl_async_ctx_resume_detached(ctx);
    else hl_net_op_complete(ctx->net_ctx, (HlSuspendOp *)&ctx->op);
}

static int wait_ready(const HlExecStream *s, int kind)
{
    switch (kind) {
    case HL_EXEC_WAIT_READ:  return s->head || hl_exec_stream_drained(s);
    case HL_EXEC_WAIT_WRITE: return s->woff >= s->wlen || s->stdin_closed || !s->live;
    default:                 return s->result != NULL;
    }
}

/* Wake the waits of @p s whose condition now holds, from the loop. */
static void stream_wake(HlExecStream *s)
{
    for (int k = 0; k < 3; k++) {
        ExecWait *w = s->w[k];
        if (!w || w->done || w->timer || !wait_ready(s, k) || !s->loop) continue;
        w->timer = hl_async_backend()->timer_add(s->loop, stream_test_wake_ms(), wait_fire, w);
    }
}

static void stream_drop_queue(HlExecStream *s)
{
    while (s->head) {
        HlExecChunk *c = s->head;
        s->head = c->next;
        chunk_free(s, c);
    }
    s->tail = NULL;
    if (s->woff < s->wlen) s->wfail = 1;
    stream_wbuf_free(s);
}

/* Stdin is over: a write still pending fails, and nothing is pending any
 * more (hl_exec_stream_writing() reads 0, so a parked write sees the
 * failure; review of PR 2, M1). */
static void stream_stdin_end(HlExecStream *s)
{
    s->stdin_closed = 1;
    if (s->woff < s->wlen) s->wfail = 1;
    stream_wbuf_free(s);
}

/* The call ended without a DONE (the broker lost, stopped or not answering).
 * The call's pending slot goes too, with a KILL sent if the channel takes
 * it: `live` 0 means no slot refers to the stream, so nothing reaches it
 * once it is freed (review of PR 2, H1: a failed CREDIT / STDIN write left
 * the slot, close() then sent no cancel, and a later frame or the lost-
 * broker sweep wrote into the freed stream). */
static void stream_lost(HlExecStream *s, const char *why, int wake)
{
    Pending *pd = pend_find(s->id);
    if (pd && pd->stream == s) exec_cancel_ex(s->id, "lost", why);
    s->live = 0;
    stream_stdin_end(s);
    if (!s->result && (s->result = calloc(1, sizeof *s->result)) != NULL) {
        s->result->kind = HL_EXEC_R_LOST;
        s->result->stream = 1;
        snprintf(s->result->msg, sizeof s->result->msg, "%s", why);
    }
    if (wake) stream_wake(s);
}

/* Send what the stdin credit allows; EOF once everything queued is out. */
static void stdin_flush(HlExecStream *s)
{
    while (s->live && s->woff < s->wlen && s->credit > 0) {
        size_t n = s->wlen - s->woff;
        if (n > s->credit) n = s->credit;
        if (n > HL_EXEC_STREAM_CHUNK_MAX) n = HL_EXEC_STREAM_CHUNK_MAX;
        uint8_t fl = (s->stdin_eof_want && s->woff + n == s->wlen) ? HX_STDIN_EOF : 0;
        if (stream_test_fail_write() ||
            hl_chan_write_frame(g_fd, HX_MAGIC, HX_STDIN, s->id, &fl, 1, s->wbuf + s->woff,
                                n, hl_chan_mono_ms() + 1000) != 0) {
            stream_lost(s, "exec: the command broker is not answering", 1);
            return;
        }
        s->woff += n;
        s->credit -= (uint32_t)n;
        if (fl) s->stdin_eof_sent = 1;
    }
    if (s->woff >= s->wlen) {
        stream_wbuf_free(s);
        if (s->live && s->stdin_eof_want && !s->stdin_eof_sent) {
            uint8_t fl = HX_STDIN_EOF;
            if (hl_chan_write_frame(g_fd, HX_MAGIC, HX_STDIN, s->id, &fl, 1, NULL, 0,
                                    hl_chan_mono_ms() + 1000) != 0)
                stream_lost(s, "exec: the command broker is not answering", 1);
            s->stdin_eof_sent = 1;
        }
    }
}

static uint64_t get64le(const uint8_t *p)
{
    return (uint64_t)hl_chan_get32(p) | (uint64_t)hl_chan_get32(p + 4) << 32;
}

/* A frame from the broker for streamed call @p pd. */
static void stream_deliver(Pending *pd, uint32_t type, const uint8_t *p, size_t len)
{
    HlExecStream *s = pd->stream;
    if (type == HX_CHUNK) {
        if (len < 2 || (p[0] != 1 && p[0] != 2)) return;
        HlExecChunk *c = malloc(sizeof *c + len - 1);
        if (!c) { hl_exec_stream_close(s); return; }   /* better than a silent gap */
        if (stream_hold(s, len - 1) != 0) {
            /* Past the VM's off-heap ceiling (other copies hold the rest of
             * it): the call ends, said so, rather than a silent gap. */
            free(c);
            stream_lost(s, "exec.stream: its queued output would take the VM's off-heap "
                        "copies past its heap limit", 1);
            return;
        }
        c->next = NULL;
        c->fd = p[0];
        c->len = len - 1;
        memcpy(c->data, p + 1, len - 1);
        if (s->tail) s->tail->next = c; else s->head = c;
        s->tail = c;
        stream_wake(s);
        return;
    }
    if (type == HX_STDIN_CREDIT) {
        if (len != 5) return;
        s->credit += hl_chan_get32(p + 1);
        if (p[0] & HX_STDIN_CLOSED) {
            stream_stdin_end(s);
        } else {
            stdin_flush(s);
        }
        stream_wake(s);
        return;
    }
    if (type != HX_DONE && type != HX_BUSY && type != HX_REFUSED) return;
    pd->used = 0;
    s->live = 0;
    stream_stdin_end(s);
    HlExecReply *r = calloc(1, sizeof *r);
    if (r) {
        r->stream = 1;
        if (type == HX_DONE && len >= 32) {
            r->kind = HL_EXEC_R_DONE;
            r->status = (int32_t)hl_chan_get32(p);
            int signo = (int)hl_chan_get32(p + 4);
            if (signo) {
                char b[16];
                snprintf(r->signal, sizeof r->signal, "%s", sig_name(signo, b, sizeof b));
            }
            r->timed_out = p[8] != 0;
            r->idle_timed_out = p[8] == 2;
            r->out_cut = p[9];
            r->err_cut = p[10];
            r->duration_ms = hl_chan_get32(p + 12);
            r->out_bytes = get64le(p + 16);
            r->err_bytes = get64le(p + 24);
        } else if (type == HX_DONE) {
            r->kind = HL_EXEC_R_LOST;
            snprintf(r->msg, sizeof r->msg, "exec: a malformed reply from the broker");
        } else {
            r->kind = type == HX_BUSY ? HL_EXEC_R_BUSY : HL_EXEC_R_REFUSED;
            size_t n = len < sizeof r->msg - 1 ? len : sizeof r->msg - 1;
            if (n) memcpy(r->msg, p, n);
            r->msg[n] = '\0';
        }
    }
    exec_audit_end(pd, exec_audit_outcome(r), r, r ? NULL : "exec: out of memory");
    pd->stream = NULL;
    hl_exec_reply_free(s->result);
    s->result = r;
    stream_wake(s);
}

HlExecStream *hl_exec_stream_start(HlAsyncBackendCtx *loop, const char *name,
                                   size_t name_len, const char *const *args,
                                   const size_t *lens, size_t n,
                                   const void *in, size_t in_len, int stdin_open,
                                   uint64_t stdin_max, char *why, size_t why_size)
{
    HlExecStream *s = calloc(1, sizeof *s);
    if (!s) { snprintf(why, why_size, "exec: out of memory"); return NULL; }
    s->refs = 1;
    s->stdin_open = stdin_open;
    s->stdin_max = stdin_max;
    s->stdin_sent = in_len;
    if (!stdin_open) s->stdin_closed = 1;
    unsigned fl = HX_RUN_STREAM | (stdin_open ? HX_RUN_STDIN_OPEN : 0);
    if (submit_ex(loop, name, name_len, args, lens, n, in, in_len, fl, NULL, NULL, s,
                  &s->id, why, why_size) != 0) {
        free(s);
        return NULL;
    }
    s->live = 1;
    s->loop = g_loop;
    return s;
}

/* ── hull test stub streams ─────────────────────────────────────────── */

HlExecStream *hl_exec_stream_stub(int stdin_open, uint64_t stdin_max, size_t in_len,
                                  uint64_t stdout_max, uint64_t stderr_max)
{
    HlExecStream *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->refs = 1;
    s->stub = 1;
    s->stdin_open = stdin_open;
    s->stdin_max = stdin_max;
    s->stdin_sent = in_len;
    if (!stdin_open) s->stdin_closed = 1;
    s->stub_cap[0] = stdout_max;
    s->stub_cap[1] = stderr_max;
    return s;
}

int hl_exec_stream_is_stub(const HlExecStream *s) { return s && s->stub; }

void hl_exec_stream_hold_on(HlExecStream *s, size_t *total, size_t limit)
{
    if (!s) return;
    s->hold_total = total;
    s->hold_limit = limit;
}

static void stub_mark_killed(HlExecStream *s)
{
    s->stub_killed = 1;
    if (s->result) {
        s->result->status = 0;
        snprintf(s->result->signal, sizeof s->result->signal, "SIGKILL");
    }
}

int hl_exec_stream_stub_chunk(HlExecStream *s, int fd, const void *data, size_t len)
{
    if (!s->stub || (fd != 1 && fd != 2)) return -1;
    if (s->stub_killed || s->closed) return 0;     /* the command is gone */
    int k = fd - 1;
    uint64_t room = s->stub_cap[k] - s->stub_total[k];
    int cut = 0;
    if ((uint64_t)len > room) { len = (size_t)room; cut = 1; }
    /* Into chunks of at most CHUNK_MAX, as the broker sends them. */
    const uint8_t *p = data;
    while (len) {
        size_t n = len < HL_EXEC_STREAM_CHUNK_MAX ? len : HL_EXEC_STREAM_CHUNK_MAX;
        if (stream_hold(s, n) != 0) return -2;
        HlExecChunk *c = malloc(sizeof *c + n);
        if (!c) { stream_unhold(s, n); return -1; }
        c->next = NULL;
        c->fd = (uint8_t)fd;
        c->len = n;
        memcpy(c->data, p, n);
        if (s->tail) s->tail->next = c; else s->head = c;
        s->tail = c;
        s->stub_total[k] += n;
        p += n;
        len -= n;
    }
    if (s->result) {
        s->result->out_bytes = s->stub_total[0];
        s->result->err_bytes = s->stub_total[1];
    }
    if (cut) {
        /* The broker kills a stream at its total (§5). */
        s->stub_cut[k] = 1;
        if (s->result) { s->result->out_cut = s->stub_cut[0]; s->result->err_cut = s->stub_cut[1]; }
        stub_mark_killed(s);
    }
    return 0;
}

int hl_exec_stream_stub_finish(HlExecStream *s, int32_t status, const char *signal,
                               int timed_out, uint32_t duration_ms)
{
    if (!s->stub || s->result) return -1;
    HlExecReply *r = calloc(1, sizeof *r);
    if (!r) return -1;
    r->kind = HL_EXEC_R_DONE;
    r->stream = 1;
    r->status = status;
    if (signal) snprintf(r->signal, sizeof r->signal, "%s", signal);
    r->timed_out = (uint8_t)(timed_out != 0);
    r->idle_timed_out = (uint8_t)(timed_out == 2);
    r->duration_ms = duration_ms;
    r->out_bytes = s->stub_total[0];
    r->err_bytes = s->stub_total[1];
    r->out_cut = s->stub_cut[0];
    r->err_cut = s->stub_cut[1];
    s->result = r;
    if (s->stub_killed) stub_mark_killed(s);
    return 0;
}

int hl_exec_stream_stub_write(HlExecStream *s, size_t n, char *why, size_t why_size,
                              const char **code)
{
    *code = "stdin_closed";
    if (!s->stdin_open) {
        snprintf(why, why_size, "exec.stream: stdin is not open (pass stdin_open)");
        return -1;
    }
    if (s->stdin_eof_want || s->closed || s->stub_killed) {
        snprintf(why, why_size, "exec.stream: the command's stdin is closed");
        return -1;
    }
    if (n > HL_EXEC_STREAM_WRITE_MAX || n > s->stdin_max - s->stdin_sent) {
        *code = "stdin_too_large";
        snprintf(why, why_size, n > HL_EXEC_STREAM_WRITE_MAX
                 ? "exec.stream: a write is at most %u bytes"
                 : "exec.stream: stdin past the command's stdin_max",
                 (unsigned)HL_EXEC_STREAM_WRITE_MAX);
        return -1;
    }
    s->stdin_sent += n;
    return 0;
}

void hl_exec_stream_retain(HlExecStream *s) { if (s) s->refs++; }

void hl_exec_stream_release(HlExecStream *s)
{
    if (!s || --s->refs > 0) return;
    hl_exec_stream_close(s);
    Pending *pd = pend_find(s->id);       /* never left pointing at freed memory */
    if (pd && pd->stream == s) { pd->used = 0; pd->stream = NULL; }
    stream_drop_queue(s);
    hl_offheap_drop(&s->hold);
    hl_exec_reply_free(s->result);
    free(s);
}

HlExecChunk *hl_exec_stream_take(HlExecStream *s)
{
    /* A read is parked (or woken and not yet resumed): what is queued is
     * its. A second reader gets nothing and is then refused as busy when
     * it tries to wait (review of PR 2, M2). */
    if (s->w[HL_EXEC_WAIT_READ]) return NULL;
    HlExecChunk *c = s->head;
    if (!c) return NULL;
    s->head = c->next;
    if (!s->head) s->tail = NULL;
    c->next = NULL;
    stream_unhold(s, c->len);   /* the caller copies it onto the VM heap and frees it */
    /* The app has it now: the broker may read that much more. */
    if (s->live) {
        uint8_t pl[5];
        pl[0] = c->fd;
        hl_chan_put32(pl + 1, (uint32_t)c->len);
        if (stream_test_fail_write() ||
            hl_chan_write_frame(g_fd, HX_MAGIC, HX_CREDIT, s->id, pl, 5, NULL, 0,
                                hl_chan_mono_ms() + 1000) != 0)
            stream_lost(s, "exec: the command broker is not answering", 1);
    }
    return c;
}

size_t hl_exec_stream_peek(const HlExecStream *s)
{
    if (s->w[HL_EXEC_WAIT_READ]) return (size_t)-1;   /* a parked read's (take) */
    return s->head ? s->head->len : (size_t)-1;
}

int hl_exec_stream_drained(const HlExecStream *s)
{
    return !s->head && (s->result != NULL || s->closed);
}

const HlExecReply *hl_exec_stream_result(const HlExecStream *s) { return s->result; }
int hl_exec_stream_closed(const HlExecStream *s) { return s->closed; }
int hl_exec_stream_writing(const HlExecStream *s) { return s->woff < s->wlen; }
int hl_exec_stream_write_would_wait(const HlExecStream *s, size_t n)
{
    return n > s->credit || s->w[HL_EXEC_WAIT_WRITE] != NULL || s->woff < s->wlen;
}
int hl_exec_stream_write_ok(const HlExecStream *s) { return !s->wfail && s->woff >= s->wlen; }

int hl_exec_stream_write(HlExecStream *s, const void *p, size_t n,
                         char *why, size_t why_size, const char **code)
{
    *code = "stdin_closed";
    if (!s->stdin_open) {
        snprintf(why, why_size, "exec.stream: stdin is not open (pass stdin_open)");
        return -1;
    }
    if (s->stdin_closed || s->stdin_eof_want || !s->live) {
        snprintf(why, why_size, "exec.stream: the command's stdin is closed");
        return -1;
    }
    if (s->woff < s->wlen || s->w[HL_EXEC_WAIT_WRITE]) {
        *code = "busy";
        snprintf(why, why_size, "exec.stream: a write is already waiting");
        return -1;
    }
    if (n > HL_EXEC_STREAM_WRITE_MAX || n > s->stdin_max - s->stdin_sent) {
        *code = "stdin_too_large";
        snprintf(why, why_size, n > HL_EXEC_STREAM_WRITE_MAX
                 ? "exec.stream: a write is at most %u bytes"
                 : "exec.stream: stdin past the command's stdin_max (%u)",
                 n > HL_EXEC_STREAM_WRITE_MAX ? (unsigned)HL_EXEC_STREAM_WRITE_MAX
                                              : (unsigned)(s->stdin_max > UINT32_MAX
                                                           ? UINT32_MAX : s->stdin_max));
        return -1;
    }
    if (n == 0) return 1;
    if (stream_hold(s, n) != 0) {
        *code = "busy";
        snprintf(why, why_size, "exec.stream: the VM's off-heap copies are at its heap "
                 "limit");
        return -1;
    }
    s->wbuf = malloc(n);
    if (!s->wbuf) {
        stream_unhold(s, n);
        *code = "busy";
        snprintf(why, why_size, "exec.stream: out of memory");
        return -1;
    }
    memcpy(s->wbuf, p, n);
    s->wlen = n;
    s->woff = 0;
    s->wfail = 0;
    s->stdin_sent += n;
    stdin_flush(s);
    return s->woff >= s->wlen ? 1 : 0;
}

int hl_exec_stream_close_write(HlExecStream *s, char *why, size_t why_size)
{
    if (!s->stdin_open) {
        snprintf(why, why_size, "exec.stream: stdin is not open (pass stdin_open)");
        return -1;
    }
    if (s->stdin_eof_want) return 0;
    s->stdin_eof_want = 1;
    if (s->live && !s->stdin_closed) stdin_flush(s);
    return 0;
}

void hl_exec_stream_close(HlExecStream *s)
{
    if (!s || s->closed) return;
    s->closed = 1;
    if (s->live) {
        hl_exec_cancel(s->id);       /* KILL; the broker sends nothing more */
        s->live = 0;
    }
    s->stdin_closed = 1;
    stream_drop_queue(s);
    if (!s->result && (s->result = calloc(1, sizeof *s->result)) != NULL) {
        s->result->kind = HL_EXEC_R_DONE;
        s->result->stream = 1;
        snprintf(s->result->signal, sizeof s->result->signal, "SIGKILL");
    }
    stream_wake(s);
}

static void wait_free(void *driver)
{
    ExecWait *w = driver;
    if (!w) return;
    HlExecStream *s = w->s;
    if (w->timer && s->loop) hl_async_backend()->timer_cancel(s->loop, w->timer);
    if (s->w[w->kind] == w) s->w[w->kind] = NULL;
    /* Freed before it was woken: a client gone, a cancelled run, a
     * teardown. Nothing will read the stream: end it. */
    if (!w->done) hl_exec_stream_close(s);
    if (w->on_free) w->on_free(w->ud);
    free(w);
    hl_exec_stream_release(s);
}

int hl_exec_stream_wait(HlAsyncCtx *ctx, HlExecStream *s, int kind)
{
    return hl_exec_stream_wait_ex(ctx, s, kind, NULL, NULL);
}

int hl_exec_stream_wait_ex(HlAsyncCtx *ctx, HlExecStream *s, int kind,
                           void (*on_free)(void *ud), void *ud)
{
    if (kind < 0 || kind > 2 || s->w[kind]) return -1;
    ExecWait *w = calloc(1, sizeof *w);
    if (!w) return -1;
    w->s = s;
    w->ctx = ctx;
    w->kind = kind;
    w->on_free = on_free;
    w->ud = ud;
    hl_exec_stream_retain(s);
    s->w[kind] = w;
    ctx->driver = w;
    ctx->free_driver = wait_free;
    if (!s->loop) s->loop = g_loop;
    stream_wake(s);                  /* already ready: woken from the loop */
    return 0;
}

HlExecStream *hl_exec_stream_of_wait(void *driver)
{
    return driver ? ((ExecWait *)driver)->s : NULL;
}

void *hl_exec_stream_wait_ud(void *driver)
{
    return driver ? ((ExecWait *)driver)->ud : NULL;
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
        exec_cancel_ex(op->id, "lost", "exec: the command broker did not answer");
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
HlExecStream *hl_exec_stream_start(HlAsyncBackendCtx *loop, const char *name,
                                   size_t name_len, const char *const *args,
                                   const size_t *lens, size_t n,
                                   const void *in, size_t in_len, int stdin_open,
                                   uint64_t stdin_max, char *why, size_t why_size)
{
    (void)loop; (void)name; (void)name_len; (void)args; (void)lens; (void)n;
    (void)in; (void)in_len; (void)stdin_open; (void)stdin_max;
    snprintf(why, why_size, "exec: this host has no command broker");
    return NULL;
}
void hl_exec_stream_retain(HlExecStream *s) { (void)s; }
void hl_exec_stream_release(HlExecStream *s) { (void)s; }
HlExecChunk *hl_exec_stream_take(HlExecStream *s) { (void)s; return NULL; }
HlExecStream *hl_exec_stream_stub(int stdin_open, uint64_t stdin_max, size_t in_len,
                                  uint64_t stdout_max, uint64_t stderr_max)
{
    (void)stdin_open; (void)stdin_max; (void)in_len; (void)stdout_max; (void)stderr_max;
    return NULL;
}
int hl_exec_stream_is_stub(const HlExecStream *s) { (void)s; return 0; }
void hl_exec_stream_hold_on(HlExecStream *s, size_t *total, size_t limit)
{
    (void)s; (void)total; (void)limit;
}
int hl_exec_stream_stub_chunk(HlExecStream *s, int fd, const void *data, size_t len)
{
    (void)s; (void)fd; (void)data; (void)len;
    return -1;
}
int hl_exec_stream_stub_finish(HlExecStream *s, int32_t status, const char *signal,
                               int timed_out, uint32_t duration_ms)
{
    (void)s; (void)status; (void)signal; (void)timed_out; (void)duration_ms;
    return -1;
}
int hl_exec_stream_stub_write(HlExecStream *s, size_t n, char *why, size_t why_size,
                              const char **code)
{
    (void)s; (void)n;
    *code = "stdin_closed";
    snprintf(why, why_size, "exec.stream: this host has no command broker");
    return -1;
}
size_t hl_exec_stream_peek(const HlExecStream *s) { (void)s; return (size_t)-1; }
int hl_exec_stream_drained(const HlExecStream *s) { (void)s; return 1; }
const HlExecReply *hl_exec_stream_result(const HlExecStream *s) { (void)s; return NULL; }
int hl_exec_stream_write(HlExecStream *s, const void *p, size_t n,
                         char *why, size_t why_size, const char **code)
{
    (void)s; (void)p; (void)n;
    *code = "stdin_closed";
    snprintf(why, why_size, "exec: this host has no command broker");
    return -1;
}
int hl_exec_stream_writing(const HlExecStream *s) { (void)s; return 0; }
int hl_exec_stream_write_would_wait(const HlExecStream *s, size_t n) { (void)s; (void)n; return 0; }
int hl_exec_stream_write_ok(const HlExecStream *s) { (void)s; return 0; }
int hl_exec_stream_close_write(HlExecStream *s, char *why, size_t why_size)
{
    (void)s;
    snprintf(why, why_size, "exec: this host has no command broker");
    return -1;
}
void hl_exec_stream_close(HlExecStream *s) { (void)s; }
int hl_exec_stream_closed(const HlExecStream *s) { (void)s; return 1; }
int hl_exec_stream_wait(HlAsyncCtx *ctx, HlExecStream *s, int kind)
{
    (void)ctx; (void)s; (void)kind;
    return -1;
}
HlExecStream *hl_exec_stream_of_wait(void *driver) { (void)driver; return NULL; }
int hl_exec_stream_wait_ex(HlAsyncCtx *ctx, HlExecStream *s, int kind,
                           void (*on_free)(void *ud), void *ud)
{
    (void)ctx; (void)s; (void)kind; (void)on_free; (void)ud;
    return -1;
}
void *hl_exec_stream_wait_ud(void *driver) { (void)driver; return NULL; }
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
