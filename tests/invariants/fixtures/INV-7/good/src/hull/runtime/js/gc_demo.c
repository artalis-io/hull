/* INV-7 good: the only collection is the charged retry helper's. */
int hl_js_gc_for_retry(JSContext *ctx)
{
    if (hl_js_budget_charge(ctx, 100)) return -1;
    JS_RunGC(JS_GetRuntime(ctx));
    return 0;
}

int demo_retry(JSContext *ctx)
{
    return hl_js_gc_for_retry(ctx);
}
