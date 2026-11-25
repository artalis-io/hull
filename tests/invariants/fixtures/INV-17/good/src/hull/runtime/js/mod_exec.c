/* INV-17 good: the binding matches, then a helper starts the call. */
static JSValue run_broker(JSContext *ctx, const char *name)
{
    return hl_exec_op_start(ctx, name) == 0 ? JS_UNDEFINED : JS_EXCEPTION;
}

static JSValue js_exec_run(JSContext *ctx, const char *name, const char *v)
{
    if (hl_exec_check_call(policy_of(ctx), name, v, 1) != 0) return JS_EXCEPTION;
    return run_broker(ctx, name);
}
