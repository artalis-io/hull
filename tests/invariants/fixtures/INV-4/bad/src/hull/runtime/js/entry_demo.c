/* INV-4 bad: a new entry that ends with a bare stale-transaction guard
 * instead of hl_js_entry_end_txn_check (its run tag is never failed). */
static void finish(HlJS *js)
{
    hl_db_registry_guard_stale_txns(js->base.db_registry);
}

int demo_entry(HlJS *js)
{
    hl_js_budget_arm(js);
    run_handler(js);
    finish(js);
    return 0;
}
