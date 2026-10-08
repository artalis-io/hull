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

#endif /* HL_ENABLE_DB */
