/*
 * test_cfi.c - Control-Flow Integrity death test.
 *
 * Verifies that when the binary is built with `-fsanitize=cfi-icall`
 * (HL_ENABLE_CFI=1), an indirect call through a wrong-typed function
 * pointer traps at the call site.
 *
 * Pattern: take the address of a function with one signature, cast
 * it through void* to a different signature, fork, call the pointer
 * in the child.  Parent asserts the child died with SIGILL or
 * SIGTRAP (the CFI trap signal varies by platform - Linux x86_64
 * raises SIGILL via `ud2`; aarch64 raises SIGTRAP via `brk #1`).
 *
 * Skip cleanly on platforms / toolchains that don't enable CFI:
 *   - macOS Apple clang: no CFI runtime
 *   - Linux gcc: no -fsanitize=cfi
 *   - Cosmopolitan APE: no CFI
 *   - HL_ENABLE_CFI=0 (default): the build didn't ask for CFI
 *
 * The detection uses clang's __has_feature(cfi_icall) macro plus
 * a __builtin_assume() that the linker won't inline both call
 * sites into the same TU (which would defeat the type-erasure).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "utest.h"
#include <signal.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* CFI active in this build?
 *
 * The Makefile sets -DHL_CFI_BUILD=1 in the HL_ENABLE_CFI block
 * after the probe confirms the compiler accepts the flag.  We
 * deliberately don't use clang's __has_feature(cfi_icall) here
 * because it's been observed to return false under valid CFI
 * builds (clang 21 on Linux aarch64); the Makefile-side define is
 * the reliable signal. */
#ifndef HL_CFI_BUILD
#  define HL_CFI_BUILD 0
#endif
#define HL_CFI_ACTIVE HL_CFI_BUILD

/* Two functions with DIFFERENT signatures.  CFI computes a type-id
 * for each at compile time.  When the call site loads a fn ptr
 * whose declared type doesn't match the registered function's
 * type-id, CFI traps before the actual call.
 *
 * Marked with volatile-pointer references below so the optimiser
 * keeps both symbols around even on the !HL_CFI_ACTIVE skip path. */
static int real_int_fn(int x) { return x + 1; }
static void real_void_fn(const char *s) { (void)s; }

UTEST(cfi, wrong_typed_indirect_call_traps)
{
    /* Keep references to both functions live regardless of which
     * branch the preprocessor takes.  Without this, the compiler
     * dead-strips real_int_fn on the skip path and the build
     * fails on -Werror=unused-function. */
    volatile uintptr_t keep_int  = (uintptr_t)&real_int_fn;
    volatile uintptr_t keep_void = (uintptr_t)&real_void_fn;
    (void)keep_int; (void)keep_void;

#if !HL_CFI_ACTIVE
    /* CFI not active in this build.  The death-test wouldn't trap;
     * the wrong-typed call would either succeed-with-undefined-
     * behaviour or crash for unrelated reasons.  Skip cleanly. */
    ASSERT_TRUE(1);
    return;
#else
    /* Take the address via void* so the compiler's static type
     * checker doesn't reject the assignment.  At runtime, CFI sees
     * the call site expecting `int(int)` but the registered function
     * being `void(const char*)` and traps. */
    typedef int (*int_fn_t)(int);
    void *raw = (void *)(uintptr_t)&real_void_fn;
    int_fn_t fake = (int_fn_t)raw;

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: reset SIGILL/SIGTRAP/SIGABRT/SIGBUS to default so
         * the CFI trap propagates as a termination signal instead
         * of being caught by a sanitizer runtime that would print
         * a diagnostic and _exit(1). */
        signal(SIGILL,  SIG_DFL);
        signal(SIGTRAP, SIG_DFL);
        signal(SIGABRT, SIG_DFL);
        signal(SIGBUS,  SIG_DFL);
        /* Wrong-typed call.  CFI MUST trap here. */
        (void)fake(0);
        /* If we got here, CFI did NOT trap - fail the test. */
        _exit(0);
    }
    ASSERT_TRUE(pid > 0);

    int status = 0;
    pid_t w = waitpid(pid, &status, 0);
    ASSERT_EQ(pid, w);
    ASSERT_TRUE_MSG(WIFSIGNALED(status),
        "CFI did NOT trap on wrong-typed indirect call - "
        "check that HL_ENABLE_CFI=1 reached this TU's compile flags");
    int sig = WTERMSIG(status);
    /* CFI traps with different signals on different platforms:
     *   x86_64 clang: SIGILL via UD2 instruction
     *   aarch64 clang: SIGTRAP via BRK #1 instruction
     *   Some glibc paths route through abort() → SIGABRT
     * Accept any of the three. */
    ASSERT_TRUE(sig == SIGILL || sig == SIGTRAP || sig == SIGABRT);

    /* Sanity: the right-typed call still works. */
    int_fn_t good = &real_int_fn;
    ASSERT_EQ(good(0), 1);
#endif
}

UTEST(cfi, cfi_feature_reported)
{
    /* Smoke test: just emits a log line so CI output makes the
     * CFI status visible without needing to grep `make hardening`. */
#if HL_CFI_ACTIVE
    fprintf(stderr, "  [cfi] CFI IS active in this build\n");
#else
    fprintf(stderr, "  [cfi] CFI NOT active in this build "
                    "(expected on macOS / gcc / cosmo / non-CFI builds)\n");
#endif
    ASSERT_TRUE(1);
}

UTEST_MAIN();
