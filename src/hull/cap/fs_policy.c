/*
 * fs_policy.c - compiled path-authorization policy for hull.fs. Parse manifest
 * grants, compile them into two independent authorization-entry sets by a
 * descriptor-bound walk, and select deterministically by most-specific match.
 * See include/hull/cap/fs_policy.h and docs/hull_fs_design.md sec. 6.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/cap/fs_policy.h"
#include "hull/cap/fs_resolve.h"
#include "hull/utils/alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#ifndef HL_FS_PATH_MAX
#define HL_FS_PATH_MAX 4096     /* buffer for the SUBTREE contained-follow probe path */
#endif
#define ANCHOR_RECLASS_MAX 64   /* bound the classify<->open swap retries (compile) */

/* ── small allocation helpers (sizes recovered for the tracked free) ─────────── */

static char *dup_bytes(HlAllocator *a, const char *s, size_t len)
{
    char *p = (char *)hl_alloc_malloc(a, len + 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

static void free_str(HlAllocator *a, char *s)
{
    if (s) hl_alloc_free(a, s, strlen(s) + 1);
}

static void free_comps(HlAllocator *a, char **comp, size_t n)
{
    if (!comp) return;
    for (size_t i = 0; i < n; i++) free_str(a, comp[i]);
    hl_alloc_free(a, comp, n * sizeof(char *));
}

static char **dup_comps(HlAllocator *a, char *const *src, size_t n)
{
    size_t cap = n ? n : 1;                       /* actual array capacity allocated */
    char **out = (char **)hl_alloc_calloc(a, cap, sizeof(char *));
    if (!out) return NULL;
    for (size_t i = 0; i < n; i++) {
        out[i] = dup_bytes(a, src[i], strlen(src[i]));
        if (!out[i]) {
            /* free the strings built so far, then the array at its ALLOCATED
             * capacity (cap), not the partial count i - otherwise the tracked
             * allocator keeps (cap - i) pointers of `used` and can later false-OOM. */
            for (size_t k = 0; k < i; k++) free_str(a, out[k]);
            hl_alloc_free(a, out, cap * sizeof(char *));
            return NULL;
        }
    }
    return out;
}

/* Join components with '/' into buf (for the SUBTREE contained-follow probe).
 * Returns 0, or -1 if the joined path does not fit. */
static int join_path(char *const *comp, size_t n, char *buf, size_t bufsz)
{
    size_t pos = 0;
    for (size_t i = 0; i < n; i++) {
        size_t l = strlen(comp[i]);
        if (pos + l + (pos ? 1 : 0) + 1 > bufsz) return -1;
        if (pos) buf[pos++] = '/';
        memcpy(buf + pos, comp[i], l);
        pos += l;
    }
    buf[pos] = '\0';
    return 0;
}

/* ── component pattern metacharacter validation ──────────────────────────────── */

/* Returns 0 if `seg` (len bytes) is an acceptable v1 component: only '*' is a
 * wildcard; '?', '[', ']', '{', '}', '\\' and the adjacent sequence "**" are
 * unsupported. Sets *has_star. Returns -1 (with *err) on an unsupported char. */
static int scan_component(const char *seg, size_t len, int *has_star, const char **err)
{
    *has_star = 0;
    int prev_star = 0;
    for (size_t i = 0; i < len; i++) {
        char c = seg[i];
        switch (c) {
        case '*':
            if (prev_star) { *err = "unsupported_pattern"; return -1; }  /* "**" */
            *has_star = 1; prev_star = 1;
            continue;
        case '?': case '[': case ']': case '{': case '}': case '\\':
            *err = "unsupported_pattern"; return -1;
        default:
            prev_star = 0;
        }
    }
    return 0;
}

/* ── single-component wildcard matcher (bounded DP, fixed rows, no recursion) ─── */

/* Match pattern `pat` (pn bytes, may contain '*') against literal `s` (sn bytes),
 * within one path component. '*' matches zero or more bytes. O(pn*sn), two fixed
 * NAME_MAX+2 rows on the stack: no VLA, no heap, no recursion. */
static int pat_match(const char *pat, size_t pn, const char *s, size_t sn)
{
    if (pn > NAME_MAX || sn > NAME_MAX) return 0;   /* rejected earlier; defensive */
    unsigned char prev[NAME_MAX + 2], cur[NAME_MAX + 2];
    prev[0] = 1;                                     /* empty pattern matches empty text */
    for (size_t j = 0; j < pn; j++)
        prev[j + 1] = (unsigned char)(prev[j] && pat[j] == '*');
    for (size_t i = 0; i < sn; i++) {
        cur[0] = 0;                                  /* non-empty text vs empty pattern */
        for (size_t j = 0; j < pn; j++) {
            if (pat[j] == '*')
                cur[j + 1] = (unsigned char)(cur[j] || prev[j + 1]);
            else
                cur[j + 1] = (unsigned char)(prev[j] && (unsigned char)pat[j] == (unsigned char)s[i]);
        }
        memcpy(prev, cur, pn + 1);
    }
    return prev[pn];
}

/* ── grant parse ─────────────────────────────────────────────────────────────── */

int hl_fs_grant_parse(const char *raw, size_t raw_len, HlAllocator *alloc,
                      HlFsGrant *out, const char **err)
{
    const char *e = "invalid_path";
    if (!raw || !out) { if (err) *err = "invalid_args"; return -1; }
    if (raw_len == 0) { if (err) *err = "invalid_path"; return -1; }
    if (raw[0] == '/' || raw[0] == '\\') { if (err) *err = "invalid_path"; return -1; }
    if (memchr(raw, '\0', raw_len)) { if (err) *err = "invalid_path"; return -1; }

    int directory_intent = (raw[raw_len - 1] == '/');

    char *tmp[HL_FS_MAX_DEPTH];
    size_t n = 0;
    size_t first_pattern = (size_t)-1;

    size_t i = 0;
    while (i < raw_len) {
        while (i < raw_len && raw[i] == '/') i++;      /* skip separators */
        if (i >= raw_len) break;
        size_t start = i;
        while (i < raw_len && raw[i] != '/') i++;
        size_t seglen = i - start;
        const char *seg = raw + start;

        if (seglen == 1 && seg[0] == '.') continue;    /* "." adds no depth */
        if (seglen == 2 && seg[0] == '.' && seg[1] == '.') { e = "invalid_path"; goto fail; }
        if (seglen > NAME_MAX) { e = "invalid_path"; goto fail; }

        int has_star = 0;
        if (scan_component(seg, seglen, &has_star, &e) != 0) goto fail;
        if (has_star && first_pattern == (size_t)-1) first_pattern = n;

        if (n >= HL_FS_MAX_DEPTH) { e = "invalid_path"; goto fail; }
        tmp[n] = dup_bytes(alloc, seg, seglen);
        if (!tmp[n]) { e = "io_error"; goto fail; }
        n++;
    }

    /* n == 0 is the BASE-ROOT grant ("." / "./" / a bare "/" is rejected earlier as
     * absolute): it authorizes the app root and every descendant (a SUBTREE at
     * base_fd). It compiles to a grant with 0 components - the least-specific
     * entry, shadowed by any narrower grant. directory_intent is meaningless for
     * the root (always a directory), so it is normalized to 0. */
    if (first_pattern == (size_t)-1) first_pattern = n;
    if (directory_intent && first_pattern < n) {       /* trailing-slash pattern grant */
        e = "unsupported_pattern"; goto fail;
    }

    char **comp = NULL;
    if (n > 0) {
        comp = (char **)hl_alloc_calloc(alloc, n, sizeof(char *));
        if (!comp) { e = "io_error"; goto fail; }
        memcpy(comp, tmp, n * sizeof(char *));
    }

    out->comp = comp;
    out->n = n;
    out->first_pattern = first_pattern;
    out->directory_intent = (n == 0) ? 0 : directory_intent;
    out->alloc = alloc;
    return 0;

fail:
    for (size_t k = 0; k < n; k++) free_str(alloc, tmp[k]);
    if (err) *err = e;
    return -1;
}

void hl_fs_grant_free(HlFsGrant *grant)
{
    if (!grant || !grant->comp) { if (grant) *grant = HL_FS_GRANT_INIT; return; }
    free_comps(grant->alloc, grant->comp, grant->n);
    *grant = HL_FS_GRANT_INIT;
}

/* ── grant identity + specificity ────────────────────────────────────────────── */

/* Component-wise byte comparison of two grants' components: <0 / 0 / >0. */
static int comps_cmp(char *const *a, size_t an, char *const *b, size_t bn)
{
    size_t m = an < bn ? an : bn;
    for (size_t i = 0; i < m; i++) {
        int c = strcmp(a[i], b[i]);
        if (c) return c;
    }
    if (an != bn) return an < bn ? -1 : 1;
    return 0;
}

/* Two grants are "the same path" iff component-identical. */
static int grants_same_path(const HlFsGrant *a, const HlFsGrant *b)
{
    return a->n == b->n && comps_cmp(a->comp, a->n, b->comp, b->n) == 0;
}

static void specificity(char *const *comp, size_t n,
                        size_t *literal_components, size_t *literal_bytes)
{
    size_t lc = 0, lb = 0;
    for (size_t i = 0; i < n; i++) {
        int has_star = 0;
        for (const char *p = comp[i]; *p; p++) {
            if (*p == '*') has_star = 1; else lb++;
        }
        if (!has_star) lc++;
    }
    *literal_components = lc;
    *literal_bytes = lb;
}

/* ── descriptor-bound compile of ONE grant into an entry ─────────────────────── */

static void entry_free(HlAllocator *a, HlFsAuthEntry *e)
{
    if (e->anchor_fd >= 0) { close(e->anchor_fd); e->anchor_fd = -1; }
    free_comps(a, e->grant, e->grant_n);
    e->grant = NULL; e->grant_n = 0;
}

/* Walk the existing literal prefix of `g` from base_fd, O_NOFOLLOW per component
 * (refusing symlinks), and classify. Fills `out` (deep-copying grant comps). */
static int compile_one(int base_fd, HlAllocator *alloc, const HlFsGrant *g,
                       HlFsAuthEntry *out, const char **err)
{
    const char *e;                          /* every `goto fail` sets this before use */
    size_t lit = g->first_pattern;          /* literal prefix length */

    /* BASE-ROOT grant (0 components, "."): a SUBTREE anchored at base_fd itself,
     * matching every caller path. anchor is base (F_DUPFD_CLOEXEC); no walk. */
    if (g->n == 0) {
        int afd = fcntl(base_fd, F_DUPFD_CLOEXEC, 0);
        if (afd < 0) { *err = "io_error"; return -1; }
        out->kind = HL_FS_ENTRY_SUBTREE;
        out->anchor_fd = afd;
        out->grant = NULL;                  /* 0 components; entry_free handles NULL */
        out->grant_n = 0;
        out->anchor_depth = 0;
        out->first_pattern = 0;
        out->terminal = HL_FS_TERMINAL_FILE;
        out->literal_components = 0;
        out->literal_bytes = 0;
        return 0;
    }

    /* A pure-literal grant that resolves to a DIRECTORY is a SUBTREE. Per the
     * approved design (sec. 6), a SUBTREE FOLLOWS in-root symlinks with virtual-root
     * confinement, so its anchor is obtained by a CONTAINED-FOLLOW resolution (the
     * HL_FS_OPEN_DIR resolver mode: each component O_NOFOLLOW + a contained readlink
     * splice, so a symlinked grant directory still loads and cannot escape the
     * root). If it instead resolves to a non-directory (EXACT) or is missing
     * (CREATE), fall through to the O_NOFOLLOW refuse walk below, which REFUSES any
     * symlink for EXACT/CREATE. PATTERN grants (first_pattern < n) skip this probe
     * and always refuse symlinks. */
    if (g->first_pattern == g->n) {
        char path[HL_FS_PATH_MAX];
        if (join_path(g->comp, g->n, path, sizeof(path)) == 0) {
            const char *de = "io_error";
            int dfd = hl_fs_open_at(base_fd, path, HL_FS_OPEN_DIR, 0, &de);
            if (dfd >= 0) {
                char **grant = dup_comps(alloc, g->comp, g->n);
                if (!grant) { close(dfd); *err = "io_error"; return -1; }
                out->kind = HL_FS_ENTRY_SUBTREE;
                out->anchor_fd = dfd;
                out->grant = grant;
                out->grant_n = g->n;
                out->anchor_depth = g->n;
                out->first_pattern = g->first_pattern;
                out->terminal = HL_FS_TERMINAL_FILE;
                specificity(g->comp, g->n, &out->literal_components, &out->literal_bytes);
                return 0;
            }
            /* not_a_directory (a file -> EXACT) or not_found (-> CREATE) continue to
             * the refuse walk; any other token (symlink_loop, permission, ...) is
             * terminal. */
            if (strcmp(de, "not_a_directory") != 0 && strcmp(de, "not_found") != 0) {
                *err = de; return -1;
            }
        }
    }

    /* F_DUPFD_CLOEXEC, not dup(): a plain dup() clears FD_CLOEXEC, and this fd is
     * RETAINED as the anchor when the grant anchors at base (root-level
     * EXACT/CREATE/PATTERN) - it must not leak across exec. */
    int cur = fcntl(base_fd, F_DUPFD_CLOEXEC, 0);
    if (cur < 0) { *err = "io_error"; return -1; }

    size_t depth = 0;                        /* existing literal components held by `cur` */
    int is_exact = 0;                        /* last literal component is an existing file */

    for (size_t i = 0; i < lit; i++) {
        int reclass = 0;
        for (;;) {
            struct stat st;
            if (fstatat(cur, g->comp[i], &st, AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno == ENOENT) goto done_walk;         /* missing from component i */
                e = (errno == EACCES || errno == EPERM) ? "permission" : "io_error";
                goto fail;
            }
            if (S_ISLNK(st.st_mode)) { e = "symlink_denied"; goto fail; }
            if (!S_ISDIR(st.st_mode)) {
                /* a non-directory literal component */
                if (i == lit - 1 && lit == g->n && !g->directory_intent) {
                    is_exact = 1; goto done_walk;             /* EXACT: existing file leaf */
                }
                e = "not_a_directory"; goto fail;             /* non-dir mid-path or dir-intent */
            }
            int nf = openat(cur, g->comp[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (nf < 0) {
                if ((errno == ELOOP || errno == EMLINK) ) { e = "symlink_denied"; goto fail; }
                if ((errno == ENOTDIR || errno == ENOENT) && ++reclass <= ANCHOR_RECLASS_MAX)
                    continue;                                 /* classify/open swap: retry */
                e = (errno == EACCES || errno == EPERM) ? "permission" : "io_error";
                goto fail;
            }
            close(cur); cur = nf; depth = i + 1;
            break;
        }
    }

done_walk:;
    HlFsEntryKind kind;
    HlFsTerminalKind term = HL_FS_TERMINAL_FILE;

    if (is_exact) {
        kind = HL_FS_ENTRY_EXACT;                 /* anchor = parent (depth == g->n - 1) */
    } else if (depth == lit) {
        if (g->first_pattern < g->n) kind = HL_FS_ENTRY_PATTERN;   /* prefix all dirs + patterns */
        else                          kind = HL_FS_ENTRY_SUBTREE;   /* grant dir exists */
    } else {
        /* some literal component missing (depth < lit) */
        if (g->first_pattern < g->n) {
            kind = HL_FS_ENTRY_PATTERN;           /* missing literal middle + patterns */
        } else {
            kind = HL_FS_ENTRY_CREATE;
            term = g->directory_intent ? HL_FS_TERMINAL_SUBTREE : HL_FS_TERMINAL_FILE;
        }
    }

    char **grant = dup_comps(alloc, g->comp, g->n);
    if (!grant) { e = "io_error"; goto fail; }

    out->kind = kind;
    out->anchor_fd = cur;
    out->grant = grant;
    out->grant_n = g->n;
    out->anchor_depth = depth;
    out->first_pattern = g->first_pattern;
    out->terminal = term;
    specificity(g->comp, g->n, &out->literal_components, &out->literal_bytes);
    return 0;

fail:
    if (cur >= 0) close(cur);
    if (err) *err = e;
    return -1;
}

/* ── set compile: dedup, conflict-reject, compile, sort ──────────────────────── */

static int entry_priority_cmp(const void *pa, const void *pb)
{
    const HlFsAuthEntry *a = (const HlFsAuthEntry *)pa;
    const HlFsAuthEntry *b = (const HlFsAuthEntry *)pb;
    if (a->literal_components != b->literal_components)
        return a->literal_components > b->literal_components ? -1 : 1;   /* DESC */
    if (a->literal_bytes != b->literal_bytes)
        return a->literal_bytes > b->literal_bytes ? -1 : 1;            /* DESC */
    return comps_cmp(a->grant, a->grant_n, b->grant, b->grant_n);        /* grant-text ASC */
}

static int compile_set(int base_fd, HlAllocator *alloc,
                       const HlFsGrant *grants, size_t n,
                       HlFsAuthEntry **out, size_t *out_n, const char **err)
{
    *out = NULL; *out_n = 0;
    if (n == 0) return 0;

    /* Pass 1: mark survivors (dedup identical grants within the set) and reject a
     * same-path conflicting-intent pair. Comparing only against KEPT grants is
     * complete: a conflict with a skipped duplicate is also a conflict with the
     * kept original (same path). The mask lets pass 2 allocate EXACTLY the
     * survivor count, so the free size always matches the alloc size. */
    unsigned char *keep = (unsigned char *)hl_alloc_calloc(alloc, n, 1);
    if (!keep) { if (err) *err = "io_error"; return -1; }
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        int dup = 0;
        for (size_t j = 0; j < i; j++) {
            if (!keep[j]) continue;
            if (grants_same_path(&grants[i], &grants[j])) {
                if (grants[i].directory_intent != grants[j].directory_intent) {
                    if (err) *err = "conflicting_grant";
                    hl_alloc_free(alloc, keep, n);
                    return -1;
                }
                dup = 1; break;
            }
        }
        if (!dup) { keep[i] = 1; m++; }
    }

    /* Pass 2: compile the survivors into an exactly-m-sized array. */
    HlFsAuthEntry *entries = (HlFsAuthEntry *)hl_alloc_calloc(alloc, m, sizeof(HlFsAuthEntry));
    if (!entries) { hl_alloc_free(alloc, keep, n); if (err) *err = "io_error"; return -1; }
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (!keep[i]) continue;
        if (compile_one(base_fd, alloc, &grants[i], &entries[w], err) != 0) {
            for (size_t k = 0; k < w; k++) entry_free(alloc, &entries[k]);
            hl_alloc_free(alloc, entries, m * sizeof(HlFsAuthEntry));
            hl_alloc_free(alloc, keep, n);
            return -1;
        }
        w++;
    }
    hl_alloc_free(alloc, keep, n);

    if (m > 1)
        qsort(entries, m, sizeof(HlFsAuthEntry), entry_priority_cmp);

    *out = entries;
    *out_n = m;
    return 0;
}

/* ── public compile / free ───────────────────────────────────────────────────── */

int hl_fs_policy_compile(const char *base_dir, HlAllocator *alloc,
                         const HlFsGrant *read, size_t read_n,
                         const HlFsGrant *write, size_t write_n,
                         HlFsPolicy *out, const char **err)
{
    const char *e = "io_error";
    if (!base_dir || !alloc || !out) { if (err) *err = "invalid_args"; return -1; }

    HlFsPolicy p = HL_FS_POLICY_INIT;
    p.alloc = alloc;
    p.base_fd = hl_fs_open_base(base_dir, &e);
    if (p.base_fd < 0) { if (err) *err = e; return -1; }

    if (compile_set(p.base_fd, alloc, read, read_n, &p.read, &p.read_n, &e) != 0) goto fail;
    if (compile_set(p.base_fd, alloc, write, write_n, &p.write, &p.write_n, &e) != 0) goto fail;

    *out = p;
    return 0;

fail:
    hl_fs_policy_free(&p);
    if (err) *err = e;
    return -1;
}

int hl_fs_policy_compile_manifest(const char *base_dir, HlAllocator *alloc,
                                  const char *const *read, size_t read_n,
                                  const char *const *write, size_t write_n,
                                  HlFsPolicy *out, const char **err)
{
    const char *e = "io_error";
    HlFsGrant *rg = NULL, *wg = NULL;
    size_t ri = 0, wi = 0;
    int rc = -1;

    if (read_n) {
        rg = (HlFsGrant *)hl_alloc_calloc(alloc, read_n, sizeof(HlFsGrant));
        if (!rg) { e = "io_error"; goto done; }
    }
    if (write_n) {
        wg = (HlFsGrant *)hl_alloc_calloc(alloc, write_n, sizeof(HlFsGrant));
        if (!wg) { e = "io_error"; goto done; }
    }
    /* A grant that fails to PARSE (absolute, "..", unsupported pattern, malformed,
     * or an allocation failure) FAILS compilation with its precise token - a
     * declared capability must not silently become unusable (contract in
     * fs_policy.h). Absolute / external-root grants are not base-relative policy
     * grants and are rejected here; an app must use an in-root relative path. */
    for (ri = 0; ri < read_n; ri++)
        if (hl_fs_grant_parse(read[ri], strlen(read[ri]), alloc, &rg[ri], &e) != 0) goto done;
    for (wi = 0; wi < write_n; wi++)
        if (hl_fs_grant_parse(write[wi], strlen(write[wi]), alloc, &wg[wi], &e) != 0) goto done;

    rc = hl_fs_policy_compile(base_dir, alloc, rg, read_n, wg, write_n, out, &e);

done:
    for (size_t i = 0; i < ri; i++) hl_fs_grant_free(&rg[i]);
    for (size_t i = 0; i < wi; i++) hl_fs_grant_free(&wg[i]);
    if (rg) hl_alloc_free(alloc, rg, read_n * sizeof(HlFsGrant));
    if (wg) hl_alloc_free(alloc, wg, write_n * sizeof(HlFsGrant));
    if (err) *err = e;
    return rc;
}

void hl_fs_policy_free(HlFsPolicy *policy)
{
    if (!policy) return;
    HlAllocator *a = policy->alloc;
    for (size_t i = 0; i < policy->read_n; i++) entry_free(a, &policy->read[i]);
    for (size_t i = 0; i < policy->write_n; i++) entry_free(a, &policy->write[i]);
    if (policy->read)  hl_alloc_free(a, policy->read,  policy->read_n  * sizeof(HlFsAuthEntry));
    if (policy->write) hl_alloc_free(a, policy->write, policy->write_n * sizeof(HlFsAuthEntry));
    if (policy->base_fd >= 0) close(policy->base_fd);
    *policy = HL_FS_POLICY_INIT;
}

/* ── selection (pure: no filesystem access) ──────────────────────────────────── */

/* Split a validated caller path into components (skipping "."). Returns component
 * count, or -1 with *err on a lexical violation (absolute / ".." / trailing slash
 * / empty / over-deep / over-long component). `comp`/`len` index into `path`.
 * `allow_empty` = 1 permits a ZERO-component result ("." / "./"): valid only for an
 * ENUMERATION target (fs.list of the anchor root itself), never for a leaf op
 * (read/write/stat need a named terminal, so they pass 0 and a 0-component path is
 * rejected "invalid_path"). */
static long split_caller(const char *path, const char **comp, size_t *len,
                         int allow_empty, const char **err)
{
    if (!path || path[0] == '\0') { *err = "invalid_path"; return -1; }
    if (path[0] == '/' || path[0] == '\\') { *err = "invalid_path"; return -1; }
    size_t plen = strlen(path);
    if (path[plen - 1] == '/') { *err = "invalid_path"; return -1; }   /* trailing slash */

    long n = 0;
    size_t i = 0;
    while (path[i]) {
        while (path[i] == '/') i++;
        if (!path[i]) break;
        size_t start = i;
        while (path[i] && path[i] != '/') i++;
        size_t l = i - start;
        const char *s = path + start;
        if (l == 1 && s[0] == '.') continue;
        if (l == 2 && s[0] == '.' && s[1] == '.') { *err = "invalid_path"; return -1; }
        if (l > NAME_MAX) { *err = "invalid_path"; return -1; }
        if (n >= HL_FS_MAX_DEPTH) { *err = "invalid_path"; return -1; }
        comp[n] = s; len[n] = l; n++;
    }
    if (n == 0 && !allow_empty) { *err = "invalid_path"; return -1; }
    return n;
}

/* Byte-exact equality of a grant component against a caller component. */
static int comp_lit_eq(const char *grant_comp, const char *cc, size_t cl)
{
    return strlen(grant_comp) == cl && memcmp(grant_comp, cc, cl) == 0;
}

int hl_fs_pattern_match(const char *pattern, size_t plen, const char *name, size_t nlen)
{
    if (!pattern || !name) return 0;
    return pat_match(pattern, plen, name, nlen);
}

/* Does the entry authorize caller components ccomp[0..cn)? */
static int entry_matches(const HlFsAuthEntry *e, const char **ccomp, const size_t *clen, long cn)
{
    /* literal comparison of a caller component against a grant component (byte-exact) */
    #define LIT_EQ(gi, ci) (strlen(e->grant[gi]) == clen[ci] && \
                            memcmp(e->grant[gi], ccomp[ci], clen[ci]) == 0)

    if (e->kind == HL_FS_ENTRY_SUBTREE) {
        if ((size_t)cn < e->grant_n) return 0;             /* must be at/under the grant dir */
        for (size_t i = 0; i < e->grant_n; i++) if (!LIT_EQ(i, i)) return 0;
        return 1;
    }
    if (e->kind == HL_FS_ENTRY_EXACT) {
        if ((size_t)cn != e->grant_n) return 0;
        for (size_t i = 0; i < e->grant_n; i++) if (!LIT_EQ(i, i)) return 0;
        return 1;
    }
    if (e->kind == HL_FS_ENTRY_CREATE) {
        if (e->terminal == HL_FS_TERMINAL_SUBTREE) {
            if ((size_t)cn < e->grant_n) return 0;
        } else {
            if ((size_t)cn != e->grant_n) return 0;        /* terminal file: exact */
        }
        for (size_t i = 0; i < e->grant_n; i++) if (!LIT_EQ(i, i)) return 0;
        return 1;
    }
    /* PATTERN: exactly grant_n comps; literal prefix exact; pattern tail matched.
     * first_pattern <= grant_n == cn always (parse invariant); assert it so the
     * analyzer can prove ccomp[i]/clen[i] below stay within the caller components
     * split_caller filled ([0, cn)). */
    if ((size_t)cn != e->grant_n) return 0;
    if (e->first_pattern > (size_t)cn) return 0;
    for (size_t i = 0; i < e->first_pattern; i++) if (!LIT_EQ(i, i)) return 0;
    for (size_t i = e->first_pattern; i < e->grant_n; i++)
        if (!pat_match(e->grant[i], strlen(e->grant[i]), ccomp[i], clen[i])) return 0;
    return 1;
    #undef LIT_EQ
}

/* Build the residual for a matched entry into scratch and return the selection.
 * residual = caller components [anchor_depth, residual_end) joined by '/', or "."
 * for the anchor itself. */
static HlFsSelection make_selection(const HlFsAuthEntry *ent, const char **ccomp,
                                    const size_t *clen, size_t residual_end,
                                    char *scratch, size_t scratch_len)
{
    HlFsSelection sel = { NULL, NULL, "permission" };
    size_t pos = 0;
    for (size_t k = ent->anchor_depth; k < residual_end; k++) {
        size_t need = clen[k] + (pos ? 1 : 0);
        if (pos + need + 1 > scratch_len) { sel.err = "invalid_args"; return sel; }
        if (pos) scratch[pos++] = '/';
        memcpy(scratch + pos, ccomp[k], clen[k]);
        pos += clen[k];
    }
    if (pos == 0) {
        if (scratch_len < 2) { sel.err = "invalid_args"; return sel; }
        scratch[pos++] = '.';                          /* grant root: "." not "" */
    }
    scratch[pos] = '\0';
    sel.entry = ent;
    sel.residual = scratch;
    sel.err = NULL;
    return sel;
}

/* Shared leaf/node selector over one entry set. `allow_empty` = 1 permits a
 * 0-component caller ("." = the app root), which stat uses but read/write/mmap
 * do not (they need a named leaf). Deterministic: entries are pre-sorted, first
 * match wins. */
static HlFsSelection select_in_set(const HlFsAuthEntry *set, size_t set_n,
                                   const char *caller_path, int allow_empty,
                                   char *scratch, size_t scratch_len)
{
    HlFsSelection sel = { NULL, NULL, "permission" };
    if (!scratch || scratch_len == 0) { sel.err = "invalid_args"; return sel; }

    const char *ccomp[HL_FS_MAX_DEPTH];
    size_t clen[HL_FS_MAX_DEPTH];
    const char *e = NULL;
    long cn = split_caller(caller_path, ccomp, clen, allow_empty, &e);
    if (cn < 0) { sel.err = e; return sel; }

    for (size_t i = 0; i < set_n; i++) {                  /* pre-sorted: first match wins */
        const HlFsAuthEntry *ent = &set[i];
        if (!entry_matches(ent, ccomp, clen, cn)) continue;
        return make_selection(ent, ccomp, clen, (size_t)cn, scratch, scratch_len);
    }
    sel.err = "permission";
    return sel;
}

HlFsSelection hl_fs_policy_select(const HlFsPolicy *policy, const char *caller_path,
                                  HlFsOpenMode mode, char *scratch, size_t scratch_len)
{
    HlFsSelection sel = { NULL, NULL, "invalid_args" };
    if (!policy) return sel;

    const HlFsAuthEntry *set;
    size_t set_n;
    if (mode == HL_FS_OPEN_READ)       { set = policy->read;  set_n = policy->read_n; }
    else if (mode == HL_FS_OPEN_WRITE) { set = policy->write; set_n = policy->write_n; }
    else { sel.err = "permission"; return sel; }          /* invalid mode: fail closed */

    return select_in_set(set, set_n, caller_path, 0 /*leaf op*/, scratch, scratch_len);
}

HlFsSelection hl_fs_policy_select_stat(const HlFsPolicy *policy, const char *caller_path,
                                       char *scratch, size_t scratch_len)
{
    HlFsSelection sel = { NULL, NULL, "invalid_args" };
    if (!policy) return sel;
    /* stat authorizes the same nodes as a read but ALSO permits the app root
     * itself (0-component "."), when a base-root grant governs it - read/write/mmap
     * need a named leaf and reject it. */
    return select_in_set(policy->read, policy->read_n, caller_path, 1 /*allow root*/,
                         scratch, scratch_len);
}

/* ── enumeration selection (fs.list) ─────────────────────────────────────────── */

HlFsListSelection hl_fs_policy_select_list(const HlFsPolicy *policy,
                                           const char *caller_path,
                                           char *scratch, size_t scratch_len)
{
    HlFsListSelection sel = { NULL, NULL, NULL, HL_FS_SYMLINK_FOLLOW, "permission" };
    if (!policy || !scratch || scratch_len == 0) { sel.err = "invalid_args"; return sel; }

    /* A list target may be the anchor root itself ("."), so 0 components is valid. */
    const char *ccomp[HL_FS_MAX_DEPTH];
    size_t clen[HL_FS_MAX_DEPTH];
    const char *e = NULL;
    long cn = split_caller(caller_path, ccomp, clen, 1 /*allow_empty*/, &e);
    if (cn < 0) { sel.err = e; return sel; }

    /* Scan in stored priority order (most-specific first). MOST-SPECIFIC SHADOWING
     * RUNS BEFORE THE LISTABILITY CHECK: the FIRST entry that GOVERNS `caller_path`
     * decides, even a governing-but-unlistable one (which denies) - it is never
     * skipped to fall through to a broader grant. */
    for (size_t i = 0; i < policy->read_n; i++) {
        const HlFsAuthEntry *ent = &policy->read[i];
        int governs = 0, listable = 0;
        HlFsSymlink sym = HL_FS_SYMLINK_FOLLOW;
        const char *filter = NULL;
        size_t residual_end = (size_t)cn;

        if (ent->kind == HL_FS_ENTRY_PATTERN && (size_t)cn == ent->first_pattern) {
            /* Listing the pattern's literal-prefix DIRECTORY: enumerate + filter. */
            int ok = 1;
            for (size_t k = 0; k < ent->first_pattern; k++)
                if (!comp_lit_eq(ent->grant[k], ccomp[k], clen[k])) { ok = 0; break; }
            if (ok) {
                governs = 1;
                if (ent->first_pattern == ent->grant_n - 1) {   /* single terminal: listable */
                    listable = 1;
                    sym = HL_FS_SYMLINK_REFUSE;
                    filter = ent->grant[ent->first_pattern];
                    residual_end = ent->first_pattern;          /* == cn here */
                }
                /* multi-component pattern: governs but is NOT listable in v1 (deny). */
            }
        } else if (entry_matches(ent, ccomp, clen, cn)) {
            /* The caller is an authorized NODE (not a pattern prefix). Govern it and
             * let the DIR open decide the outcome: a directory (a SUBTREE descendant,
             * or a created CREATE-subtree) lists; an authorized FILE (an EXACT grant,
             * a matched PATTERN terminal, a CREATE-file) reports "not_a_directory";
             * an absent CREATE target reports "not_found". This preserves the ratified
             * distinction - the exact path is authorized but is not a directory - so
             * its unauthorized parent / siblings stay "permission" while the path
             * itself does not. */
            governs = 1;
            listable = 1;
            sym = (ent->kind == HL_FS_ENTRY_SUBTREE) ? HL_FS_SYMLINK_FOLLOW
                                                     : HL_FS_SYMLINK_REFUSE;
        }

        if (!governs) continue;
        if (!listable) { sel.err = "permission"; return sel; }   /* multi-pattern shadow */

        HlFsSelection base = make_selection(ent, ccomp, clen, residual_end,
                                            scratch, scratch_len);
        if (!base.entry) { sel.err = base.err; return sel; }     /* scratch too small */
        sel.entry = base.entry;
        sel.residual = base.residual;
        sel.filter = filter;
        sel.sympol = sym;
        sel.err = NULL;
        return sel;
    }

    sel.err = "permission";
    return sel;
}
