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
