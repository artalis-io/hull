/*
 * plain_value.h - QuickJS values to and from the build plugin channel's
 * value encoding (include/hull/shared/plain_value.h).
 *
 * A string is text (a lone surrogate is refused), an ArrayBuffer or typed
 * array is bytes; a number must be an integer within 2^53, a BigInt within
 * int64. Plain objects (own enumerable string-keyed data properties; a
 * null / undefined value is skipped) are maps, arrays (no holes, no null)
 * are arrays. Anything else - a Proxy, a function, a getter, a Date, a
 * boxed primitive - is refused, and nothing the walk reads runs app code.
 * Decoding gives text as a string, bytes as an ArrayBuffer, an int as a
 * number (a BigInt beyond 2^53), maps as plain objects.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HL_JS_PLAIN_VALUE_H
#define HL_JS_PLAIN_VALUE_H

#include "hull/shared/plain_value.h"
#include "quickjs.h"

/* Encode @p v into @p w. HL_PV_OK or an HL_PV_E_* code (also in w->err);
 * never leaves an exception pending. */
int hl_js_pv_encode(JSContext *ctx, JSValueConst v, HlPvWriter *w);

/* Check and decode @p buf. The value, or JS_EXCEPTION with *err set (an
 * HL_PV_E_* code; HL_PV_E_NOMEM leaves QuickJS's exception pending, any
 * other code none). */
JSValue hl_js_pv_decode(JSContext *ctx, const void *buf, size_t len, int *err);

#endif /* HL_JS_PLAIN_VALUE_H */
