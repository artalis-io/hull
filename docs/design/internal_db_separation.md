# Design amendment: Hull-owned internal database (audit convergence §7.2)

- **Status:** Implemented (rollout phase 4 of the audit-convergence design)
- **Amends:** §7.2 "Internal data separation" of the audit-convergence design
  (`audit_convergence.md` beside this file, from PR #977). Read it with §10 (testing) and §11 (risks).
- **Code:** `src/hull/cap/db_internal.c`, `include/hull/cap/db_internal.h`,
  `src/hull/cap/db_registry.c` (default internal DSN), `src/hull/sandbox.c` (grants),
  `tests/e2e_internal_db.sh`

## 1. What §7.2 asked for

`databases.internal` defaults to a Hull-owned SQLite file at `<app>/.hull/internal.db`, so
app database files never contain `_hull_*` tables and a planted or cloned app database has
no internal data to reach. Existing apps are migrated on first start;
`--keep-internal-in-app-db` keeps the old layout for one release.

This amendment records where the implementation differs from that text, and why.

## 2. Where the internal database lives

The default internal database follows the **default database**, not only the app directory:

| Default connection (`-d`, or the implicit `<app>/data.db`) | Default internal database |
|---|---|
| a SQLite file inside the app directory | `<app>/.hull/internal.db` (`.hull` made `0700`) |
| a SQLite file outside the app directory (a deploy volume) | `<that file>.hull-internal`, beside it |
| `:memory:` (`hull test`, `hull agent`, an `app.main` program with no `-d`) | a separate `:memory:` database |
| Postgres / MySQL / MariaDB / DuckDB / `$VAR` / none | none: the old layout (see section 5) |
| the internal file itself (`-d .hull/internal.db`) | none |

**Why not always `<app>/.hull/internal.db`.** Hull's own deploy generators put the data on a
volume outside the app directory: the Dockerfile runs with `WORKDIR /srv` and
`--hull-d /data/data.db` on `VOLUME ["/data"]`, and the systemd unit makes only `data_dir`
writable (`ProtectSystem=strict`, `ReadWritePaths=`). `/srv/.hull/internal.db` would sit in
the container's writable layer (sessions, roles and TOTP secrets lost on every redeploy),
and under the systemd unit it could not be created at all. Beside the database, it is
persisted, backed up and made writable exactly where the operator already does that for the
app's data. No app capability reaches a file outside the app directory (fs paths, `db.open`
and `databases.named` are confined to the app tree), so this placement is as unreachable for
app code as `.hull`.

**Why not `$HOME/.hull/apps/<hash>/internal.db`.** Considered and rejected: a service account
often has no writable home (`DynamicUser=`, `ProtectHome=yes`, a container's ephemeral
`/root`); the data would silently move away from the app's own data and backups; moving or
renaming the app directory would change the hash and orphan every session and role; and
`~/.hull` holds the cache keys, which app processes must not be granted.

**Against the threat model (§4).**

- *A1 (remote client):* unchanged surface; internal rows are no longer in the app database,
  so a stored view an app's SQL could reach has nothing internal to read.
- *A2 (malicious repo / planted file):* a cloned tree can ship `.hull/internal.db` just as
  it can ship `data.db`, so the internal file is not more trusted than the app database
  was. What changes: the stdlib never reads `_hull_*` tables from the app database, and
  once the move is recorded (section 4) internal tables that reappear there are ignored,
  not merged. `.hull` (or the leaf / sidecars) being a symbolic link is refused on every
  open (`hl_db_path_has_symlink`, base = the app directory), and `.gitignore` /
  `.dockerignore` templates already exclude `.hull/`.
- *A3 (app author):* the file is in `<app>/.hull`, which every capability refuses (the fs
  resolver by name and inode, `db.open` through the fs policy), and a `databases.named`
  file DSN naming `.hull` in any letter case is now refused too (`confine_manifest_dsn`).
  Only a declared `databases.internal` may point into `.hull`. App SQL on the internal
  connection is impossible: no binding hands it out.

## 3. Kernel sandbox grants

App code runs in the same process as Hull, so the kernel sandbox cannot tell them apart;
the capability layer is the boundary for `.hull` (as it already was for the dev-session
sidecars, audit 37). The grants only make Hull's own access work and keep the rest of
`.hull` closed where the kernel can express it:

- **macOS (Seatbelt):** after the `(deny file-read* file-write* (subpath APP_HULL))` rule,
  which stays, the profile allows `file-read* file-write*` on four literals - the database,
  `-wal`, `-shm`, `-journal` - and `file-read-metadata` on the `.hull` directory itself (the
  registry's lstat re-check). SBPL's last matching rule wins, so nothing else in `.hull` is
  allowed (unit test `sandbox_internal_db.*`, e2e on macOS with the sandbox active).
- **OpenBSD (unveil):** `.hull` stays hidden (`unveil(.hull, "")`); the four files are
  unveiled `rwc` individually.
- **Linux (Landlock polyfill):** Landlock cannot hide `.hull` and a file rule cannot let
  SQLite create a sidecar, so the `.hull` directory is unveiled `rwc` when the internal
  database is there (the beside-the-database case mirrors the `-d` file grant). The
  capability layer remains the boundary on Linux, as before.
- The internal database is opened (and migrated) before the phase-2 sandbox applies.

## 4. Migration

Runs once the manifest is wired, the module set resolved and a signed policy checked, and
before the sandbox applies, in both runners (`hl_db_internal_startup`, serve.c and
serve_cli.c), unless `--no-migrate`. An app whose resolved modules reach no internal table
(`hl_module_set_uses_internal_db`), whose database holds none and that has no internal file
yet gets no file at all.

- **Which tables move** (the modules that already reached `hull.db._internal`): session,
  audit-log, rbac (4 tables), totp (7), idempotency, auth-flows (3) - 17 tables,
  `hl_db_internal_tables()`.
- **Which stay on the app connection, by design:** outbox, inbox and jobs (their rows must
  commit in the same transaction as the app's writes), attachment (a delete inside the
  app's own `db.batch` must roll back with it, audit 5 DA-M1 - so its two tables stay
  beside the app's, on a declared `databases.internal` as before or else the default
  connection), search (its FTS index reads app tables), the kv SQL store (on the
  connection the app opens it with), and `_hull_migrations` (it tracks the app's own
  migrations and is written only by Hull's trusted statements).
- **Idempotency keys do move**, as they already did with a declared `databases.internal`:
  `idempotency.respond` inside the app's own `db.batch` then commits separately from the
  app's writes (a crash between the two leaves the key in flight until its lease expires,
  and the retry is answered 409 meanwhile, never replayed wrongly). So **"app database files never contain `_hull_*` tables" holds only
  for the movable tables**; the app database still holds these, guarded by the SQL-text
  check and (SQLite) the authorizer, with the open-time stored-object checks as before.
- **The users table of auth-flows' `standard_users` adapter** is an app table (made by the
  app's migrations). It stays on the app connection unless `databases.internal` is
  declared (then it lives there, as before): `hull.db._internal.app_connection()`, the
  same accessor attachment uses.
- **Procedure (SQLite to SQLite):** for each table in dependency order, in one transaction
  on the internal database: create it from the app database's own DDL (table, then its
  indexes) when missing - only `CREATE TABLE` / `CREATE [UNIQUE] INDEX` statements are
  replayed, since a planted file can hold any text in `sqlite_master` and a replayed
  trigger would keep firing on Hull's writes - and copy every row of the columns both
  sides have with `INSERT OR IGNORE`. Only after every copy committed are the tables dropped from the app
  database, referrers first (rbac's foreign keys). Then the internal database records the
  move (`_hull_internal_meta`, key `app_db_moved`).
- **Drop, not rename.** A renamed copy would keep session tokens and TOTP secrets in the app
  file, which is what §7.2 removes. Reversibility comes from the keep flag instead.
- **Crash safety.** A crash between tables leaves some tables on both sides; the next start
  has no move record, copies them again (`INSERT OR IGNORE`: nothing lost, nothing doubled)
  and drops them. A failed copy rolls its transaction back, drops nothing, and stops the
  app with an error naming the cause (it must not start without its sessions and roles).
- **After the move:** internal tables that turn up in the app database again (a `data.db`
  restored from an old backup, or planted) are not merged into the live internal data; a
  WARN names them.
- **Rows written before the move.** A stdlib `init()` at app top level runs before the
  migration and may create (empty) tables, or seed rows, in the internal database first;
  `INSERT OR IGNORE` merges the app database's rows into them, keeping a seeded row on a key
  conflict.
- **`--keep-internal-in-app-db`** (a runtime flag in `hl_runtime_flags()`, a downgrade, so a
  built binary takes it only as `--hull-keep-internal-in-app-db`): the internal connection
  is the default one again, and tables an earlier start moved are moved back into the app
  database the same way, clearing the move record, so the next start without the flag moves
  them in again. Kept for one release.

## 5. Postgres, MySQL and DuckDB default connections

§7.2 says these follow the same rule. This implementation does **not** give them a local
SQLite internal database by default, and moves nothing:

- A Postgres / MySQL deployment commonly runs several app instances against one database. A
  per-instance local SQLite file would split sessions, roles, lockouts and idempotency keys
  across instances - a login on one instance unknown to the next. That is a correctness
  regression, not a hardening.
- Copying from a network database also needs dialect translation of the DDL (Postgres /
  MySQL / DuckDB types, DuckDB identity sequences) into SQLite.

So with a network or DuckDB default and no `databases.internal`, the internal tables stay on
the default connection, and the existing startup WARN still says to declare
`databases.internal` - a separate database / schema under a role the app's connection has no
grants on, which is the real separation for those backends. Manual steps (Postgres):

1. Create a role and schema the app's role has no grants on (`CREATE ROLE hull_internal
   LOGIN ...; CREATE SCHEMA hull_internal AUTHORIZATION hull_internal; REVOKE ALL ON SCHEMA
   hull_internal FROM PUBLIC;`).
2. Stop the app; copy each table in `hl_db_internal_tables()` that exists from the app schema
   into `hull_internal` (`CREATE TABLE hull_internal.t (LIKE public.t INCLUDING ALL);
   INSERT INTO hull_internal.t SELECT * FROM public.t;`), then drop it from the app schema.
3. Declare `databases.internal = "$HULL_INTERNAL_DSN"` (and the variable in `secrets`) and
   start the app.

MySQL is the same with a second database and `CREATE TABLE ... LIKE` / `INSERT ... SELECT`.
Open question 3 of the design (DuckDB read-only sharing) no longer arises: no default
configuration shares a DuckDB file with internal data unless the app declares it.

## 6. Tests

- Unit (`tests/hull/cap/test_db_registry.c`, `db_internal.*`): default-path resolution for
  every kind of default DSN; the registry's default internal connection (`.hull` made 0700,
  inactive under the keep flag, a declared `databases.internal` wins, a symlinked `.hull`
  refused and nothing created through it); a `databases.named` DSN naming `.hull` refused in
  any case; the move (rows, BLOBs, indexes, foreign keys, the crash-state merge, app and
  non-internal `_hull_*` tables left alone).
- Unit (`tests/hull/test_sandbox_grant.c`, macOS): the Seatbelt grant names only the four
  files, after the `.hull` deny.
- E2E (`tests/e2e_internal_db.sh`, `make e2e-internal-db`, both runtimes): fresh app; an
  existing app migrated on first start; the keep flag and back; the crash state completed;
  planted tables ignored; `-d` outside the app; a declared `databases.internal` wins;
  `databases.named` refused; on macOS, the same with the sandbox active.
- Not covered: a real `kill -9` between two tables (the e2e builds the exact on-disk state
  such a crash leaves instead, which is deterministic); Postgres / MySQL migration (not
  implemented, section 5).
