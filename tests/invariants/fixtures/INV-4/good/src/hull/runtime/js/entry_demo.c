/* INV-4 good: guard at the start, end through hl_js_entry_end_txn_check. */
static void finish(HlJS *js)
{
    (void)hl_js_entry_end_txn_check(js);
}

int demo_entry(HlJS *js)
{
    hl_js_budget_arm(js);
    hl_db_registry_guard_stale_txns(js->base.db_registry);
    run_handler(js);
    finish(js);
    return 0;
}
