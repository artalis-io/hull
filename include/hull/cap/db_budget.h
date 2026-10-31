/*
 * cap/db_budget.h: SQL work charged to the calling run's instruction budget
 *
 * A SQLite statement runs inside one binding call, where neither runtime's
 * instruction hook fires: a recursive CTE in one conn.query held the event
 * loop for good. Every SQLite connection Hull opens (hl_cap_db_guard) gets a
 * progress handler that charges HL_DB_PROGRESS_OPS units per call to the
 * budget bound on the CALLING THREAD, and interrupts the statement once that
 * budget is exhausted (audit 9 H4).
 *
 * The cap layer stays runtime-agnostic: a runtime binds a charge function for
 * its budget (the Lua VM's HlLuaBudget, the QuickJS context's counter, a
 * worker VM's own) when it arms a run, and unbinds it when the budget goes
 * away. Code that runs SQL outside any app run - migrations, agent queries -
 * swaps the binding out for its duration (hl_db_budget_swap(NULL, NULL)), so
 * it is never charged to, or stopped by, a run's leftover budget. A db.async
 * op is charged to an HlDbOpBudget of its own on the worker thread.
 *
 * The functions live in cap/db_common.c (the base), so a runtime can bind
 * without linking the SQLite feature.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_BUDGET_H
#define HL_CAP_DB_BUDGET_H

#include <stddef.h>
#include <stdint.h>

/* SQLite virtual-machine instructions between progress-handler calls; each
 * call charges this many units (one SQLite instruction ~ one script one).
 * Small, so a tripped statement stops within a few opcodes (audit 10 H3). */
#define HL_DB_PROGRESS_OPS 100

/* One opcode can do unbounded work off the VM heap - randomblob, replace,
 * printf, a sorter run, temp b-trees under temp_store=MEMORY - which the
 * progress handler sees as one instruction. So every allocation SQLite makes
 * is charged too, one unit per HL_DB_ALLOC_UNIT_BYTES (rounded up), through
 * the SQLITE_CONFIG_MALLOC wrapper hl_cap_db_sqlite_setup installs; once the
 * bound budget is exhausted those allocations fail, so the statement stops at
 * once (audit 10 H3). */
#define HL_DB_ALLOC_UNIT_BYTES 64

/* Charge @p units to the budget @p ud; non-zero = exhausted (and sticky):
 * interrupt the statement. */
typedef int (*HlDbBudgetFn)(void *ud, int64_t units);

typedef struct {
    HlDbBudgetFn fn;
    void        *ud;
} HlDbBudgetBinding;

/* Bind @p fn / @p ud as this thread's budget (NULL fn: none) and return the
 * binding it replaces, for hl_db_budget_restore. */
HlDbBudgetBinding hl_db_budget_swap(HlDbBudgetFn fn, void *ud);
void hl_db_budget_restore(HlDbBudgetBinding prev);
/* This thread's binding, to put back with hl_db_budget_restore. */
HlDbBudgetBinding hl_db_budget_current(void);

/* Drop this thread's binding when it is @p ud's (the budget is going away). */
void hl_db_budget_unbind(const void *ud);

/* Charge this thread's bound budget (the progress handler). 0 = go on; no
 * binding is never an interrupt. While the worker pool is being freed
 * (hl_db_op_shutdown) every bound charge interrupts, so a worker.dispatch
 * job's SQL wait does not hold the join either (audit 28). */
int hl_db_budget_charge(int64_t units);

/* A statement about to be sent to a network database (Postgres / MySQL):
 * one round trip, which the per-frame charge paid ~1 unit for (audit 41).
 * Charges HL_WORK_NET_SQL_UNITS (hull/shared/work_charge.h) to this
 * thread's binding BEFORE the send. 0 = send it; -1 = the run is over its
 * budget, nothing sent. A ROLLBACK (@p is_rollback) is charged but never
 * refused: it is how a tripped run's transaction ends. SQLite is not
 * charged here - its progress handler and allocator charge its work. */
int hl_db_net_stmt_charge(int is_rollback);

/* A plain budget for code with no VM of its own (a db.async op).
 *
 * Units alone never stopped a wait: a Postgres / MySQL / Valkey wait asks
 * with a 0-unit charge, so pg_sleep(1e6), a lock wait or a silent server with
 * ?read_timeout=0 pinned a pool thread for good, and the SIGTERM pool join
 * waited on it (audit 27). So the budget also trips - on any charge, 0
 * included - once its wall-clock deadline has passed, once *cancelled is set
 * (the op's continuation went: request cancelled), or once the process is
 * shutting its worker pool down (hl_db_op_shutdown). The backends' trip
 * paths then close the connection. Fields past `tripped` may be left zero. */
typedef struct {
    int64_t     limit;        /* 0 = none */
    int64_t     used;
    int         tripped;
    int         reason;       /* HL_DB_OP_STOP_*, once tripped */
    uint64_t    deadline_ms;  /* CLOCK_MONOTONIC ms, 0 = none */
    const int  *cancelled;    /* read atomically, NULL = none */
} HlDbOpBudget;
#define HL_DB_OP_STOP_UNITS    1
#define HL_DB_OP_STOP_DEADLINE 2
#define HL_DB_OP_STOP_CANCEL   3
int hl_db_op_budget_charge(void *ud, int64_t units);

/* CLOCK_MONOTONIC in ms (never 0), for HlDbOpBudget.deadline_ms. */
uint64_t hl_db_monotonic_ms(void);

/* Set while the process frees its worker pool (on before pool_free, off once
 * it has returned): every HlDbOpBudget trips at its next charge, and every
 * other bound charge (a worker.dispatch job's) interrupts, so an op waiting
 * on the server does not hold the join. */
void hl_db_op_shutdown(int on);

/* The largest string or blob a SQLite connection Hull opens may build
 * (SQLITE_LIMIT_LENGTH): randomblob(1e9) / zeroblob allocate outside the VM
 * heap limit. The runtimes report their heap cap at init; the largest wins.
 * 0 (nothing reported, e.g. `hull migrate`) leaves SQLite's own limit. */
void   hl_db_note_heap_limit(size_t bytes);
size_t hl_db_max_value_bytes(void);

#endif /* HL_CAP_DB_BUDGET_H */
