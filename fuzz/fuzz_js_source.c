/*
 * fuzz_js_source.c - continuous libFuzzer harness for the JS parser (hull:source:parser).
 *
 * Drives the parser through the restricted QuickJS tooling session via the TEST-ONLY compact
 * entry hull:source:tests:fuzz_parse, which calls parse() directly (retains nothing), validates
 * the whole SourceUnit in JS, and returns a tiny verdict. This harness never accumulates state,
 * so the campaign explores the parser instead of drifting into resource-exhaustion testing.
 * Design: docs/js_source_fuzz_design.md.
 *
 * Classification (matches the real session API's two channels):
 *   rc == 0 : the entry ran; the verdict JSON says ok:true (clean/syntax/unsupported and
 *             parser-level js.limit.* are all valid) or ok:false (a validation breach OR a
 *             detected js.internal -> abort).
 *   rc != 0 with a host js.limit.* : EXPECTED resource exhaustion (heap/stack/instruction/
 *             source/result). Not a crash. Triggers the active session-reuse proof.
 *   rc != 0 with js.internal / malformed / unclassified : abort.
 *
 * Build/run: make fuzz-js-source CC=clang     (sanitizer split; see mk/tests.mk)
 * Local smoke (needs QuickJS + the session, so use the make target, not a bare cc):
 *   the -DHL_FUZZ_STANDALONE main below runs file args through the same path.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/frontend/js_session.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECREATE_EVERY 512               /* recreate the session every N inputs (hygiene) */

static HlJsSession *S = NULL;
static unsigned long g_count = 0;

static void die(const char *msg, const char *detail)
{
    fprintf(stderr, "fuzz_js_source: %s%s%s\n",
            msg, detail ? ": " : "", detail ? detail : "");
    abort();
}

/* Tight-but-real limits: exhaustion is a classified host js.limit.*, never a crash. */
static HlJsSession *make_session(void)
{
    HlJsSessionLimits lim = {
        .max_heap_bytes   = (size_t)64 * 1024 * 1024,
        .max_stack_bytes  = (size_t)1 * 1024 * 1024,
        .max_instructions = (int64_t)100 * 1000 * 1000,   /* finite -> runaway hits js.limit.instructions */
        .max_source_bytes = (size_t)8 * 1024 * 1024,      /* > -max_len and any staged repo file */
        .max_result_bytes = (size_t)1 * 1024 * 1024,      /* the verdict is tiny */
    };
    HlJsSession *s = hl_js_session_create(&lim);
    if (!s) die("hl_js_session_create failed", NULL);
    return s;
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    S = make_session();
    return 0;
}

/* Run the compact entry once. rc is the session rc; *out (caller frees) is the JSON. */
static int run_entry(const uint8_t *data, size_t size, const char *opts, char **out)
{
    size_t out_len = 0;
    *out = NULL;
    return hl_js_session_analyze(S, "hull:source:tests:fuzz_parse", "fuzz",
                                 data, size, "f.js",
                                 opts, opts ? strlen(opts) : 0, out, &out_len);
}

/* After any host-level js.limit.*, prove the session is still usable: a fixed tiny valid input
 * must return a clean ok:true verdict. Catches a pending exception / poisoned session state. */
static void prove_reuse(void)
{
    static const char PROBE[] = "const a = 1;";
    char *out = NULL;
    int rc = run_entry((const uint8_t *)PROBE, sizeof(PROBE) - 1, "{\"maxDiagnostics\":4096}", &out);
    int ok = (rc == 0 && out && strstr(out, "\"ok\":true") != NULL);
    free(out);
    if (!ok) die("session poisoned after host js.limit.* (reuse probe failed)", NULL);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!S) LLVMFuzzerInitialize(NULL, NULL);

    /* Deterministically derive maxDiagnostics, covering 0 / small finite / default. */
    long md;
    if (size == 0) md = 0;
    else switch (data[0] & 3) { case 0: md = 0; break; case 1: md = 1; break; case 2: md = 3; break; default: md = 4096; }
    char opts[48];
    snprintf(opts, sizeof opts, "{\"maxDiagnostics\":%ld}", md);

    char *out = NULL;
    int rc = run_entry(data, size, opts, &out);

    if (rc == 0) {
        if (!out) die("rc==0 but no verdict JSON", NULL);
        if (strstr(out, "\"ok\":true")) {
            /* pass */
        } else if (strstr(out, "\"ok\":false")) {
            char detail[256]; detail[0] = '\0';
            const char *r = strstr(out, "\"reason\":");
            if (r) { snprintf(detail, sizeof detail, "%.200s", r); }
            free(out);
            die("parser invariant violated", detail[0] ? detail : NULL);
        } else {
            free(out);
            die("rc==0 but verdict has no ok field", NULL);
        }
    } else {
        /* rc != 0: host-classified. Only js.limit.* is expected; everything else aborts. */
        int host_limit = (out && strstr(out, "\"js.limit.") != NULL);
        int internal   = (out && strstr(out, "\"js.internal\"") != NULL);
        if (host_limit && !internal) {
            free(out); out = NULL;
            prove_reuse();                 /* active session-reuse proof after exhaustion */
        } else {
            char detail[256]; snprintf(detail, sizeof detail, "rc=%d out=%.180s", rc, out ? out : "(null)");
            free(out);
            die("host rc!=0 not js.limit.* (internal/malformed/unclassified)", detail);
        }
    }
    free(out);

    /* Defensive hygiene: recreate the session every N inputs. */
    if (++g_count % RECREATE_EVERY == 0) {
        hl_js_session_destroy(S);
        S = make_session();
    }
    return 0;
}

#ifdef HL_FUZZ_STANDALONE
/* Local smoke driver (no libFuzzer): run each file arg (or stdin) through the harness. */
static int run_file(const char *path)
{
    FILE *f = path ? fopen(path, "rb") : stdin;
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    static uint8_t buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    if (path) fclose(f);
    LLVMFuzzerTestOneInput(buf, n);
    return 0;
}

int main(int argc, char **argv)
{
    LLVMFuzzerInitialize(&argc, &argv);
    if (argc <= 1) run_file(NULL);
    else for (int i = 1; i < argc; i++) run_file(argv[i]);
    fprintf(stderr, "fuzz_js_source: %d input(s) OK\n", argc > 1 ? argc - 1 : 1);
    return 0;
}
#endif
