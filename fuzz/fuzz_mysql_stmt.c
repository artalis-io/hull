/*
 * fuzz_mysql_stmt.c: libFuzzer harness for the MySQL backend's statement
 * classifiers (cap/db_mysql.c, audits 9 / 10 / 12):
 *
 *   - my_sql_single_statement: whether a text is ONE statement. The
 *     connection runs with CLIENT_MULTI_STATEMENTS and every classifier reads
 *     the first statement only, so a text it calls single must be single.
 *   - sql_is_rollback: the text the backend answers "done" WITHOUT sending
 *     when the server already rolled the transaction back (a deadlock, a lost
 *     connection); anything after the ROLLBACK would be reported done unrun.
 *   - my_sql_txn_kind: the reader with MySQL's comment rules.
 *
 * The functions are static, so the backend TU is included; the wire client it
 * calls is stubbed in fuzz_mysql_stmt_stubs.c (never reached), which keeps
 * cap/crypto and Keel's TLS out of the link.
 *
 * RAW mode (first byte < 0x80): metamorphic oracles - a single statement
 * stays single with trailing whitespace / ';', and stops being single once a
 * second statement is appended after a newline; a ROLLBACK the backend would
 * answer unsent stops being one likewise; ASCII case changes nothing.
 *
 * CONSTRUCT mode (first byte >= 0x80): a token program renders a text whose
 * statement count under MySQL's lexer is known (words, '..' / ".." / `..`
 * quotes with doubled quotes, "-- " and block comments, ';'). Two or more
 * statements must never be called single. A backslash inside a '..' / ".."
 * literal (whose meaning follows NO_BACKSLASH_ESCAPES) must make the text
 * unreadable (not single), as documented.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_ENABLE_MYSQL
#define HL_ENABLE_MYSQL 1
#endif
#include "../src/hull/cap/db_mysql.c"

#include <stddef.h>
#include <stdint.h>

static void fail(const char *what, const char *sql)
{
    fprintf(stderr, "fuzz_mysql_stmt: %s\n  sql: \"", what);
    for (const char *p = sql; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') fputc(c, stderr);
        else fprintf(stderr, "\\x%02x", c);
    }
    fprintf(stderr, "\"\n");
    abort();
}

static char *fz_cat(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    char *s = malloc(la + lb + 1);
    if (!s) abort();
    memcpy(s, a, la);
    memcpy(s + la, b, lb + 1);
    return s;
}

static char *fz_recase(const char *s, int upper)
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

static void raw_mode(const char *sql)
{
    int single = my_sql_single_statement(sql);
    int rb = sql_is_rollback(sql);
    HlSqlTxnKind k = my_sql_txn_kind(sql);

    char *up = fz_recase(sql, 1), *lo = fz_recase(sql, 0);
    /* MariaDB's executable comment is spelled with a capital M only, so
     * recasing a slash-star-m-bang legitimately changes the verdict. */
    if (!strstr(lo, "/*m!")) {
    if (my_sql_single_statement(up) != single || my_sql_single_statement(lo) != single)
        fail("ASCII case changed my_sql_single_statement", sql);
    if (sql_is_rollback(up) != rb || sql_is_rollback(lo) != rb)
        fail("ASCII case changed sql_is_rollback", sql);
    if (my_sql_txn_kind(up) != k || my_sql_txn_kind(lo) != k)
        fail("ASCII case changed my_sql_txn_kind", sql);
    }
    free(up); free(lo);

    if (rb && (!single || k != HL_SQL_TXN_ROLLBACK))
        fail("sql_is_rollback true for a text that is not one ROLLBACK", sql);

    if (single) {
        static const char *const keep[] = { " ", "\n", ";", " ; ", "\t;\n" };
        for (size_t i = 0; i < sizeof keep / sizeof keep[0]; i++) {
            char *t = fz_cat(sql, keep[i]);
            if (!my_sql_single_statement(t))
                fail("single statement stopped being single with a trailing "
                     "separator", t);
            free(t);
        }
    }
    static const char *const tails[] = {
        "\n;SELECT 2", "\n; DELETE FROM t", "\n;;SELECT 2"
    };
    for (size_t i = 0; i < sizeof tails / sizeof tails[0]; i++) {
        char *t = fz_cat(sql, tails[i]);
        if (my_sql_single_statement(t) && single)
            fail("a text with a second statement appended still reads as one "
                 "statement", t);
        if (sql_is_rollback(t))
            fail("ROLLBACK + a second statement would be answered \"done\" "
                 "unsent", t);
        free(t);
    }
}

/* ── CONSTRUCT mode ─────────────────────────────────────────────────── */

typedef struct { const uint8_t *p; size_t n; } Rd;
static int rd_byte(Rd *r) { if (!r->n) return -1; r->n--; return *r->p++; }

typedef struct {
    char   buf[8192];
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

/* Content bytes (no NUL; nothing in @p ban); returns 1 if a backslash went in. */
static int rd_content(Rd *r, char *out, size_t max, const char *ban)
{
    out[0] = '\0';
    int n = rd_byte(r);
    if (n < 0) return 0;
    size_t want = (size_t)n % (max + 1), k = 0;
    int bs = 0;
    while (k < want) {
        int c = rd_byte(r);
        if (c < 0) break;
        if (c == 0 || strchr(ban, c)) continue;
        if (c == '\\') bs = 1;
        out[k++] = (char)c;
    }
    out[k] = '\0';
    return bs;
}

static void construct(Rd *r)
{
    static const char *const words[] = {
        "SELECT", "1", "x", "FROM", "t", "ROLLBACK", "COMMIT", "DELETE", "-", "*", "("
    };
    Sb b = { .len = 0 };
    b.buf[0] = '\0';
    int stmts = 0, cur_has_code = 0, backslash = 0;
    for (int i = 0; i < 48; i++) {
        int op = rd_byte(r);
        if (op < 0) break;
        char content[48];
        switch (op % 8) {
        case 0: case 1: {
            int w = rd_byte(r);
            sb_puts(&b, words[(size_t)(w < 0 ? 0 : w) % (sizeof words / sizeof words[0])]);
            cur_has_code = 1;
            break;
        }
        case 2: case 3: {   /* quoted: ' " ` with the quote doubled inside */
            static const char q[] = { '\'', '"', '`' };
            char qc = q[(op / 8) % 3];
            int bs = rd_content(r, content, 40, "#");
            if (qc != '`') backslash |= bs;
            sb_put(&b, &qc, 1);
            for (char *c = content; *c; c++) {
                sb_put(&b, c, 1);
                if (*c == qc) sb_put(&b, &qc, 1);
            }
            sb_put(&b, &qc, 1);
            cur_has_code = 1;
            break;
        }
        case 4:   /* "-- " comment to LF ('#' would make it unreadable) */
            rd_content(r, content, 40, "\n#");
            sb_puts(&b, "-- ");
            sb_puts(&b, content);
            sb_puts(&b, "\n");
            break;
        case 5:   /* block comment; no "*" or "/" inside, no executable form */
            rd_content(r, content, 40, "*/#!");
            sb_puts(&b, "/* ");
            sb_puts(&b, content);
            sb_puts(&b, " */");
            break;
        case 6:
            sb_puts(&b, ";");
            if (cur_has_code) stmts++;
            cur_has_code = 0;
            break;
        default:
            sb_puts(&b, (op & 8) ? "\n" : " ");
            break;
        }
        sb_puts(&b, " ");   /* keep neighbours from fusing ("-" "-" -> "--") */
    }
    if (cur_has_code) stmts++;
    if (b.overflow) return;
    int single = my_sql_single_statement(b.buf);
    if (stmts >= 2 && single)
        fail("two or more statements (MySQL lexer) read as one statement", b.buf);
    if (backslash && single)
        fail("a backslash in a '..' / \"..\" literal did not make the text "
             "unreadable", b.buf);
    if (stmts >= 2 && sql_is_rollback(b.buf))
        fail("multi-statement text classified as a lone ROLLBACK", b.buf);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    log_set_quiet(true);   /* the backend TU logs; keep the run's stderr small */
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > 4096) return 0;
    if (data[0] < 0x80) {
        char *sql = malloc(size);
        if (!sql) return 0;
        memcpy(sql, data + 1, size - 1);
        sql[size - 1] = '\0';
        raw_mode(sql);
        free(sql);
    } else {
        Rd r = { data + 1, size - 1 };
        construct(&r);
    }
    return 0;
}
