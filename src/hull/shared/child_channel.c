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

pid_t hl_chan_spawn_env(const char *exe, char *const argv[], int fd,
                        int (*env_ok)(const char *kv), const char *const *extra)
{
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    if (posix_spawn_file_actions_init(&fa) != 0) return -1;
    if (posix_spawnattr_init(&at) != 0) {
        posix_spawn_file_actions_destroy(&fa);
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
    if (pid < 0) errno = err;
    return pid;
}

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

void hl_chan_close_extra_fds(void)
{
#if defined(__linux__) && defined(__NR_close_range)
    if (syscall(__NR_close_range, 4u, ~0u, 0u) == 0)
        return;
#endif
    struct rlimit rl;
    long max = 4096;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
        rl.rlim_cur < 65536)
        max = (long)rl.rlim_cur;
    for (long fd = 4; fd < max; fd++)
        (void)close((int)fd);
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
