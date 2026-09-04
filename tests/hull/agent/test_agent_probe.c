/*
 * test_agent_probe.c - deterministic coverage for the `hull agent status`
 * TCP-connect liveness probe (agent/probe.c).
 *
 * White-box: direct-includes probe.c (built with -DHL_AGENT_PROBE_TEST_HOOKS)
 * to reach hl_agent_tcp_probe and the socket-provider test seam. A fake
 * provider hands the probe REAL socketpair fds the private KlEventCtx can
 * watch (one clock domain): a "pending" address is a socketpair whose send
 * buffer is filled so its fd is never write-ready (deterministic timeout);
 * "succeed" leaves it writable with get_so_error == 0; "fail" reports
 * ECONNREFUSED; "immediate" makes connect() return 0; "hardfail" returns a
 * non-pending error inline. Provider close() calls are counted (close-once).
 * Two op tables prove both the io_status path and the hosted-errno fallback.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "../../../src/hull/agent/probe.c"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include <keel/sockaddr.h>

/* A watchdog so a wedged wait can never hang CI: SIGALRM aborts the test. */
static void ap_watchdog_fired(int sig) { (void)sig; _exit(77); }
static void ap_arm_watchdog(void) { signal(SIGALRM, ap_watchdog_fired); alarm(5); }
static void ap_disarm_watchdog(void) { alarm(0); }

enum { D_IMMEDIATE, D_PENDING, D_SUCCEED, D_FAIL, D_HARDFAIL };
static int        g_disp;
static int        g_close_n;          /* provider close() calls (close-once proof) */
static int        g_a = -1, g_b = -1; /* the mock socketpair */
static KlIoStatus g_io_status_val;    /* value fk_io_status returns */

static void fk_reset(int disp)
{
    g_disp = disp;
    g_close_n = 0;
    g_a = g_b = -1;
    g_io_status_val = KL_IO_OK;
}

static KlSocketHandle fk_socket(void *c, int d, int t, int p)
{
    (void)c; (void)d; (void)t; (void)p;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return KL_INVALID_SOCKET;
    int small = 2048;
    setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
    g_a = sv[0]; g_b = sv[1];
    return (KlSocketHandle)sv[0];
}
static int fk_set_nb(void *c, KlSocketHandle fd)
{ (void)c; int a = (int)fd; int fl = fcntl(a, F_GETFL, 0); return fcntl(a, F_SETFL, fl | O_NONBLOCK); }
static int fk_set_blocking(void *c, KlSocketHandle fd)
{ (void)c; int a = (int)fd; int fl = fcntl(a, F_GETFL, 0); return fcntl(a, F_SETFL, fl & ~O_NONBLOCK); }
static int fk_set_int(void *c, KlSocketHandle fd, int on) { (void)c; (void)fd; (void)on; return 0; }
static void fk_set_void(void *c, KlSocketHandle fd) { (void)c; (void)fd; }
static int fk_get_local(void *c, KlSocketHandle fd, KlSockAddr *out)
{ (void)c; (void)fd; uint8_t ip[4] = {127,0,0,1}; return kl_sockaddr_from_ipv4(out, ip, 0); }

static int fk_connect(void *c, KlSocketHandle fd, const KlSockAddr *addr)
{
    (void)c; (void)addr;
    int s = (int)fd;
    if (g_disp == D_IMMEDIATE) { errno = 0; return 0; }   /* rc == 0 fast path */
    if (g_disp == D_PENDING) {
        char buf[2048];
        memset(buf, 'x', sizeof buf);
        while (write(s, buf, sizeof buf) > 0) { }          /* fill SNDBUF: never write-ready */
    }
    if (g_disp == D_HARDFAIL) { errno = ECONNREFUSED; g_io_status_val = KL_IO_FATAL; return -1; }
    errno = EINPROGRESS; g_io_status_val = KL_IO_PENDING;
    return -1;
}
static int fk_get_so_error(void *c, KlSocketHandle fd, int *out)
{ (void)c; (void)fd; *out = (g_disp == D_FAIL) ? ECONNREFUSED : 0; return 0; }
static int fk_close(void *c, KlSocketHandle fd)
{ (void)c; int a = (int)fd; g_close_n++; if (g_b >= 0) { close(g_b); g_b = -1; } return close(a); }
static KlIoStatus fk_io_status(void *c) { (void)c; return g_io_status_val; }

#define FK_COMMON \
    .set_nonblocking = fk_set_nb, .set_blocking = fk_set_blocking, \
    .set_cloexec = fk_set_void, .set_nosigpipe = fk_set_void, \
    .set_reuseaddr = fk_set_int, .set_reuseport = fk_set_int, \
    .set_ipv6only = fk_set_int, .set_tcp_nodelay = fk_set_int, .set_cork = fk_set_int, \
    .socket = fk_socket, .connect = fk_connect, .close = fk_close, \
    .get_local_addr = fk_get_local, .get_so_error = fk_get_so_error

/* FK_OPS has NO io_status (probe uses the hosted-errno fallback); FK_OPS_IOS
 * supplies io_status (probe uses it, never errno). */
static const KlSocketOps FK_OPS     = { FK_COMMON };
static const KlSocketOps FK_OPS_IOS = { FK_COMMON, .io_status = fk_io_status };
static const KlSocketProvider FK_PROVIDER =
    { .ops = &FK_OPS, .context = NULL, .capabilities = KL_SOCK_CAP_NATIVE_FD };
static const KlSocketProvider FK_PROVIDER_IOS =
    { .ops = &FK_OPS_IOS, .context = NULL, .capabilities = KL_SOCK_CAP_NATIVE_FD };

/* ── immediate success: connect() rc == 0, no event loop entered ────── */
UTEST(agent_probe, immediate_success)
{
    fk_reset(D_IMMEDIATE);
    hl_agent_probe_test_provider = &FK_PROVIDER;
    int r = hl_agent_tcp_probe(39899, 1000);
    hl_agent_probe_test_provider = NULL;
    ASSERT_EQ(r, 1);
    ASSERT_EQ(g_close_n, 1);   /* closed exactly once */
}

/* ── pending -> succeed via the io_status path ──────────────────────── */
UTEST(agent_probe, pending_succeed_io_status)
{
    fk_reset(D_SUCCEED); ap_arm_watchdog();
    hl_agent_probe_test_provider = &FK_PROVIDER_IOS;
    int r = hl_agent_tcp_probe(39899, 1000);
    hl_agent_probe_test_provider = NULL;
    ap_disarm_watchdog();
    ASSERT_EQ(r, 1);
    ASSERT_EQ(g_close_n, 1);
}

/* ── pending -> succeed via the hosted-errno fallback (no io_status op) ─ */
UTEST(agent_probe, pending_succeed_hosted_errno)
{
    fk_reset(D_SUCCEED); ap_arm_watchdog();
    hl_agent_probe_test_provider = &FK_PROVIDER;   /* connect leaves errno = EINPROGRESS */
    int r = hl_agent_tcp_probe(39899, 1000);
    hl_agent_probe_test_provider = NULL;
    ap_disarm_watchdog();
    ASSERT_EQ(r, 1);
    ASSERT_EQ(g_close_n, 1);
}

/* ── pending -> timeout: fd never write-ready, the deadline fires ────── */
UTEST(agent_probe, pending_timeout)
{
    fk_reset(D_PENDING); ap_arm_watchdog();
    hl_agent_probe_test_provider = &FK_PROVIDER;
    int r = hl_agent_tcp_probe(39899, 100);   /* 100 ms bound; never writable */
    hl_agent_probe_test_provider = NULL;
    ap_disarm_watchdog();
    ASSERT_EQ(r, 0);
    ASSERT_EQ(g_close_n, 1);
}

/* ── refusal surfaced by get_so_error after writability ─────────────── */
UTEST(agent_probe, refusal_via_so_error)
{
    fk_reset(D_FAIL); ap_arm_watchdog();
    hl_agent_probe_test_provider = &FK_PROVIDER_IOS;
    int r = hl_agent_tcp_probe(39899, 1000);
    hl_agent_probe_test_provider = NULL;
    ap_disarm_watchdog();
    ASSERT_EQ(r, 0);
    ASSERT_EQ(g_close_n, 1);
}

/* ── hard failure: connect() returns a non-pending error inline ─────── */
UTEST(agent_probe, immediate_hard_failure)
{
    fk_reset(D_HARDFAIL);
    hl_agent_probe_test_provider = &FK_PROVIDER_IOS;
    int r = hl_agent_tcp_probe(39899, 1000);
    hl_agent_probe_test_provider = NULL;
    ASSERT_EQ(r, 0);
    ASSERT_EQ(g_close_n, 1);   /* no watcher armed, still closed once */
}

UTEST_MAIN()
