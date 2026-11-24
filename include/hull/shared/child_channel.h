/*
 * child_channel.h - a helper process on one inherited socket: spawn it with
 * the socket as its fd 3, exchange length-framed messages with deadlines,
 * reap or kill it. Shared by the GPU compile helper (cap/gpu_helper.c) and
 * the build plugin runner (docs/buildcontext_design.md §9.1).
 *
 * Frame: a 16-byte header - magic | type | id | payload length, each a
 * little-endian u32 - then the payload. The magic names the protocol; a
 * frame with another magic, or a payload longer than the reader's max, is a
 * protocol error.
 *
 * HL_CHAN_SUPPORTED: every POSIX build and a cosmo APE, on every host it runs
 * on (Windows included, #1188); not a native Windows build.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_SHARED_CHILD_CHANNEL_H
#define HL_SHARED_CHILD_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#if defined(__COSMOPOLITAN__) || !defined(_WIN32)
#define HL_CHAN_SUPPORTED 1
#include <sys/types.h>
#endif

#define HL_CHAN_HDR 16u

static inline void hl_chan_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t hl_chan_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* The frame types, the reader and the clock are portable (a caller on a
 * host with no channel still compiles); everything that touches a socket or
 * a process is POSIX only. */

/* CLOCK_MONOTONIC in milliseconds; deadlines below are on this clock. */
uint64_t hl_chan_mono_ms(void);

/* An incremental frame reader over a non-blocking socket. Initialise with
 * the protocol's magic and the largest payload taken:
 *   HlChanFrame r = { .magic = M, .max = N }; */
typedef struct {
    uint8_t  hdr[HL_CHAN_HDR];
    size_t   got;        /* bytes of header + payload read so far */
    uint32_t type, id, len;
    uint8_t *buf;        /* the payload (len bytes), malloc'd, zeroed */
    uint32_t magic;      /* the protocol's magic */
    uint32_t max;        /* the largest payload taken */
} HlChanFrame;

#define HL_CHAN_MORE      0
#define HL_CHAN_DONE      1
#define HL_CHAN_EOF      -1
#define HL_CHAN_ERROR    -2
#define HL_CHAN_OVERSIZE -3

/* Free the payload and make @p r ready for the next frame (keeps magic and
 * max). */
void hl_chan_frame_reset(HlChanFrame *r);

#ifdef HL_CHAN_SUPPORTED

/* 1 when @p fd is a channel end: a socket, or on a cosmo APE running on
 * Windows a pipe (its socketpair is a pair of duplex pipes there). */
int hl_chan_fd_is_channel(int fd);

/* Milliseconds left until @p deadline as a poll timeout (0 = no deadline:
 * -1, wait for ever). */
int hl_chan_ms_left(uint64_t deadline);

void hl_chan_set_nonblock(int fd);
void hl_chan_set_nosigpipe(int fd);

/* Write @p n bytes to a non-blocking socket, waiting for room at most until
 * @p deadline (0 = none). 0, or -1 (error, peer gone, deadline). */
int hl_chan_write_all(int fd, const void *buf, size_t n, uint64_t deadline);

/* One frame: the header, then @p a and @p b (either may be empty). 0 / -1. */
int hl_chan_write_frame(int fd, uint32_t magic, uint32_t type, uint32_t id,
                        const void *a, size_t alen, const void *b, size_t blen,
                        uint64_t deadline);

/* Read what the socket has. HL_CHAN_DONE when a whole frame is in @p r (the
 * caller takes it, then hl_chan_frame_reset), HL_CHAN_MORE when it needs
 * more, else an error (HL_CHAN_EOF before a whole frame, HL_CHAN_ERROR - a
 * wrong magic included - or HL_CHAN_OVERSIZE past r->max). */
int hl_chan_frame_read_some(int fd, HlChanFrame *r);

/* Read one whole frame, waiting at most until @p deadline (0 = none).
 * HL_CHAN_DONE, HL_CHAN_MORE on the deadline, or an error. */
int hl_chan_frame_read(int fd, HlChanFrame *r, uint64_t deadline);

/* Spawn @p exe with @p argv and the environment entries @p env_ok accepts
 * (NULL = none), @p fd as its fd 3, stdin / stdout / stderr on /dev/null,
 * signals unblocked and defaulted. The pid, or -1 with errno set. */
pid_t hl_chan_spawn(const char *exe, char *const argv[], int fd,
                    int (*env_ok)(const char *kv));

/* hl_chan_spawn, and the "NAME=value" entries of @p extra (NULL-terminated,
 * may be NULL) added after the filtered ones. */
pid_t hl_chan_spawn_env(const char *exe, char *const argv[], int fd,
                        int (*env_ok)(const char *kv), const char *const *extra);

/* This process's binary, as a path posix_spawn takes (Linux:
 * /proc/self/exe itself; a cosmo APE: GetProgramExecutableName, on any
 * host). 0 / -1. */
int hl_chan_self_exe(char *out, size_t sz);

/* In the child: close every descriptor above 3 (it inherits only 0-3).
 * Linux: close_range, else every entry of /proc/self/fd; a cosmo APE:
 * closefrom; else every number up to the descriptor limit.
 * Async-signal-safe. */
void hl_chan_close_extra_fds(void);

#ifdef HL_CHAN_TEST_HOOKS
/* Tests only (child_channel_th.o): 1 skips close_range, 2 skips /proc too,
 * so the fallbacks run on a kernel that has both. */
void hl_chan_test_force_close_fallback(int mode);
#endif

/* Reap @p pid within @p ms (it was killed, or is exiting). 1 = reaped (its
 * status in *status, its peak RSS in bytes in *maxrss when non-NULL),
 * 0 = still there. */
int hl_chan_reap(pid_t pid, int *status, uint32_t ms, uint64_t *maxrss);

#endif /* HL_CHAN_SUPPORTED */

#endif /* HL_SHARED_CHILD_CHANNEL_H */
