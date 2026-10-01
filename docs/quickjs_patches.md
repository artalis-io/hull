# QuickJS patches

Hull vendors QuickJS directly in `vendor/quickjs/` (edited in tree, unlike
WAMR, which is a submodule patched into `build/wamr-patched/`; see
[wamr_patches.md](wamr_patches.md)). Every local change is marked in the source
with a `HULL PATCH` comment and listed here.

**On a QuickJS upgrade:** grep the new tree for `HULL PATCH`, confirm each entry
below is still needed against upstream, and re-apply the ones that are.

## Patch 0001 - initialise `label_lvalue` in array destructuring

**File:** `vendor/quickjs/quickjs.c`, `js_parse_destructuring_element`
**Found by:** MSan, via `stdlib/js/hull/tests/*.js` being wired into the test
run for the first time (#547)
**Upstream:** should go upstream; not yet reported.

`js_parse_destructuring_element` handles both object and array destructuring.
For the declaration form (`tok` non-zero: `var` / `let` / `const`), the two
object branches set three locals together:

```c
opcode = OP_scope_get_var;
scope = s->cur_func->scope_level;
label_lvalue = -1;
```

The array branch set only the first two. `label_lvalue` then reached the
`put_lvalue(s, opcode, scope, var_name, label_lvalue, ...)` call at the bottom
of the loop never having been written, so any array-destructuring declaration,
`const [a, b] = x`, read an uninitialised variable.

**Effect: none observable.** `put_lvalue` reads its `label` argument only under
`case OP_get_ref_value`, and this path is `OP_scope_get_var`, so the garbage is
passed and discarded. No miscompilation, no memory unsafety.

**Why patch it anyway.** It is still a read of an uninitialised variable, which
is UB, and MSan is correct to flag it. Left alone it also fails CI: `const [ok,
errors] = ...` is ordinary JS, present both in Hull's JS test suites and in
seven shipped stdlib modules (`jobs`, `path`, `tui`, `web/flash`,
`web/htmx/sort`, `web/middleware/oauth`, `web/middleware/outbox`), so the MSan
job trips on every run once those are parsed.

The fix is the one line the object branches already have, in the branch that
was missing it.

**Guard:** the MSan CI job. If an upgrade drops this patch, `MSan + UBSan` fails
again with `use-of-uninitialized-value ... in js_parse_destructuring_element`.
Note that the report is only readable because `QJS_CFLAGS` under `MSAN` carries
`-g` (mk/tests.mk); without it the trace names the function and no line.

## Patch 0002 - a stack frame for bound-function calls

**File:** `vendor/quickjs/quickjs.c`, `js_call_bound_function`
**Found by:** the second capability audit (stdlib-identity spoofing)
**Upstream:** Hull-specific; not a QuickJS bug.

Hull decides whether a database call may touch the reserved `_hull_*` tables
by asking who called it: `JS_GetScriptOrModuleName(ctx, 1)` inside the native
binding names the immediate JS caller, and a `hull:` module is the stdlib.
`js_call_c_function` pushes a frame for the native function itself, so level 1
is its caller.

A bound function had no frame. Calling `f.bind(conn, sql)` ran the native
target with the bound function's CALLER as its level-1 frame. So a stdlib
helper that invokes an app-supplied callback - `retry.run(fn, { retryOn })`
calls `opts.retryOn(value)` - lent its stdlib identity to
`conn.exec.bind(conn)`, and app SQL ran against `_hull_*` tables.

The patch pushes a frame for the bound function in the non-constructor branch,
set up the way `js_call_c_function` sets up its own (`prev_frame`, `js_mode`,
`cur_func`, `arg_buf`, `arg_count`), and pops it on return. The frame's
function is the bound function object, which is not bytecode, so
`JS_GetScriptOrModuleName` returns no name for it and Hull treats the call as
app code. Frame walkers already handle non-bytecode frames (every native call
has one): backtraces print the bound function as a native frame, and the
strict/math-mode checks see the same zero `js_mode` a native frame carries.

The constructor branch (`new boundFn()`) is unchanged: a constructor call
cannot target a database binding.

**Guard:** `tests/hull/runtime/js/test_js.c`,
`js_cap.stdlib_helpers_do_not_lend_their_identity`. Without the patch the
bound-`retryOn` case runs the `_hull_*` statement and the test fails.
