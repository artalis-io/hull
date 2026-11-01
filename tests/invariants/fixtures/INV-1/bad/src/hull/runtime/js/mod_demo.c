/* INV-1 bad: a function list instantiated by JS_SetPropertyFunctionList -
 * QuickJS makes those functions with its own constructor, unmarked, so
 * their calls skip the binding dispatcher. */
static JSValue js_spin(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    do_expensive_thing();
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry demo_funcs[] = {
    JS_CFUNC_DEF("spin", 0, js_spin),
};

void demo_init(JSContext *ctx, JSValue obj)
{
    JS_SetPropertyFunctionList(ctx, obj, demo_funcs, 1);
}
