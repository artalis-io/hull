# SQLite as a composable feature — design

**Status:** Phase A + B + C shipped (#123–#129) and Phase C.2 (the per-runtime
`mod_db`-udf bridge split) shipped — the runtime archives are now SQLite-free and
a db-free app on a SQLite-less base drops SQLite entirely. Phase D
(embed-as-default) remains. Tracks the capstone of the base-subtraction axis
(runtime #113 → HTTP #114 → WASM #118 → **DB backend**).
**Prereq:** the DB-backend vtable (`HlDbBackend`, `cap/db_backend.h`) already
exists and is proven by the `postgres` / `mysql` / `duckdb` features. This epic
does for the *default* backend what those did for the optional ones — with the
twist that SQLite's default-ness makes it the hardest of the four.

## Premise

At the vtable level SQLite already **is** just another backend. `HlDbBackend`
is the abstraction; `hl_db_backend_select` (`cap/db_select.c`) picks one per
connection by DSN scheme; the weak `hl_db_feature_backends` hook
(`db_select.c:95`) already lets postgres/mysql/duckdb compose in from outside
the base. So the *mechanism* to pull SQLite out of the base is already built —
SQLite is simply hardcoded into the base `BACKENDS[]` table (`db_select.c:49`)
and as the scheme-less default (`db_select.c` `#ifdef HL_ENABLE_SQLITE` branch)
instead of going through the hook.

## Why do it

- **Size.** `sqlite3.o` is the single biggest vendored dependency — ~700 KB–1 MB
  native, ~2 MB in the cosmo build (bigger than WAMR's 256 KB). A Postgres-only
  app, or any compute / CLI / signing app, would drop all of it.
- **Consistency.** It makes "every external DB engine is a feature; the base is
  DB-vtable-only" *literally* true — the same shape the base already has for
  runtimes, HTTP, and WASM. The base becomes genuinely engine-agnostic.
- **Capstone.** It's the natural end of the base-subtraction axis. After it,
  there is nothing left to subtract (crypto + the DB vtable are the permanent
  core).

## The model decision: embedded auto-composed, NOT installable

This is the crux, and it's what separates SQLite from postgres/mysql/duckdb.
Hull has **two** feature models:

| Model | Members | Ships | Composed |
|-------|---------|-------|----------|
| **Installable additive** | duckdb, postgres, mysql, gpu, tui | `hull feature install` → `~/.hull/feature/` | explicit `--with=` (or manifest-inferred) |
| **Embedded auto-composed** | runtime (lua/js), http core, wasm | inside the `hull` binary | auto, from app signals; never installed |

SQLite **must** follow the *embedded auto-composed* model (like the runtime and
WASM), NOT the installable one (like postgres). Because unlike postgres — which
is always DSN-explicit — SQLite is:

1. **The default.** Scheme-less DSNs, `:memory:`, and bare file paths hardcode
   SQLite (`db_select.c` scheme-less branch; `app_context.c` `:memory:`
   default). The zero-config `hull new → build → runs-with-a-local-DB` flow
   depends on it.
2. **The test substrate.** `hull test` runs every app's tests against
   `:memory:`; `hull dev` auto-migrates against the app's SQLite file. The
   *toolchain itself* needs SQLite.
3. **The floor for SQLite-only caps.** `db.udf` and `hull/search` (FTS5) already
   fail-closed on other backends; they only work on SQLite.

If SQLite were `hull feature install`-gated, all of that would break for anyone
who hadn't installed it. Embedded-auto-composed keeps it zero-config: the `hull`
binary keeps SQLite (for its own test harness + the embedded default), and
**produced app binaries drop it when they don't need it** — exactly the
WAMR/WASM story, applied to the DB.

> This revises the standing stance in `docs/roadmap.md` ("Deliberately kept
> core (never a feature): … the default SQLite backend"). The runtime was
> *also* default+mandatory and got featurified as embedded-auto-composed; the
> same logic applies. "Kept core" for SQLite becomes "kept embedded +
> auto-composed."

## The seam

The DB-backend seam already exists — `hl_db_feature_backends(size_t *count)`,
a weak default in `db_select.c:95` returning 0 backends, strongly overridden by
the composed feature's generated registry. Today SQLite sits in the base
`BACKENDS[]` array *ahead* of that hook. The move:

1. Drop `&hl_db_backend_sqlite` from the base `BACKENDS[]` (`db_select.c:49`)
   and the `cap/db_sqlite.c` + vendored `sqlite3.c` compile from the base.
2. Ship them in `libhull_feature-sqlite.a`, embedded in `hull`, whose generated
   registry fills `hl_db_feature_backends` with the SQLite backend (composing
   alongside any `--with=postgres/mysql/duckdb`, which fill the same hook — the
   existing collector already merges multiple feature backends).
3. Route the **scheme-less / `:memory:` / bare-file default** through the hook
   instead of the hardcoded `&hl_db_backend_sqlite`: the default resolver asks
   `hl_db_feature_backends` for a backend claiming the `sqlite`/`file` schemes
   (or a designated default). No SQLite → a clear "no default DB backend
   composed" error (same shape as the existing scheme-less-without-SQLite error).

## The three hard couplings (and how each is solved)

The `HlDbBackend` abstraction is sound; three things bypass it and are the real
work. (Full coupling map in the epic tracker; key sites below.)

### 1. Default-DSN resolution — the smallest
`db_select.c` and `app_context.c` name `&hl_db_backend_sqlite` /
`hl_db_sqlite_raw` directly. Route both through the hook + a
`hl_db_backend_for_scheme("sqlite")` helper. Mechanical.

### 2. Agent introspection — the hardest
`agent/db.c`, `agent/sql.c`, `agent/helpers.c` are ~100% raw `sqlite3_*`
(`sqlite_master`, `PRAGMA table_info`, `sqlite3_open(":memory:")`,
`hl_db_sqlite_wrap`/`_unwrap`). They are already `#ifdef HL_ENABLE_SQLITE`, so
they **move wholesale into `libhull_feature-sqlite.a`** behind a weak-stub seam
in the base (mirrors `wasm_weakstub.c`): the base carries weak
`hl_agent_db_*` stubs returning "sqlite feature not composed"; the feature's
strong defs win when composed. `hull agent db|schema|migrate|sql` are already
SQLite-only, so this is honest — they light up exactly when SQLite is present.

### 3. The `needs_sqlite` signal — the gate (like `needs_wasm` for #118)
"Needs SQLite" is not a single manifest flag. A multi-signal gate at
`hull build` time:
- **S1** — app declares `hull/search`, or uses `db.udf` (SQLite-only caps).
- **S2** — app ships `migrations/` **and** its default connection is SQLite
  (scheme-less / `:memory:` / `file:` / bare path — i.e. not an explicit
  `postgres://` / `mysql://` / `duckdb://` default).
- **S3** — a genuine app that uses `db` with the default (SQLite) DSN.
- **The `hull` binary keeps SQLite in-base — it is NOT toolchain-force-loaded.**
  `hull test` (always `:memory:`) and the SQLite agent commands need SQLite for
  every app, and the `hull` toolchain is a build tool where a ~2 MB engine is not
  worth removing. So `hull` stays SQLite-full; only **produced app binaries** get
  the SQLite-less base + composed archive. This is the decision the doc's model
  section already implied ("the `hull` binary keeps SQLite … produced app
  binaries drop it") and it sidesteps the `HL_TUI_TOOLCHAIN`-style force-load.

  > A Phase B.4 spike DID wire a `HL_SQLITE_TOOLCHAIN` force-load (mirroring
  > `HL_TUI_TOOLCHAIN`): the SQLite-less `hull` linked, and the direct sqlite
  > path (`hull agent db`) worked, but the in-process `hull test` harness failed
  > to resolve `require("hull.db")` (the serving runtime's `LUA_LOADED_TABLE`
  > lacked the module despite registration). Since keeping SQLite in the `hull`
  > binary is the cleaner end-state anyway, that path was abandoned rather than
  > debugged. Preserved as a local WIP for reference.

A genuinely SQLite-free produced app (explicit non-SQLite default, no
`hull/search` / `db.udf`, no SQLite migrations) links **zero** `sqlite3.*`
(verifiable: `nm app | grep sqlite3_open` → empty), the payoff.

## What moves into `libhull_feature-sqlite.a`

`cap/db_sqlite.c`, `cap/db_udf.c`, vendored `sqlite3.c` (with FTS5), the SQLite
agent introspection (`agent/db.c` + `agent/sql.c` + the SQLite half of
`agent/helpers.c`), and the per-runtime SQLite-UDF binding shims in
`mod_db.c` (already `#ifdef HL_ENABLE_SQLITE`). The base keeps: the vtable,
`db_select.c` (selector + hook), `db_registry.c`, `db_common.c` (the `_hull_*`
namespace guard), `db_dynamic.c`, the generic `db.*` caps, and `migrate.c`
(runs through the vtable, dialect-portable tracking table).

## Cosmo

Cosmo stays monolithic — a fat APE can't force-load a native feature archive, so
the cosmo base compiles SQLite in (exactly as it does the runtimes / HTTP /
WASM). `HL_ENABLE_SQLITE` stays the compile switch; the feature is the
*distribution* unit layered on the native base.

## Phase plan (each phase independently green)

- **Phase A — the seam, NO behavior change.** Route the default-DSN resolution
  and agent introspection through weak hooks; SQLite still compiles into the
  base and provides the strong overrides, so behavior is **byte-identical**.
  The de-risking refactor (like #113 Phase 1 / #118 Phase 0). Verify: full
  `make test` + e2e green; a `grep`/`nm` assertion that the default resolver no
  longer *directly* names `hl_db_backend_sqlite`.
- **Phase B — extract `libhull_feature-sqlite.a` + prove the compose.**
  ✅ SHIPPED (#126 B.1+B.2, #127 B.3). B.1 built the archive (`make
  feature-sqlite`); B.2 added `HL_SQLITE_FEATURE=1` — a SQLite-less DB-core base
  (`nm libhull_platform.a | grep sqlite3_open` → 0, DB core intact) via the
  umbrella decouple; B.3 wired the `sqlite` `FEATURE_SPECS` entry + split
  `agent/helpers.c` so `hull build --with=sqlite` composes the SQLite-less base +
  archive into an app whose `db.query` runs through the composed backend
  (`tests/e2e_feature_sqlite.sh`). The architecture is PROVEN end to end.
- **Phase C — the `needs_sqlite` auto-compose gate.** ✅ SHIPPED (the gate).
  `tool.modules_resolve` exposes `needs_sqlite` (= the app uses `HL_MOD_CAP_DB`,
  mirroring `needs_wasm`); `hull build` auto-infers `--with=sqlite` when
  `needs_sqlite` **and** the resolved base lacks the SQLite backend (probed with
  `nm`, so it's a no-op on a stock SQLite-full base and only fires on a
  `HL_SQLITE_FEATURE=1` base). A db app on a SQLite-less base composes SQLite and
  runs with no explicit `--with` (`tests/e2e_feature_sqlite.sh`). No toolchain
  force-load — the `hull` binary keeps SQLite in-base.
  - **Phase C.2 — the per-runtime `mod_db`-udf bridge split.** ✅ SHIPPED.
    `mod_db`'s SQLite UDF bindings (`sqlite3_value_*` etc., the sole per-runtime
    `sqlite3_*` consumer) used to sit inline in `mod_db.c`, so the embedded
    runtime archive `libhull_feature-<rt>.a` carried 17 undefined `sqlite3_*`
    refs and a whole-archived runtime dragged them into EVERY app — a db-free
    app on a SQLite-less base could not drop SQLite (the size payoff).
    - **C.2a (refactor):** the udf region moved into a per-runtime bridge TU
      `runtime/{lua,js}/mod_db_udf.c` behind a weak `hl_{lua,js}_db_attach_udf`
      seam. `mod_db.c` calls the weak no-op; the bridge provides the strong
      override. `mod_db.o` now carries **zero** `sqlite3_*` refs (nm-verified,
      both runtimes). The JS bridge reuses `js_call_handle` /
      `new_bound_subobject` via a new internal header `runtime/js/mod_db.h`.
    - **C.2b (packaging):** the bridge ships as its own embedded, auto-composed
      archive `libhull_feature-sqlite-<rt>.a` (mirrors wasm-`<rt>`), filtered
      out of the runtime archive. `hull build` composes it whenever the app uses
      a udf-capable DB (`needs_sqlite`, reusing the Phase C signal). The runtime
      archives `libhull_feature-{lua,js}.a` are now **SQLite-free** (0 refs).
    - **Payoff, proven:** on a `HL_SQLITE_FEATURE=1` base, a db-free app composes
      neither the engine nor the udf bridge and links **zero** `sqlite3_*`
      (`nm app | grep sqlite3_open` → empty); a udf app composes the bridge and
      `db.udf` runs; a db app composes the engine and `db.query` runs. Covered by
      `tests/e2e_feature_sqlite.sh`.
    - **Known-orthogonal:** a JS `app.main` + `db.udf` + clean-exit path trips a
      pre-existing `JS_FreeRuntime` GC-leak assertion (reproduces on pre-C.2
      `main`; the DB isn't closed before the runtime is freed). Tracked
      separately; unrelated to the bridge split.
- **Phase D — embed + publish.** Embed the SQLite-less base + the archive in the
  distributed `hull` so a stock install produces SQLite-dropping apps, add the
  composed-feature signing entry (platform domain, like the runtime archives),
  and make the SQLite-less base the default app-build target. Cosmo keeps SQLite
  in-base (fat APE can't force-load a native archive).

## Testing

- Extend `e2e_build_flavor.sh` / a new `e2e_feature_sqlite.sh`: a SQLite-free
  app (explicit `postgres://` default, no search/udf) links **zero** `sqlite3_*`
  and a `hull/search` app composes SQLite and runs FTS5; both runtimes.
- The existing `test_db*`, `e2e_named_connections`, `e2e_postgres`,
  `e2e_mysql`, and the auth-flows suites must stay green throughout (they prove
  the vtable path is untouched).
- `hull test` on a plain app keeps working because the `hull` binary keeps
  SQLite in-base (no toolchain force-load); only produced apps go SQLite-less.

## Non-goals

- Changing the app-facing `db.*` API. Handles-only acquisition, the connection
  methods, `db.udf`, `hull/search` all stay identical.
- Making `db.udf` / FTS5 portable across backends — they remain SQLite-only and
  fail-closed elsewhere (that's a separate, larger piece of work).
- Retiring `HL_ENABLE_SQLITE` — it stays the base compile switch (cosmo needs it
  on; a native single-backend build can still set it off directly).

## Related cleanup (done)

`hull/web/auth-flows` `bump_failed_login` used a raw `INSERT ... ON CONFLICT`
(valid on SQLite/Postgres/DuckDB, **not** MySQL) — a latent portability bug on
the shared stdlib path. Rewritten to a portable atomic `CASE`-based `UPDATE` +
guarded `INSERT` (both runtimes), so the DB-backed stdlib is one step closer to
backend-agnostic regardless of this epic. Landed separately.
