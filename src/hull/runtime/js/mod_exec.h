/* mod_exec.h - hull:exec (JS) and its hull test stubs
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_JS_MOD_EXEC_H
#define HULL_JS_MOD_EXEC_H

#include "quickjs.h"
#include "hull/runtime/js.h"

int     hl_js_init_exec_module(JSContext *ctx, HlJS *js);
/* test.execStub(name, result | fn | null) - hull test only. */
JSValue hl_js_exec_stub(JSContext *ctx, JSValueConst this_val,
                        int argc, JSValueConst *argv);
/* (Re)create the stub table in the internal store: hull test's
 * registration and each file's reset. Its presence tells exec.run it runs
 * under hull test. */
void    hl_js_exec_stubs_reset(JSContext *ctx);

#endif /* HULL_JS_MOD_EXEC_H */
