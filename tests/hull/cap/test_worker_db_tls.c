/*
 * test_worker_db_tls.c - the worker DB connection list's thread-local key is
 * created before its first use. A pool thread's first op could run before
 * hl_worker_db_init (only called with a default DSN): worker_db_key was still
 * 0, which on glibc is the FIRST key the process made - another subsystem's
 * (the JS worker VM's, in the poll CLI runner) - and the connection list
 * overwrote that slot. The next worker.dispatch on the thread then jumped
 * through a garbage pointer (audit 14 G1 e2e, found under ASan).
 *
 * Its own executable, so this file's key is made before Hull's.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/worker_db.h"

#include <pthread.h>
#include <stdlib.h>

static pthread_key_t other_key;
static int other_key_ok;

/* What another subsystem keeps in its slot: zero-filled, so a walk that
 * mistook it for a connection list would find no entries and not crash. */
static void *sentinel;

typedef struct { void *seen; int got_db; } ThreadResult;

static void *thread_main(void *arg)
{
    ThreadResult *r = (ThreadResult *)arg;
    pthread_setspecific(other_key, sentinel);
    /* No hl_worker_db_init: the first worker-DB use on any thread. */
    HlWorkerDb *wdb = hl_worker_db_get_for(":memory:");
    r->got_db = wdb != NULL;
    hl_worker_db_invalidate(":memory:");
    r->seen = pthread_getspecific(other_key);
    pthread_setspecific(other_key, NULL);
    return NULL;
}

UTEST(worker_db_tls, first_use_does_not_touch_another_key)
{
    /* Made before Hull's key, so it is the slot an unset worker_db_key
     * (0) aliased. */
    other_key_ok = pthread_key_create(&other_key, NULL) == 0;
    ASSERT_TRUE(other_key_ok);
    sentinel = calloc(1, 4096);
    ASSERT_TRUE(sentinel != NULL);

    ThreadResult r = { NULL, 0 };
    pthread_t t;
    ASSERT_EQ(0, pthread_create(&t, NULL, thread_main, &r));
    ASSERT_EQ(0, pthread_join(t, NULL));

#ifdef HL_ENABLE_SQLITE
    EXPECT_TRUE(r.got_db);             /* (a network-only build opens none) */
#endif
    EXPECT_TRUE(r.seen == sentinel);   /* the other subsystem's slot is intact */
    free(sentinel);
    pthread_key_delete(other_key);
}

UTEST_MAIN();
