/*
 * cap/db_common.c - backend-agnostic database helpers
 *
 * Shared db helpers with no backend or SQL-engine dependency, so they compile
 * in every DB flavor (including Postgres-only). Currently the _hull_* namespace
 * guard, previously wedged into the DSN selector (db_select.c); a natural home
 * for future dialect-neutral helpers.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_DB

#include "hull/cap/db.h"
#include "hull/cap/db_budget.h"
#include "hull/cap/db_backend.h"
#include "hull/cap/db_sql_kw.h"
#include <limits.h>   /* PATH_MAX */
#include <stdatomic.h>
#include <stdint.h>   /* SIZE_MAX */
#include <stdlib.h>   /* realpath */
#include <string.h>
#include <strings.h>
#include <time.h>

/* ── Transaction generation (cap/db_backend.h, audit 36) ───────────── */

static _Thread_local uint64_t tl_txn_gen;

uint64_t hl_db_txn_gen(void)
{
    return tl_txn_gen;
}

/* 1 when @p sql holds a statement after its first one (text past a ';'
 * outside quotes and comments). Read conservatively: a backslash escape is
 * not understood, so a ';' inside such a literal can count - which only
 * changes the generation once more than needed. */
static int txn_sql_multi(const char *sql)
{
    char q = 0;
    for (const char *p = sql; *p; p++) {
        if (q) {
            if (*p == q) q = 0;
            continue;
        }
        if (*p == '\'' || *p == '"' || *p == '`') {
            q = *p;
        } else if (p[0] == '-' && p[1] == '-') {
            while (*p && *p != '\n' && *p != '\r') p++;
            if (!*p) return 0;
        } else if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            if (!e) return 0;
            p = e + 1;
        } else if (*p == ';') {
            const char *r = hl_sql_skip_space_ex(p + 1, 0);
            if (*r == ';') { p = r - 1; continue; }
            return *r != '\0';
        }
    }
    return 0;
}

void hl_db_txn_note(HlDbHandle *h, HlDbTxnSnap before, const char *sql)
{
    int now = hl_db_in_txn(h);
    /* Session state that counts as a transaction for the wait gates
     * (audit 40) starting or ending is a change too: a coroutine that
     * yielded after `SET autocommit = 0` must not be resumed in a session
     * reset and set again since. */
    int changed = before.in_txn != now ||
                  before.held != hl_db_session_held(h);
    if (!changed && before.in_txn) {
        /* Still inside one: was it still the same transaction? A statement
         * that ends one and opens the next shows no change. */
        uint64_t seq = hl_db_txn_seq(h);
        if (before.seq && seq) {
            /* The backend counts transaction ends: no guessing from the
             * text, so a multi-statement INSERT inside a db.batch keeps the
             * generation (audit 37). */
            changed = seq != before.seq;
        } else if (!sql) {
            changed = 1;   /* begin / commit / rollback / the guard */
        } else {
            /* Either comment rule (Postgres nests block comments, SQLite
             * and MySQL do not): a transaction word under either counts. */
            HlSqlTxnKind k0 = hl_sql_txn_kind_ex(sql, 0);
            HlSqlTxnKind k1 = hl_sql_txn_kind_ex(sql, 1);
            changed = (k0 != HL_SQL_TXN_NONE && k0 != HL_SQL_TXN_SAVEPOINT) ||
                      (k1 != HL_SQL_TXN_NONE && k1 != HL_SQL_TXN_SAVEPOINT) ||
                      txn_sql_multi(sql);
        }
    }
    if (changed) tl_txn_gen++;
}

/* ── The calling run's budget (cap/db_budget.h) ────────────────────── */

static _Thread_local HlDbBudgetFn tl_budget_fn;
static _Thread_local void        *tl_budget_ud;

HlDbBudgetBinding hl_db_budget_swap(HlDbBudgetFn fn, void *ud)
{
    HlDbBudgetBinding prev = { tl_budget_fn, tl_budget_ud };
    tl_budget_fn = fn;
    tl_budget_ud = fn ? ud : NULL;
    return prev;
}

HlDbBudgetBinding hl_db_budget_current(void)
{
    HlDbBudgetBinding cur = { tl_budget_fn, tl_budget_ud };
    return cur;
}

void hl_db_budget_restore(HlDbBudgetBinding prev)
{
    tl_budget_fn = prev.fn;
    tl_budget_ud = prev.ud;
}

void hl_db_budget_unbind(const void *ud)
{
    if (tl_budget_fn && tl_budget_ud == ud) {
        tl_budget_fn = NULL;
        tl_budget_ud = NULL;
    }
}

static atomic_int g_op_shutdown;

int hl_db_budget_charge(int64_t units)
{
    if (!tl_budget_fn) return 0;
    int stop = tl_budget_fn(tl_budget_ud, units);
    /* The pool is being freed: a worker.dispatch job's SQL charges its VM's
     * budget, not an HlDbOpBudget, and a wait charges no units - so a
     * pg_sleep in a dispatch held the join until the job's deadline, for
     * ever with --max-run-ms 0 (audit 28). Any bound run's SQL stops (the
     * binding was still asked, so an HlDbOpBudget records its reason). */
    return stop || atomic_load_explicit(&g_op_shutdown, memory_order_acquire);
}

void hl_db_op_shutdown(int on)
{
    atomic_store(&g_op_shutdown, on ? 1 : 0);
}

uint64_t hl_db_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 1;
    uint64_t ms = (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
    return ms ? ms : 1;
}

static int op_budget_trip(HlDbOpBudget *b, int reason)
{
    b->tripped = 1;
    b->reason = reason;
    return 1;
}

int hl_db_op_budget_charge(void *ud, int64_t units)
{
    HlDbOpBudget *b = (HlDbOpBudget *)ud;
    if (b->tripped) return 1;
    /* Stops that hold for a 0-unit charge too (audit 27). */
    if ((b->cancelled && __atomic_load_n(b->cancelled, __ATOMIC_ACQUIRE)) ||
        atomic_load_explicit(&g_op_shutdown, memory_order_acquire))
        return op_budget_trip(b, HL_DB_OP_STOP_CANCEL);
    if (b->deadline_ms && hl_db_monotonic_ms() >= b->deadline_ms)
        return op_budget_trip(b, HL_DB_OP_STOP_DEADLINE);
    if (b->limit <= 0) return 0;
    b->used = units > INT64_MAX - b->used ? INT64_MAX : b->used + units;
    if (b->used > b->limit) return op_budget_trip(b, HL_DB_OP_STOP_UNITS);
    return 0;
}

static _Atomic size_t g_max_value_bytes;

void hl_db_note_heap_limit(size_t bytes)
{
    size_t cur = atomic_load(&g_max_value_bytes);
    while (bytes > cur &&
           !atomic_compare_exchange_weak(&g_max_value_bytes, &cur, bytes))
        ;
}

size_t hl_db_max_value_bytes(void)
{
    return atomic_load(&g_max_value_bytes);
}

/* ── Namespace protection ──────────────────────────────────────────── */

/* Backend-agnostic string check (no SQL execution): reject any SQL touching a
 * reserved `_hull_*` table. User code is gated by a call-stack check at each
 * binding site; stdlib bypasses via its `hull.` / `hull:` source prefix. */
int hl_cap_db_check_namespace(const char *sql)
{
    if (!sql)
        return HL_DB_ERR_DENIED;
    for (const char *p = sql; *p; p++) {
        if ((*p == '_' || *p == 'H' || *p == 'h') &&
            strncasecmp(p, "_hull_", 6) == 0)
            return HL_DB_ERR_DENIED;
        /* A Postgres Unicode-escape identifier, U&"\005fhull_sessions",
         * spells the name with escapes this text check cannot see: the
         * server decodes it to _hull_sessions. App SQL may not use one. The
         * U&'...' string literal stays legal - it cannot name a table (only
         * dynamic SQL could make one, the documented limit). */
        if ((*p == 'U' || *p == 'u') && p[1] == '&' && p[2] == '"')
            return HL_DB_ERR_DENIED;
    }
    return HL_DB_OK;
}

/* Per-thread "this SQL is the stdlib's / Hull's own" flag the SQLite
 * authorizer reads (audit 39; see the header). Untrusted by default. */
static _Thread_local int g_db_ns_trusted;

int hl_cap_db_ns_trust(int trusted)
{
    int prev = g_db_ns_trusted;
    /* Only HL_DB_NS_TRUSTED (1) trusts; HL_DB_NS_STDLIB_SCOPED (2) is a
     * stdlib caller whose SQL touches no `_hull_*` table the authorizer
     * guards (hull.search, audit 40) and so runs untrusted. */
    g_db_ns_trusted = (trusted == 1);
    return prev;
}

void hl_cap_db_ns_restore(int prev)
{
    g_db_ns_trusted = prev ? 1 : 0;
}

int hl_cap_db_ns_trusted(void)
{
    return g_db_ns_trusted;
}

int hl_cap_db_name_reserved(const char *name)
{
    return name && strncasecmp(name, "_hull_", 6) == 0;
}

/* ── Stored virtual tables (audit 40) ──────────────────────────────── */

/* A tiny reader for the CREATE VIRTUAL TABLE text SQLite keeps in
 * sqlite_schema, tokenized the way SQLite's tokenizer reads it: whitespace
 * and both comment forms are gaps (a block comment does not nest and an
 * unterminated one runs to the end), identifiers are bare or "..." / `...` /
 * [...] quoted, strings '...' with a doubled quote for a quote. */
typedef struct {
    int         kind;   /* VT_END, VT_IDENT, VT_STRING, VT_PUNCT */
    const char *s;      /* the token as written (quotes included) */
    size_t      n;
} HlVtTok;

enum { VT_END, VT_IDENT, VT_STRING, VT_PUNCT, VT_BAD };

static const char *vt_skip_gap(const char *p)
{
    for (;;) {
        while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
            p++;
        if (p[0] == '-' && p[1] == '-') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            p = e ? e + 2 : p + strlen(p);
            continue;
        }
        return p;
    }
}

static int vt_bare_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c >= 0x80;
}

static const char *vt_next(const char *p, HlVtTok *t)
{
    p = vt_skip_gap(p);
    t->s = p;
    t->n = 0;
    if (!*p) { t->kind = VT_END; return p; }
    char close = 0;
    if (*p == '"' || *p == '`' || *p == '\'') close = *p;
    else if (*p == '[') close = ']';
    if (close) {
        const char *q = p + 1;
        for (;;) {
            if (!*q) { t->kind = VT_BAD; return q; }
            if (*q == close) {
                if (close != ']' && q[1] == close) { q += 2; continue; }
                break;
            }
            q++;
        }
        t->kind = (*p == '\'') ? VT_STRING : VT_IDENT;
        t->n = (size_t)(q + 1 - p);
        return q + 1;
    }
    if (vt_bare_char((unsigned char)*p)) {
        const char *q = p;
        while (vt_bare_char((unsigned char)*q)) q++;
        t->kind = VT_IDENT;
        t->n = (size_t)(q - p);
        return q;
    }
    t->kind = VT_PUNCT;
    t->n = 1;
    return p + 1;
}

/* The token's value with its quotes removed (a doubled quote read as one)
 * into @p out; -1 when it does not fit. */
static int vt_value(const HlVtTok *t, char *out, size_t cap)
{
    const char *s = t->s;
    size_t n = t->n, o = 0;
    if (t->kind != VT_IDENT && t->kind != VT_STRING) return -1;
    char q = 0;
    if (n >= 2 && (*s == '"' || *s == '`' || *s == '\'' || *s == '[')) {
        q = (*s == '[') ? ']' : *s;
        s++;
        n -= 2;
    }
    for (size_t i = 0; i < n; i++) {
        if (q && q != ']' && s[i] == q) i++;   /* doubled quote */
        if (o + 1 >= cap) return -1;
        out[o++] = s[i];
    }
    out[o] = '\0';
    return 0;
}

static int vt_kw(const HlVtTok *t, const char *kw)
{
    return t->kind == VT_IDENT && t->n == strlen(kw) &&
           strncasecmp(t->s, kw, t->n) == 0;
}

static int vt_plain_ident(const char *s)
{
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_'))
        return 0;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '_'))
            return 0;
    return 1;
}

/* hull.search's tokenize grammar: an identifier, then single-space separated
 * identifiers or integers. */
static int vt_tokenize_ok(const char *s)
{
    int first = 1;
    for (;;) {
        const char *w = s;
        if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')) {
            while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                   (*s >= '0' && *s <= '9') || *s == '_')
                s++;
        } else if (!first && *s >= '0' && *s <= '9') {
            while (*s >= '0' && *s <= '9') s++;
        }
        if (s == w) return 0;
        first = 0;
        if (!*s) return 1;
        if (*s != ' ') return 0;
        s++;
    }
}

int hl_cap_db_sql_is_virtual_table(const char *sql)
{
    HlVtTok t;
    if (!sql) return 0;
    const char *p = vt_next(sql, &t);
    if (!vt_kw(&t, "create")) return 0;
    (void)vt_next(p, &t);
    return vt_kw(&t, "virtual");
}

/* Whether the arguments of an `fts5(...)` definition named @p name are the
 * shape hull.search's create_index writes: `id`, one or more columns, then
 * any of tokenize / content / content_rowid once each, content naming an app
 * table (never a `_hull_*` one). @p p points just past the `(`. */
static int vt_search_args_ok(const char *p)
{
    char v[256], key[64];
    int ncols = 0, seen_tok = 0, seen_content = 0, seen_rowid = 0, opts = 0;
    for (;;) {
        HlVtTok a, b, c;
        p = vt_next(p, &a);
        if (a.kind != VT_IDENT || vt_value(&a, key, sizeof key) != 0)
            return 0;
        p = vt_next(p, &b);
        if (b.kind == VT_PUNCT && *b.s == '=') {
            p = vt_next(p, &c);
            if (vt_value(&c, v, sizeof v) != 0) return 0;
            if (strcasecmp(key, "tokenize") == 0 && !seen_tok) {
                seen_tok = 1;
                if (!vt_tokenize_ok(v)) return 0;
            } else if (strcasecmp(key, "content") == 0 && !seen_content) {
                seen_content = 1;
                if (!vt_plain_ident(v) || hl_cap_db_name_reserved(v))
                    return 0;
            } else if (strcasecmp(key, "content_rowid") == 0 && !seen_rowid) {
                seen_rowid = 1;
                if (!vt_plain_ident(v) || hl_cap_db_name_reserved(v))
                    return 0;
            } else {
                return 0;
            }
            opts = 1;
            p = vt_next(p, &b);
        } else {
            /* a column: before every option, plain, `id` first */
            if (opts || !vt_plain_ident(key) || hl_cap_db_name_reserved(key))
                return 0;
            if (ncols == 0 && strcasecmp(key, "id") != 0) return 0;
            ncols++;
        }
        if (b.kind != VT_PUNCT) return 0;
        if (*b.s == ')') break;
        if (*b.s != ',') return 0;
    }
    if (ncols < 2) return 0;
    HlVtTok e;
    (void)vt_next(p, &e);
    return e.kind == VT_END;
}

int hl_cap_db_vtab_definition_ok(const char *name, const char *sql)
{
    if (!name || !sql) return 0;
    if (!hl_cap_db_name_reserved(name))
        /* An app's own virtual table: it may not name a `_hull_*` object
         * anywhere (an fts5 content= / content_rowid=, an fts5vocab over a
         * search index, any other module's argument) - read like app SQL. */
        return hl_cap_db_check_namespace(sql) == 0;

    /* A `_hull_*` virtual table can only be one of hull.search's indexes. */
    if (strncasecmp(name, "_hull_fts_", 10) != 0 || !vt_plain_ident(name + 10))
        return 0;
    HlVtTok t;
    char v[256];
    const char *p = vt_next(sql, &t);
    if (!vt_kw(&t, "create")) return 0;
    p = vt_next(p, &t);
    if (!vt_kw(&t, "virtual")) return 0;
    p = vt_next(p, &t);
    if (!vt_kw(&t, "table")) return 0;
    p = vt_next(p, &t);
    if (vt_kw(&t, "if")) {
        p = vt_next(p, &t);
        if (!vt_kw(&t, "not")) return 0;
        p = vt_next(p, &t);
        if (!vt_kw(&t, "exists")) return 0;
        p = vt_next(p, &t);
    }
    if (vt_value(&t, v, sizeof v) != 0) return 0;
    HlVtTok d;
    const char *q = vt_next(p, &d);
    if (d.kind == VT_PUNCT && *d.s == '.') {   /* schema.name */
        if (strcasecmp(v, "main") != 0) return 0;
        p = vt_next(q, &t);
        if (vt_value(&t, v, sizeof v) != 0) return 0;
    }
    if (strcasecmp(v, name) != 0) return 0;
    p = vt_next(p, &t);
    if (!vt_kw(&t, "using")) return 0;
    p = vt_next(p, &t);
    if (!vt_kw(&t, "fts5")) return 0;
    p = vt_next(p, &t);
    if (t.kind != VT_PUNCT || *t.s != '(') return 0;
    return vt_search_args_ok(p);
}

/* ── DuckDB dynamic-SQL table functions (audit 25) ─────────────────── */

static int dyn_ident_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c >= 0x80;
}

static int dyn_name_is_dynamic(const char *s, size_t n)
{
    static const char *const names[] = {
        "query", "query_table", "json_execute_serialized_sql", NULL
    };
    for (int i = 0; names[i]; i++)
        if (strlen(names[i]) == n && strncasecmp(s, names[i], n) == 0)
            return 1;
    return 0;
}

/* Skip whitespace and comments from @p p; @p nest = whether a block comment
 * nests (DuckDB's parser descends from Postgres's, but both readings are
 * tried so neither hides a call). Returns the first other character, or NULL
 * once @p budget runs out. A skip runs to the end of an unterminated comment,
 * and every dynamic name re-runs it, so `query--query--...` was quadratic
 * (audit 30 re-audit): the skips of one scan share one byte budget. */
static const char *dyn_skip_gap(const char *p, int nest, size_t *budget)
{
    const char *start = p;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == '\f' || *p == '\v')
            p++;
        if (p[0] == '-' && p[1] == '-') {
            while (*p && *p != '\n' && *p != '\r') p++;   /* CR ends it too */
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            int depth = 1;
            p += 2;
            while (*p && depth > 0) {
                if (p[0] == '*' && p[1] == '/') { depth--; p += 2; }
                else if (nest && p[0] == '/' && p[1] == '*') { depth++; p += 2; }
                else p++;
            }
            continue;
        }
        break;
    }
    size_t used = (size_t)(p - start);
    if (used > *budget) return NULL;
    *budget -= used;
    return p;
}

/* A skip past the budget reads as a call: the statement is refused. */
static int dyn_followed_by_call(const char *p, size_t *budget)
{
    const char *a = dyn_skip_gap(p, 1, budget);
    if (!a || *a == '(') return 1;
    const char *b = dyn_skip_gap(p, 0, budget);
    return !b || *b == '(';
}

int hl_cap_db_sql_calls_dynamic_sql(const char *sql)
{
    if (!sql) return 0;
    /* Gap skips after dynamic names, in total: real SQL uses a sliver. */
    size_t budget = 2 * strlen(sql) + 65536;
    for (const char *p = sql; *p; ) {
        unsigned char c = (unsigned char)*p;
        if ((c == 'U' || c == 'u') && p[1] == '&' && p[2] == '"')
            return 1;                 /* a name spelled with escapes */
        if (c == '"') {               /* "quoted" identifier, "" = a quote */
            char buf[64];
            size_t n = 0;
            int fits = 1;
            const char *q = p + 1;
            for (; *q; q++) {
                if (*q == '"') {
                    if (q[1] == '"') { q++; } else break;
                }
                /* Stop once the name cannot fit: it cannot be a dynamic
                 * name then, and `after` is read only when it fits. Run to
                 * the closing quote from every '"' (see p++ below), a text
                 * of N quotes scanned O(N^2) - 1 MB held the loop for
                 * minutes before any budget charge (audit 30). */
                if (n < sizeof buf) buf[n++] = *q; else { fits = 0; break; }
            }
            const char *after = *q ? q + 1 : q;
            if (fits && dyn_name_is_dynamic(buf, n) && dyn_followed_by_call(after, &budget))
                return 1;
            /* Go on at the next byte, not past the name: string literals and
             * comments are not skipped, so a '"' inside one opened a "name"
             * that swallowed a real query(...) after it (audit 28). Every
             * byte is scanned for a bare name too. */
            p++;
            continue;
        }
        if (dyn_ident_char(c) && !(c >= '0' && c <= '9')) {
            const char *start = p;
            while (*p && dyn_ident_char((unsigned char)*p)) p++;
            if (dyn_name_is_dynamic(start, (size_t)(p - start)) &&
                dyn_followed_by_call(p, &budget))
                return 1;
            continue;
        }
        p++;
    }
    return 0;
}

/* ── DuckDB whole-database file statements (audit 32) ──────────────── */

/* DuckDB runs SQL it reads from a file, or copies every table, without the
 * statement naming one: IMPORT DATABASE 'dir' runs dir/schema.sql and
 * dir/load.sql (files an app with fs.write can author), EXPORT DATABASE
 * 'dir' writes every table - _hull_* ones included - to files read_csv can
 * read back, and COPY FROM DATABASE a TO b copies every table into an
 * attached file. IMPORT is a PRAGMA underneath (import_database), so the
 * pragma names count too, bare or "quoted". */
static int whole_db_name(const char *s, size_t n)
{
    static const char *const names[] = {
        "import_database", "export_database", "copy_database", NULL
    };
    for (int i = 0; names[i]; i++)
        if (strlen(names[i]) == n && strncasecmp(s, names[i], n) == 0)
            return 1;
    return 0;
}

/* Whether the next word after @p p (past whitespace and comments, read with
 * a nesting and a non-nesting block-comment rule) is @p kw. A skip past the
 * budget reads as a match: the statement is refused. Sets *@p after to the
 * text after the word on the reading that matched. */
static int whole_db_next_is(const char *p, const char *kw, size_t *budget,
                            const char **after)
{
    for (int nest = 1; nest >= 0; nest--) {
        const char *q = dyn_skip_gap(p, nest, budget);
        if (!q) { *after = NULL; return 1; }
        size_t n = 0;
        while (dyn_ident_char((unsigned char)q[n])) n++;
        if (n == strlen(kw) && strncasecmp(q, kw, n) == 0) {
            *after = q + n;
            return 1;
        }
    }
    return 0;
}

int hl_cap_db_sql_reads_whole_database(const char *sql)
{
    if (!sql) return 0;
    size_t budget = 2 * strlen(sql) + 65536;   /* shared by every gap skip */
    for (const char *p = sql; *p; ) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {               /* "quoted" pragma / function name */
            char buf[32];
            size_t n = 0;
            int fits = 1;
            for (const char *q = p + 1; *q; q++) {
                if (*q == '"') {
                    if (q[1] == '"') { q++; } else break;
                }
                if (n < sizeof buf) buf[n++] = *q; else { fits = 0; break; }
            }
            if (fits && whole_db_name(buf, n)) return 1;
            p++;                      /* literals are not skipped: see above */
            continue;
        }
        if (dyn_ident_char(c) && !(c >= '0' && c <= '9')) {
            const char *start = p;
            while (*p && dyn_ident_char((unsigned char)*p)) p++;
            size_t n = (size_t)(p - start);
            const char *after = NULL;
            if (whole_db_name(start, n)) return 1;
            if ((n == 6 && (strncasecmp(start, "import", 6) == 0 ||
                            strncasecmp(start, "export", 6) == 0)) &&
                whole_db_next_is(p, "database", &budget, &after))
                return 1;
            if (n == 4 && strncasecmp(start, "copy", 4) == 0 &&
                whole_db_next_is(p, "from", &budget, &after) &&
                (!after || whole_db_next_is(after, "database", &budget, &after)))
                return 1;
            continue;
        }
        p++;
    }
    return 0;
}

/* ── DuckDB catalog / schema switching (audit 33) ──────────────────── */

/* ATTACH / DETACH another database file, USE a database or schema, or SET /
 * RESET / PRAGMA of `schema` / `search_path` all change which catalog an
 * unqualified name resolves to. A stored view or macro in a second catalog
 * then reads the app's `_hull_*` tables cross-catalog, and the name never
 * appears in the app's own SQL text, so hl_cap_db_check_namespace (lexical)
 * cannot see it. App SQL may use none of them (audit 33). */
static int catalog_kw(const char *s, size_t n)
{
    static const char *const names[] = { "attach", "detach", "use", NULL };
    for (int i = 0; names[i]; i++)
        if (strlen(names[i]) == n && strncasecmp(s, names[i], n) == 0)
            return 1;
    return 0;
}

int hl_cap_db_sql_switches_catalog(const char *sql)
{
    if (!sql) return 0;
    size_t budget = 2 * strlen(sql) + 65536;   /* shared by every gap skip */
    for (const char *p = sql; *p; ) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {               /* "quoted" keyword, "" = a quote */
            char buf[16];
            size_t n = 0;
            int fits = 1;
            for (const char *q = p + 1; *q; q++) {
                if (*q == '"') {
                    if (q[1] == '"') { q++; } else break;
                }
                if (n < sizeof buf) buf[n++] = *q; else { fits = 0; break; }
            }
            if (fits && catalog_kw(buf, n)) return 1;
            p++;                      /* literals are not skipped: see above */
            continue;
        }
        if (dyn_ident_char(c) && !(c >= '0' && c <= '9')) {
            const char *start = p;
            while (*p && dyn_ident_char((unsigned char)*p)) p++;
            size_t n = (size_t)(p - start);
            if (catalog_kw(start, n)) return 1;
            /* SET / RESET / PRAGMA [GLOBAL|SESSION|LOCAL] (schema|search_path) */
            if ((n == 3 && strncasecmp(start, "set", 3) == 0) ||
                (n == 5 && strncasecmp(start, "reset", 5) == 0) ||
                (n == 6 && strncasecmp(start, "pragma", 6) == 0)) {
                const char *after = NULL;
                const char *q = p;
                if (whole_db_next_is(q, "global", &budget, &after) ||
                    whole_db_next_is(q, "session", &budget, &after) ||
                    whole_db_next_is(q, "local", &budget, &after)) {
                    if (!after) return 1;   /* budget exhausted: refuse */
                    q = after;
                }
                if (whole_db_next_is(q, "schema", &budget, &after) ||
                    whole_db_next_is(q, "search_path", &budget, &after))
                    return 1;
            }
            continue;
        }
        p++;
    }
    return 0;
}

/* ── DuckDB values that name a table or a file (audit 34) ──────────── */

/* DuckDB constant-folds a table function's arguments, so a table name or a
 * file path handed to one as a VALUE can be built at run time
 * (concat('_', 'hull_sessions'), chr(95) || ..., a bound parameter), and no
 * lexical check sees it. The readers below accept such an argument only when
 * it is a plain literal or name - one whose text the namespace check (and the
 * protected-file check) has already read - and refuse everything else. Every
 * byte one of them reads past the main scan is charged to one budget per
 * scan, and a scan past it fails closed, so each stays linear. */

/* Charge @p n bytes; 0 once the budget is gone. */
static int fv_take(size_t *budget, size_t n)
{
    if (n > *budget) return 0;
    *budget -= n;
    return 1;
}

/* The body of the '...' or "..." at @p p (doubled quote = one quote), copied
 * unescaped into @p out (cap bytes; NULL = do not copy; *out_n = SIZE_MAX
 * when it does not fit). Returns the text after the closing quote, or NULL
 * for an unterminated one or once the budget is gone (setting *@p exhausted,
 * when given, in that case only). */
static const char *fv_quoted_st(const char *p, size_t *budget, char *out,
                                size_t cap, size_t *out_n, int *exhausted)
{
    char q = *p;
    const char *s = p + 1;
    size_t n = 0;
    for (;;) {
        if (!*s) {
            /* Charged too: callers re-read from many positions, and an
             * unterminated quote read uncharged each time was quadratic. */
            if (!fv_take(budget, (size_t)(s - p)) && exhausted)
                *exhausted = 1;
            return NULL;
        }
        if (*s == q) {
            if (s[1] != q) break;
            s++;
        }
        if (out) {
            if (n + 1 < cap) out[n] = *s;
            n++;
        }
        s++;
    }
    if (!fv_take(budget, (size_t)(s + 1 - p))) {
        if (exhausted) *exhausted = 1;
        return NULL;
    }
    if (out) {
        if (n + 1 >= cap) { *out_n = SIZE_MAX; out[0] = '\0'; }
        else { out[n] = '\0'; *out_n = n; }
    }
    return s + 1;
}

static const char *fv_quoted(const char *p, size_t *budget, char *out,
                             size_t cap, size_t *out_n)
{
    return fv_quoted_st(p, budget, out, cap, out_n, NULL);
}

/* The first byte of a bare name: a parameter ($1, $name) or a dollar-quoted
 * string ($$...$$) starts with '$', which the scans' word reader allows. */
static int fv_name_start(unsigned char c)
{
    return dyn_ident_char(c) && c != '$' && !(c >= '0' && c <= '9');
}

/* A bare or "quoted" name chain (a.b."c") at @p p. Returns the text after
 * it, or NULL when @p p holds none or the budget is gone. */
static const char *fv_name_chain(const char *p, int nest, size_t *budget)
{
    for (;;) {
        if (*p == '"') {
            p = fv_quoted(p, budget, NULL, 0, NULL);
            if (!p) return NULL;
        } else if (fv_name_start((unsigned char)*p)) {
            const char *s = p;
            while (dyn_ident_char((unsigned char)*p)) p++;
            if (!fv_take(budget, (size_t)(p - s))) return NULL;
        } else {
            return NULL;
        }
        const char *g = dyn_skip_gap(p, nest, budget);
        if (!g) return NULL;
        if (*g != '.') return p;
        p = dyn_skip_gap(g + 1, nest, budget);
        if (!p) return NULL;
    }
}

/* The LAST name of a bare or "quoted" name chain at @p p, read with one
 * comment rule: `histogram`, `main.histogram`, `system.main."histogram"`,
 * any depth, gaps allowed around each '.'. A catalog / schema qualifier must
 * not hide the function name it qualifies: the table-position check read
 * only the first name, so `FROM main.histogram('_hu' || 'll_sessions', c)`
 * ran the macro DuckDB resolves through system.main (audit 36). The name is
 * copied into @p buf (*@p n = SIZE_MAX when it does not fit @p cap). Returns
 * 1 with *@p after = the text after that name, 0 when @p p holds no name
 * (or an unterminated quote), -1 once the budget is gone. Every byte read
 * is charged, so a scan that re-reads chains stays linear or fails closed. */
static int fv_chain_last(const char *p, int nest, size_t *budget, char *buf,
                         size_t cap, size_t *n, const char **after)
{
    for (;;) {
        const char *e;
        if (*p == '"') {
            int exhausted = 0;
            e = fv_quoted_st(p, budget, buf, cap, n, &exhausted);
            if (!e) return exhausted ? -1 : 0;
        } else if (fv_name_start((unsigned char)*p)) {
            e = p;
            while (dyn_ident_char((unsigned char)*e)) e++;
            size_t len = (size_t)(e - p);
            if (!fv_take(budget, len)) return -1;
            if (len >= cap) {
                *n = SIZE_MAX;
            } else {
                memcpy(buf, p, len);
                buf[len] = '\0';
                *n = len;
            }
        } else {
            return 0;
        }
        const char *g = dyn_skip_gap(e, nest, budget);
        if (!g) return -1;
        if (*g != '.') { *after = e; return 1; }
        p = dyn_skip_gap(g + 1, nest, budget);
        if (!p) return -1;
    }
}

/* What may end a plain argument: another argument, the call's end, or (for
 * the `PRAGMA name = value` spelling) the statement's end. */
static int fv_arg_end(char c)
{
    return c == ',' || c == ')' || c == ';' || c == '\0';
}

static int fv_name_in(const char *s, size_t n, const char *const *names)
{
    for (int i = 0; names[i]; i++)
        if (strlen(names[i]) == n && strncasecmp(s, names[i], n) == 0)
            return 1;
    return 0;
}

/* Table functions that take a TABLE NAME as a value and are nothing else:
 * checked wherever they are called. */
static int fv_table_by_value_name(const char *s, size_t n)
{
    static const char *const names[] = {
        "pragma_table_info", "pragma_show", "histogram_values",
        "duckdb_table_sample", NULL
    };
    return fv_name_in(s, n, names);
}

/* `show` / `table_info`: pragmas, but ordinary column names too. Their
 * `name = value` spelling exists only after PRAGMA, so a bare `show = 1`
 * (SET, WHERE) is a column (audit 35: #912 refused `UPDATE items SET show =
 * 1`). A call, `show(...)`, can only be the pragma: checked anywhere. */
static int fv_pragma_by_value_name(const char *s, size_t n)
{
    return (n == 4 && strncasecmp(s, "show", 4) == 0) ||
           (n == 10 && strncasecmp(s, "table_info", 10) == 0);
}

/* The words after which a table function can stand: a FROM item, a CALL,
 * a PRAGMA, a table operand of a statement. `,` and `(` count too (a
 * comma join, a parenthesised FROM item). */
static int fv_table_position_kw(const char *s, size_t n)
{
    static const char *const names[] = {
        "from", "join", "lateral", "call", "pragma", "using", "describe",
        "summarize", "show", "pivot", "unpivot", "table", "in", NULL
    };
    return fv_name_in(s, n, names);
}

/* Names refused in every spelling: storage_info returns column statistics
 * (min / max values) of any table, and the profiling / logging calls are
 * CALL-able past lock_configuration (they write query text to stdout, a log
 * table or a file). */
static int fv_always_refused_name(const char *s, size_t n)
{
    static const char *const names[] = {
        "storage_info", "pragma_storage_info",
        "enable_profiling", "disable_profiling",
        "enable_profile", "disable_profile",
        "enable_logging", "disable_logging", "truncate_duckdb_logs",
        /* POINTER arguments (audit 35): no SQL value converts to POINTER
         * in DuckDB 1.5.4, so they cannot be reached with an address; not
         * left to that. */
        "arrow_scan", "arrow_scan_dumb",
        /* They block inside one execution task for an app-chosen time
         * (audit 38): duckdb_interrupt stops only the NEXT task, so
         * sleep_ms(5000) ran its 5 s past a tripped budget and the run
         * deadline. pg_sleep is pg_catalog's macro over sleep_ms. DuckDB
         * 1.5.4 has no other built-in that waits (no http / s3 extension is
         * loadable with external access off). */
        "sleep_ms", "pg_sleep", NULL
    };
    for (int i = 0; names[i]; i++)
        if (strlen(names[i]) == n && strncasecmp(s, names[i], n) == 0)
            return 1;
    return 0;
}

/* After a table-by-value name at @p p, read with one comment rule: 1 when
 * it is called (`(`, or `=` under @p eq) with anything but one plain
 * argument. Under @p strict (a text that creates a macro or function) only
 * a '...' literal is plain: a bare name there can be a macro PARAMETER,
 * substituted at call time with a value built at run time (audit 37). */
static int fv_table_arg_refused_ex(const char *p, int nest, int eq,
                                   int strict, size_t *budget)
{
    const char *q = dyn_skip_gap(p, nest, budget);
    if (!q) return 1;
    if (*q != '(' && !(eq && *q == '=')) return 0;  /* a column, SHOW TABLES */
    int paren = (*q == '(');
    q = dyn_skip_gap(q + 1, nest, budget);
    if (!q) return 1;
    if (paren && *q == ')') return 0;          /* no argument */
    if (*q == '\'')
        q = fv_quoted(q, budget, NULL, 0, NULL);
    else if (strict)
        return 1;
    else
        q = fv_name_chain(q, nest, budget);
    if (!q) return 1;
    q = dyn_skip_gap(q, nest, budget);
    return !q || !fv_arg_end(*q);
}

static int fv_table_arg_refused(const char *p, int nest, int strict,
                                size_t *budget)
{
    return fv_table_arg_refused_ex(p, nest, 1, strict, budget);
}

/* Whether the call whose `(` is at @p p, read with one comment rule, has
 * exactly ONE argument: 1 only when that is certain. Anything this reader
 * cannot follow (a dollar-quoted string, an E'' escape, an unterminated
 * quote, the end of the text, the budget) is 0. The `histogram` table macro
 * takes (source, column, ...); the aggregate `histogram(expr)` one. */
static int fv_call_has_one_arg(const char *p, int nest, size_t *budget)
{
    int depth = 0;
    const char *q = p + 1;
    for (;;) {
        q = dyn_skip_gap(q, nest, budget);
        if (!q) return 0;
        char c = *q;
        if (c == '\0' || c == '$' || c == ';') return 0;
        if ((c == 'e' || c == 'E') && q[1] == '\'') {
            const char *e = q + 2;
            while (*e && *e != '\'' && *e != '\\') e++;
            if (*e != '\'') return 0;           /* a backslash escape */
            q++;
            continue;
        }
        if (c == '\'' || c == '"') {
            q = fv_quoted(q, budget, NULL, 0, NULL);
            if (!q) return 0;
            continue;
        }
        if (!fv_take(budget, 1)) return 0;
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') {
            if (depth == 0) return c == ')';
            depth--;
        } else if (c == ',' && depth == 0) {
            return 0;
        }
        q++;
    }
}

/* `histogram` after a table-position word: refused when called with more
 * than one argument (or one this reader cannot count) whose first is not
 * plain - the table macro's source, which it hands to query_table. The
 * aggregate `histogram(lower(name))` passes, and so does any histogram in a
 * select list (audit 35: #912 refused both). */
static int fv_histogram_refused(const char *p, int strict, size_t *budget)
{
    for (int nest = 1; nest >= 0; nest--) {
        const char *q = dyn_skip_gap(p, nest, budget);
        if (!q) return 1;
        if (*q != '(') continue;
        if (fv_call_has_one_arg(q, 1, budget) &&
            fv_call_has_one_arg(q, 0, budget))
            continue;
        if (fv_table_arg_refused_ex(q, 1, 0, strict, budget) ||
            fv_table_arg_refused_ex(q, 0, 0, strict, budget))
            return 1;
    }
    return 0;
}

/* After a table-position word (or `,` / `(`) at @p p: the name that
 * follows, bare or "quoted", past any catalog / schema qualifiers
 * (fv_chain_last), read with both comment rules. 1 when it is `histogram`
 * called as refused above, or - after PRAGMA - `show` / `table_info` given
 * anything but one plain argument. */
static int fv_table_position_refused(const char *p, int pragma, int strict,
                                     size_t *budget)
{
    for (int nest = 1; nest >= 0; nest--) {
        const char *q = dyn_skip_gap(p, nest, budget);
        if (!q) return 1;
        char buf[16];
        size_t n = 0;
        const char *after = NULL;
        int got = fv_chain_last(q, nest, budget, buf, sizeof buf, &n, &after);
        if (got < 0) return 1;               /* budget gone: refuse */
        if (got == 0) continue;              /* no name */
        if (n == SIZE_MAX) continue;         /* longer than any name here */
        if (n == 9 && strncasecmp(buf, "histogram", 9) == 0 &&
            fv_histogram_refused(after, strict, budget))
            return 1;
        if (pragma && fv_pragma_by_value_name(buf, n) &&
            (fv_table_arg_refused(after, 1, strict, budget) ||
             fv_table_arg_refused(after, 0, strict, budget)))
            return 1;
    }
    return 0;
}

/* nextval / currval take a SEQUENCE NAME as a value, constant-folded like a
 * table function's argument: nextval(concat('_hu', 'll_jobs_id_seq'))
 * advanced a sequence the stdlib owns (audit 38). Only one '...' literal -
 * text the namespace check has read - is accepted, in every reading (a bare
 * name is a column or a macro parameter: a value at run time). DuckDB 1.5.4
 * has no setval. */
static int fv_sequence_fn_name(const char *s, size_t n)
{
    return n == 7 && (strncasecmp(s, "nextval", 7) == 0 ||
                      strncasecmp(s, "currval", 7) == 0);
}

/* 1 when the call at @p p (after the name, one comment rule) is `( )`: no
 * argument at all; 0 when it is not; -1 once the budget is gone. */
static int fv_call_is_empty(const char *p, int nest, size_t *budget)
{
    const char *q = dyn_skip_gap(p, nest, budget);
    if (!q) return -1;
    if (*q != '(') return 0;
    q = dyn_skip_gap(q + 1, nest, budget);
    if (!q) return -1;
    return *q == ')';
}

/* @p dotted: the name follows a '.' (x.nextval(), DuckDB's method-call
 * spelling of nextval(x)): its argument is the receiver, which the '.'
 * reader (fv_dotted_refused) checks, so `()` is fine there. Undotted, an
 * empty call is refused outright (audit 39): it has no reading but the
 * dotted one. */
static int fv_sequence_arg_refused(const char *p, int dotted, size_t *budget)
{
    if (!dotted && (fv_call_is_empty(p, 1, budget) != 0 ||
                    fv_call_is_empty(p, 0, budget) != 0))
        return 1;
    return fv_table_arg_refused_ex(p, 1, 0, 1, budget) ||
           fv_table_arg_refused_ex(p, 0, 0, 1, budget);
}

/* One argument of a read_duckdb call, at @p q: 1 when it is a table_name /
 * schema_name option (`:=`, `=>` or `=`) whose value is not one plain '...'
 * literal. read_duckdb('f.duckdb', table_name := concat(...)) names the
 * table it reads by a value built at run time (audit 38). */
static int fv_read_duckdb_arg_refused(const char *q, int nest, size_t *budget)
{
    q = dyn_skip_gap(q, nest, budget);
    if (!q) return 1;
    char buf[16];
    size_t n = 0;
    const char *e;
    if (*q == '"') {
        int exhausted = 0;
        e = fv_quoted_st(q, budget, buf, sizeof buf, &n, &exhausted);
        if (!e) return 1;
        if (n == SIZE_MAX) return 0;         /* longer than either option */
    } else if (fv_name_start((unsigned char)*q)) {
        e = q;
        while (dyn_ident_char((unsigned char)*e)) e++;
        n = (size_t)(e - q);
        if (!fv_take(budget, n)) return 1;
        if (n >= sizeof buf) return 0;
        memcpy(buf, q, n);
        buf[n] = '\0';
    } else {
        return 0;                            /* a positional value */
    }
    if (!((n == 10 && strncasecmp(buf, "table_name", 10) == 0) ||
          (n == 11 && strncasecmp(buf, "schema_name", 11) == 0)))
        return 0;
    q = dyn_skip_gap(e, nest, budget);
    if (!q) return 1;
    if ((q[0] == ':' && q[1] == '=') || (q[0] == '=' && q[1] == '>'))
        q += 2;
    else if (q[0] == '=')
        q += 1;
    else
        return 1;                            /* not a plain option: refuse */
    q = dyn_skip_gap(q, nest, budget);
    if (!q || *q != '\'') return 1;
    q = fv_quoted(q, budget, NULL, 0, NULL);
    if (!q) return 1;
    q = dyn_skip_gap(q, nest, budget);
    return !q || !fv_arg_end(*q);
}

/* After `read_duckdb` at @p p, read with each comment rule: 1 when its call
 * has a table_name / schema_name option refused above, or cannot be read to
 * its end (a dollar-quoted string, an E'' escape, the budget). Its path is
 * the protected-file check's. */
static int fv_read_duckdb_refused(const char *p, int dotted, size_t *budget)
{
    for (int nest = 1; nest >= 0; nest--) {
        const char *q = dyn_skip_gap(p, nest, budget);
        if (!q) return 1;
        if (*q != '(') continue;
        int depth = 0;
        q++;
        /* 'f.duckdb'.read_duckdb(table_name := ...): dotted, the path is the
         * receiver and the FIRST parenthesised argument may be an option
         * (audit 39). */
        if (dotted && fv_read_duckdb_arg_refused(q, nest, budget))
            return 1;
        for (;;) {
            q = dyn_skip_gap(q, nest, budget);
            if (!q) return 1;
            char c = *q;
            if (c == '\0' || c == '$') return 1;
            if ((c == 'e' || c == 'E') && q[1] == '\'') {
                const char *e = q + 2;
                while (*e && *e != '\'' && *e != '\\') e++;
                if (*e != '\'' || !fv_take(budget, (size_t)(e - q)))
                    return 1;
                q++;
                continue;
            }
            if (c == '\'' || c == '"') {
                q = fv_quoted(q, budget, NULL, 0, NULL);
                if (!q) return 1;
                continue;
            }
            if (!fv_take(budget, 1)) return 1;
            if (c == '(' || c == '[' || c == '{') {
                depth++;
            } else if (c == ')' || c == ']' || c == '}') {
                if (depth == 0) break;
                depth--;
            } else if (c == ',' && depth == 0 &&
                       fv_read_duckdb_arg_refused(q + 1, nest, budget)) {
                return 1;
            }
            q++;
        }
    }
    return 0;
}

static int fv_read_duckdb_name(const char *s, size_t n)
{
    return n == 11 && strncasecmp(s, "read_duckdb", 11) == 0;
}

/* ── Method-call spelling: x.f(...) is f(x, ...) (audit 39) ──────────
 *
 * DuckDB rewrites `x.f(args)` as `f(x, args)`, so the value a guarded
 * function takes first can stand BEFORE the dot:
 * concat('_hu', 'll_outbox_id_seq').nextval() advanced a stdlib sequence
 * while nextval(concat(...)) was refused. The readers below run at every
 * '.' of the text (literals are not skipped, as elsewhere): when a guarded
 * name follows it (bare or "quoted", whitespace / comments around) and is
 * called, the receiver must be one plain '...' literal. */

enum { FV_RECV_OTHER = 0, FV_RECV_LITERAL, FV_RECV_NAME };

static int fv_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

/* Whether the text just before @p at (whitespace only) is a '.'. */
static int fv_prev_is_dot(const char *sql, const char *at)
{
    const char *b = at;
    while (b > sql && fv_space(b[-1])) b--;
    return b > sql && b[-1] == '.';
}

/* What stands before the '.' at @p dot, read backwards over whitespace
 * only (a comment there makes it OTHER: the stricter reading). LITERAL: one
 * '...' literal with nothing joined to it - no prefix letter (E'', X'',
 * U&''), no literal before it (DuckDB joins '_hu' 'll_x' across a newline);
 * *@p lit is its opening quote. NAME: a bare name chain (a, a.b) not
 * following an expression. Every byte read is charged; past the budget,
 * OTHER. */
static int fv_receiver_kind(const char *sql, const char *dot,
                            const char **lit, size_t *budget)
{
    const char *e = dot;
    while (e > sql && fv_space(e[-1])) e--;
    if (e == sql) return FV_RECV_OTHER;
    if (e[-1] == '\'') {
        /* Back to the opening quote: a doubled quote is one quote. */
        const char *k = e - 1;
        for (;;) {
            const char *j = k;
            while (j > sql && j[-1] != '\'') j--;
            if (!fv_take(budget, (size_t)(k - j) + 1)) return FV_RECV_OTHER;
            if (j == sql) return FV_RECV_OTHER;      /* no opening quote */
            j--;                                     /* j: a quote */
            if (j > sql && j[-1] == '\'') { k = j - 1; continue; }
            const char *b = j;
            if (b > sql && (dyn_ident_char((unsigned char)b[-1]) ||
                            b[-1] == '&'))
                return FV_RECV_OTHER;                /* E'..', U&'..' */
            while (b > sql && fv_space(b[-1])) b--;
            if (b > sql && b[-1] == '\'')
                return FV_RECV_OTHER;                /* '..' '..' joined */
            *lit = j;
            return FV_RECV_LITERAL;
        }
    }
    if (dyn_ident_char((unsigned char)e[-1]) || e[-1] == '"') {
        /* A chain of bare and "quoted" names, a."b".c - no gaps. */
        const char *b = e;
        for (;;) {
            if (b > sql && b[-1] == '"') {
                const char *k = b - 1;           /* the closing quote */
                for (;;) {
                    const char *j = k;
                    while (j > sql && j[-1] != '"') j--;
                    if (!fv_take(budget, (size_t)(k - j) + 1))
                        return FV_RECV_OTHER;
                    if (j == sql) return FV_RECV_OTHER;
                    j--;
                    if (j > sql && j[-1] == '"') { k = j - 1; continue; }
                    b = j;
                    break;
                }
            } else {
                const char *s0 = b;
                while (b > sql && dyn_ident_char((unsigned char)b[-1])) b--;
                if (!fv_take(budget, (size_t)(s0 - b))) return FV_RECV_OTHER;
                if (b == s0 || !fv_name_start((unsigned char)*b))
                    return FV_RECV_OTHER;
            }
            if (b > sql + 1 && b[-1] == '.' &&
                (dyn_ident_char((unsigned char)b[-2]) || b[-2] == '"')) {
                b--;
                continue;
            }
            break;
        }
        if (b > sql && dyn_ident_char((unsigned char)b[-1]))
            return FV_RECV_OTHER;                    /* U&"..": a prefix */
        const char *c = b;
        while (c > sql && fv_space(c[-1])) c--;
        if (c > sql && (c[-1] == ')' || c[-1] == ']' || c[-1] == '}' ||
                        c[-1] == '\'' || c[-1] == '"' || c[-1] == ':' ||
                        c[-1] == '.'))
            return FV_RECV_OTHER;                    /* (expr)::t.f() */
        return FV_RECV_NAME;
    }
    return FV_RECV_OTHER;
}

/* After the '.' at @p dot, read with one comment rule: the name there (bare
 * or "quoted") copied into @p buf, and whether it is called. 1 = called,
 * with *@p paren at its '('; 0 = no name / not called; -1 = budget gone. */
static int fv_dotted_name(const char *dot, int nest, size_t *budget,
                          char *buf, size_t cap, size_t *n,
                          const char **paren)
{
    const char *q = dyn_skip_gap(dot + 1, nest, budget);
    if (!q) return -1;
    const char *e;
    if (*q == '"') {
        int exhausted = 0;
        e = fv_quoted_st(q, budget, buf, cap, n, &exhausted);
        if (!e) return exhausted ? -1 : 0;
    } else if (fv_name_start((unsigned char)*q)) {
        e = q;
        while (dyn_ident_char((unsigned char)*e)) e++;
        size_t len = (size_t)(e - q);
        if (!fv_take(budget, len)) return -1;
        if (len >= cap) {
            *n = SIZE_MAX;
        } else {
            memcpy(buf, q, len);
            buf[len] = '\0';
            *n = len;
        }
    } else {
        return 0;
    }
    if (*n == SIZE_MAX) return 0;            /* longer than any guarded name */
    const char *g = dyn_skip_gap(e, nest, budget);
    if (!g) return -1;
    if (*g != '(') return 0;
    *paren = g;
    return 1;
}

/* At the '.' at @p dot of @p sql: 1 when a guarded function is called in
 * method form with a receiver it may not take. A '...' literal receiver is
 * always fine. A name chain before the dot is also how a schema-qualified
 * call is spelled (main.pragma_table_info('users'), which DuckDB binds as
 * such when `main` is a schema): with parenthesised arguments - checked as
 * arguments by the main scan - it passes; with none, `n.nextval()` is
 * nextval(n), a column's run-time value, refused for the sequence functions
 * and in a macro body (a parameter). Any other receiver - an expression,
 * concat(...), a cast - is refused. The histogram table macro (two or more
 * arguments in all) takes a literal or, outside a macro body, a name; with
 * none in its parentheses it is the aggregate. read_duckdb's options are
 * read from its first parenthesised argument on (its path is the file
 * reader's). */
static int fv_dotted_refused(const char *sql, const char *dot, int strict,
                             size_t *budget)
{
    for (int nest = 1; nest >= 0; nest--) {
        char buf[32];
        size_t n = 0;
        const char *paren = NULL;
        int got = fv_dotted_name(dot, nest, budget, buf, sizeof buf, &n, &paren);
        if (got < 0) return 1;
        if (got == 0) continue;
        if (fv_always_refused_name(buf, n)) return 1;
        int value_strict = fv_sequence_fn_name(buf, n) ||
                           fv_table_by_value_name(buf, n) ||
                           fv_pragma_by_value_name(buf, n);
        int histogram = n == 9 && strncasecmp(buf, "histogram", 9) == 0;
        if (fv_read_duckdb_name(buf, n) &&
            fv_read_duckdb_refused(paren, 1, budget))
            return 1;
        if (!value_strict && !histogram) continue;
        if (histogram && fv_call_is_empty(paren, 1, budget) == 1 &&
            fv_call_is_empty(paren, 0, budget) == 1)
            continue;                        /* histogram(x): the aggregate */
        const char *lit = NULL;
        int kind = fv_receiver_kind(sql, dot, &lit, budget);
        if (kind == FV_RECV_LITERAL) continue;
        if (kind != FV_RECV_NAME) return 1;
        if (histogram) {
            if (strict) return 1;
            continue;
        }
        int empty1 = fv_call_is_empty(paren, 1, budget);
        int empty0 = fv_call_is_empty(paren, 0, budget);
        if (empty1 < 0 || empty0 < 0) return 1;
        if (!empty1 && !empty0) continue;    /* qualified: args checked */
        if (strict || fv_sequence_fn_name(buf, n)) return 1;
    }
    return 0;
}

/* The modifiers DuckDB's grammar allows between CREATE and MACRO / FUNCTION
 * (OR REPLACE, then OptTemp), read loosely: any run of them is skipped. */
static int fv_macro_modifier(const char *s, size_t n)
{
    static const char *const names[] = {
        "or", "replace", "temp", "temporary", "local", "global", "unlogged",
        "persistent", NULL
    };
    return fv_name_in(s, n, names);
}

int hl_cap_db_sql_creates_macro(const char *sql)
{
    if (!sql) return 0;
    size_t budget = 2 * strlen(sql) + 65536;   /* shared by every look-ahead */
    for (const char *p = sql; *p; ) {
        unsigned char c = (unsigned char)*p;
        if (!fv_name_start(c)) {
            if (dyn_ident_char(c))      /* a digit or '$' run: one word */
                while (*p && dyn_ident_char((unsigned char)*p)) p++;
            else
                p++;
            continue;
        }
        const char *start = p;
        while (*p && dyn_ident_char((unsigned char)*p)) p++;
        if (!(p - start == 6 && strncasecmp(start, "create", 6) == 0))
            continue;
        /* CREATE, any modifiers, then MACRO / FUNCTION - each word read past
         * whitespace and comments under both comment rules. A look-ahead past
         * the budget counts as a macro: the stricter reading. */
        const char *q = p;
        for (;;) {
            const char *after = NULL;
            if (whole_db_next_is(q, "macro", &budget, &after) ||
                whole_db_next_is(q, "function", &budget, &after))
                return 1;
            int moved = 0;
            for (int nest = 1; nest >= 0 && !moved; nest--) {
                const char *w = dyn_skip_gap(q, nest, &budget);
                if (!w) return 1;
                size_t n = 0;
                while (dyn_ident_char((unsigned char)w[n])) n++;
                if (n && fv_macro_modifier(w, n)) {
                    if (!fv_take(&budget, n)) return 1;
                    q = w + n;
                    moved = 1;
                }
            }
            if (!moved) break;
        }
    }
    return 0;
}

int hl_cap_db_sql_names_table_by_value_ex(const char *sql, int in_macro)
{
    if (!sql) return 0;
    /* A text that creates a macro or function is read strictly: a bare name
     * in its body may be a parameter, which the call fills with a value built
     * at run time (audit 37). */
    const int strict = in_macro || hl_cap_db_sql_creates_macro(sql);
    size_t budget = 2 * strlen(sql) + 65536;
    for (const char *p = sql; *p; ) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {               /* "quoted" function / pragma name */
            char buf[32];
            size_t n = 0;
            int fits = 1;
            const char *q = p + 1;
            for (; *q; q++) {
                if (*q == '"') {
                    if (q[1] == '"') { q++; } else break;
                }
                if (n < sizeof buf) buf[n++] = *q; else { fits = 0; break; }
            }
            if (fits && fv_always_refused_name(buf, n)) return 1;
            if (fits && *q == '"' && fv_sequence_fn_name(buf, n) &&
                fv_sequence_arg_refused(q + 1, fv_prev_is_dot(sql, p), &budget))
                return 1;
            if (fits && *q == '"' && fv_read_duckdb_name(buf, n) &&
                fv_read_duckdb_refused(q + 1, 0, &budget))
                return 1;
            if (fits && *q == '"' && fv_table_by_value_name(buf, n) &&
                (fv_table_arg_refused(q + 1, 1, strict, &budget) ||
                 fv_table_arg_refused(q + 1, 0, strict, &budget)))
                return 1;
            if (fits && *q == '"' && fv_pragma_by_value_name(buf, n) &&
                (fv_table_arg_refused_ex(q + 1, 1, 0, strict, &budget) ||
                 fv_table_arg_refused_ex(q + 1, 0, 0, strict, &budget)))
                return 1;
            if (strict && fits && *q == '"' && n == 9 &&
                strncasecmp(buf, "histogram", 9) == 0 &&
                fv_histogram_refused(q + 1, strict, &budget))
                return 1;
            p++;                      /* literals are not skipped: see above */
            continue;
        }
        if (c == ',' || c == '(') {
            if (fv_table_position_refused(p + 1, 0, strict, &budget)) return 1;
            p++;
            continue;
        }
        if (c == '.') {               /* x.f(...) is f(x, ...) (audit 39) */
            if (fv_dotted_refused(sql, p, strict, &budget)) return 1;
            p++;
            continue;
        }
        if (dyn_ident_char(c) && !(c >= '0' && c <= '9')) {
            const char *start = p;
            while (*p && dyn_ident_char((unsigned char)*p)) p++;
            size_t n = (size_t)(p - start);
            if (fv_always_refused_name(start, n)) return 1;
            if (fv_sequence_fn_name(start, n) &&
                fv_sequence_arg_refused(p, fv_prev_is_dot(sql, start), &budget))
                return 1;
            if (fv_read_duckdb_name(start, n) &&
                fv_read_duckdb_refused(p, 0, &budget))
                return 1;
            if (fv_table_by_value_name(start, n) &&
                (fv_table_arg_refused(p, 1, strict, &budget) ||
                 fv_table_arg_refused(p, 0, strict, &budget)))
                return 1;
            /* show(...) / table_info(...) anywhere; `= value` after PRAGMA. */
            if (fv_pragma_by_value_name(start, n) &&
                (fv_table_arg_refused_ex(p, 1, 0, strict, &budget) ||
                 fv_table_arg_refused_ex(p, 0, 0, strict, &budget)))
                return 1;
            /* In a macro body, the histogram table macro's call anywhere, not
             * only after a table-position word: every word is read, so the
             * last name of main.histogram is too (audit 37). */
            if (strict && n == 9 && strncasecmp(start, "histogram", 9) == 0 &&
                fv_histogram_refused(p, strict, &budget))
                return 1;
            if (fv_table_position_kw(start, n) &&
                fv_table_position_refused(p, n == 6 &&
                    strncasecmp(start, "pragma", 6) == 0, strict, &budget))
                return 1;
            continue;
        }
        p++;
    }
    return 0;
}

int hl_cap_db_sql_names_table_by_value(const char *sql)
{
    return hl_cap_db_sql_names_table_by_value_ex(sql, 0);
}

/* The protected database file: its directory (NULL: none) and name. */
typedef struct {
    const char *dir;
    const char *base;
    size_t      blen;
} FvProtect;

/* Whether @p s (@p n bytes, a path or a literal's text) names the protected
 * file on path-component boundaries: the name preceded by the start or a
 * separator, and followed by the end, a separator, or a `.wal` / `.tmp`
 * sidecar suffix (`.wal.checkpoint`, `.wal.recovery`, `.tmp/` ...).
 * Case-insensitive, as the file system may be. A bare substring refused
 * `approvals` for `app` and every `_hull_migrations` statement for `hull`
 * (audit 35). */
static int fv_names_protected(const char *s, size_t n, const FvProtect *pr)
{
    size_t bl = pr->blen;
    if (n < bl) return 0;
    for (size_t i = 0; i + bl <= n; i++) {
        if (i > 0 && s[i - 1] != '/' && s[i - 1] != '\\') continue;
        if (strncasecmp(s + i, pr->base, bl) != 0) continue;
        size_t j = i + bl;
        if (j == n || s[j] == '/' || s[j] == '\\') return 1;
        if (s[j] == '.' && n - j >= 4 &&
            (strncasecmp(s + j, ".wal", 4) == 0 ||
             strncasecmp(s + j, ".tmp", 4) == 0))
            return 1;
    }
    return 0;
}

/* Whether any literal in @p sql names the protected file: the text between
 * every two consecutive `'`, every two `"` and every two `$` is read as a
 * literal's body, whatever the parity (a quote inside a comment shifts it),
 * so every real literal - '...', E'...', "...", $$...$$, $tag$...$tag$ - is
 * one of the spans read. A path reaches DuckDB only as a literal: a file
 * function's argument and a COPY target must be one (checked below), and a
 * replacement scan (FROM 'x.csv') is one. Each byte is in one span per
 * quote character. */
static int fv_literal_names_protected(const char *sql, const FvProtect *pr)
{
    static const char quotes[] = { '\'', '"', '$' };
    for (size_t k = 0; k < sizeof quotes; k++) {
        const char *prev = NULL;
        for (const char *p = strchr(sql, quotes[k]); p;
             p = strchr(p + 1, quotes[k])) {
            if (prev && fv_names_protected(prev + 1, (size_t)(p - prev - 1), pr))
                return 1;
            prev = p;
        }
    }
    return 0;
}

/* Whether a glob path literal can reach a file in @p protect_dir: its
 * directory part (everything before the first wildcard, up to the last '/')
 * resolves to that directory or one of its ancestors. A path whose directory
 * cannot be resolved, or that starts with `~` (DuckDB expands it), counts. */
static int fv_glob_reaches(const char *body, const char *protect_dir)
{
    const char *s = body;
    if (strncasecmp(s, "file://", 7) == 0) s += 7;
    size_t meta = strcspn(s, "*?[{");
    if (!s[meta]) return 0;           /* not a glob: the name check reads it */
    if (s[0] == '~') return 1;
    char prefix[PATH_MAX];
    size_t cut = 0;
    int slash = 0;
    for (size_t i = 0; i < meta; i++)
        if (s[i] == '/') { cut = i; slash = 1; }
    if (!slash) {
        prefix[0] = '.'; prefix[1] = '\0';
    } else {
        if (cut == 0) cut = 1;        /* "/x*" */
        if (cut >= sizeof prefix) return 1;
        memcpy(prefix, s, cut);
        prefix[cut] = '\0';
    }
    char rp[PATH_MAX];
    if (!realpath(prefix, rp)) return 1;
    size_t rn = strlen(rp);
    if (rn == 1 && rp[0] == '/') return 1;
    return strncasecmp(protect_dir, rp, rn) == 0 &&
           (protect_dir[rn] == '\0' || protect_dir[rn] == '/');
}

/* A path literal at @p p ('...' or "..."): NULL to refuse (not one, too
 * long, naming the protected file, a glob reaching its directory, budget
 * gone), else the text after. */
static const char *fv_path_literal(const char *p, const FvProtect *pr,
                                   size_t *budget)
{
    if (*p != '\'' && *p != '"') return NULL;
    char body[PATH_MAX];
    size_t n = 0;
    const char *after = fv_quoted(p, budget, body, sizeof body, &n);
    if (!after || n == SIZE_MAX) return NULL;
    if (pr && pr->base && fv_names_protected(body, n, pr)) return NULL;
    if (pr && pr->dir && fv_glob_reaches(body, pr->dir)) return NULL;
    return after;
}

/* DuckDB's file-reading table functions: every read_* and parquet_* one,
 * plus glob and sniff_csv. */
static int fv_file_function_name(const char *s, size_t n)
{
    if (n > 5 && strncasecmp(s, "read_", 5) == 0) return 1;
    if (n > 8 && strncasecmp(s, "parquet_", 8) == 0) return 1;
    return (n == 4 && strncasecmp(s, "glob", 4) == 0) ||
           (n == 9 && strncasecmp(s, "sniff_csv", 9) == 0);
}

/* After a file-function name at @p p, read with one comment rule: 1 when it
 * is called (`(`) with a first argument that is not a path literal or a
 * [list] of them, or with any argument under @p deny. */
static int fv_file_call_refused(const char *p, int nest, int deny,
                                const FvProtect *pr, size_t *budget)
{
    const char *q = dyn_skip_gap(p, nest, budget);
    if (!q) return 1;
    if (*q != '(') return 0;          /* a column named read_x, a GLOB b */
    if (deny) return 1;
    q = dyn_skip_gap(q + 1, nest, budget);
    if (!q) return 1;
    if (*q == '[') {
        for (;;) {
            q = dyn_skip_gap(q + 1, nest, budget);
            if (!q) return 1;
            q = fv_path_literal(q, pr, budget);
            if (!q) return 1;
            q = dyn_skip_gap(q, nest, budget);
            if (!q) return 1;
            if (*q == ']') { q++; break; }
            if (*q != ',') return 1;
        }
    } else {
        q = fv_path_literal(q, pr, budget);
        if (!q) return 1;
    }
    q = dyn_skip_gap(q, nest, budget);
    return !q || (*q != ',' && *q != ')');
}

/* At the '.' at @p dot: 1 when a file function is called in method form
 * (`'a.csv'.read_csv(...)` is read_csv('a.csv', ...), audit 39) and its
 * receiver - the path - is not one path literal that passes, or under
 * @p deny at all. */
static int fv_dotted_file_refused(const char *sql, const char *dot, int deny,
                                  const FvProtect *pr, size_t *budget)
{
    for (int nest = 1; nest >= 0; nest--) {
        char buf[32];
        size_t n = 0;
        const char *paren = NULL;
        int got = fv_dotted_name(dot, nest, budget, buf, sizeof buf, &n, &paren);
        if (got < 0) return 1;
        if (got == 0 || !fv_file_function_name(buf, n)) continue;
        if (deny) return 1;
        const char *lit = NULL;
        int kind = fv_receiver_kind(sql, dot, &lit, budget);
        if (kind == FV_RECV_NAME) continue;  /* main.read_csv('x'): the
                                              * main scan reads its path */
        if (kind != FV_RECV_LITERAL) return 1;
        if (!fv_path_literal(lit, pr, budget)) return 1;
    }
    return 0;
}

/* After the word COPY at @p p, read with one comment rule: 1 when its TO /
 * FROM target (the first TO or FROM outside parentheses) is not a path
 * literal or a bare name - a (constant expression) is one DuckDB folds -
 * or is a literal naming the protected file or a glob reaching its
 * directory. A dollar-quoted string anywhere in
 * the statement is refused, as the parenthesis count cannot read it. */
static int fv_copy_refused(const char *p, int nest, const FvProtect *pr,
                           size_t *budget)
{
    int depth = 0;
    const char *q = p;
    for (;;) {
        q = dyn_skip_gap(q, nest, budget);
        if (!q) return 1;
        char c = *q;
        if (c == '\0' || (c == ';' && depth == 0)) return 0;
        if (c == '\'' || c == '"') {
            q = fv_quoted(q, budget, NULL, 0, NULL);
            if (!q) return 1;
            continue;
        }
        if (c == '$') return 1;
        if (c == '(') { depth++; q++; continue; }
        if (c == ')') {
            if (--depth < 0) return 0;   /* COPY was a name in a subquery */
            q++;
            continue;
        }
        if (dyn_ident_char((unsigned char)c) && !(c >= '0' && c <= '9')) {
            const char *s = q;
            while (dyn_ident_char((unsigned char)*q)) q++;
            size_t n = (size_t)(q - s);
            if (!fv_take(budget, n)) return 1;
            if (depth != 0 ||
                !((n == 2 && strncasecmp(s, "to", 2) == 0) ||
                  (n == 4 && strncasecmp(s, "from", 4) == 0)))
                continue;
            q = dyn_skip_gap(q, nest, budget);
            if (!q) return 1;
            if (*q == '\'' || *q == '"') {
                q = fv_path_literal(q, pr, budget);
            } else if (fv_name_start((unsigned char)*q)) {
                while (dyn_ident_char((unsigned char)*q)) q++;
            } else {
                return 1;             /* TO (expr), TO $$...$$, ... */
            }
            if (!q) return 1;
            q = dyn_skip_gap(q, nest, budget);
            if (!q) return 1;
            /* What may follow a target: the end, an option list, WITH /
             * FORMAT ... - not a quote (E'...' read as the name E). */
            return !(*q == '\0' || *q == ';' || *q == '(' || *q == ')' ||
                     *q == ',' ||
                     (dyn_ident_char((unsigned char)*q) &&
                      !(*q >= '0' && *q <= '9')));
        }
        if (!fv_take(budget, 1)) return 1;
        q++;
    }
}

int hl_cap_db_sql_reaches_db_files(const char *sql, int deny_file_functions,
                                   const char *protect_path)
{
    if (!sql) return 0;
    size_t len = strlen(sql);
    size_t budget = 2 * len + 65536;

    /* The protected file's name, in any spelling of its path: every path
     * DuckDB is handed that names the database, a WAL sidecar or the temp
     * directory (<db>.wal, <db>.tmp/) holds it as a path component. Read in
     * literals only (fv_literal_names_protected), where a path can be. */
    char protect_dir[PATH_MAX];
    const char *base = NULL;
    size_t blen = 0;
    if (protect_path && protect_path[0]) {
        const char *slash = strrchr(protect_path, '/');
        base = slash ? slash + 1 : protect_path;
        blen = strlen(base);
        size_t dn = slash ? (size_t)(slash - protect_path) : 0;
        if (slash && dn == 0) dn = 1;
        if (dn >= sizeof protect_dir) return 1;
        memcpy(protect_dir, protect_path, dn);
        protect_dir[dn] = '\0';
        if (blen == 0) return 1;
    }
    FvProtect prot = { (base && protect_dir[0]) ? protect_dir : NULL, base, blen };
    const FvProtect *pr = base ? &prot : NULL;
    if (pr && fv_literal_names_protected(sql, pr)) return 1;

    for (const char *p = sql; *p; ) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {               /* "quoted" function name */
            char buf[32];
            size_t n = 0;
            int fits = 1;
            const char *q = p + 1;
            for (; *q; q++) {
                if (*q == '"') {
                    if (q[1] == '"') { q++; } else break;
                }
                if (n < sizeof buf) buf[n++] = *q; else { fits = 0; break; }
            }
            if (fits && *q == '"' && fv_file_function_name(buf, n) &&
                (fv_file_call_refused(q + 1, 1, deny_file_functions, pr, &budget) ||
                 fv_file_call_refused(q + 1, 0, deny_file_functions, pr, &budget)))
                return 1;
            p++;                      /* literals are not skipped: see above */
            continue;
        }
        if (dyn_ident_char(c) && !(c >= '0' && c <= '9')) {
            const char *start = p;
            while (*p && dyn_ident_char((unsigned char)*p)) p++;
            size_t n = (size_t)(p - start);
            /* E'...' takes backslash escapes, which spell a path without its
             * text (E'\x61pp.duckdb'): one holding a backslash is refused.
             * The look stops at the next quote or backslash, so the looks
             * of one text never overlap. */
            if (n == 1 && (*start == 'u' || *start == 'U') && p[0] == '&' &&
                p[1] == '\'') {
                const char *q = p + 2;
                while (*q && *q != '\'' && *q != '\\') q++;
                if (*q == '\\') return 1;
                continue;
            }
            if (n == 1 && (*start == 'e' || *start == 'E') && *p == '\'') {
                const char *q = p + 1;
                while (*q && *q != '\'' && *q != '\\') q++;
                if (*q == '\\') return 1;
                continue;
            }
            if (fv_file_function_name(start, n) &&
                (fv_file_call_refused(p, 1, deny_file_functions, pr, &budget) ||
                 fv_file_call_refused(p, 0, deny_file_functions, pr, &budget)))
                return 1;
            if (n == 4 && strncasecmp(start, "copy", 4) == 0 &&
                (fv_copy_refused(p, 1, pr, &budget) ||
                 fv_copy_refused(p, 0, pr, &budget)))
                return 1;
            continue;
        }
        if (c == '.' &&
            fv_dotted_file_refused(sql, p, deny_file_functions, pr, &budget))
            return 1;
        p++;
    }
    return 0;
}

#endif /* HL_ENABLE_DB */
