/*
 * js_frontend_dump.h - shared core of the JS-frontend output snapshot (test-only).
 *
 * The TypeScript work (docs/typescript_design.md section 3) adds a TypeScript mode to the
 * JS lexer and parser. JS mode must not change while it does. This header runs one source
 * through every JS-mode entry the tests can reach - the lexer (`lex`), the parser with its
 * annotation attachment (`parse`) and the scope resolver (`resolveScope`) - and folds the
 * exact JSON each returns into one SHA-256 per file.
 *
 * Two users:
 *   test_js_golden.c      - compares the digests of a FROZEN input set with the committed
 *                           tests/fixtures/js_frontend_golden/golden.txt (every make test).
 *   js_frontend_dump.c    - the CLI scripts/js_frontend_diff.sh builds at the merge base and
 *                           at the PR head, to compare full outputs over the live repo JS.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_TEST_JS_FRONTEND_DUMP_H
#define HL_TEST_JS_FRONTEND_DUMP_H

#include "hull/frontend/js_session.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

/* The entries of hull:source:lextest a snapshot covers, in digest order. A new JS-mode
 * entry the TypeScript work could change is added here (and the golden regenerated). */
static const char *const JFD_METHODS[] = { "lex", "parse", "resolveScope" };
#define JFD_NMETHODS (sizeof(JFD_METHODS) / sizeof(JFD_METHODS[0]))

/* -- SHA-256 (self-contained: no crypto-cap link dependency) -- */
typedef struct { uint32_t s[8]; uint64_t n; uint8_t b[64]; size_t bl; } JfdSha;
static const uint32_t JFD_K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
static inline uint32_t jfd_ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline void jfd_block(JfdSha *c, const uint8_t *p)
{
    uint32_t w[64], a, b, cc, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 | (uint32_t)p[i*4+2] << 8 | p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = jfd_ror(w[i-15], 7) ^ jfd_ror(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = jfd_ror(w[i-2], 17) ^ jfd_ror(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = c->s[0]; b = c->s[1]; cc = c->s[2]; d = c->s[3]; e = c->s[4]; f = c->s[5]; g = c->s[6]; h = c->s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (jfd_ror(e, 6) ^ jfd_ror(e, 11) ^ jfd_ror(e, 25)) + ((e & f) ^ (~e & g)) + JFD_K[i] + w[i];
        uint32_t t2 = (jfd_ror(a, 2) ^ jfd_ror(a, 13) ^ jfd_ror(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->s[0] += a; c->s[1] += b; c->s[2] += cc; c->s[3] += d; c->s[4] += e; c->s[5] += f; c->s[6] += g; c->s[7] += h;
}
static inline void jfd_sha_init(JfdSha *c)
{
    static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(c->s, iv, sizeof(iv)); c->n = 0; c->bl = 0;
}
static inline void jfd_sha_update(JfdSha *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    c->n += (uint64_t)len * 8;
    while (len) {
        size_t k = 64 - c->bl; if (k > len) k = len;
        memcpy(c->b + c->bl, p, k); c->bl += k; p += k; len -= k;
        if (c->bl == 64) { jfd_block(c, c->b); c->bl = 0; }
    }
}
static inline void jfd_sha_hex(JfdSha *c, char out[65])
{
    uint64_t n = c->n;
    uint8_t pad = 0x80, zero = 0, len[8];
    jfd_sha_update(c, &pad, 1);
    while (c->bl != 56) jfd_sha_update(c, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(n >> (56 - 8 * i));
    jfd_sha_update(c, len, 8);
    static const char hx[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) for (int j = 3; j >= 0; j--) {
        uint8_t byte = (uint8_t)(c->s[i] >> (j * 8));
        *out++ = hx[byte >> 4]; *out++ = hx[byte & 15];
    }
    *out = '\0';
}

/* -- fail-closed enumeration (the conformance harness's rules) -- */
typedef struct { char **items; size_t len, cap; } JfdPaths;
static inline int jfd_push(JfdPaths *v, const char *p)
{
    if (v->len == v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 256;
        char **ni = (char **)realloc(v->items, nc * sizeof(char *));
        if (!ni) return -1;
        v->items = ni; v->cap = nc;
    }
    v->items[v->len] = strdup(p);
    return v->items[v->len++] ? 0 : -1;
}
static inline void jfd_paths_free(JfdPaths *v)
{
    for (size_t i = 0; i < v->len; i++) free(v->items[i]);
    free(v->items); v->items = NULL; v->len = v->cap = 0;
}
static inline int jfd_cmp(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

/* `exts` is a NULL-terminated list of accepted suffixes. Regular files only, no symlinks,
 * dot-directories, `static` and `node_modules` pruned. Any failure returns -1 (never a
 * partial list reported as complete). Sort the result with jfd_sort. */
static inline int jfd_walk(const char *dir, const char *const *exts, JfdPaths *out)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (!e) { if (errno != 0) rc = -1; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[4096];
        int k = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (k < 0 || (size_t)k >= sizeof(path)) { rc = -1; break; }
        struct stat st;
        if (lstat(path, &st) != 0) { rc = -1; break; }
        if (S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) {
            if (e->d_name[0] == '.' || !strcmp(e->d_name, "static") || !strcmp(e->d_name, "node_modules")) continue;
            if (jfd_walk(path, exts, out) != 0) { rc = -1; break; }
        } else if (S_ISREG(st.st_mode)) {
            size_t nl = strlen(e->d_name);
            for (const char *const *x = exts; *x; x++) {
                size_t xl = strlen(*x);
                if (nl > xl && !strcmp(e->d_name + nl - xl, *x)) {
                    if (jfd_push(out, path) != 0) rc = -1;
                    break;
                }
            }
            if (rc != 0) break;
        }
    }
    if (closedir(d) != 0 && rc == 0) rc = -1;
    return rc;
}
static inline void jfd_sort(JfdPaths *v) { if (v->len) qsort(v->items, v->len, sizeof(char *), jfd_cmp); }

/* Exact-size read; NULL on any failure. */
static inline char *jfd_read(const char *path, size_t *out_len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    size_t n = (size_t)st.st_size, got = 0;
    char *buf = (char *)malloc(n + 1);
    if (!buf) { close(fd); return NULL; }
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(fd);
    if (got != n) { free(buf); return NULL; }
    buf[n] = '\0'; *out_len = n;
    return buf;
}

/* Run every snapshot method over one source. outs[i] / lens[i] receive each method's JSON
 * (malloc'd; a failed call's structured diagnostic counts as its output) and rcs[i] its
 * return code. `path` is what the frontend sees (it is part of the output: diagnostics
 * carry it). */
static inline void jfd_run(HlJsSession *s, const char *src, size_t len, const char *path,
                    char *outs[JFD_NMETHODS], size_t lens[JFD_NMETHODS], int rcs[JFD_NMETHODS])
{
    for (size_t i = 0; i < JFD_NMETHODS; i++) {
        outs[i] = NULL; lens[i] = 0;
        rcs[i] = hl_js_session_analyze(s, "hull:source:lextest", JFD_METHODS[i],
                                       (const uint8_t *)src, len, path, NULL, 0, &outs[i], &lens[i]);
        if (!outs[i]) lens[i] = 0;
    }
}

/* One digest over every method's name, return code and exact output bytes. */
static inline void jfd_digest(char *outs[JFD_NMETHODS], size_t lens[JFD_NMETHODS],
                       int rcs[JFD_NMETHODS], char hex[65])
{
    JfdSha c;
    jfd_sha_init(&c);
    for (size_t i = 0; i < JFD_NMETHODS; i++) {
        char hdr[64];
        int k = snprintf(hdr, sizeof(hdr), "%s %d %zu", JFD_METHODS[i], rcs[i], lens[i]);
        jfd_sha_update(&c, hdr, (size_t)k + 1);      /* the NUL separates header and body */
        if (outs[i]) jfd_sha_update(&c, outs[i], lens[i]);
    }
    jfd_sha_hex(&c, hex);
}

static inline void jfd_free_outs(char *outs[JFD_NMETHODS])
{
    for (size_t i = 0; i < JFD_NMETHODS; i++) { free(outs[i]); outs[i] = NULL; }
}

#endif /* HL_TEST_JS_FRONTEND_DUMP_H */
