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
#include <stdatomic.h>
#include <string.h>
#include <strings.h>

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

int hl_db_budget_charge(int64_t units)
{
    return tl_budget_fn ? tl_budget_fn(tl_budget_ud, units) : 0;
}

int hl_db_op_budget_charge(void *ud, int64_t units)
{
    HlDbOpBudget *b = (HlDbOpBudget *)ud;
    if (b->tripped) return 1;
    if (b->limit <= 0) return 0;
    b->used = units > INT64_MAX - b->used ? INT64_MAX : b->used + units;
    if (b->used > b->limit) b->tripped = 1;
    return b->tripped;
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
 * tried so neither hides a call). Returns the first other character. */
static const char *dyn_skip_gap(const char *p, int nest)
{
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == '\f' || *p == '\v')
            p++;
        if (p[0] == '-' && p[1] == '-') {
            while (*p && *p != '\n') p++;
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
        return p;
    }
}

static int dyn_followed_by_call(const char *p)
{
    return *dyn_skip_gap(p, 1) == '(' || *dyn_skip_gap(p, 0) == '(';
}

int hl_cap_db_sql_calls_dynamic_sql(const char *sql)
{
    if (!sql) return 0;
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
                if (n < sizeof buf) buf[n++] = *q; else fits = 0;
            }
            const char *after = *q ? q + 1 : q;
            if (fits && dyn_name_is_dynamic(buf, n) && dyn_followed_by_call(after))
                return 1;
            p = after;
            continue;
        }
        if (dyn_ident_char(c) && !(c >= '0' && c <= '9')) {
            const char *start = p;
            while (*p && dyn_ident_char((unsigned char)*p)) p++;
            if (dyn_name_is_dynamic(start, (size_t)(p - start)) &&
                dyn_followed_by_call(p))
                return 1;
            continue;
        }
        p++;
    }
    return 0;
}

#endif /* HL_ENABLE_DB */
