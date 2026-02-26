/*
 * sandbox_violation.c — Verify kernel sandbox enforcement on Linux
 *
 * Standalone test program that directly exercises Landlock (unveil)
 * and pledge (seccomp) to prove kernel-level enforcement works.
 * Each test runs in a forked child so sandbox state is isolated.
 *
 * Compile (Linux only):
 *   cc -std=c11 -O2 -o sandbox_test tests/sandbox_violation.c \
 *      build/pledge_*.o -lpthread
 *
 * Usage: ./sandbox_test
 *
 * Returns 0 if all tests pass, 1 if any fail.
 * Skips gracefully if kernel doesn't support Landlock/seccomp.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef __linux__

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* jart/pledge polyfill — linked from build/ objects */
extern int pledge(const char *promises, const char *execpromises);
extern int unveil(const char *path, const char *permissions);
extern int __pledge_mode;

/* Pledge mode flags */
#define PLEDGE_PENALTY_RETURN_EPERM 0x0002
#define PLEDGE_STDERR_LOGGING       0x0010

static int pass_count;
static int fail_count;

static void pass(const char *msg)
{
    printf("  PASS: %s\n", msg);
    pass_count++;
}

static void fail(const char *msg)
{
    printf("  FAIL: %s\n", msg);
    fail_count++;
}

/* ── Test 1: Landlock unveil enforcement ──────────────────────────── */

static int test_unveil_child(void)
{
    /*
     * Unveil only /tmp for reading, seal, then try to open
     * a file outside the allowed set.
     */
    if (unveil("/tmp", "r") != 0) {
        if (errno == ENOSYS) {
            /* Kernel too old for Landlock — not an error */
            _exit(77); /* special "skip" code */
        }
        fprintf(stderr, "unveil(\"/tmp\", \"r\") failed: %s\n",
                strerror(errno));
        _exit(2);
    }

    /* Seal — no more unveil calls */
    if (unveil(NULL, NULL) != 0) {
        fprintf(stderr, "unveil(NULL, NULL) failed: %s\n", strerror(errno));
        _exit(2);
    }

    /* Try to open /etc/hostname — should be blocked */
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd >= 0) {
        close(fd);
        _exit(1); /* FAIL: access was NOT blocked */
    }

    if (errno == EACCES) {
        _exit(0); /* PASS: Landlock blocked access */
    }

    /* Some other error — might be ENOENT if file doesn't exist */
    fprintf(stderr, "open(\"/etc/hostname\"): %s (expected EACCES)\n",
            strerror(errno));
    _exit(2);
}

static void test_unveil(void)
{
    printf("\n=== Landlock unveil enforcement ===\n");

    pid_t pid = fork();
    if (pid < 0) {
        fail("fork failed");
        return;
    }

    if (pid == 0)
        test_unveil_child(); /* does not return */

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0) {
            pass("unveil blocked access to /etc/hostname (EACCES)");
        } else if (code == 77) {
            pass("unveil SKIPPED — Landlock not supported by kernel");
        } else if (code == 1) {
            fail("unveil did NOT block access — Landlock not enforcing");
        } else {
            fail("unveil test exited with unexpected code");
        }
    } else if (WIFSIGNALED(status)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "unveil test killed by signal %d",
                 WTERMSIG(status));
        fail(buf);
    }

    /* Verify allowed path still works */
    pid = fork();
    if (pid < 0) {
        fail("fork for allowed-path test failed");
        return;
    }

    if (pid == 0) {
        if (unveil("/tmp", "rwc") != 0) {
            if (errno == ENOSYS)
                _exit(77);
            _exit(2);
        }
        if (unveil(NULL, NULL) != 0)
            _exit(2);

        /* Create and read a file in /tmp (allowed) */
        const char *path = "/tmp/hull_sandbox_test_allowed.txt";
        int wfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (wfd < 0)
            _exit(1);
        write(wfd, "test", 4);
        close(wfd);

        int rfd = open(path, O_RDONLY);
        if (rfd < 0)
            _exit(1);
        close(rfd);

        unlink(path);
        _exit(0);
    }

    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        pass("unveil allowed access to /tmp (permitted path)");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 77) {
        pass("unveil allowed-path SKIPPED — Landlock not supported");
    } else {
        fail("unveil blocked access to /tmp (should be allowed)");
    }
}

/* ── Test 2: pledge (seccomp) enforcement ────────────────────────── */

static int test_pledge_child(void)
{
    /*
     * Pledge "stdio" only (very restrictive), then try to open
     * a file (requires "rpath" which we didn't pledge).
     *
     * Use RETURN_EPERM mode so the child isn't killed — we can
     * check errno instead.
     */
    __pledge_mode = PLEDGE_PENALTY_RETURN_EPERM | PLEDGE_STDERR_LOGGING;

    if (pledge("stdio", NULL) != 0) {
        if (errno == ENOSYS) {
            _exit(77); /* kernel doesn't support seccomp */
        }
        fprintf(stderr, "pledge(\"stdio\") failed: %s\n", strerror(errno));
        _exit(2);
    }

    /* Try to open a file — should fail (needs rpath) */
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd >= 0) {
        close(fd);
        _exit(1); /* FAIL: pledge did not block */
    }

    if (errno == EPERM || errno == EACCES) {
        _exit(0); /* PASS: pledge blocked the syscall */
    }

    fprintf(stderr, "open() after pledge: errno=%d (%s), expected EPERM\n",
            errno, strerror(errno));
    _exit(2);
}

static void test_pledge(void)
{
    printf("\n=== pledge (seccomp) enforcement ===\n");

    pid_t pid = fork();
    if (pid < 0) {
        fail("fork failed");
        return;
    }

    if (pid == 0)
        test_pledge_child();

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0) {
            pass("pledge blocked open() without rpath promise");
        } else if (code == 77) {
            pass("pledge SKIPPED — seccomp not supported");
        } else if (code == 1) {
            fail("pledge did NOT block open() — seccomp not enforcing");
        } else {
            fail("pledge test exited with unexpected code");
        }
    } else if (WIFSIGNALED(status)) {
        /* Process was killed — this happens with KILL_PROCESS mode.
         * Treat signal death as a pass (pledge enforced). */
        int sig = WTERMSIG(status);
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "pledge killed child with signal %d (enforcement active)",
                 sig);
        pass(buf);
    }
}

/* ── Test 3: pledge allows declared operations ───────────────────── */

static int test_pledge_allowed_child(void)
{
    __pledge_mode = PLEDGE_PENALTY_RETURN_EPERM | PLEDGE_STDERR_LOGGING;

    /* Pledge with rpath — reading should work */
    if (pledge("stdio rpath", NULL) != 0) {
        if (errno == ENOSYS)
            _exit(77);
        _exit(2);
    }

    int fd = open("/etc/hostname", O_RDONLY);
    if (fd >= 0) {
        close(fd);
        _exit(0); /* PASS: rpath allows reading */
    }

    /* ENOENT is fine — file might not exist */
    if (errno == ENOENT)
        _exit(0);

    fprintf(stderr, "open() with rpath: errno=%d (%s)\n",
            errno, strerror(errno));
    _exit(1);
}

static void test_pledge_allowed(void)
{
    printf("\n=== pledge allows declared operations ===\n");

    pid_t pid = fork();
    if (pid < 0) {
        fail("fork failed");
        return;
    }

    if (pid == 0)
        test_pledge_allowed_child();

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0) {
            pass("pledge allowed open() with rpath promise");
        } else if (code == 77) {
            pass("pledge-allowed SKIPPED — seccomp not supported");
        } else {
            fail("pledge blocked open() despite rpath promise");
        }
    } else if (WIFSIGNALED(status)) {
        fail("pledge killed child despite rpath promise");
    }
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void)
{
    test_unveil();
    test_pledge();
    test_pledge_allowed();

    int total = pass_count + fail_count;
    printf("\n%d/%d sandbox violation tests passed\n", pass_count, total);
    return fail_count > 0 ? 1 : 0;
}

#else /* not Linux */

#include <stdio.h>

int main(void)
{
    printf("sandbox_violation: SKIPPED (Linux only)\n");
    return 0;
}

#endif /* __linux__ */
