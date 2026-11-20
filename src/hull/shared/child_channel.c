/*
 * child_channel.c - a helper process on one inherited socket (see
 * include/hull/shared/child_channel.h). Moved here from cap/gpu_helper.c,
 * its first user, unchanged but for the magic each protocol now names.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/shared/child_channel.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t hl_chan_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void hl_chan_frame_reset(HlChanFrame *r)
{
    free(r->buf);
    uint32_t magic = r->magic, max = r->max;
    memset(r, 0, sizeof *r);
    r->magic = magic;
    r->max = max;
}

#ifdef HL_CHAN_SUPPORTED

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#endif

extern char **environ;

#ifdef MSG_NOSIGNAL
#define CHAN_SEND_FLAGS MSG_NOSIGNAL
#else
#define CHAN_SEND_FLAGS 0
#endif

void hl_chan_set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

void hl_chan_set_nosigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#else
    (void)fd;
#endif
}

int hl_chan_ms_left(uint64_t deadline)
{
    if (!deadline) return -1;
    uint64_t now = hl_chan_mono_ms();
    if (now >= deadline) return 0;
    uint64_t left = deadline - now;
    return left > INT_MAX ? INT_MAX : (int)left;
}

int hl_chan_write_all(int fd, const void *buf, size_t n, uint64_t deadline)
{
    const uint8_t *p = buf;
    while (n > 0) {
        ssize_t w = send(fd, p, n, CHAN_SEND_FLAGS);
        if (w > 0) { p += w; n -= (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pf = { .fd = fd, .events = POLLOUT };
            int left = hl_chan_ms_left(deadline);
            if (left == 0) return -1;
            int pr = poll(&pf, 1, left);
            if (pr < 0 && errno == EINTR) continue;
            if (pr <= 0) return -1;
            if (pf.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

int hl_chan_write_frame(int fd, uint32_t magic, uint32_t type, uint32_t id,
                        const void *a, size_t alen, const void *b, size_t blen,
                        uint64_t deadline)
{
    if (alen + blen > UINT32_MAX) return -1;
    uint8_t hdr[HL_CHAN_HDR];
    hl_chan_put32(hdr, magic);
    hl_chan_put32(hdr + 4, type);
    hl_chan_put32(hdr + 8, id);
    hl_chan_put32(hdr + 12, (uint32_t)(alen + blen));
    if (hl_chan_write_all(fd, hdr, sizeof hdr, deadline) != 0) return -1;
    if (alen && hl_chan_write_all(fd, a, alen, deadline) != 0) return -1;
    if (blen && hl_chan_write_all(fd, b, blen, deadline) != 0) return -1;
    return 0;
}

int hl_chan_frame_read_some(int fd, HlChanFrame *r)
{
    for (;;) {
        if (r->got < HL_CHAN_HDR) {
            ssize_t n = recv(fd, r->hdr + r->got, HL_CHAN_HDR - r->got, 0);
            if (n == 0) return HL_CHAN_EOF;
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return HL_CHAN_MORE;
                return HL_CHAN_ERROR;
            }
            r->got += (size_t)n;
            if (r->got < HL_CHAN_HDR) continue;
            if (hl_chan_get32(r->hdr) != r->magic) return HL_CHAN_ERROR;
            r->type = hl_chan_get32(r->hdr + 4);
            r->id   = hl_chan_get32(r->hdr + 8);
            r->len  = hl_chan_get32(r->hdr + 12);
            if (r->len > r->max) return HL_CHAN_OVERSIZE;
            if (r->len > 0) {
                r->buf = calloc(1, r->len);   /* zeroed: never read uninitialised */
                if (!r->buf) return HL_CHAN_ERROR;
            }
        }
        size_t have = r->got - HL_CHAN_HDR;
        if (have == r->len) return HL_CHAN_DONE;
        ssize_t n = recv(fd, r->buf + have, r->len - have, 0);
        if (n == 0) return HL_CHAN_EOF;
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return HL_CHAN_MORE;
            return HL_CHAN_ERROR;
        }
        r->got += (size_t)n;
    }
}

int hl_chan_frame_read(int fd, HlChanFrame *r, uint64_t deadline)
{
    for (;;) {
        int rc = hl_chan_frame_read_some(fd, r);
        if (rc != HL_CHAN_MORE) return rc;
        struct pollfd pf = { .fd = fd, .events = POLLIN };
        int left = hl_chan_ms_left(deadline);
        if (left == 0) return HL_CHAN_MORE;
        int pr = poll(&pf, 1, left);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) return HL_CHAN_ERROR;
        if (pr == 0) return HL_CHAN_MORE;
    }
}

/* The environment entries @p env_ok accepts, borrowed from environ, then
 * @p extra. */
static char **filtered_env(int (*env_ok)(const char *kv), const char *const *extra)
{
    size_t n = 0, x = 0;
    for (char **e = environ; e && *e; e++) n++;
    while (extra && extra[x]) x++;
    char **out = calloc(n + x + 1, sizeof *out);
    if (!out) return NULL;
    size_t k = 0;
    if (env_ok)
        for (char **e = environ; e && *e; e++)
            if (env_ok(*e)) out[k++] = *e;
    for (size_t i = 0; i < x; i++) out[k++] = (char *)(uintptr_t)extra[i];   /* posix_spawn does not modify it */
    out[k] = NULL;
    return out;
}

pid_t hl_chan_spawn(const char *exe, char *const argv[], int fd,
                    int (*env_ok)(const char *kv))
{
    return hl_chan_spawn_env(exe, argv, fd, env_ok, NULL);
}

/* A copy of the channel @p fd at 3 or above, close-on-exec, when @p fd is one
 * of 0-2: a process started with stdio closed gets its socketpair there, and
 * the /dev/null set-up of 0-2 replaced it before it reached fd 3 (audit 50).
 * @p fd itself otherwise. *owned is set when the caller must close the copy. */
static int chan_fd_above_stdio(int fd, int *owned)
{
    *owned = 0;
    if (fd > 2) return fd;
    int hi = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (hi >= 0) *owned = 1;
    return hi;
}

#if defined(__linux__) && !defined(__COSMOPOLITAN__)
/* Linux: posix_spawn has no close-by-default (Apple's spawn takes
 * POSIX_SPAWN_CLOEXEC_DEFAULT below), so a descriptor the parent holds
 * without close-on-exec - one a library or an earlier open left - reached
 * the child: a build plugin, a cloned repository's code under Landlock,
 * read or wrote through it (audit 49). Fork, and in the child set up the
 * same layout the spawn would, close every descriptor from 4 on and exec.
 * Everything is allocated before the fork, and the child calls only
 * async-signal-safe functions (the parent may have threads).
 *
 * A failed set-up or execve is reported as posix_spawn reports it: the child
 * writes its errno to a close-on-exec socket, which reads EOF once the exec
 * succeeded (audit 50: the pid of a child that only _exit(127)ed). */
static void close_fds_from(unsigned lo);

static void child_fail(int ew)
{
    int e = errno;
    ssize_t w = write(ew, &e, sizeof e);
    (void)w;
    _exit(127);
}

pid_t hl_chan_spawn_env(const char *exe, char *const argv[], int fd,
                        int (*env_ok)(const char *kv), const char *const *extra)
{
    int owned;
    fd = chan_fd_above_stdio(fd, &owned);
    if (fd < 0) return -1;
    int ep[2];
    /* A socketpair, not pipe2 (which glibc hides outside _GNU_SOURCE): made
     * close-on-exec atomically, so another thread's spawn cannot hold it. */
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ep) != 0) {
        int e = errno;
        if (owned) close(fd);
        errno = e;
        return -1;
    }
    char **env = filtered_env(env_ok, extra);
    if (!env) {
        close(ep[0]); close(ep[1]);
        if (owned) close(fd);
        errno = ENOMEM;
        return -1;
    }
    sigset_t all, prev;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &prev);   /* no handler runs in the child */
    pid_t pid = fork();
    if (pid == 0) {
        /* The report pipe out of the way of 0-4 until its final place. */
        int ew = fcntl(ep[1], F_DUPFD_CLOEXEC, 5);
        if (ew < 0) _exit(127);
        int nul = open("/dev/null", O_RDWR);
        if (nul < 0 || dup2(nul, 0) < 0 || dup2(nul, 1) < 0 || dup2(nul, 2) < 0)
            child_fail(ew);
        if (fd == 3) {
            if (fcntl(3, F_SETFD, 0) < 0) child_fail(ew);
        } else if (dup2(fd, 3) < 0) {
            child_fail(ew);
        }
        if (ew != 4) {
            if (dup2(ew, 4) < 0 || fcntl(4, F_SETFD, FD_CLOEXEC) < 0) child_fail(ew);
            ew = 4;
        }
        close_fds_from(5);
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        for (int sig = 1; sig < NSIG; sig++)
            (void)sigaction(sig, &dfl, NULL);   /* SIGKILL / SIGSTOP refuse: fine */
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        execve(exe, argv, env);
        child_fail(ew);
    }
    int err = errno;
    pthread_sigmask(SIG_SETMASK, &prev, NULL);
    free(env);
    close(ep[1]);
    if (owned) close(fd);
    if (pid > 0) {
        int child_err = 0;
        ssize_t r;
        do r = read(ep[0], &child_err, sizeof child_err);
        while (r < 0 && errno == EINTR);
        if (r == (ssize_t)sizeof child_err) {
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR) { }
            pid = -1;
            err = child_err ? child_err : ECHILD;
        }
    }
    close(ep[0]);
    if (pid < 0) errno = err;
    return pid;
}
#else
pid_t hl_chan_spawn_env(const char *exe, char *const argv[], int fd,
                        int (*env_ok)(const char *kv), const char *const *extra)
{
    int owned;
    fd = chan_fd_above_stdio(fd, &owned);
    if (fd < 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    if (posix_spawn_file_actions_init(&fa) != 0) {
        if (owned) close(fd);
        return -1;
    }
    if (posix_spawnattr_init(&at) != 0) {
        posix_spawn_file_actions_destroy(&fa);
        if (owned) close(fd);
        return -1;
    }
    int ok = 1;
    ok = ok && posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0) == 0;
    ok = ok && posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0) == 0;
    ok = ok && posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0) == 0;
    ok = ok && posix_spawn_file_actions_adddup2(&fa, fd, 3) == 0;
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    /* Apple: every descriptor not named above is closed in the child. */
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    ok = ok && posix_spawnattr_setflags(&at, flags) == 0;
    ok = ok && posix_spawnattr_setsigmask(&at, &none) == 0;
    ok = ok && posix_spawnattr_setsigdefault(&at, &all) == 0;
    char **env = ok ? filtered_env(env_ok, extra) : NULL;
    pid_t pid = -1;
    int err = ok && env ? posix_spawn(&pid, exe, &fa, &at, argv, env) : ENOMEM;
    if (err != 0) pid = -1;
    free(env);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);
    if (owned) close(fd);
    if (pid < 0) errno = err;
    return pid;
}
#endif

int hl_chan_self_exe(char *out, size_t sz)
{
#if defined(__linux__)
    if (sz < sizeof "/proc/self/exe") return -1;
    memcpy(out, "/proc/self/exe", sizeof "/proc/self/exe");
    return 0;
#elif defined(__APPLE__)
    char raw[PATH_MAX];
    uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n) != 0) return -1;
    char real[PATH_MAX];
    if (!realpath(raw, real)) return -1;
    if (strlen(real) >= sz) return -1;
    memcpy(out, real, strlen(real) + 1);
    return 0;
#else
    (void)out; (void)sz;
    return -1;
#endif
}

#ifdef HL_CHAN_TEST_HOOKS
static int g_close_fallback;   /* 1: skip close_range, 2: skip /proc too */
void hl_chan_test_force_close_fallback(int mode) { g_close_fallback = mode; }
#else
enum { g_close_fallback = 0 };
#endif

#if defined(__linux__)
/* Close every descriptor from @p lo on that /proc/self/fd lists, through the
 * raw getdents64 into a stack buffer (async-signal-safe: no malloc, no
 * opendir). Passes repeat until one closes nothing, since closing entries
 * while reading the directory may skip some. 0, or -1 when /proc is not
 * there. */
static int close_fds_proc(unsigned lo)
{
    int dfd = open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return -1;
    for (int closed = 1; closed; ) {
        closed = 0;
        if (lseek(dfd, 0, SEEK_SET) < 0) { close(dfd); return -1; }
        char buf[4096] __attribute__((aligned(8)));
        for (;;) {
            long n = syscall(SYS_getdents64, dfd, buf, sizeof buf);
            if (n < 0) { close(dfd); return -1; }
            if (n == 0) break;
            for (long off = 0; off < n; ) {
                unsigned short reclen;
                memcpy(&reclen, buf + off + 16, sizeof reclen);
                const char *name = buf + off + 19;   /* d_ino, d_off, d_reclen, d_type */
                off += reclen;
                if (reclen == 0) break;
                if (name[0] < '0' || name[0] > '9') continue;
                long v = 0;
                for (const char *c = name; *c >= '0' && *c <= '9' && v <= INT_MAX; c++)
                    v = v * 10 + (*c - '0');
                if (v >= (long)lo && v <= INT_MAX && v != dfd) {
                    (void)close((int)v);
                    closed = 1;
                }
            }
        }
    }
    close(dfd);
    return 0;
}
#endif

static void close_fds_from(unsigned lo)
{
#if defined(__linux__) && defined(__NR_close_range)
    if (g_close_fallback < 1 && syscall(__NR_close_range, lo, ~0u, 0u) == 0)
        return;
#endif
#if defined(__linux__)
    /* An old kernel (< 5.9) or a filter that answers ENOSYS (audit 50: the
     * loop below stopped at 4096 under a large limit, and the descriptors
     * past it reached the child). */
    if (g_close_fallback < 2 && close_fds_proc(lo) == 0)
        return;
#endif
    /* Up to the soft limit, or the hard one when that is finite and higher
     * (a descriptor opened before the soft limit was lowered can sit above
     * it), capped. */
    struct rlimit rl;
    long max = 1L << 20;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rlim_t lim = rl.rlim_cur;
        if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max > lim) lim = rl.rlim_max;
        if (lim != RLIM_INFINITY && lim < (rlim_t)max) max = (long)lim;
    }
    for (long fd = (long)lo; fd < max; fd++)
        (void)close((int)fd);
}

void hl_chan_close_extra_fds(void)
{
    close_fds_from(4);
}

int hl_chan_reap(pid_t pid, int *status, uint32_t ms, uint64_t *maxrss)
{
    uint64_t deadline = hl_chan_mono_ms() + ms;
    for (;;) {
        struct rusage ru;
        memset(&ru, 0, sizeof ru);
        pid_t r = wait4(pid, status, WNOHANG, &ru);
        if (r == pid && maxrss) {
#if defined(__APPLE__)
            *maxrss = (uint64_t)ru.ru_maxrss;            /* bytes */
#else
            *maxrss = (uint64_t)ru.ru_maxrss * 1024u;    /* KiB */
#endif
        }
        if (r == pid) return 1;
        if (r < 0 && errno != EINTR) return 1;   /* not ours any more */
        if (hl_chan_mono_ms() >= deadline) return 0;
        struct timespec ts = { 0, 5 * 1000000L };
        nanosleep(&ts, NULL);
    }
}

#endif /* HL_CHAN_SUPPORTED */
