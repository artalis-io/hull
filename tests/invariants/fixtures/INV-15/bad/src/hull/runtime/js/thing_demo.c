/* INV-15 bad: close() clears the opaque, and a getter reads `this` with
 * JS_GetOpaque2, which throws on NULL and leaves the TypeError pending. */
static JSValue demo_close(JSContext *ctx, JSValueConst this_val)
{
    Thing *t = JS_GetOpaque2(ctx, this_val, demo_class_id);
    if (t) {
        thing_free(t);
        JS_SetOpaque(this_val, NULL);
    }
    return JS_UNDEFINED;
}

static JSValue demo_get_size(JSContext *ctx, JSValueConst this_val)
{
    Thing *t = JS_GetOpaque2(ctx, this_val, demo_class_id);
    if (!t) return JS_NewInt32(ctx, 0);
    return JS_NewInt32(ctx, t->size);
}
