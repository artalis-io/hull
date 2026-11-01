# Invariant lint

`make check-invariants` turns the "a new X must Y" rules in CLAUDE.md into
checks over the C sources, so a change that breaks an established invariant
fails CI before review ([design/audit_convergence.md](../design/audit_convergence.md)
section 5.2). It runs in the `invariants` job of `.github/workflows/ci.yml`
and in `make lint`.

| Piece | Path |
|---|---|
| The lint (python3, no other dependency) | `scripts/check_invariants.py` |
| Justified exceptions | `scripts/invariants_allowlist.txt` |
| Self-test (`make check-invariants-selftest`) | `tests/check_invariants_selftest.sh` |
| Good / bad fixture trees per rule | `tests/invariants/fixtures/INV-N/{good,bad*}/` |

## Rules

The checks are lexical: a small C reader strips comments, blanks string
literals, finds function definitions by brace matching and records the calls
in each body. Macros are read as text, so a rule can be fooled by a macro or
an `#if` branch; each rule is tuned to catch the shape the invariant forbids,
and the exact decision procedure is documented in a comment above the rule in
`scripts/check_invariants.py`.

| ID | Invariant | How it decides |
|---|---|---|
| INV-1 | Every exported Lua / JS binding is registered with the binding dispatcher | Phase 2 (docs/design/audit_convergence_dispatcher.md): the dispatcher charges each call its measured wall time, so a binding must go through it. (a) No `luaL_setfuncs` / `luaL_newlib` / `luaL_register` in `src/hull/runtime/lua/` (outside `budget.c` and the tool VM's `mod_tool.c`), and no `JS_SetPropertyFunctionList` / `JS_SetModuleExportList` in `src/hull/runtime/js/` outside `hl_js_set_function_list` (QuickJS instantiates a function list unmarked; `JS_NewCFunction*` functions are marked by QuickJS HULL PATCH 0011). (b) A Lua binding - a `lua_CFunction`-signature function in `src/hull/runtime/lua/` whose address is taken - must be a row of a `luaL_Reg` table the file passes to `hl_lua_setfuncs` / `hl_lua_newlib` (or pushes row by row with `hl_lua_pushcclosure`), or be pushed with `hl_lua_pushcfunction` / `hl_lua_pushcclosure`. An explicit charge alone does not pass; functions pushed raw on purpose (finalizers, protected-call bodies, the coroutine library) are allowlisted. |
| INV-2 | No `sqlite3_*` call outside `src/hull/cap/` | Any `sqlite3_<name>(` call in `src/hull/` outside `cap/`. |
| INV-3 | Every JS operation that parks calls `hl_js_db_refuse_wait` before arming | In `src/hull/runtime/js/`, every same-file root that reaches a call of `hl_js_async_cont_create` / `hl_js_op_suspend` / `hl_cap_multipart_park` must reach `hl_js_db_refuse_wait` (or, for a resume that parks again, `hl_js_run_yield_check`). |
| INV-4 | Every JS entry ends through `hl_js_entry_end_txn_check` | A function that arms a run (`hl_js_budget_arm*`) must reach `hl_js_entry_end_txn_check` (or `hl_js_entry_park`); and a function that calls `hl_db_registry_guard_stale_txns` must arm the run itself (the guard belongs at entry START only). |
| INV-5 | Every disk loader checks `hl_vfs_embedded` | A loader calls `hl_vfs_disk_gate_check`, or (under `src/hull/runtime/`) names `app_dir` and opens a file. The loader or a same-file caller must call `hl_vfs_embedded`. |
| INV-6 | Terminal and log output goes through the shared escaper | `fprintf(stderr\|stdout, ...)` / `printf` with a `%s` / `%c` conversion, and `fputs` / `fwrite` of anything but a string literal to stderr / stdout, outside the escapers. The rule cannot tell Hull's text from app text, so each such function is reviewed once and allowlisted with the reason its text is Hull's. |
| INV-7 | No `JS_RunGC` outside `hl_js_gc_for_retry` | Any `JS_RunGC(` in `src/hull/` or `tests/` outside that function. |
| INV-8 | Every SQLite open registers with `hl_db_live_file_add` before reading | After each `sqlite3_open*(` (not `":memory:"`), the enclosing function must call `hl_db_live_file_add` (or a same-file function that does) before `hl_cap_db_init` / `hl_cap_db_guard` / `sqlite3_exec` / `sqlite3_prepare*` / `hl_migrate_run`. |
| INV-9 | No `lua_closethread(..., NULL)` / `lua_resume` without the resumer | `lua_closethread(x, NULL)` and `lua_resume(co, NULL, ...)` anywhere in `src/hull/`; and a function calling `lua_resume` must also call `hl_lua_resume_status`. |
| INV-10 | Vendored-engine changes carry a documented `HULL PATCH NNNN` marker | For every engine Hull edits in tree - `vendor/lua`, `vendor/quickjs`, `vendor/pledge`, `vendor/stb`, `vendor/sqlite`, `vendor/mbedtls` - the patch numbers its markers use equal the live `## Patch NNNN` headings of `docs/<engine>_patches.md` (the file may be absent only while the engine carries no marker; pledge / stb / sqlite / mbedtls markers must be numbered); `vendor/tweetnacl` carries its unnumbered markers; every `patches/wamr/NNNN-*.patch` is listed in `docs/wamr_patches.md`, and the commit `vendor/wamr` is pinned at equals the doc's "Pinned WAMR base commit" (`vendor/keel` is Hull's own library, changed upstream and bumped, never patched here). In the diff against the base (`INVARIANTS_BASE`, else the merge-base with `origin/main`) every added line in those trees has a `HULL PATCH` marker in its hunk or within 40 lines of it, except in Hull-owned files (`vendor/stb/stb_impl.c`, `vendor/mbedtls/hull_config.h`) and in a version upgrade (a diff that changes the engine's version line). Audit 43 widened it from lua / quickjs / tweetnacl. |

## The allowlist

`scripts/invariants_allowlist.txt` holds the justified current exceptions, one
per line:

```
INV-N  path:symbol  # reason
```

- `symbol` is the enclosing C function. Path and symbol may be shell globs:
  `path:*` covers a whole file, `src/hull/runtime/*:luaopen_*` a family.
- Every entry needs a reason that says why the invariant does not apply (for
  example "pure accessor: reads a field, no syscall").
- An entry that no longer matches a finding is **stale** and fails the lint,
  so the list only shrinks: fix the code, then delete the line.
- A new entry needs the reviewer's agreement that its reason holds. Seeding
  INV-1 with today's raw-pushed functions is what makes the rule useful now:
  a NEW binding that skips the dispatcher fails.

## Adding a rule

Any new "a new X must Y" statement added to CLAUDE.md must add a lint rule in
the same PR:

1. Add a function decorated `@rule("INV-N", "...")` to
   `scripts/check_invariants.py`, with a comment block saying how it decides.
2. Add `tests/invariants/fixtures/INV-N/good/` (must pass) and at least one
   `bad*/` tree (must fail with an `INV-N` finding). The self-test fails if a
   rule has no fixtures.
3. Run `make check-invariants` and allowlist (with reasons) the existing sites
   that are justified exceptions; fix the rest.
4. Add the row to the table above.

## The audit-fix regression gate

`scripts/check_regression_test.sh` (CI: `.github/workflows/audit-regression.yml`)
checks that an audit fix's new UTEST cases fail on the pre-fix code
([audit_process.md](../audit_process.md) 2.1). Since audit 43 a test that only
fails to BUILD there proves nothing (any test of the fix's new API does): the
gate then reruns with the PR's headers kept and the PR's
`tests/regression_stubs/*.c` - pre-fix stand-ins for the new functions, with
the OLD behaviour - linked in (`make REGRESSION_STUBS=...`), and needs a case
to fail at runtime. A fix that is nothing but new API opts out explicitly with
a `REGRESSION-GATE: build-only <reason>` trailer in a commit message or the PR
body, which the gate logs. `tests/check_regression_gate_selftest.sh` (run by
the self-test) proves each outcome on throwaway repositories.

## Running it

```sh
make check-invariants                      # all rules, with the allowlist
make check-invariants-selftest             # every rule bites on its bad fixtures
python3 scripts/check_invariants.py --rule INV-6 -v       # one rule, show allowlisted sites
python3 scripts/check_invariants.py --no-allowlist        # every site, no exceptions
```
