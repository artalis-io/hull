/*
 * sandbox_violation.c - Verify kernel sandbox enforcement
 *
 * Standalone test program that directly exercises OS-level sandbox
 * mechanisms to prove kernel-level enforcement works.
 * Each test runs in a forked child so sandbox state is isolated.
 *
 * Supports:
 *   OpenBSD      - native pledge/unveil (KILL mode)
 *   Cosmopolitan - pledge/unveil built into cosmo libc (KILL mode)
 *   Linux        - jart/pledge polyfill (RETURN_EPERM mode)
 *   macOS        - Seatbelt sandbox_init_with_parameters
 *
 * Compile (Linux):
 *   cc -std=c11 -O2 -o sandbox_test tests/sandbox_violation.c \
 *      build/pledge_*.o -lpthread
 *
 * Compile (Cosmopolitan):
 *   cosmocc -std=c11 -O2 -o sandbox_test tests/sandbox_violation.c
 *
 * Compile (macOS):
 *   cc -std=c11 -O2 -o sandbox_test tests/sandbox_violation.c
 *
 * Usage: ./sandbox_test
 *
 * Returns 0 if all tests pass, 1 if any fail.
 * Skips gracefully if kernel doesn't support the sandbox mechanism.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

/* On Linux glibc, `syscall(2)` is only declared by <unistd.h> when
 * _GNU_SOURCE (or _DEFAULT_SOURCE) is defined. Newer clang treats the
 * resulting implicit declaration as a hard error under
 * -Wimplicit-function-declaration, so we set it before any include.
 * No effect on macOS/BSD/Cosmo. */
#if defined(__linux__) && !defined(__COSMOPOLITAN__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

/* ── Platform detection ────────────────────────────────────────────── */

#if defined(__COSMOPOLITAN__)
#define SANDBOX_SUPPORTED 1
#define HAS_PLEDGE_MODE   0
#define HAS_SEATBELT      0
/* Cosmopolitan libc provides pledge() and unveil() natively. */
extern int pledge(const char *promises, const char *execpromises);
extern int unveil(const char *path, const char *permissions);

#elif defined(__OpenBSD__)
#define SANDBOX_SUPPORTED 1
#define HAS_PLEDGE_MODE   0
#define HAS_SEATBELT      0
/* OpenBSD: pledge/unveil are native system calls in <unistd.h>. */
#include <unistd.h>

#elif defined(__linux__)
#define SANDBOX_SUPPORTED 1
#define HAS_PLEDGE_MODE   1
#define HAS_SEATBELT      0

/* jart/pledge polyfill - linked from build/ objects */
extern int pledge(const char *promises, const char *execpromises);
extern int unveil(const char *path, const char *permissions);
extern int __pledge_mode;

#define PLEDGE_PENALTY_RETURN_EPERM 0x0002
#define PLEDGE_STDERR_LOGGING       0x0010

#elif defined(__APPLE__)
#define SANDBOX_SUPPORTED 1
#define HAS_PLEDGE_MODE   0
#define HAS_SEATBELT      1

#include <stdint.h>
#include <stdlib.h> /* free (for sandbox error strings) */

extern int sandbox_init_with_parameters(const char *profile,
                                         uint64_t flags,
                                         const char **parameters,
                                         char **errorbuf);

#else
#define SANDBOX_SUPPORTED 0
#define HAS_PLEDGE_MODE   0
#define HAS_SEATBELT      0
#endif

/* ── Test implementation (Linux + Cosmopolitan) ───────────────────── */

#if SANDBOX_SUPPORTED

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__) && !defined(__COSMOPOLITAN__)
#include <sys/syscall.h>
#endif

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

/* ── Tests 1-3: pledge/unveil enforcement (Linux + Cosmopolitan) ─── */

#if !HAS_SEATBELT

/* ── Test 1: Landlock unveil enforcement ──────────────────────────── */

static int test_unveil_child(void)
{
    /*
     * Unveil only /tmp for reading, seal, then try to open
     * a file outside the allowed set.
     */
    if (unveil("/tmp", "r") != 0) {
        if (errno == ENOSYS) {
            /* Kernel too old for Landlock - not an error */
            _exit(77); /* special "skip" code */
        }
        fprintf(stderr, "unveil(\"/tmp\", \"r\") failed: %s\n",
                strerror(errno));
        _exit(2);
    }

    /* Seal - no more unveil calls */
    if (unveil(NULL, NULL) != 0) {
        fprintf(stderr, "unveil(NULL, NULL) failed: %s\n", strerror(errno));
        _exit(2);
    }

    /* Try to open /etc/hostname - should be blocked */
    int fd = open("/etc/hostname", O_RDONLY);
    if (fd >= 0) {
        close(fd);
        _exit(1); /* FAIL: access was NOT blocked */
    }

    if (errno == EACCES) {
        _exit(0); /* PASS: Landlock blocked access */
    }

    /* Some other error - might be ENOENT if file doesn't exist */
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
            pass("unveil SKIPPED - Landlock not supported by kernel");
        } else if (code == 1) {
            fail("unveil did NOT block access - Landlock not enforcing");
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
        pass("unveil allowed-path SKIPPED - Landlock not supported");
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
     * Linux (polyfill): use RETURN_EPERM mode so the child isn't
     * killed - we can check errno instead.
     *
     * Cosmopolitan: uses KILL mode by default - parent detects
     * signal death as enforcement.
     */
#if HAS_PLEDGE_MODE
    __pledge_mode = PLEDGE_PENALTY_RETURN_EPERM | PLEDGE_STDERR_LOGGING;
#endif

    if (pledge("stdio", NULL) != 0) {
        if (errno == ENOSYS) {
            _exit(77); /* kernel doesn't support seccomp */
        }
        fprintf(stderr, "pledge(\"stdio\") failed: %s\n", strerror(errno));
        _exit(2);
    }

    /* Try to open a file - should fail (needs rpath) */
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
            pass("pledge SKIPPED - seccomp not supported");
        } else if (code == 1) {
            fail("pledge did NOT block open() - seccomp not enforcing");
        } else {
            fail("pledge test exited with unexpected code");
        }
    } else if (WIFSIGNALED(status)) {
        /* Process was killed - this happens with KILL mode (Cosmopolitan).
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
#if HAS_PLEDGE_MODE
    __pledge_mode = PLEDGE_PENALTY_RETURN_EPERM | PLEDGE_STDERR_LOGGING;
#endif

    /* Pledge with rpath - reading should work */
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

    /* ENOENT is fine - file might not exist */
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
            pass("pledge-allowed SKIPPED - seccomp not supported");
        } else {
            fail("pledge blocked open() despite rpath promise");
        }
    } else if (WIFSIGNALED(status)) {
        fail("pledge killed child despite rpath promise");
    }
}

/* ── W^X tests (Linux + Cosmopolitan): no executable guest memory ──
 *
 * These tests prove that after pledge() with Hull's default phase-2
 * promise set ("stdio rpath wpath cpath flock"), the kernel refuses:
 *   - mmap with PROT_WRITE | PROT_EXEC simultaneously
 *   - mmap(PROT_WRITE) followed by mprotect adding PROT_EXEC
 *   - memfd_create (sealed memfd → mmap PROT_EXEC trick)
 *   - execve from the post-pledge child
 * AND that ordinary non-exec memory still works (negative control).
 *
 * Linux polyfill mode: RETURN_EPERM. Cosmopolitan: KILL_PROCESS - so
 * the test parent treats either non-zero exit or signal death as PASS.
 */

static int test_wx_mmap_wx_child(void)
{
#if HAS_PLEDGE_MODE
    __pledge_mode = PLEDGE_PENALTY_RETURN_EPERM | PLEDGE_STDERR_LOGGING;
#endif

    if (pledge("stdio rpath wpath cpath flock", NULL) != 0) {
        if (errno == ENOSYS) _exit(77);
        _exit(2);
    }

    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        munmap(p, 4096);
        _exit(1); /* FAIL: PROT_W|PROT_X mmap succeeded */
    }
    _exit(0); /* PASS: kernel denied PROT_W|PROT_X */
}

static void test_wx_mmap_wx(void)
{
    printf("\n=== W^X: mmap(PROT_WRITE|PROT_EXEC) denied ===\n");
    pid_t pid = fork();
    if (pid < 0) { fail("fork failed"); return; }
    if (pid == 0) test_wx_mmap_wx_child();

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0)
            pass("mmap(PROT_W|PROT_X) denied");
        else if (code == 77)
            pass("W^X mmap test SKIPPED - seccomp not supported");
        else if (code == 1)
            fail("mmap(PROT_W|PROT_X) succeeded - W^X NOT enforced");
        else
            fail("W^X mmap test exited with unexpected code");
    } else if (WIFSIGNALED(status)) {
        pass("W^X mmap test killed (KILL mode enforcement active)");
    }
}

static int test_wx_mprotect_child(void)
{
#if HAS_PLEDGE_MODE
    __pledge_mode = PLEDGE_PENALTY_RETURN_EPERM | PLEDGE_STDERR_LOGGING;
#endif

    if (pledge("stdio rpath wpath cpath flock", NULL) != 0) {
        if (errno == ENOSYS) _exit(77);
        _exit(2);
    }

    /* Allocate writable memory first (should succeed) */
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        /* RW mmap itself failed - environment problem, not a W^X test */
        _exit(2);
    }

    /* Now try to promote to executable - must fail */
    if (mprotect(p, 4096, PROT_READ | PROT_EXEC) == 0) {
        munmap(p, 4096);
        _exit(1); /* FAIL: W→X transition succeeded */
    }

    munmap(p, 4096);
    _exit(0); /* PASS: kernel denied W→X transition */
}

static void test_wx_mprotect(void)
{
    printf("\n=== W^X: mprotect adding PROT_EXEC denied ===\n");
    pid_t pid = fork();
    if (pid < 0) { fail("fork failed"); return; }
    if (pid == 0) test_wx_mprotect_child();

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0)
            pass("mprotect → PROT_EXEC denied (no W→X transition)");
        else if (code == 77)
            pass("W^X mprotect test SKIPPED - seccomp not supported");
        else if (code == 1)
            fail("mprotect → PROT_EXEC succeeded - W^X NOT enforced");
        else
            fail("W^X mprotect test exited with unexpected code");
    } else if (WIFSIGNALED(status)) {
        pass("W^X mprotect test killed (KILL mode enforcement active)");
    }
}

#if defined(__linux__) && !defined(__COSMOPOLITAN__)
static int test_wx_memfd_child(void)
{
#if HAS_PLEDGE_MODE
    __pledge_mode = PLEDGE_PENALTY_RETURN_EPERM | PLEDGE_STDERR_LOGGING;
#endif

    if (pledge("stdio rpath wpath cpath flock", NULL) != 0) {
        if (errno == ENOSYS) _exit(77);
        _exit(2);
    }

    /* memfd_create is filtered to ENOSYS by the pledge polyfill prefix.
     * Direct syscall avoids glibc memfd_create wrapper missing on old libc.
     * On arches we don't know the syscall number for, SKIP - falling back
     * to syscall(0, ...) would dispatch to `read` on x86_64 (syscall #0)
     * and return 0 success, producing a spurious test failure. */
#ifndef __NR_memfd_create
# if defined(__x86_64__)
#  define __NR_memfd_create 319
# elif defined(__aarch64__)
#  define __NR_memfd_create 279
# else
    _exit(77); /* SKIP: unknown arch */
# endif
#endif
#ifdef __NR_memfd_create
    long r = syscall(__NR_memfd_create, "wxprobe", 0u);
    if (r >= 0) {
        close((int)r);
        _exit(1); /* FAIL: memfd_create succeeded */
    }
    _exit(0); /* PASS */
#endif
}

static void test_wx_memfd(void)
{
    printf("\n=== W^X: memfd_create denied ===\n");
    pid_t pid = fork();
    if (pid < 0) { fail("fork failed"); return; }
    if (pid == 0) test_wx_memfd_child();

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0)
            pass("memfd_create denied (ENOSYS / EPERM)");
        else if (code == 77)
            pass("W^X memfd test SKIPPED - seccomp not supported");
        else if (code == 1)
            fail("memfd_create succeeded - bypass available");
        else
            fail("W^X memfd test exited with unexpected code");
    } else if (WIFSIGNALED(status)) {
        pass("W^X memfd test killed (KILL mode enforcement active)");
    }
}
#endif /* __linux__ && !__COSMOPOLITAN__ */

static int test_wx_execve_child(void)
{
#if HAS_PLEDGE_MODE
    __pledge_mode = PLEDGE_PENALTY_RETURN_EPERM | PLEDGE_STDERR_LOGGING;
#endif

    if (pledge("stdio rpath wpath cpath flock", NULL) != 0) {
        if (errno == ENOSYS) _exit(77);
        _exit(2);
    }

    char *argv[] = { (char *)"/bin/true", NULL };
    char *envp[] = { NULL };
    execve("/bin/true", argv, envp);
    /* If execve returns, it failed → that's the expected outcome */
    if (errno == EPERM || errno == EACCES || errno == ENOSYS)
        _exit(0);
    _exit(1);
}

static void test_wx_execve(void)
{
    printf("\n=== W^X: execve denied (no `exec` pledge) ===\n");
    pid_t pid = fork();
    if (pid < 0) { fail("fork failed"); return; }
    if (pid == 0) test_wx_execve_child();

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0)
            pass("execve denied without `exec` promise");
        else if (code == 77)
            pass("W^X execve test SKIPPED - seccomp not supported");
        else
            fail("execve returned unexpected outcome");
    } else if (WIFSIGNALED(status)) {
        pass("W^X execve test killed (KILL mode enforcement active)");
    }
}

static int test_wx_anon_rw_child(void)
{
    /* Negative control: ordinary anonymous read/write mmap must STILL work
     * after pledge - proves the seccomp filter targets only PROT_EXEC, not
     * legitimate libc/runtime allocation patterns. */
#if HAS_PLEDGE_MODE
    __pledge_mode = PLEDGE_PENALTY_RETURN_EPERM | PLEDGE_STDERR_LOGGING;
#endif
    if (pledge("stdio rpath wpath cpath flock", NULL) != 0) {
        if (errno == ENOSYS) _exit(77);
        _exit(2);
    }
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) _exit(1);
    /* Touch it to prove it's usable */
    *(volatile unsigned char *)p = 0xAB;
    munmap(p, 4096);
    _exit(0);
}

static void test_wx_anon_rw(void)
{
    printf("\n=== W^X: ordinary anon r/w mmap still works ===\n");
    pid_t pid = fork();
    if (pid < 0) { fail("fork failed"); return; }
    if (pid == 0) test_wx_anon_rw_child();

    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0)
            pass("anon r/w mmap still works post-pledge (filter not over-broad)");
        else if (code == 77)
            pass("W^X anon-rw control SKIPPED - seccomp not supported");
        else
            fail("anon r/w mmap broke under pledge - over-broad filter");
    } else if (WIFSIGNALED(status)) {
        fail("anon r/w mmap killed - filter over-broad");
    }
}

#endif /* !HAS_SEATBELT */

/* ── Test 4: Seatbelt deny-all enforcement (macOS only) ──────────── */

#if HAS_SEATBELT

static int test_seatbelt_deny_child(void)
{
    /* Apply a deny-all Seatbelt profile, then try to open a file */
    const char *profile =
        "(version 1)\n"
        "(deny default)\n"
        "(allow file-read* (subpath \"/System/Library\")"
        " (subpath \"/usr/lib\"))\n"
        "(allow file-map-executable (subpath \"/System/Library\")"
        " (subpath \"/usr/lib\"))\n"
        "(allow sysctl-read)\n";

    const char *params[] = { NULL };
    char *errorbuf = NULL;

    if (sandbox_init_with_parameters(profile, 0, params, &errorbuf) != 0) {
        fprintf(stderr, "sandbox_init failed: %s\n",
                errorbuf ? errorbuf : "unknown");
        if (errorbuf) free(errorbuf);
        _exit(2);
    }

    /* Try to open /etc/hosts - should be blocked */
    int fd = open("/etc/hosts", O_RDONLY);
    if (fd >= 0) {
        close(fd);
        _exit(1); /* FAIL: access was NOT blocked */
    }

    if (errno == EPERM || errno == EACCES) {
        _exit(0); /* PASS: Seatbelt blocked access */
    }

    fprintf(stderr, "open(\"/etc/hosts\"): %s (expected EPERM/EACCES)\n",
            strerror(errno));
    _exit(2);
}

static void test_seatbelt_deny(void)
{
    printf("\n=== Seatbelt deny-all enforcement ===\n");

    pid_t pid = fork();
    if (pid < 0) {
        fail("fork failed");
        return;
    }

    if (pid == 0)
        test_seatbelt_deny_child();

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0) {
            pass("seatbelt blocked access to /etc/hosts");
        } else if (code == 1) {
            fail("seatbelt did NOT block access - not enforcing");
        } else {
            fail("seatbelt deny test exited with unexpected code");
        }
    } else if (WIFSIGNALED(status)) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "seatbelt deny test killed by signal %d", WTERMSIG(status));
        fail(buf);
    }
}

/* ── Test 5: Seatbelt allows permitted paths ─────────────────────── */

static int test_seatbelt_allow_child(void)
{
    /* Allow /tmp for reading + writing.
     * On macOS, /tmp → /private/tmp (symlink).  Seatbelt resolves
     * symlinks before matching, so we must use the real path. */
    char real_tmp[256];
    if (!realpath("/tmp", real_tmp)) {
        fprintf(stderr, "realpath(\"/tmp\") failed\n");
        _exit(2);
    }

    const char *profile =
        "(version 1)\n"
        "(deny default)\n"
        "(allow file-read* (subpath \"/System/Library\")"
        " (subpath \"/usr/lib\")"
        " (subpath (param \"ALLOWED\")))\n"
        "(allow file-write* (subpath (param \"ALLOWED\")))\n"
        "(allow file-map-executable (subpath \"/System/Library\")"
        " (subpath \"/usr/lib\"))\n"
        "(allow sysctl-read)\n";

    const char *params[] = { "ALLOWED", real_tmp, NULL };
    char *errorbuf = NULL;

    if (sandbox_init_with_parameters(profile, 0, params, &errorbuf) != 0) {
        fprintf(stderr, "sandbox_init failed: %s\n",
                errorbuf ? errorbuf : "unknown");
        if (errorbuf) free(errorbuf);
        _exit(2);
    }

    /* Create and read a file in /tmp (allowed).
     * Must use resolved path - Seatbelt resolves symlinks. */
    char test_path[512];
    snprintf(test_path, sizeof(test_path),
             "%s/hull_seatbelt_test.txt", real_tmp);
    const char *path = test_path;
    int wfd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0) {
        fprintf(stderr, "write open failed: %s\n", strerror(errno));
        _exit(1);
    }
    write(wfd, "test", 4);
    close(wfd);

    int rfd = open(path, O_RDONLY);
    if (rfd < 0) {
        fprintf(stderr, "read open failed: %s\n", strerror(errno));
        _exit(1);
    }
    close(rfd);

    unlink(path);
    _exit(0);
}

static void test_seatbelt_allow(void)
{
    printf("\n=== Seatbelt allows permitted paths ===\n");

    pid_t pid = fork();
    if (pid < 0) {
        fail("fork failed");
        return;
    }

    if (pid == 0)
        test_seatbelt_allow_child();

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        pass("seatbelt allowed access to /tmp (permitted path)");
    } else {
        fail("seatbelt blocked access to /tmp (should be allowed)");
    }
}

#endif /* HAS_SEATBELT */

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void)
{
#if HAS_SEATBELT
    test_seatbelt_deny();
    test_seatbelt_allow();
#else
    test_unveil();
    test_pledge();
    test_pledge_allowed();
    test_wx_mmap_wx();
    test_wx_mprotect();
#if defined(__linux__) && !defined(__COSMOPOLITAN__)
    test_wx_memfd();
#endif
    test_wx_execve();
    test_wx_anon_rw();
#endif

    int total = pass_count + fail_count;
    printf("\n%d/%d sandbox violation tests passed\n", pass_count, total);
    return fail_count > 0 ? 1 : 0;
}

#else /* not Linux or Cosmopolitan */

#include <stdio.h>

int main(void)
{
    printf("sandbox_violation: SKIPPED (requires OpenBSD, Linux, macOS, or Cosmopolitan)\n");
    return 0;
}

#endif /* SANDBOX_SUPPORTED */
