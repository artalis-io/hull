# Design amendment: SQL session control (audit convergence phase 3, §7.1)

- **Status:** Implemented (MySQL / MariaDB / Postgres half of phase 3)
- **Amends:** `docs/design/audit_convergence.md` §7.1 (on branch
  `design/audit-convergence-phase1`), §10, §11
- **Related:** CLAUDE.md "Session reset at every entry boundary",
  "Session-altering SQL", "No transaction across a wait", "Lost connections";
  `docs/app_api_reference.md` "no session state across entries"

## 1. Problem

Rounds 39-41 each found Mediums in the same place: Hull reading MySQL /
Postgres session state out of the SQL text an app sent (`my_track_session`,
`pg_track_session`) and out of status flags, then acting on its guess - resetting
a session only when it believed one was dirty, refusing waits while it believed
a lock was held, refusing a reconnect while it believed a lost session had held
autocommit off. Every new spelling (a `--` comment rule, a form feed, a
multi-assignment `SET`, a `CALL` that changed the session) was a new gap, and
the trackers grew to about 500 lines of classifier and bookkeeping.

## 2. Decision

Stop inferring. Two mechanisms replace the trackers:

1. **Unconditional reset of used connections.** Every connection carries a
   `used` flag, set by any statement and cleared by a reset. The
   stale-transaction guard (`hl_db_registry_guard_stale_txns`, run at every
   entry start, end, park and resume) rolls back an open transaction as before
   and then calls the new `HlDbBackend.reset_session`: MySQL
   `COM_RESET_CONNECTION` followed by `SET NAMES ..., autocommit = 1`, Postgres
   `DISCARD ALL`. The worker pool does the same after every op and job. What a
   session holds no longer matters: whatever an entry left, it is gone before
   the next entry runs.
2. **Refusal of session-altering app SQL** (`cap/db_session_sql.c`), so the
   ordinary spellings of state that a reset would silently drop fail loudly
   instead of half-working within an entry.

`in_txn` stays, read from the server's own status (IN_TRANS / `tx_status`): it
is authoritative and is what the wait gates and `db.batch` need. The failed-
statement `COM_PING` refresh (round 41) stays because it serves `in_txn`, and
the lost-connection-inside-a-transaction refusal stays because it is IN_TRANS
based.

## 3. Decisions on the open points

| Point | Decision | Why |
|---|---|---|
| MySQL `SET TRANSACTION ISOLATION LEVEL` | A bare `SET TRANSACTION` (next transaction only) is allowed; `SET SESSION|LOCAL|GLOBAL|PERSIST TRANSACTION` and assignments to `transaction_isolation` / `tx_isolation` / `transaction_read_only` / `tx_read_only` are refused | The scoped forms persist across statements; symmetric with Postgres `SET SESSION CHARACTERISTICS`, which the spec refuses. The bare form cannot leak: the reset clears a pending next-transaction setting |
| MySQL `SET NAMES` / `CHARACTER SET`, `SET ROLE` | Refused | The connection charset is what `quote_identifier` relies on (audit 20: a gbk session made a doubled backtick part of a character), and the stdlib runs on the same connection within an entry. `SET ROLE` mirrors Postgres |
| Postgres `LISTEN` / `UNLISTEN` | Refused in app SQL | `conn.wait_notify` is the API; Hull's own LISTEN is issued by `pg_wait_notify` through `hl_pg_exec_simple`, below the vtable, on a worker connection. A worker connection that LISTENs is reset with DISCARD ALL's steps minus `UNLISTEN *`, so it keeps its LISTENs between waits |
| Postgres `SET LOCAL ROLE` / `SET LOCAL ...` | Allowed | Transaction-scoped: it ends with the transaction (a useful RLS pattern) |
| Postgres `set_config()` | Refused for `role`, `session_authorization`, `default_transaction_*`, or a first argument that is not a plain literal | The function form of the same SETs |
| Postgres `PREPARE TRANSACTION` | Refused | The analogue of MySQL XA; it outlives the session |
| Postgres `U&"..."` | Refused | Its escapes can spell any name (the namespace check already refuses it for app SQL) |
| Migration lock | Hull's `GET_LOCK('hull_migrate')` / `pg_advisory_lock` and their release run with a thread-local trust flag (`hl_db_session_sql_trust`), set only in `migrate.c` | The migration files themselves are app SQL and stay checked; the lock is held by a pinned handle, which is never reset |
| Manifest shape | `databases.named.<x>` is a DSN string or `{ dsn = "...", session_control = true }` (JS also `sessionControl`) | Keeps every existing manifest valid |
| `session_control` on `default` | Refused at load (`hl_manifest_check_env_refs`) | The default connection is the stdlib's |
| `session_control` and the stdlib | The backend refuses a stdlib (ns-trusted) call on a session_control connection | The stdlib's SQL must not run inside app session state (autocommit off, a lock) |
| `session_control` and the worker pool | Pooled per worker like every connection (keyed by DSN); the op carries `session_control`, which the worker sets on its handle for the op only, and the connection is reset after the op | The per-op reset gives a clean session per op, so a dedicated, unpooled worker connection buys nothing |
| `session_control` lost mid-entry | Calls refuse until the entry ends, then reconnect | Its session state is the app's; a silent reconnect would drop it. Hull does not guess what it held |
| MariaDB `session_control` at the boundary | Closed rather than reset | Whether `COM_RESET_CONNECTION` ends a `BACKUP LOCK` / `STAGE` is undocumented; ending the session does |
| Reset budget | The guard's rollback and reset run under a 2 s cleanup budget of their own | A tripped run would abandon the connection at the first reply frame; an unbound wait never stops |
| Reset cost | Only used connections are reset: an entry that touched no database pays nothing; one that did pays one round trip per connection (MySQL: two packets, COM_RESET + SET) | Acceptable against the class of bugs removed |

## 4. The classifier

One pass a word at a time with the two previous words. Statement boundaries
are `;`; a word anchors a statement when it is the statement's first (and, in a
MySQL compound body, the word after BEGIN / THEN / ELSE / DO / LOOP / REPEAT).
Pair rules (`LOCK TABLES`, `WITH READ LOCK`, `XA START`, ...) and the lock
function names match anywhere; the `SET` rules apply to an anchoring `SET`.
Quoted identifiers are words (a quoted function name still matches); string
literals and comments are skipped with each dialect's rules (MySQL `#` and
`--`+whitespace comments, non-nesting block comments; Postgres nesting block
comments, `--` always).

Conservative reading: a backslash anywhere, a MySQL executable comment, or an
unterminated quote / comment / dollar quote makes the text "raw" - quotes and
comments count as code and every word anchors. A Postgres dollar-quoted body
(a DO block or function body runs it) and the statement a MySQL `PREPARE` /
`EXECUTE IMMEDIATE` takes from a string are always read raw. The classifier is
fuzzed in `fuzz_sql_kw` (deterministic, case-insensitive, and a refused
statement appended after a newline is refused whatever precedes it).

Known limit, by design: SQL assembled at run time (`CONCAT` + `PREPARE`,
`EXECUTE format(...)` in a DO block, a stored procedure that runs `SET
autocommit = 0` or `GET_LOCK`) is not seen. The reset at the boundary is the
guarantee; the refusal only makes the common spellings fail loudly. A
procedure-made change lasts at most until the entry ends or waits.

## 5. What was deleted

`db_mysql.c`: the tracked lock / XA / backup / named-lock fields, `MyTrack`,
`my_track_word`, `my_scan_raw_words`, `my_sql_track_unreadable`,
`my_track_session`, `my_session_leftover`, `mysql_session_held`,
`mysql_session_dirty`, `MyRestore` / `my_restore_kind`, `my_lost_held`,
`ac_off_acked` and the held-state branch of `my_ready`, and the guard's
leftover diagnosis. `db_postgres.c`: `adv_locks`, `pg_track_word`,
`pg_track_session`, `pg_session_dirty` and the guard's
`pg_advisory_unlock_all`. `db_backend.h`: `session_dirty`, `session_held`,
`hl_db_session_dirty`, `hl_db_session_held`, `hl_db_holds_txn`,
`HlDbTxnSnap.held`. `db_sql_kw.h`: `hl_sql_scan_words`. The audit 39 / 40 / 41
e2e legs (`tests/lib/hull_db_session_audit{39,40,41}.sh`) and the unit tests of
the trackers.

## 6. Testing (§10)

- `test_db_backend` `db_session_sql.*`: every class in its ordinary spellings,
  the conservative cases (executable comments, backslashes, unterminated
  text, MySQL `--` without whitespace, dollar bodies, `U&`), and ordinary
  statements that only name the words (strings, comments, columns, user
  variables, `SET LOCAL`, `_xact_` locks).
- `test_mysql_conn` / `test_pg_conn` (mock servers over a socketpair):
  refusal before anything is sent, the trust flag, session_control taking the
  statements and refusing the stdlib, reset only of a used connection, rollback
  then reset, abandonment when the reset fails or a transaction survives its
  ROLLBACK, the pinned exemption, LISTEN-preserving Postgres reset, and lost
  connections (shared reconnects; session_control refuses until the entry
  ends).
- `tests/lib/hull_db_session_control.sh`, run by `e2e_mysql.sh` (MySQL 8.0,
  8.4, MariaDB 11) and `e2e_postgres.sh` in both runtimes: every refused
  statement errors clearly; BEGIN / COMMIT and `db.batch` work; a session
  variable set in one request is clear in the next; a session_control
  connection takes LOCK TABLES / autocommit off / GET_LOCK (advisory lock /
  session characteristics on Postgres) and the next entry on it starts clean,
  with the lock free from another session; migrations take their lock; a
  migration file that takes one fails the start; a server with global
  autocommit off keeps plain writes. `conn.wait_notify` keeps its e2e leg.

## 7. Risks (§11)

Apps relying on session state across requests break: a startup `SET
search_path` (app.main is an entry, so the reset clears it before serving), a
session-level advisory lock or `GET_LOCK` held between requests, a temporary
table reused across requests, `SET autocommit = 0` idioms. The CHANGELOG
migration notes cover each; `session_control` covers what genuinely needs a
session, bounded to one entry.
