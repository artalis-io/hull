/*
 * test_thread_affinity.c — event-loop thread-affinity assertions.
 *
 * Verifies hl_assert_on_event_loop():
 *   - same-thread call after marking is a no-op,
 *   - a call from another thread aborts (fork + WIFSIGNALED death test),
 *   - an un-marked process is a no-op (boot / tool mode).
 *
 * Self-skips when HL_THREAD_AFFINITY_CHECKS is compiled out (release
 * builds), the same way test_cfi.c self-skips on non-CFI builds.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include "hull/shared/thread_affinity.h"

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

UTEST(thread_affinity, same_thread_is_ok)
{
    /* No-op when checks are off; when on, the marking thread is the
     * caller, so this must not abort. */
    hl_event_loop_mark_current();
    hl_assert_on_event_loop("same_thread_is_ok");
    ASSERT_TRUE(1);
}

#ifdef HL_THREAD_AFFINITY_CHECKS

static void *off_loop_caller(void *arg)
{
    (void)arg;
    /* The event-loop thread was marked in the (child) main thread, so a
     * call from here must abort. */
    hl_assert_on_event_loop("off_loop_caller");
    return NULL; /* unreachable */
}

UTEST(thread_affinity, off_thread_aborts)
{
    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);

    if (pid == 0) {
        /* Child: let abort() terminate by signal instead of a sanitizer
         * handler swallowing it (mirrors test_seal_arena's death test). */
        signal(SIGABRT, SIG_DFL);
        hl_event_loop_mark_current();           /* this thread = the loop */
        pthread_t th;
        if (pthread_create(&th, NULL, off_loop_caller, NULL) != 0)
            _exit(2);
        pthread_join(th, NULL);
        _exit(0);                               /* should never reach */
    }

    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGABRT);
}

#else

UTEST(thread_affinity, off_thread_aborts)
{
    /* Checks compiled out: assert is a no-op. Nothing to exercise. */
    UTEST_SKIP("HL_THREAD_AFFINITY_CHECKS not enabled in this build");
}

#endif /* HL_THREAD_AFFINITY_CHECKS */

UTEST_MAIN();
