/* INV-1 good: a JS binding that charges before its work. */
static JSValue js_work(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    if (hl_js_budget_charge(ctx, 64)) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry demo_funcs[] = {
    JS_CFUNC_DEF("work", 0, js_work),
};
