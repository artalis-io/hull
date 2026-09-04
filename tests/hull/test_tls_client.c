/*
 * test_tls_client.c - focused coverage for the TLS-handshake fd-mode management +
 * deadline precedence in shared/tls_client.c (docs/valkey_keel_transport_slice5.md).
 *
 * White-box: direct-includes tls_client.c (built with -DHL_TLS_CLIENT_TEST_HOOKS)
 * to reach the static helpers + tls_handshake_loop and the fcntl test seam. A fake
 * KlTls scripts handshake results; the seam forces F_GETFL / F_SETFL failures.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "../../src/hull/shared/tls_client.c"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ── Helper-level restoration coverage ──────────────────────────────────────── */

/* A blocking fd: set_nonblocking enables O_NONBLOCK + saves the mode; restore puts
 * it back to blocking. The on-success restoration path. */
UTEST(tls_fd_mode, enter_nonblocking_then_restore_blocking)
{
    int sv[2]; ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    ASSERT_EQ(fcntl(sv[0], F_GETFL, 0) & O_NONBLOCK, 0);   /* starts blocking */

    int saved = -1;
    ASSERT_EQ(tls_fd_set_nonblocking(sv[0], &saved), 0);
    ASSERT_NE(fcntl(sv[0], F_GETFL, 0) & O_NONBLOCK, 0);   /* handshake sees non-blocking */

    ASSERT_EQ(tls_fd_restore(sv[0], saved), 0);
    ASSERT_EQ(fcntl(sv[0], F_GETFL, 0) & O_NONBLOCK, 0);   /* restored to blocking */

    close(sv[0]); close(sv[1]);
}

/* If the caller's fd was ALREADY non-blocking, set_nonblocking must not clobber it
 * and restore keeps it non-blocking (mode preserved exactly). */
UTEST(tls_fd_mode, restore_preserves_original_nonblocking_mode)
{
    int sv[2]; ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    int f = fcntl(sv[0], F_GETFL, 0); ASSERT_EQ(fcntl(sv[0], F_SETFL, f | O_NONBLOCK), 0);

    int saved = -1;
    ASSERT_EQ(tls_fd_set_nonblocking(sv[0], &saved), 0);
    ASSERT_NE(saved & O_NONBLOCK, 0);                      /* saved the non-blocking mode */
    ASSERT_EQ(tls_fd_restore(sv[0], saved), 0);
    ASSERT_NE(fcntl(sv[0], F_GETFL, 0) & O_NONBLOCK, 0);   /* still non-blocking */

    close(sv[0]); close(sv[1]);
}

/* ── Fail-closed paths + deadline precedence, through tls_handshake_loop ─────── */

/* Fake KlTls: only ->handshake is exercised by the loop; script its results. */
static int         g_hs_calls;
static KlTlsResult g_hs_seq[8];
static int         g_hs_n;
static KlTlsResult fake_handshake(KlTls *self, KlSocketHandle fd)
{
    (void)self; (void)fd;
    int i = g_hs_calls++;
    return (i < g_hs_n) ? g_hs_seq[i] : KL_TLS_WANT_READ;
}
static KlTls make_fake_tls(void)
{
    KlTls t; memset(&t, 0, sizeof t);   /* the loop only calls ->handshake */
    t.handshake = fake_handshake;
    return t;
}

/* fcntl seam: force F_GETFL, or the Nth F_SETFL, to fail. */
static int g_fail_getfl;
static int g_fail_setfl_nth;
static int g_setfl_n;
static int seam_fcntl(int fd, int cmd, int arg)
{
    if (cmd == F_GETFL) {
        if (g_fail_getfl) { errno = EBADF; return -1; }
        return fcntl(fd, F_GETFL, arg);
    }
    if (cmd == F_SETFL) {
        if (g_fail_setfl_nth && ++g_setfl_n == g_fail_setfl_nth) { errno = EBADF; return -1; }
        return fcntl(fd, F_SETFL, arg);
    }
    return fcntl(fd, cmd, arg);
}
static void seam_reset(void)
{
    g_hs_calls = 0; g_hs_n = 0;
    g_fail_getfl = 0; g_fail_setfl_nth = 0; g_setfl_n = 0;
    tls_test_fcntl = seam_fcntl;
}

/* F_GETFL failure -> setup fails closed; the handshake step is never attempted. */
UTEST(tls_handshake, getfl_failure_fails_closed_no_step)
{
    int sv[2]; ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    seam_reset();
    g_fail_getfl = 1;
    KlTls tls = make_fake_tls();
    ASSERT_EQ(tls_handshake_loop(&tls, sv[0], 100), -1);
    ASSERT_EQ(g_hs_calls, 0);                    /* handshake never called */
    tls_test_fcntl = NULL;
    close(sv[0]); close(sv[1]);
}

/* enable-nonblocking F_SETFL failure -> setup fails closed; step never attempted.
 * The fd is blocking, so set_nonblocking's enable is the 1st (and only) F_SETFL. */
UTEST(tls_handshake, enable_nonblocking_failure_fails_closed)
{
    int sv[2]; ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    seam_reset();
    g_fail_setfl_nth = 1;
    KlTls tls = make_fake_tls();
    ASSERT_EQ(tls_handshake_loop(&tls, sv[0], 100), -1);
    ASSERT_EQ(g_hs_calls, 0);
    tls_test_fcntl = NULL;
    close(sv[0]); close(sv[1]);
}

/* restore F_SETFL failure AFTER KL_TLS_OK -> the successful handshake is converted
 * into a failure so the caller retires the session + descriptor (never left
 * non-blocking under the wire client's blocking I/O). */
UTEST(tls_handshake, restore_failure_after_ok_fails_handshake)
{
    int sv[2]; ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    seam_reset();
    g_hs_seq[0] = KL_TLS_OK; g_hs_n = 1;         /* first step succeeds */
    g_fail_setfl_nth = 2;                        /* the 2nd F_SETFL (the restore) fails */
    KlTls tls = make_fake_tls();
    ASSERT_EQ(tls_handshake_loop(&tls, sv[0], 100), -1);
    ASSERT_EQ(g_hs_calls, 1);                    /* handshake ran once, returned OK */
    tls_test_fcntl = NULL;
    close(sv[0]); close(sv[1]);
}

/* Deadline precedence: a handshake that only completes AFTER the deadline must be
 * rejected. The first step consumes the whole budget (returns WANT_READ), then the
 * top-of-loop deadline check expires before the (would-be-OK) second step runs. */
static int g_spin_ms;
static KlTlsResult fake_handshake_spin_then_ok(KlTls *self, KlSocketHandle fd)
{
    (void)self; (void)fd;
    int i = g_hs_calls++;
    if (i == 0) {
        uint64_t start = kl_monotonic_ms();
        while (kl_monotonic_ms() < start + (uint64_t)g_spin_ms) { /* spin past deadline */ }
        return KL_TLS_WANT_READ;
    }
    return KL_TLS_OK;   /* post-expiry: MUST NOT be accepted */
}
UTEST(tls_handshake, result_after_deadline_is_rejected)
{
    int sv[2]; ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    seam_reset();
    g_spin_ms = 30;
    KlTls tls; memset(&tls, 0, sizeof tls); tls.handshake = fake_handshake_spin_then_ok;
    ASSERT_EQ(tls_handshake_loop(&tls, sv[0], 10), -1);   /* 10ms budget, first step spins 30ms */
    ASSERT_EQ(g_hs_calls, 1);                             /* the post-expiry OK step never ran */
    tls_test_fcntl = NULL;
    close(sv[0]); close(sv[1]);
}

UTEST_MAIN()
