/* INV-3 bad: a new parking op that never checks for an open transaction. */
static int arm_op(HlJS *js)
{
    return hl_js_op_suspend(js, NULL);
}

static JSValue js_wait(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv)
{
    return arm_op(JS_GetContextOpaque(ctx)) ? JS_EXCEPTION : JS_UNDEFINED;
}
