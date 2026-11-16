/*
 * js_frontend_dump.c - dump the JS-mode frontend output of a source tree (test-only tool).
 *
 *   js_frontend_dump [--out DIR] [--time] PATH...
 *
 * Each PATH is a file or a directory walked fail-closed for .js / .mjs / .cjs / .src
 * (js_frontend_dump.h). Without --out, prints `<sha256>  <path>` per file, the digest
 * test_js_golden keeps. With --out, writes DIR/<path>.<method>.json for every snapshot
 * method (its return code on the first line, then the exact JSON), so two dumps can be
 * compared with diff -r. --time prints the time spent in the frontend to stderr.
 *
 * scripts/js_frontend_diff.sh builds this at the merge base and at the PR head and
 * compares the two dumps of the same inputs. Never shipped.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "js_frontend_dump.h"

#include <time.h>

static const char *const EXTS[] = { ".js", ".mjs", ".cjs", ".src", NULL };

static int mkdirs(const char *path)
{
    char buf[4096];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buf)) return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\0') {
            char c = buf[i];
            buf[i] = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
            buf[i] = c;
        }
    }
    return 0;
}

static int write_out(const char *dir, const char *path, const char *method, int rc,
                     const char *out, size_t len)
{
    char file[4096];
    int k = snprintf(file, sizeof(file), "%s/%s.%s.json", dir, path, method);
    if (k < 0 || (size_t)k >= sizeof(file)) return -1;
    char *slash = strrchr(file, '/');
    *slash = '\0';
    if (mkdirs(file) != 0) return -1;
    *slash = '/';
    FILE *f = fopen(file, "wb");
    if (!f) return -1;
    fprintf(f, "rc=%d\n", rc);
    if (out && len) fwrite(out, 1, len, f);
    fputc('\n', f);
    return fclose(f) == 0 ? 0 : -1;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    const char *out_dir = NULL;
    int timing = 0;
    JfdPaths pv = {0};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) { out_dir = argv[++i]; continue; }
        if (!strcmp(argv[i], "--time")) { timing = 1; continue; }
        if (argv[i][0] == '-') { fprintf(stderr, "usage: js_frontend_dump [--out DIR] [--time] PATH...\n"); return 2; }
        struct stat st;
        if (lstat(argv[i], &st) != 0) { fprintf(stderr, "js_frontend_dump: %s: %s\n", argv[i], strerror(errno)); return 2; }
        if (S_ISDIR(st.st_mode)) {
            if (jfd_walk(argv[i], EXTS, &pv) != 0) { fprintf(stderr, "js_frontend_dump: walk of %s failed\n", argv[i]); return 2; }
        } else if (S_ISREG(st.st_mode)) {
            if (jfd_push(&pv, argv[i]) != 0) return 2;
        } else {
            fprintf(stderr, "js_frontend_dump: %s: not a file or directory\n", argv[i]);
            return 2;
        }
    }
    if (pv.len == 0) { fprintf(stderr, "usage: js_frontend_dump [--out DIR] [--time] PATH...\n"); return 2; }
    jfd_sort(&pv);

    HlJsSession *s = hl_js_session_create(NULL);
    if (!s) { fprintf(stderr, "js_frontend_dump: cannot create the tooling session\n"); return 2; }
    int rc = 0;
    double spent = 0;
    for (size_t i = 0; i < pv.len; i++) {
        if (i > 0 && !strcmp(pv.items[i], pv.items[i - 1])) continue;     /* a path given twice */
        size_t n = 0;
        char *src = jfd_read(pv.items[i], &n);
        if (!src) { fprintf(stderr, "js_frontend_dump: cannot read %s\n", pv.items[i]); rc = 2; break; }
        char *outs[JFD_NMETHODS]; size_t lens[JFD_NMETHODS]; int rcs[JFD_NMETHODS];
        double t0 = now_ms();
        jfd_run(s, src, n, pv.items[i], outs, lens, rcs);
        spent += now_ms() - t0;
        if (out_dir) {
            for (size_t m = 0; m < JFD_NMETHODS; m++)
                if (write_out(out_dir, pv.items[i], JFD_METHODS[m], rcs[m], outs[m], lens[m]) != 0) {
                    fprintf(stderr, "js_frontend_dump: cannot write the output of %s\n", pv.items[i]);
                    rc = 2;
                }
        } else {
            char hex[65];
            jfd_digest(outs, lens, rcs, hex);
            printf("%s  %s\n", hex, pv.items[i]);
        }
        jfd_free_outs(outs);
        free(src);
        if (rc) break;
    }
    if (timing) fprintf(stderr, "js_frontend_dump: %zu files, %.1f ms in the frontend\n", pv.len, spent);
    hl_js_session_destroy(s);
    jfd_paths_free(&pv);
    return rc;
}
