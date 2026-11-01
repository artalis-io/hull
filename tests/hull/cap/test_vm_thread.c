/*
 * test_vm_thread.c - cap/vm_thread.c: the --vm-thread baton and abandonment
 * (audit convergence §6.3 phase 1).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/cap/vm_thread.h"
#include "hull/cap/run_watchdog.h"
#include "hull/cap/db_backend.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts = { (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L };
    nanosleep(&ts, NULL);
}

UTEST(vm_thread, effective_deadline)
{
    /* The run armed the watch: its deadline, whatever it is. */
    ASSERT_EQ(hl_vmt_effective_deadline(1, 5000, 1000, 60000), (uint64_t)5000);
    ASSERT_EQ(hl_vmt_effective_deadline(1, 0, 1000, 60000), (uint64_t)0);
    /* Not armed yet: the deadline its arm will set. */
    ASSERT_EQ(hl_vmt_effective_deadline(0, 5000, 1000, 60000), (uint64_t)61000);
    /* --max-run-ms 0: none. */
    ASSERT_EQ(hl_vmt_effective_deadline(0, 0, 1000, 0), (uint64_t)0);
}

UTEST(vm_thread, watch_peek_sequence_moves)
{
    HlRunWatch w;
    memset(&w, 0, sizeof w);
    uint64_t d0, s0, d1, s1;
    hl_run_watch_peek(&w, &d0, &s0);
    ASSERT_EQ(d0, (uint64_t)0);
    hl_run_watch_arm(&w, 10000);
    hl_run_watch_peek(&w, &d1, &s1);
    ASSERT_NE(s0, s1);
    ASSERT_GT(d1, (uint64_t)0);
    hl_run_watch_disarm(&w);
    hl_run_watch_peek(&w, &d0, &s0);
    ASSERT_NE(s0, s1);
    ASSERT_EQ(d0, (uint64_t)0);
}

typedef struct {
    pthread_t caller;
    int       ran;
    int       on_other_thread;
    int       nested_inline;
    HlVmThread *t;
    uint64_t  gen_seen;
} Job;

static void nested_fn(void *ud)
{
    Job *j = (Job *)ud;
    j->nested_inline = pthread_equal(pthread_self(), j->caller) ? 0 : 1;
}

static void job_fn(void *ud)
{
    Job *j = (Job *)ud;
    j->ran++;
    j->on_other_thread = !pthread_equal(pthread_self(), j->caller);
    j->gen_seen = hl_db_txn_gen();
    hl_db_txn_gen_set(j->gen_seen + 2);
    /* An entry started from inside a run runs inline on the VM thread. */
    if (j->t) {
        int rc = hl_vmt_run(j->t, nested_fn, j, NULL, NULL);
        if (rc != HL_VMT_DONE) j->nested_inline = -1;
    }
}

static int switches;
static void count_switch(void *ud) { (void)ud; __atomic_fetch_add(&switches, 1, __ATOMIC_RELAXED); }

UTEST(vm_thread, inline_without_thread)
{
    Job j;
    memset(&j, 0, sizeof j);
    j.caller = pthread_self();
    ASSERT_EQ(hl_vmt_run(NULL, job_fn, &j, NULL, NULL), HL_VMT_DONE);
    ASSERT_EQ(j.ran, 1);
    ASSERT_EQ(j.on_other_thread, 0);
}

UTEST(vm_thread, runs_on_its_thread_and_hands_state_back)
{
    HlRunWatch w;
    memset(&w, 0, sizeof w);
    HlVmThreadConfig c;
    memset(&c, 0, sizeof c);
    c.watch = &w;
    c.thread_switch = count_switch;
    HlVmThread *t = NULL;
    switches = 0;
    hl_db_txn_gen_set(40);
    ASSERT_EQ(hl_vmt_create(&t, &c), 0);

    Job j;
    memset(&j, 0, sizeof j);
    j.caller = pthread_self();
    j.t = t;
    for (int i = 0; i < 100; i++) {
        hl_db_txn_gen_set(40 + (uint64_t)i * 10);
        ASSERT_EQ(hl_vmt_run(t, job_fn, &j, NULL, NULL), HL_VMT_DONE);
        /* The VM saw the loop's generation and the loop got the VM's back. */
        ASSERT_EQ(j.gen_seen, 40 + (uint64_t)i * 10);
        ASSERT_EQ(hl_db_txn_gen(), 42 + (uint64_t)i * 10);
    }
    ASSERT_EQ(j.ran, 100);
    ASSERT_EQ(j.on_other_thread, 1);
    ASSERT_EQ(j.nested_inline, 1);         /* ran on the VM thread, inline */
    ASSERT_EQ(__atomic_load_n(&switches, __ATOMIC_RELAXED), 200);

    HlVmtStats st;
    hl_vmt_stats(t, &st);
    ASSERT_EQ(st.jobs, (uint64_t)100);
    ASSERT_EQ(st.abandoned, (uint64_t)0);

    hl_db_txn_gen_set(7);
    hl_vmt_destroy(t);
    /* Destroy hands the VM's last generation back. */
    ASSERT_EQ(hl_db_txn_gen(), (uint64_t)1032);
    hl_run_watch_disarm(&w);
}

/* A run that arms a short deadline and then sleeps far past it in "native"
 * code, as a stuck builtin does. */
typedef struct {
    HlRunWatch *w;
    unsigned    deadline_ms;
    unsigned    sleep_ms;
    int         finished;
} Stuck;

static void stuck_fn(void *ud)
{
    Stuck *s = (Stuck *)ud;
    hl_run_watch_arm(s->w, s->deadline_ms);
    hl_vmt_test_native_sleep(s->sleep_ms);
    s->finished = 1;
}

typedef struct {
    int      port;
    unsigned delay_ms;
    char     resp[256];
    uint64_t took_ms;
} Client;

static void *client_main(void *ud)
{
    Client *c = (Client *)ud;
    sleep_ms(c->delay_ms);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)c->port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    uint64_t t0 = ms_now();
    if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
        const char *rq = "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n";
        (void)send(fd, rq, strlen(rq), 0);
        size_t got = 0;
        ssize_t r;
        while (got < sizeof c->resp - 1 &&
               (r = recv(fd, c->resp + got, sizeof c->resp - 1 - got, 0)) > 0)
            got += (size_t)r;
        c->resp[got] = '\0';
    }
    c->took_ms = ms_now() - t0;
    close(fd);
    return NULL;
}

UTEST(vm_thread, abandons_and_answers_503)
{
    HlRunWatch w;
    memset(&w, 0, sizeof w);

    /* A plaintext listener standby answers on. */
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(lfd, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(lfd, (struct sockaddr *)&a, sizeof a), 0);
    ASSERT_EQ(listen(lfd, 16), 0);
    socklen_t al = sizeof a;
    ASSERT_EQ(getsockname(lfd, (struct sockaddr *)&a, &al), 0);
    intptr_t lfd_h = lfd;

    /* The request's own socket: a pair, the test reads the raw 503. */
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    HlVmThreadConfig c;
    memset(&c, 0, sizeof c);
    c.watch = &w;
    c.grace_ms = 100;
    c.ceiling_ms = 0;
    c.listen_fd = &lfd_h;
    HlVmThread *t = NULL;
    ASSERT_EQ(hl_vmt_create(&t, &c), 0);

    Client cl;
    memset(&cl, 0, sizeof cl);
    cl.port = ntohs(a.sin_port);
    cl.delay_ms = 400;              /* while the run is abandoned */
    pthread_t ct;
    ASSERT_EQ(pthread_create(&ct, NULL, client_main, &cl), 0);

    Stuck s = { &w, 100, 900, 0 };
    HlVmtHttp http = { sv[0], 1 };
    int early = 0;
    uint64_t t0 = ms_now();
    int rc = hl_vmt_run(t, stuck_fn, &s, &http, &early);
    uint64_t took = ms_now() - t0;
    pthread_join(ct, NULL);

    ASSERT_EQ(rc, HL_VMT_ABANDONED);
    ASSERT_EQ(s.finished, 1);       /* it returned before hl_vmt_run did */
    ASSERT_EQ(early, 1);
    ASSERT_GE(took, (uint64_t)850);

    char buf[512];
    ssize_t n = recv(sv[1], buf, sizeof buf - 1, 0);
    ASSERT_GT(n, (ssize_t)0);
    buf[n] = '\0';
    ASSERT_TRUE(strncmp(buf, "HTTP/1.1 503 ", 13) == 0);
    /* The write side was shut: nothing more ever comes. */
    ASSERT_EQ(recv(sv[1], buf, sizeof buf, 0), (ssize_t)0);

    /* The new connection got its 503 at once, not after the run. */
    ASSERT_TRUE(strncmp(cl.resp, "HTTP/1.1 503 ", 13) == 0);
    ASSERT_LT(cl.took_ms, (uint64_t)400);

    HlVmtStats st;
    hl_vmt_stats(t, &st);
    ASSERT_EQ(st.abandoned, (uint64_t)1);
    ASSERT_EQ(st.standby_503, (uint64_t)1);

    /* And the VM thread serves the next run normally. */
    Job j;
    memset(&j, 0, sizeof j);
    j.caller = pthread_self();
    ASSERT_EQ(hl_vmt_run(t, job_fn, &j, NULL, NULL), HL_VMT_DONE);
    ASSERT_EQ(j.ran, 1);

    hl_vmt_destroy(t);
    hl_run_watch_disarm(&w);
    close(sv[0]);
    close(sv[1]);
    close(lfd);
}

UTEST(vm_thread, no_early_answer_when_not_allowed)
{
    HlRunWatch w;
    memset(&w, 0, sizeof w);
    int sv[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    HlVmThreadConfig c;
    memset(&c, 0, sizeof c);
    c.watch = &w;
    c.grace_ms = 50;
    HlVmThread *t = NULL;
    ASSERT_EQ(hl_vmt_create(&t, &c), 0);

    Stuck s = { &w, 50, 400, 0 };
    HlVmtHttp http = { sv[0], 0 };   /* TLS / HTTP/2: no raw bytes */
    int early = 1;
    ASSERT_EQ(hl_vmt_run(t, stuck_fn, &s, &http, &early), HL_VMT_ABANDONED);
    ASSERT_EQ(early, 0);
    /* Nothing was written to the request's socket. */
    struct timeval tv = { 0, 100000 };
    setsockopt(sv[1], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char b;
    ASSERT_LT(recv(sv[1], &b, 1, 0), (ssize_t)1);

    hl_vmt_destroy(t);
    hl_run_watch_disarm(&w);
    close(sv[0]);
    close(sv[1]);
}

UTEST(vm_thread, fast_runs_are_never_abandoned)
{
    HlRunWatch w;
    memset(&w, 0, sizeof w);
    HlVmThreadConfig c;
    memset(&c, 0, sizeof c);
    c.watch = &w;
    /* A wide grace: the run returns ~100 ms past its deadline, and a loaded
     * CI host overshoots a sleep by more than a tight grace allows. */
    c.grace_ms = 2000;
    HlVmThread *t = NULL;
    ASSERT_EQ(hl_vmt_create(&t, &c), 0);
    /* A run that outlives its deadline but returns within the grace. */
    Stuck s = { &w, 50, 150, 0 };
    ASSERT_EQ(hl_vmt_run(t, stuck_fn, &s, NULL, NULL), HL_VMT_DONE);
    ASSERT_EQ(s.finished, 1);
    hl_vmt_destroy(t);
    hl_run_watch_disarm(&w);
}

UTEST_MAIN()
