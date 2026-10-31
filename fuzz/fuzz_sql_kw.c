/*
 * fuzz_sql_kw.c: libFuzzer harness for Hull's SQL-text classifiers.
 *
 * Targets (all pure functions over app-supplied SQL text):
 *   - hl_sql_txn_kind_ex (cap/db_sql_kw.h), both comment rules: decides
 *     whether a text is a COMMIT / ROLLBACK, which the Postgres / MySQL
 *     backends answer "done" WITHOUT sending when the connection was lost
 *     inside a transaction (audit 12), and which DuckDB tracks state from.
 *   - hl_cap_db_sql_calls_dynamic_sql (cap/db_common.c): the DuckDB refusal
 *     of query() / query_table() / json_execute_serialized_sql() (audit 25),
 *     which keeps dynamic SQL from assembling a _hull_* name past
 *   - hl_cap_db_check_namespace (cap/db_common.c).
 *
 * The first byte picks a mode.
 *
 * RAW mode (first byte < 0x80): the rest is SQL text. Metamorphic oracles:
 *   - leading / trailing whitespace never changes the kind;
 *   - ASCII case never changes the kind, the dynamic-SQL verdict, or the
 *     namespace verdict;
 *   - with no block comment opener in the text, both comment rules agree;
 *   - a COMMIT / ROLLBACK text that does not end inside a block comment is
 *     no longer one once a second statement is appended after a newline
 *     (otherwise that statement would be answered "done" unrun);
 *   - a text naming _hull_ (any case) is refused by the namespace check.
 *
 * CONSTRUCT mode (first byte >= 0x80): the rest is a token program that
 * renders SQL whose meaning under the Postgres lexer (Postgres, DuckDB) is
 * known by construction - string literals, quoted identifiers, line comments
 * ended by LF or CR (Postgres ends a "--" comment at either), block comments,
 * identifiers and punctuation. Ground-truth oracles:
 *   - a dynamic-SQL function name (bare or quoted) followed, past only
 *     whitespace and comments, by "(" must be reported by
 *     hl_cap_db_sql_calls_dynamic_sql (its contract is conservative);
 *   - "ROLLBACK" / "COMMIT" followed by any further statement must not be
 *     classified COMMIT / ROLLBACK under the nesting (Postgres) rule.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/cap/db.h"
#include "hull/cap/db_sql_kw.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SQL 4096

/* db_common.c's DuckDB path checks expand "~" through hl_host_home
 * (shared/host.c, not linked here): no home, so a "~" path is taken as
 * written - the scanners refuse it either way. */
const char *hl_host_home(void);
const char *hl_host_home(void) { return NULL; }

/* Generated line comments may end at CR (Postgres ends one there), and a
 * '"' may sit inside a literal or comment: the two defects audit 28 found
 * and fixed in the readers. */
#define CR_TERM "\r"
#define QBAN    ""

static void fail(const char *what, const char *sql)
{
    fprintf(stderr, "fuzz_sql_kw: %s\n  sql: \"", what);
    for (const char *p = sql; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') fputc(c, stderr);
        else fprintf(stderr, "\\x%02x", c);
    }
    fprintf(stderr, "\"\n");
    abort();
}

static char *cat(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *s = malloc(la + lb + 1);
    if (!s) abort();
    memcpy(s, a, la);
    memcpy(s + la, b, lb + 1);
    return s;
}

static char *recase(const char *s, int upper)
{
    size_t n = strlen(s);
    char *r = malloc(n + 1);
    if (!r) abort();
    for (size_t i = 0; i <= n; i++) {
        char c = s[i];
        if (upper && c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (!upper && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        r[i] = c;
    }
    return r;
}

/* Does @p s end inside a block comment, read the way hl_sql_skip_space_ex
 * reads comments (with the same nesting rule)? Only meaningful for a text the
 * reader classified COMMIT / ROLLBACK: such a text is words, ';' and gaps, so
 * no quote can hide a comment opener. */
static int ends_in_block_comment(const char *p, int nest)
{
    int depth = 0;
    while (*p) {
        if (depth == 0) {
            if (p[0] == '-' && p[1] == '-') {
                while (*p && *p != '\n' && *p != '\r') p++;   /* CR ends it too */
                continue;
            }
            if (p[0] == '/' && p[1] == '*') { depth = 1; p += 2; continue; }
            p++;
        } else {
            if (nest && p[0] == '/' && p[1] == '*') { depth++; p += 2; }
            else if (p[0] == '*' && p[1] == '/')    { depth--; p += 2; }
            else p++;
        }
    }
    return depth > 0;
}

static int is_end_kind(HlSqlTxnKind k)
{
    return k == HL_SQL_TXN_COMMIT || k == HL_SQL_TXN_ROLLBACK;
}

static void raw_mode(const char *sql)
{
    static const char *const ws[] = { " ", "\t", "\n", "\r\n", "\f", "\v" };
    char *up = recase(sql, 1), *lo = recase(sql, 0);

    for (int nest = 0; nest <= 1; nest++) {
        HlSqlTxnKind k = hl_sql_txn_kind_ex(sql, nest);
        if (hl_sql_txn_kind_ex(sql, nest) != k) fail("txn kind not deterministic", sql);
        for (size_t i = 0; i < sizeof ws / sizeof ws[0]; i++) {
            char *a = cat(ws[i], sql), *b = cat(sql, ws[i]);
            if (hl_sql_txn_kind_ex(a, nest) != k) fail("leading whitespace changed txn kind", sql);
            if (hl_sql_txn_kind_ex(b, nest) != k) fail("trailing whitespace changed txn kind", sql);
            free(a); free(b);
        }
        if (hl_sql_txn_kind_ex(up, nest) != k || hl_sql_txn_kind_ex(lo, nest) != k)
            fail("ASCII case changed txn kind", sql);
        if (is_end_kind(k) && !ends_in_block_comment(sql, nest)) {
            static const char *const tails[] = {
                "\n;DELETE FROM t", "\nDELETE FROM t", "\n; ;DELETE FROM t"
            };
            for (size_t i = 0; i < sizeof tails / sizeof tails[0]; i++) {
                char *t = cat(sql, tails[i]);
                if (is_end_kind(hl_sql_txn_kind_ex(t, nest)))
                    fail("COMMIT/ROLLBACK with a second statement appended still "
                         "reads as COMMIT/ROLLBACK", t);
                free(t);
            }
        }
    }
    if (!strstr(sql, "/*") &&
        hl_sql_txn_kind_ex(sql, 0) != hl_sql_txn_kind_ex(sql, 1))
        fail("comment rules disagree on a text with no block comment", sql);

    int dyn = hl_cap_db_sql_calls_dynamic_sql(sql);
    if (hl_cap_db_sql_calls_dynamic_sql(up) != dyn ||
        hl_cap_db_sql_calls_dynamic_sql(lo) != dyn)
        fail("ASCII case changed the dynamic-SQL verdict", sql);

    int ns = hl_cap_db_check_namespace(sql);
    if (hl_cap_db_check_namespace(up) != ns || hl_cap_db_check_namespace(lo) != ns)
        fail("ASCII case changed the namespace verdict", sql);
    if (strstr(lo, "_hull_") && ns == HL_DB_OK)
        fail("text naming _hull_ passed the namespace check", sql);

    free(up); free(lo);
}

/* ── CONSTRUCT mode ─────────────────────────────────────────────────── */

typedef struct {
    char   buf[MAX_SQL * 2];
    size_t len;
    int    overflow;
} Sb;

static void sb_put(Sb *b, const char *s, size_t n)
{
    if (b->len + n + 1 > sizeof b->buf) { b->overflow = 1; return; }
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
}
static void sb_puts(Sb *b, const char *s) { sb_put(b, s, strlen(s)); }

typedef struct { const uint8_t *p; size_t n; } Rd;
static int rd_byte(Rd *r) { if (!r->n) return -1; r->n--; return *r->p++; }

/* Token kinds as the ground truth sees them. */
enum { T_GAP, T_LPAREN, T_DYNNAME, T_KEYWORD, T_OTHER };

static const char *const DYN_NAMES[] = {
    "query", "QUERY", "Query", "query_table", "json_execute_serialized_sql"
};
static const char *const IDENTS[] = {
    "select", "from", "t", "x", "queryx", "my_query", "table", "where", "delete"
};

/* Read up to @p max content bytes, skipping NUL and any byte in @p ban. */
static size_t rd_content(Rd *r, char *out, size_t max, const char *ban)
{
    out[0] = '\0';
    int n = rd_byte(r);
    if (n < 0) return 0;
    size_t want = (size_t)n % (max + 1), k = 0;
    while (k < want) {
        int c = rd_byte(r);
        if (c < 0) break;
        if (c == 0 || strchr(ban, c)) continue;
        out[k++] = (char)c;
    }
    out[k] = '\0';
    return k;
}

/* Emit one token; return its truth kind. */
static int emit_token(Sb *b, Rd *r)
{
    char content[64];
    int op = rd_byte(r);
    if (op < 0) return -1;
    switch (op % 10) {
    case 0:   /* string literal: '' doubles a quote */
        rd_content(r, content, 40, QBAN);
        sb_puts(b, "'");
        for (char *c = content; *c; c++) sb_put(b, c, 1), (*c == '\'' ? sb_put(b, "'", 1) : (void)0);
        sb_puts(b, "'");
        return T_OTHER;
    case 1: { /* quoted identifier: "" doubles a quote; maybe a dynamic name */
        int which = rd_byte(r);
        if (which >= 0 && which % 3 == 0) {
            sb_puts(b, "\"");
            sb_puts(b, DYN_NAMES[(size_t)which % (sizeof DYN_NAMES / sizeof DYN_NAMES[0])]);
            sb_puts(b, "\"");
            return T_DYNNAME;
        }
        rd_content(r, content, 40, "");
        sb_puts(b, "\"");
        for (char *c = content; *c; c++) sb_put(b, c, 1), (*c == '"' ? sb_put(b, "\"", 1) : (void)0);
        sb_puts(b, "\"");
        return T_OTHER;
    }
    case 2: { /* line comment ended by LF or CR (Postgres ends it at either) */
        int term = rd_byte(r);
        rd_content(r, content, 40, "\n\r" QBAN);
        sb_puts(b, "--");
        sb_puts(b, content);
        sb_puts(b, (term >= 0 && (term & 1)) ? CR_TERM : "\n");
        return T_GAP;
    }
    case 3:   /* block comment, no nested opener / closer in the content */
        rd_content(r, content, 40, "*/" QBAN);
        sb_puts(b, "/* ");
        sb_puts(b, content);
        sb_puts(b, " */");
        return T_GAP;
    case 4: { /* whitespace */
        static const char *const w[] = { " ", "\t", "\n", "\r", "  \n " };
        int i = rd_byte(r);
        sb_puts(b, w[(size_t)(i < 0 ? 0 : i) % 5]);
        return T_GAP;
    }
    case 5:
        sb_puts(b, "(");
        return T_LPAREN;
    case 6: {
        static const char *const p[] = { ")", ",", ";", "||", "*", "=" };
        int i = rd_byte(r);
        sb_puts(b, p[(size_t)(i < 0 ? 0 : i) % 6]);
        return T_OTHER;
    }
    case 7: {
        int i = rd_byte(r);
        sb_puts(b, DYN_NAMES[(size_t)(i < 0 ? 0 : i) % (sizeof DYN_NAMES / sizeof DYN_NAMES[0])]);
        return T_DYNNAME;
    }
    default: {
        int i = rd_byte(r);
        sb_puts(b, IDENTS[(size_t)(i < 0 ? 0 : i) % (sizeof IDENTS / sizeof IDENTS[0])]);
        return T_KEYWORD;
    }
    }
}

static void construct_dynamic(Rd *r)
{
    Sb b = { .len = 0 };
    b.buf[0] = '\0';
    int truth = 0, pending = 0;   /* pending: a dynamic name awaits "(" */
    for (int i = 0; i < 64; i++) {
        int k = emit_token(&b, r);
        if (k < 0) break;
        if (k == T_LPAREN && pending) truth = 1;
        if (k != T_GAP) pending = (k == T_DYNNAME);
        /* A gap between every token keeps neighbours from fusing into one
         * token (an identifier running into the next, "--" from two "-"). */
        sb_puts(&b, " ");
    }
    if (b.overflow) return;
    if (truth && !hl_cap_db_sql_calls_dynamic_sql(b.buf))
        fail("dynamic-SQL call (name, gaps, '(') under the Postgres lexer "
             "not reported by hl_cap_db_sql_calls_dynamic_sql", b.buf);
}

static void construct_txn(Rd *r)
{
    Sb b = { .len = 0 };
    b.buf[0] = '\0';
    int first = rd_byte(r);
    sb_puts(&b, (first >= 0 && (first & 1)) ? "ROLLBACK" : "COMMIT");
    /* Gaps only (comments, whitespace) between the statement and the next. */
    for (int i = 0; i < 16; i++) {
        int op = rd_byte(r);
        if (op < 0 || op % 4 == 3) break;
        char content[48];
        if (op % 4 == 0) {
            rd_content(r, content, 40, "\n\r");
            sb_puts(&b, "--");
            sb_puts(&b, content);
            sb_puts(&b, (op & 4) ? CR_TERM : "\n");
        } else if (op % 4 == 1) {
            rd_content(r, content, 40, "*/");
            sb_puts(&b, "/* ");
            sb_puts(&b, content);
            sb_puts(&b, " */");
        } else {
            sb_puts(&b, (op & 4) ? ";" : " ");
        }
    }
    sb_puts(&b, " ;DELETE FROM t");
    if (b.overflow) return;
    if (is_end_kind(hl_sql_txn_kind_ex(b.buf, 1)))
        fail("a COMMIT/ROLLBACK followed by another statement (Postgres lexer) "
             "classified as COMMIT/ROLLBACK: the statement would be answered "
             "\"done\" unrun", b.buf);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > MAX_SQL) return 0;
    if (data[0] < 0x80) {
        char *sql = malloc(size);
        if (!sql) return 0;
        memcpy(sql, data + 1, size - 1);
        sql[size - 1] = '\0';
        raw_mode(sql);
        free(sql);
    } else {
        Rd r = { data + 1, size - 1 };
        if (data[0] & 1) construct_txn(&r);
        else             construct_dynamic(&r);
    }
    return 0;
}
