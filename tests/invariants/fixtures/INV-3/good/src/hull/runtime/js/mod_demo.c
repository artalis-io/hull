/* INV-3 good: the binding refuses a wait inside a transaction before it
 * reaches the helper that parks. */
static int arm_op(HlJS *js)
{
    HlAsyncCont *cont = hl_js_async_cont_create(js, JS_UNDEFINED, JS_UNDEFINED, 0);
    return cont ? 0 : -1;
}

static JSValue js_wait(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    if (hl_js_db_refuse_wait(ctx, "demo.wait()")) return JS_EXCEPTION;
    return arm_op(JS_GetContextOpaque(ctx)) ? JS_EXCEPTION : JS_UNDEFINED;
}
