/* INV-15 good: a class check, then JS_GetOpaque (NULL = closed). A class
 * whose opaque is never cleared may keep JS_GetOpaque2. */
static int demo_this(JSContext *ctx, JSValueConst this_val, Thing **out)
{
    if (JS_GetClassID(this_val) != demo_class_id) {
        JS_ThrowTypeError(ctx, "not a Thing");
        return -1;
    }
    *out = JS_GetOpaque(this_val, demo_class_id);
    return 0;
}

static JSValue demo_close(JSContext *ctx, JSValueConst this_val)
{
    Thing *t;
    if (demo_this(ctx, this_val, &t) < 0) return JS_EXCEPTION;
    if (t) {
        thing_free(t);
        JS_SetOpaque(this_val, NULL);
    }
    return JS_UNDEFINED;
}

static JSValue other_get(JSContext *ctx, JSValueConst this_val)
{
    Other *o = JS_GetOpaque2(ctx, this_val, other_class_id);
    if (!o) return JS_EXCEPTION;
    return JS_NewInt32(ctx, o->n);
}
