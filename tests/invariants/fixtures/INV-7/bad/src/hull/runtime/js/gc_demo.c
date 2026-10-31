/* INV-7 bad: a retry site that collects without charging. */
int demo_retry(JSContext *ctx)
{
    JS_RunGC(JS_GetRuntime(ctx));
    return 0;
}
