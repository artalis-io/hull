/*
 * harden.c - freeze the JS intrinsics before any app code runs (audit 18)
 *
 * The Lua stdlib runs in a PRIVATE environment: its chunks see their own
 * copies of the base library and of table/string/math. The JS stdlib had no
 * such wall - app code and the stdlib share one set of intrinsics. Beyond the
 * SQL text (#773), the stdlib's SYNTAX-level operations route through those
 * shared intrinsics: array destructuring and for-of go through
 * %ArrayIteratorPrototype%.next, `await`/Promise helpers through
 * Promise.prototype / its constructor, and bare global references (Array,
 * JSON, ...) through the global bindings. An app that patched any of these
 * observed or steered values the stdlib built from private data - a decrypted
 * TOTP secret it destructured, the _hull_* rows it iterated - and
 * databases.internal did not stop it, because the leak was in-process, not in
 * the SQL.
 *
 * hl_js_harden_intrinsics runs once, as host code, after Hull's own modules
 * and globals are registered and BEFORE any app code (hl_js_load_app, the
 * manifest extractor, a worker.dispatch VM). It:
 *
 *   1. transitively freezes every standard constructor, its prototype, and
 *      everything reachable through own properties (data values and accessor
 *      get/set) with a visited set - INCLUDING the hidden intrinsics
 *      (%IteratorPrototype%, the Array/String/Map/Set/RegExpString iterator
 *      prototypes, the generator / async-generator / async-function prototypes
 *      and their constructors, %AsyncIteratorPrototype%, %TypedArray% and its
 *      prototype, %ThrowTypeError%, and the Iterator global's helpers);
 *   2. makes EVERY global binding present when it runs non-writable and
 *      non-configurable, and freezes what each one holds - the standard
 *      globals and Hull's own (`hull`, whose sleep the stdlib awaits,
 *      `console`, the worker VM's `db`); globalThis itself is NOT frozen,
 *      apps define globals. It then verifies every visited object is frozen
 *      and every binding locked, and throws on any gap; and
 *   3. applies the SES-style "override mistake" enablement on a short list of
 *      commonly assigned prototype data properties, so ordinary code such as
 *      `class X extends Error { constructor(){ super(); this.name = "X"; } }`
 *      and `const o = {}; o.toString = () => "x"` keeps working against a
 *      frozen prototype (an own data property is created on the instance),
 *      while an assignment to the prototype itself throws. The enablement
 *      accessors are C functions (JS_NewCFunctionData) so they cannot be
 *      tampered with.
 *
 * There is NO manifest opt-out: the stdlib's private state depends on this,
 * the way the Lua private environment is not optional. Failure is fatal (init
 * fails) - no fail-open.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "internal.h"
#include "quickjs.h"
#include "log.h"

#include <string.h>

/* ── Override-mistake enablement (SES "moderate" list) ──────────────────
 *
 * For an enabled prototype property we replace the data property with an
 * accessor: the getter returns the original value; the setter, when `this`
 * is NOT the holder prototype, defines an own data property on `this`
 * (writable, enumerable, configurable) - the assignment the author meant -
 * and when `this` IS the holder prototype throws, so the prototype stays
 * read-only. The transitive freeze below then makes the accessor
 * non-configurable.
 */

/* getter: func_data[0] = the original value. */
static JSValue hl_js_override_get(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic,
                                  JSValue *func_data)
{
    (void)this_val; (void)argc; (void)argv; (void)magic;
    return JS_DupValue(ctx, func_data[0]);
}

/* setter: func_data[0] = the holder prototype, func_data[1] = the name. */
static JSValue hl_js_override_set(JSContext *ctx, JSValueConst this_val,
                                  int argc, JSValueConst *argv, int magic,
                                  JSValue *func_data)
{
    (void)magic;
    JSValueConst v = argc > 0 ? argv[0] : JS_UNDEFINED;
    if (JS_SameValue(ctx, this_val, func_data[0])) {
        const char *nm = JS_ToCString(ctx, func_data[1]);
        JS_ThrowTypeError(ctx, "cannot assign to read-only property '%s' "
                          "of a frozen prototype", nm ? nm : "?");
        if (nm) JS_FreeCString(ctx, nm);
        return JS_EXCEPTION;
    }
    JSAtom a = JS_ValueToAtom(ctx, func_data[1]);
    if (a == JS_ATOM_NULL)
        return JS_EXCEPTION;
    /* Define (not set): bypass the very accessor we are standing in. A
     * primitive `this` (assignment through a boxed primitive) makes this
     * fail, as it does under SES; the exception propagates. */
    int rc = JS_DefinePropertyValue(ctx, this_val, a, JS_DupValue(ctx, v),
                                    JS_PROP_C_W_E);
    JS_FreeAtom(ctx, a);
    return rc < 0 ? JS_EXCEPTION : JS_UNDEFINED;
}

/* Replace one data property `name` of `proto` with the enablement accessor.
 * A property that is absent or already an accessor is left alone. Returns 0
 * on success (including the no-op cases), -1 on a hard failure. */
static int enable_override(JSContext *ctx, JSValueConst proto, const char *name)
{
    JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL)
        return -1;

    JSPropertyDescriptor desc;
    int has = JS_GetOwnProperty(ctx, &desc, proto, atom);
    if (has < 0) { JS_FreeAtom(ctx, atom); return -1; }
    if (has == 0) { JS_FreeAtom(ctx, atom); return 0; }   /* absent: skip */

    /* Already an accessor, or non-configurable (the poisoned Function
     * constructors): nothing to enable - leave it read-only. */
    if ((desc.flags & JS_PROP_GETSET) || !(desc.flags & JS_PROP_CONFIGURABLE) ||
        JS_IsUndefined(desc.value)) {
        JS_FreeValue(ctx, desc.value);
        JS_FreeValue(ctx, desc.getter);
        JS_FreeValue(ctx, desc.setter);
        JS_FreeAtom(ctx, atom);
        return 0;
    }

    /* Keep the original reachable only through the getter; freeze it so a
     * reader cannot extend it. */
    JSValue original = desc.value;         /* owns one reference */
    JS_FreeValue(ctx, desc.getter);
    JS_FreeValue(ctx, desc.setter);

    JSValue nameval = JS_NewString(ctx, name);
    if (JS_IsException(nameval)) {
        JS_FreeValue(ctx, original);
        JS_FreeAtom(ctx, atom);
        return -1;
    }

    JSValueConst gdata[1] = { original };
    JSValueConst sdata[2] = { proto, nameval };
    JSValue getter = JS_NewCFunctionData(ctx, hl_js_override_get, 0, 0, 1, gdata);
    JSValue setter = JS_NewCFunctionData(ctx, hl_js_override_set, 1, 0, 2, sdata);
    JS_FreeValue(ctx, nameval);

    int rc = -1;
    if (!JS_IsException(getter) && !JS_IsException(setter)) {
        /* Configurable for now; the transitive freeze makes it non-config. */
        rc = JS_DefinePropertyGetSet(ctx, proto, atom, getter, setter,
                                     JS_PROP_HAS_GET | JS_PROP_HAS_SET |
                                     JS_PROP_HAS_CONFIGURABLE | JS_PROP_THROW);
        getter = JS_UNDEFINED;   /* consumed by DefinePropertyGetSet */
        setter = JS_UNDEFINED;
    }
    JS_FreeValue(ctx, getter);
    JS_FreeValue(ctx, setter);
    JS_FreeValue(ctx, original);
    JS_FreeAtom(ctx, atom);
    return rc < 0 ? -1 : 0;
}

/* The enablement table: (prototype expression, properties). The error
 * prototypes each carry their own `name`/`message`, so a subclass
 * constructor's `this.name = ...` works against the frozen prototype. */
struct override_spec { const char *proto_expr; const char *const *props; };

static int install_enablement(JSContext *ctx)
{
    static const char *const obj_props[] = {
        "toString", "valueOf", "toLocaleString", "constructor",
        "hasOwnProperty", "isPrototypeOf", "propertyIsEnumerable", NULL
    };
    static const char *const err_props[] = {
        "name", "message", "toString", "constructor", NULL
    };
    /* `Object.assign(fn, { toString })` assigns through it (a worker.dispatch
     * caller does exactly that). Not `name` / `length`: a function's own ones
     * shadow them already, and the constructor is the poisoned stub. */
    static const char *const fn_props[] = { "toString", NULL };
    static const struct override_spec specs[] = {
        { "Object.prototype",         obj_props },
        /* The Function global is deleted by the sandbox. */
        { "Object.getPrototypeOf(Object)", fn_props },
        { "Error.prototype",          err_props },
        { "TypeError.prototype",      err_props },
        { "RangeError.prototype",     err_props },
        { "ReferenceError.prototype", err_props },
        { "SyntaxError.prototype",    err_props },
        { "EvalError.prototype",      err_props },
        { "URIError.prototype",       err_props },
        { "AggregateError.prototype", err_props },
        { NULL, NULL }
    };

    for (const struct override_spec *s = specs; s->proto_expr; s++) {
        JSValue proto = JS_Eval(ctx, s->proto_expr, strlen(s->proto_expr),
                                "<hull:harden>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(proto)) {
            /* A prototype absent in this build (e.g. AggregateError) is not a
             * failure - skip it. */
            JS_FreeValue(ctx, JS_GetException(ctx));
            continue;
        }
        if (!JS_IsObject(proto)) { JS_FreeValue(ctx, proto); continue; }
        for (const char *const *p = s->props; *p; p++) {
            if (enable_override(ctx, proto, *p) != 0) {
                JS_FreeValue(ctx, proto);
                return -1;
            }
        }
        JS_FreeValue(ctx, proto);
    }
    return 0;
}

/* ── Transitive freeze + global lockdown ────────────────────────────────
 *
 * Done in a host-evaluated snippet (allowed: host code, before app code).
 * A pure-C property walk would duplicate QuickJS's own descriptor plumbing;
 * the snippet reaches the hidden intrinsics through small expressions and
 * frozen prototypes protect the enablement accessors installed above.
 */
static const char HL_JS_HARDEN_SRC[] =
"(function () {\n"
"  'use strict';\n"
"  const freeze = Object.freeze, isFrozen = Object.isFrozen,\n"
"        gp = Object.getPrototypeOf, own = Reflect.ownKeys,\n"
"        gopd = Object.getOwnPropertyDescriptor, defp = Object.defineProperty;\n"
"  const seen = new WeakSet();\n"
"  const visited = [];\n"
"  const queue = [];\n"
"  const enq = (v) => {\n"
"    const t = typeof v;\n"
"    if (v !== null && (t === 'object' || t === 'function') && !seen.has(v)) {\n"
"      seen.add(v); visited.push(v); queue.push(v);\n"
"    }\n"
"  };\n"
"  /* globalThis itself is never frozen: apps define their own globals. */\n"
"  seen.add(globalThis);\n"
"  /* Roots 1: every own property of globalThis present now - the standard\n"
"   * globals AND Hull's own (hull, console, the worker VM's db). Everything\n"
"   * here was installed by host code; app code has not run yet. */\n"
"  const gkeys = own(globalThis);\n"
"  for (const k of gkeys) {\n"
"    const d = gopd(globalThis, k);\n"
"    if ('value' in d) enq(d.value); else { enq(d.get); enq(d.set); }\n"
"  }\n"
"  /* Roots 2: the hidden intrinsics no global property reaches. A lookup may\n"
"   * legitimately fail when this build lacks the feature (matchAll, async\n"
"   * generators), so only the LOOKUP is guarded - never the freeze. */\n"
"  const hidden = [\n"
"    () => gp(gp([][Symbol.iterator]())),       /* %IteratorPrototype% */\n"
"    () => gp([][Symbol.iterator]()),           /* %ArrayIteratorPrototype% */\n"
"    () => gp(''[Symbol.iterator]()),           /* %StringIteratorPrototype% */\n"
"    () => gp(new Map()[Symbol.iterator]()),    /* %MapIteratorPrototype% */\n"
"    () => gp(new Set()[Symbol.iterator]()),    /* %SetIteratorPrototype% */\n"
"    () => gp('a'.matchAll(/a/)),               /* %RegExpStringIteratorPrototype% */\n"
"    () => gp(Int8Array),                        /* %TypedArray% */\n"
"    () => gp(Int8Array.prototype),              /* %TypedArray%.prototype */\n"
"    () => gp(function* () {}).constructor,      /* GeneratorFunction */\n"
"    () => gp(function* () {}),                  /* %Generator% */\n"
"    () => gp(function* () {}.prototype),        /* %GeneratorPrototype% */\n"
"    () => gp(async function () {}).constructor, /* AsyncFunction */\n"
"    () => gp(async function () {}),             /* %AsyncFunction.prototype% */\n"
"    () => gp(async function* () {}).constructor,/* AsyncGeneratorFunction */\n"
"    () => gp(async function* () {}),            /* %AsyncGenerator% */\n"
"    () => gp(async function* () {}.prototype),  /* %AsyncGeneratorPrototype% */\n"
"    () => gp(gp(async function* () {}.prototype)), /* %AsyncIteratorPrototype% */\n"
"    /* The Function global is deleted by the sandbox: reach its prototype\n"
"     * (and %ThrowTypeError%, its caller/arguments getter) through Object. */\n"
"    () => gp(Object),                            /* %Function.prototype% */\n"
"    () => gopd(gp(Object), 'caller').get,        /* %ThrowTypeError% */\n"
"    () => gopd(gp(Object), 'arguments').get,\n"
"    () => [].values().map(x => x),               /* %IteratorHelperPrototype% */\n"
"    () => Iterator.from({ next() { return { done: true }; } }), /* %WrapForValidIteratorPrototype% */\n"
"  ];\n"
"  for (const fn of hidden) {\n"
"    let v; try { v = fn(); } catch (e) { continue; }   /* absent in this build */\n"
"    enq(v);\n"
"    if (v !== null && typeof v === 'object') enq(gp(v));\n"
"  }\n"
"  /* The walk: no try/catch - a freeze that fails throws, and init fails. */\n"
"  while (queue.length) {\n"
"    const o = queue.pop();\n"
"    enq(gp(o));\n"
"    for (const k of own(o)) {\n"
"      const d = gopd(o, k);\n"
"      if (!d) continue;\n"
"      if ('value' in d) enq(d.value); else { enq(d.get); enq(d.set); }\n"
"    }\n"
"    freeze(o);\n"
"  }\n"
"  /* Lock EVERY global binding present now: non-writable + non-configurable,\n"
"   * so an app can replace neither Array / JSON / Promise nor Hull's own\n"
"   * `hull` (whose sleep the stdlib awaits) / `console` / worker `db`.\n"
"   * Nothing is excluded: every binding here is host-installed, and a binding\n"
"   * already non-configurable (undefined, NaN, Infinity, the __hull_manifest\n"
"   * getter) keeps its own attributes. Globals Hull defines LATER stay\n"
"   * app-visible by design: `test` (hull test), the test harness's globals,\n"
"   * mod_app's transient __hull_app_for_router (deleted again at once). */\n"
"  for (const k of gkeys) {\n"
"    const d = gopd(globalThis, k);\n"
"    if ('value' in d) {\n"
"      if (d.configurable || d.writable)\n"
"        defp(globalThis, k, { configurable: false, writable: false });\n"
"    } else if (d.configurable) defp(globalThis, k, { configurable: false });\n"
"  }\n"
"  /* Verify, and throw on any gap: the contract is fail-closed. */\n"
"  for (const o of visited)\n"
"    if (!isFrozen(o)) throw new TypeError('harden: an intrinsic is not frozen');\n"
"  for (const k of gkeys) {\n"
"    const d = gopd(globalThis, k);\n"
"    if (d.configurable || ('value' in d && d.writable))\n"
"      throw new TypeError('harden: global ' + String(k) + ' is not locked');\n"
"  }\n"
"})();\n";

/* ── Hull's own objects ─────────────────────────────────────────────────
 *
 * The native modules' export objects (hull:crypto's `crypto`, hull:db's
 * `db`, ...) are ONE object shared by app code and the stdlib that imports
 * the same module, and the native class prototypes (Response, WasmBuffer,
 * HlImage, ...) are shared by every instance. An app that replaced
 * `crypto.random` or a response method would run inside the stdlib's calls.
 * They are frozen as they are created - lazily, at first import / first use,
 * which is after hl_js_harden_intrinsics ran.
 *
 * The walk marks an object non-extensible BEFORE visiting its properties and
 * does not descend into a non-extensible object, so cycles end and it stops
 * at the (already frozen) intrinsics. [[Prototype]] is not followed. */
static int freeze_rec(JSContext *ctx, JSValueConst obj)
{
    if (JS_PreventExtensions(ctx, obj) < 0)
        return -1;

    JSPropertyEnum *tab = NULL;
    uint32_t len = 0;
    if (JS_GetOwnPropertyNames(ctx, &tab, &len, obj,
                               JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0)
        return -1;

    int rc = 0;
    for (uint32_t i = 0; i < len && rc == 0; i++) {
        JSPropertyDescriptor d;
        int has = JS_GetOwnProperty(ctx, &d, obj, tab[i].atom);
        if (has < 0) { rc = -1; break; }
        if (has == 0) continue;
        int is_acc = (d.flags & JS_PROP_GETSET) != 0;
        int flags = JS_PROP_HAS_CONFIGURABLE | JS_PROP_THROW |
                    (is_acc ? 0 : JS_PROP_HAS_WRITABLE);
        if (JS_DefineProperty(ctx, obj, tab[i].atom, JS_UNDEFINED,
                              JS_UNDEFINED, JS_UNDEFINED, flags) < 0)
            rc = -1;
        JSValueConst kids[3] = { d.value, d.getter, d.setter };
        for (int k = 0; k < 3 && rc == 0; k++) {
            if (!JS_IsObject(kids[k]))
                continue;
            int ext = JS_IsExtensible(ctx, kids[k]);
            if (ext < 0) rc = -1;
            else if (ext) rc = freeze_rec(ctx, kids[k]);
        }
        JS_FreeValue(ctx, d.value);
        JS_FreeValue(ctx, d.getter);
        JS_FreeValue(ctx, d.setter);
    }
    JS_FreePropertyEnum(ctx, tab, len);
    return rc;
}

int hl_js_freeze_value(JSContext *ctx, JSValueConst v)
{
    if (!JS_IsObject(v))
        return 0;
    int ext = JS_IsExtensible(ctx, v);
    if (ext < 0) return -1;
    return ext ? freeze_rec(ctx, v) : 0;
}

int hl_js_export_frozen(JSContext *ctx, JSModuleDef *m, const char *name,
                        JSValue val)
{
    if (hl_js_freeze_value(ctx, val) != 0) {
        /* Fail closed: export nothing rather than a mutable object. */
        JS_FreeValue(ctx, val);
        JS_FreeValue(ctx, JS_GetException(ctx));
        log_error("[hull:c] could not freeze the export '%s'", name);
        return JS_SetModuleExport(ctx, m, name, JS_UNDEFINED);
    }
    return JS_SetModuleExport(ctx, m, name, val);
}

void hl_js_set_class_proto_frozen(JSContext *ctx, JSClassID id, JSValue proto)
{
    if (hl_js_freeze_value(ctx, proto) != 0) {
        JS_FreeValue(ctx, JS_GetException(ctx));
        log_error("[hull:c] could not freeze a class prototype");
        /* Fail closed: instances get no methods rather than a mutable
         * shared prototype. */
        JS_FreeValue(ctx, proto);
        proto = JS_NULL;
    }
    JS_SetClassProto(ctx, id, proto);
}

/* A stdlib JS module's export objects (`export const sealbox = {...}`) are,
 * like the native ones, shared by app code and every stdlib module that
 * imports them: an app that set `sealbox.open = ...` received the keyring
 * totp passes it. The loader appends this footer to every embedded hull:
 * stdlib module: it imports the module's own namespace (a module may import
 * itself; at the end of its body every binding is initialised) and freezes
 * what each export reaches, stopping at non-extensible objects and not
 * following [[Prototype]] - the same walk as hl_js_freeze_value. */
static const char HL_JS_STDLIB_FOOTER_A[] =
    "\n;import * as __hull_frozen_self from \"";
static const char HL_JS_STDLIB_FOOTER_B[] =
    "\";\n"
    "(function (ns) {\n"
    "  const freeze = Object.freeze, isExt = Object.isExtensible,\n"
    "        own = Reflect.ownKeys, gopd = Object.getOwnPropertyDescriptor;\n"
    "  const walk = (v) => {\n"
    "    const t = typeof v;\n"
    "    if (v === null || (t !== 'object' && t !== 'function') || !isExt(v)) return;\n"
    "    freeze(v);\n"
    "    for (const k of own(v)) {\n"
    "      const d = gopd(v, k);\n"
    "      if (!d) continue;\n"
    "      if ('value' in d) walk(d.value); else { walk(d.get); walk(d.set); }\n"
    "    }\n"
    "  };\n"
    "  for (const k of own(ns)) { if (typeof k === 'string') walk(ns[k]); }\n"
    "})(__hull_frozen_self);\n";

char *hl_js_stdlib_source_frozen(JSContext *ctx, const char *module_name,
                                 const char *src, size_t len, size_t *out_len)
{
    /* The name goes into a string literal: only the characters a hull:
     * module name uses. */
    for (const char *p = module_name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == ':' || c == '_' || c == '-')) {
            JS_ThrowInternalError(ctx, "bad stdlib module name");
            return NULL;
        }
    }
    size_t nlen = strlen(module_name);
    size_t a = sizeof HL_JS_STDLIB_FOOTER_A - 1;
    size_t b = sizeof HL_JS_STDLIB_FOOTER_B - 1;
    if (len > SIZE_MAX / 2 || nlen > 4096) {
        JS_ThrowInternalError(ctx, "stdlib module too large");
        return NULL;
    }
    size_t total = len + a + nlen + b;
    char *out = js_malloc(ctx, total + 1);
    if (!out)
        return NULL;
    memcpy(out, src, len);
    memcpy(out + len, HL_JS_STDLIB_FOOTER_A, a);
    memcpy(out + len + a, module_name, nlen);
    memcpy(out + len + a + nlen, HL_JS_STDLIB_FOOTER_B, b);
    out[total] = '\0';
    *out_len = total;
    return out;
}

int hl_js_harden_intrinsics(JSContext *ctx)
{
    if (!ctx)
        return -1;

    /* Enablement first, so the accessors it installs get frozen below. */
    if (install_enablement(ctx) != 0) {
        log_error("[hull:c] intrinsic enablement failed");
        return -1;
    }

    JSValue r = JS_Eval(ctx, HL_JS_HARDEN_SRC, sizeof HL_JS_HARDEN_SRC - 1,
                        "<hull:harden>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, exc);
        log_error("[hull:c] intrinsic freeze failed: %s", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, exc);
        return -1;
    }
    JS_FreeValue(ctx, r);
    return 0;
}
