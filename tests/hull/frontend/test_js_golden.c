/*
 * test_js_golden.c - the frozen JS-mode output snapshot (docs/typescript_design.md section 5).
 *
 * The TypeScript work adds a mode to the JS lexer and parser; JS mode must not change. This
 * suite runs a FROZEN input set through every JS-mode entry (js_frontend_dump.h: lex, parse +
 * annotations, scope) and compares one SHA-256 per file with the committed
 * tests/fixtures/js_frontend_golden/golden.txt. Any difference fails, naming the files.
 *
 * Frozen inputs only: the pinned Test262 subset (tests/fixtures/test262/cases) and the
 * hand-written edge cases (tests/fixtures/js_frontend_golden/cases/ *.src - a suffix the
 * repo-corpus conformance walk does not pick up, since some are deliberately malformed).
 * The live repo JS changes in ordinary PRs, so it is not frozen here; scripts/
 * js_frontend_diff.sh compares it between the merge base and the PR head instead.
 *
 * A change to JS-mode output that is MEANT: regenerate with `make js-frontend-golden`
 * (HULL_JS_GOLDEN_UPDATE=1) and say why in the PR; the golden diff is then visible in review.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"
#include "js_frontend_dump.h"

#define GOLDEN_FILE "tests/fixtures/js_frontend_golden/golden.txt"

static const char *const ROOT_T262[]  = { "tests/fixtures/test262/cases", NULL };
static const char *const ROOT_CASES[] = { "tests/fixtures/js_frontend_golden/cases", NULL };
static const char *const EXT_JS[]  = { ".js", NULL };
static const char *const EXT_SRC[] = { ".src", NULL };

typedef struct { char *path; char hex[65]; } Entry;

static int enumerate(JfdPaths *pv)
{
    if (jfd_walk(ROOT_T262[0], EXT_JS, pv) != 0) return -1;
    if (jfd_walk(ROOT_CASES[0], EXT_SRC, pv) != 0) return -1;
    jfd_sort(pv);
    return 0;
}

/* Digest every frozen input. Returns the count, -1 on an enumeration / read failure. */
static int compute(HlJsSession *s, Entry **out)
{
    JfdPaths pv = {0};
    if (enumerate(&pv) != 0) { jfd_paths_free(&pv); return -1; }
    Entry *es = (Entry *)calloc(pv.len ? pv.len : 1, sizeof(Entry));
    if (!es) { jfd_paths_free(&pv); return -1; }
    for (size_t i = 0; i < pv.len; i++) {
        size_t n = 0;
        char *src = jfd_read(pv.items[i], &n);
        if (!src) { fprintf(stderr, "[golden] cannot read %s\n", pv.items[i]); free(es); jfd_paths_free(&pv); return -1; }
        char *outs[JFD_NMETHODS]; size_t lens[JFD_NMETHODS]; int rcs[JFD_NMETHODS];
        jfd_run(s, src, n, pv.items[i], outs, lens, rcs);
        jfd_digest(outs, lens, rcs, es[i].hex);
        jfd_free_outs(outs);
        free(src);
        es[i].path = pv.items[i];
        pv.items[i] = NULL;
    }
    int count = (int)pv.len;
    jfd_paths_free(&pv);
    *out = es;
    return count;
}

static void free_entries(Entry *es, int n)
{
    for (int i = 0; i < n; i++) free(es[i].path);
    free(es);
}

/* Parse golden.txt: `#` comment lines, then `<64 hex>  <path>` lines in path order. */
static int load_golden(Entry **out)
{
    size_t n = 0;
    char *buf = jfd_read(GOLDEN_FILE, &n);
    if (!buf) return -1;
    int cap = 1024, cnt = 0;
    Entry *es = (Entry *)calloc((size_t)cap, sizeof(Entry));
    if (!es) { free(buf); return -1; }
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '#' || line[0] == '\0') continue;
        if (strlen(line) < 67 || line[64] != ' ' || line[65] != ' ') { free_entries(es, cnt); free(buf); return -1; }
        if (cnt == cap) {
            cap *= 2;
            Entry *ne = (Entry *)realloc(es, (size_t)cap * sizeof(Entry));
            if (!ne) { free_entries(es, cnt); free(buf); return -1; }
            es = ne;
        }
        memcpy(es[cnt].hex, line, 64); es[cnt].hex[64] = '\0';
        es[cnt].path = strdup(line + 66);
        if (!es[cnt].path) { free_entries(es, cnt); free(buf); return -1; }
        cnt++;
    }
    free(buf);
    *out = es;
    return cnt;
}

static int write_golden(const Entry *es, int n)
{
    const char *tmp = GOLDEN_FILE ".tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    fprintf(f, "# JS-mode frontend output snapshot: SHA-256 over the lex, parse and resolveScope\n"
               "# output of each frozen input (tests/hull/frontend/js_frontend_dump.h).\n"
               "# Checked by test_js_golden. Regenerate only for a MEANT change: make js-frontend-golden\n");
    for (int i = 0; i < n; i++) fprintf(f, "%s  %s\n", es[i].hex, es[i].path);
    if (fclose(f) != 0) { remove(tmp); return -1; }
    return rename(tmp, GOLDEN_FILE);
}

UTEST(js_golden, snapshot_matches)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    Entry *cur = NULL;
    int nc = compute(s, &cur);
    hl_js_session_destroy(s);
    ASSERT_TRUE_MSG(nc >= 0, "frozen input enumeration must not fail open");
    EXPECT_TRUE(nc >= 600);                       /* the Test262 subset alone is ~620 files */

    const char *upd = getenv("HULL_JS_GOLDEN_UPDATE");
    if (upd && !strcmp(upd, "1")) {
        EXPECT_EQ(write_golden(cur, nc), 0);
        fprintf(stderr, "[golden] wrote %d entries to %s\n", nc, GOLDEN_FILE);
        free_entries(cur, nc);
        return;
    }

    Entry *gold = NULL;
    int ng = load_golden(&gold);
    if (ng < 0) { free_entries(cur, nc); ASSERT_TRUE_MSG(0, "cannot read " GOLDEN_FILE); }

    /* Both lists are in strcmp path order: one merge pass names every difference. */
    int i = 0, j = 0, changed = 0, missing = 0, added = 0;
    while (i < nc || j < ng) {
        int c = (i >= nc) ? 1 : (j >= ng) ? -1 : strcmp(cur[i].path, gold[j].path);
        if (c == 0) {
            if (strcmp(cur[i].hex, gold[j].hex) != 0) { changed++; fprintf(stderr, "[golden changed] %s\n", cur[i].path); }
            i++; j++;
        } else if (c < 0) { added++; fprintf(stderr, "[golden new input] %s\n", cur[i].path); i++; }
        else { missing++; fprintf(stderr, "[golden input gone] %s\n", gold[j].path); j++; }
    }
    if (changed || missing || added)
        fprintf(stderr, "[golden] JS-mode output changed: %d changed, %d new, %d gone.\n"
                        "[golden] Compare full outputs with scripts/js_frontend_diff.sh; regenerate\n"
                        "[golden] only for a meant change: make js-frontend-golden\n",
                changed, added, missing);
    EXPECT_EQ(changed, 0);
    EXPECT_EQ(added, 0);
    EXPECT_EQ(missing, 0);
    free_entries(cur, nc);
    free_entries(gold, ng);
}

/* The digest must see a one-byte change in any method's output, and be stable run to run:
 * otherwise snapshot_matches could pass a regression. */
UTEST(js_golden, digest_is_sensitive_and_stable)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    static const char *const SRCS[] = {
        "const a = 1;", "const a = 1; ", "const a = 2;", "let a = 1;", "const a = 1;", "",
    };
    char hex[6][65];
    for (int k = 0; k < 6; k++) {
        char *outs[JFD_NMETHODS]; size_t lens[JFD_NMETHODS]; int rcs[JFD_NMETHODS];
        jfd_run(s, SRCS[k], strlen(SRCS[k]), "g.js", outs, lens, rcs);
        for (size_t m = 0; m < JFD_NMETHODS; m++) EXPECT_EQ(rcs[m], 0);
        jfd_digest(outs, lens, rcs, hex[k]);
        jfd_free_outs(outs);
    }
    hl_js_session_destroy(s);
    EXPECT_STREQ(hex[0], hex[4]);                 /* same source, same session: same digest */
    EXPECT_STRNE(hex[0], hex[1]);                 /* trailing space: lex ranges move */
    EXPECT_STRNE(hex[0], hex[2]);                 /* literal value */
    EXPECT_STRNE(hex[0], hex[3]);                 /* declaration kind */
    EXPECT_STRNE(hex[0], hex[5]);

    /* Known-answer: SHA-256("abc"). */
    JfdSha c; char h[65];
    jfd_sha_init(&c); jfd_sha_update(&c, "abc", 3); jfd_sha_hex(&c, h);
    EXPECT_STREQ(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    /* 56 bytes: the padding crosses a block boundary. */
    jfd_sha_init(&c);
    jfd_sha_update(&c, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
    jfd_sha_hex(&c, h);
    EXPECT_STREQ(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

/* TypeScript syntax stays a JS syntax error in JS mode, and JS mode never reports a `ts.*`
 * diagnostic (the TypeScript mode is opt-in, never chosen from a path). The inputs are the
 * frozen ts_syntax_is_js_error_*.src cases, parsed under a `.ts` path to prove the path
 * alone selects nothing. */
UTEST(js_golden, ts_syntax_is_a_js_error)
{
    HlJsSession *s = hl_js_session_create(NULL);
    ASSERT_TRUE(s != NULL);
    int seen = 0;
    for (int k = 1; k <= 32; k++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/ts_syntax_is_js_error_%d.src", ROOT_CASES[0], k);
        size_t n = 0;
        char *src = jfd_read(path, &n);
        if (!src) break;
        seen++;
        for (int as_ts = 0; as_ts < 2; as_ts++) {
            char *out = NULL; size_t ol = 0;
            int rc = hl_js_session_analyze(s, "hull:source:lextest", "parse", (const uint8_t *)src, n,
                                           as_ts ? "case.ts" : "case.js", NULL, 0, &out, &ol);
            EXPECT_EQ(rc, 0);
            EXPECT_TRUE_MSG(out && strstr(out, "\"code\":\"js.syntax\""), path);
            EXPECT_TRUE_MSG(out && !strstr(out, "\"code\":\"ts."), path);
            free(out);
        }
        free(src);
    }
    hl_js_session_destroy(s);
    EXPECT_TRUE(seen >= 9);
}

UTEST_MAIN()
