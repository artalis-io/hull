/* INV-1 good: JS functions made with JS_NewCFunction* (marked by QuickJS
 * HULL PATCH 0011) or a function list instantiated by
 * hl_js_set_function_list go through the binding dispatcher. */
static JSValue js_work(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry demo_funcs[] = {
    JS_CFUNC_DEF("work", 0, js_work),
};

void demo_init(JSContext *ctx, JSValue obj)
{
    hl_js_set_function_list(ctx, obj, demo_funcs, 1);
    JS_SetPropertyStr(ctx, obj, "again", JS_NewCFunction(ctx, js_work, "again", 0));
}
