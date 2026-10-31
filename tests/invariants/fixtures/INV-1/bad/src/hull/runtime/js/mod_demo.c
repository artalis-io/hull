/* INV-1 bad: a JS binding registered with JS_NewCFunction that charges
 * nothing. */
static JSValue js_spin(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    do_expensive_thing();
    return JS_UNDEFINED;
}

void demo_init(JSContext *ctx, JSValue obj)
{
    JS_SetPropertyStr(ctx, obj, "spin", JS_NewCFunction(ctx, js_spin, "spin", 0));
}
