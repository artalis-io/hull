# Changelog

All notable changes to Hull are documented here. Format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); Hull adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html) on the
public surface (`hull` CLI flags, embedded stdlib API, manifest schema,
release-artifact layout).

## [Unreleased]

### Fixes

- **Email change could probe whether addresses exist, without limit** (audit 33, both
  runtimes). A taken target address is answered 409 `email already in use`, and that check ran
  before the account's email-change request limit, so a signed-in user could test any number of
  addresses. It now runs after that limit (and before the per-recipient one, so a probe does
  not spend the address owner's mail allowance). `docs/app_api_reference.md` said email change
  answered alike for taken and free addresses; it now says what it does.
- **An app could kill the process by truncating a database file SQLite holds open** (audit 33,
  both runtimes). SQLite maps a WAL database's index (`<db>-shm`) `MAP_SHARED` in every
  connection, and that mapping is not one of the fs.mmap SIGBUS guard's: `compute.stream("m",
  input, {file = "data/app.db-shm"})` with an `fs.write` grant on `data`, or an `fs.write`
  through an in-root symlink to it, truncated the file in place and the next query died of
  SIGBUS - every connection with it. Every connection the SQLite backend opens now registers
  its database file (`cap/db_live_files.c`), and the fs capability refuses, with
  `database_file_in_use`, to truncate or rewrite in place (`hl_cap_fs_open_write_fd`, which
  compute.stream and the in-place `fs.write` use), replace by rename (`fs.write`, `tar.extract`)
  or delete a file whose device and inode are a live database's or its `-wal` / `-shm` /
  `-journal`'s - a replaced or removed sidecar splits the WAL index or loses committed
  transactions. An in-place write to a leaf NAMED like a sidecar is refused whatever it is.
  Covered by `make e2e-db-live-files` and `test_fs`.

- **A locked login address told a registered account from an unknown one** (audit 32, both
  runtimes). An unknown address is counted under a key made from its own spelling (ASCII case
  folded), a known one under its account, whose lookup may fold more (accents under MySQL's
  `utf8mb4_0900_ai_ci`). Failures spread over accent variants therefore locked a registered
  account's row and none of an unknown address's, and `/login` skipped both counter writes
  for an address it found locked - one commit less, visible in the response time. A locked
  address now runs the same statements in a hold mode that leaves the count and the lock as
  they were, so locked and unlocked addresses do the same database work.
- **DuckDB returned another column's value for short strings** (audit 35, both runtimes).
  DuckDB stores a VARCHAR / BLOB of up to 12 bytes inline in the vector's string struct; the
  decoder copied that struct to its own stack and pointed the value into the copy, which was
  dead once it returned. Short columns of one row then read whatever the slot held next -
  `SELECT 'x1' a, 42 n, 'y2' b` gave `a = 'y2'` - in `conn.query`, `db.async`,
  `worker.dispatch` and `table_columns`: short ids and role names could be confused. The value
  now points into the chunk's vector, which outlives the row callback.
- **A JS `tui.run` loop outside app.main renewed its own run limit** (audit 32). `tui.pollSync`
  re-arms the instruction budget and the run deadline on each event (and after 50 ms of
  waiting), meant for app.main's interactive session; but a timer, a WebSocket callback or a
  `hull:_task` task of a serving app that admits TUI could run the same synchronous loop and
  re-arm too, holding the event loop for as long as the terminal produced events or idle
  ticks. Only app.main's own run re-arms now; any other entry keeps the single run it got.
- **JS: a run failed for holding a transaction across a wait still resumed later** (audit 33).
  Audit 32 dropped a failed run's queued jobs, but forgot the run's tag as soon as its
  request was answered, and the 32-slot list it lived in overwrote its oldest entry. A
  reaction the run had left on a promise Hull does not drive was queued only when that
  promise settled - from a later request - and then ran: the handler's code after the
  `await` went on in autocommit and its writes were committed, after a 500 had reported the
  transaction rolled back. A handler that awaited such a promise holding a transaction, with
  no other run's job involved, was answered an empty 200 and continued the same way; and
  with a Hull op also in flight (`const s = hull.sleep(..); BEGIN; ...; await gate; ...;
  await s`) its code ran on as soon as the gate settled, before the sleep's resume failed
  the run. Every place a run is found waiting in a transaction now fails its tag, a failed
  tag is kept until no pending job and no reaction on a pending promise carries it (a sweep
  over QuickJS's live jobs and promises, HULL PATCH 0008's `JS_ForEachLiveJobTag`), and the
  undriven-promise case is answered as a failed run (500; app.main exits 1, a timer is
  rescheduled, a ws callback ended). Regression legs in `tests/e2e_js_runs.sh`.

- **DuckDB app SQL could read the database file, and the storage statistics of `_hull_*`
  tables** (audit 34). DuckDB keeps the open database file, its `.wal`, `.wal.checkpoint`
  and `.wal.recovery` sidecars and its `<db>.tmp/` temp directory in `allowed_paths` /
  `allowed_directories` whenever external access is off (it re-adds them when external
  access is turned off, and refuses to change `allowed_paths` after), so
  `SELECT content FROM read_blob('app.duckdb')` returned the raw file - `_hull_*` rows
  (session ids, TOTP secrets) included - and `COPY ... TO 'app.duckdb.wal'` could write
  over the WAL. And DuckDB constant-folds table-function arguments, so
  `pragma_storage_info(concat('_', 'hull_sessions'))` (any spelling: PRAGMA, CALL, FROM)
  returned column min / max values of a `_hull_*` table, and `pragma_table_info` /
  `pragma_show` / the `histogram` table macro its schema or rows, past the `_hull_*` text
  check. The DuckDB backend now refuses: SQL naming the database file (any spelling of its
  path holds its name, case-insensitive), an `E'...'` literal with a backslash, any call of
  a file-reading table function (`read_*`, `parquet_*`, `glob`, `sniff_csv`) under no fs
  policy, and otherwise one whose path is not a plain literal (or list of them) or is a glob
  reaching the database directory, a COPY whose target is a `(constant expression)` or such
  a glob, `storage_info` in every spelling, the profiling / logging calls DuckDB runs past
  `lock_configuration`, and a table-name function called with anything but a plain literal
  or name. Classifier tests in `test_db.c`, real-engine legs in `tests/e2e_feature_duckdb.sh`.

- **JS: a run failed for holding a transaction across a wait kept running** (audit 32). The
  audit-31 check rolls back the transaction of a run whose stretch of jobs ends while it is
  open and marks the run failed, but only a later async resume read that mark: the run's own
  jobs queued after it, in the same drain, still ran. A handler that ran `BEGIN; INSERT a;
  await p` with `p` settled by another request's code had `a` rolled back, then went on to
  `INSERT b` in autocommit and was answered 200 (its stray COMMIT succeeds on Postgres /
  MySQL). The job drain now drops a failed run's jobs unrun (QuickJS
  `JS_DiscardPendingJob`, HULL PATCH 0008), and the run ends failed wherever its completion
  is decided: a request is answered 500, app.main exits 1, a timer or WebSocket callback ends
  as a failed run does, its continuations share a record that stops them, and the failed-run
  list (a fixed ring of 32) frees the run's slot once nothing of it is left to stop.
- **JS: an entry that ended holding a transaction let its waiting code run on later**
  (audit 35). The run-failure checks of audits 31-34 ran only where a run was found waiting:
  an entry that simply ended - a synchronous handler, an async handler already settled, a
  middleware, a timer, an SSE or WebSocket callback, app.main's own stretch - finished with a
  bare stale-transaction rollback. A chain it left un-awaited (`(async () => { BEGIN; INSERT
  a; await gate; INSERT b; COMMIT })()`) had `a` rolled back unseen, and when a later request
  settled `gate` the chain's code ran on in autocommit and `b` was committed. Every JS entry
  now ends through one helper, `hl_js_entry_end_txn_check`, which rolls the transaction back
  AND fails the entry's run tag, so that code never runs (handler, middleware, timer, SSE,
  ws-server and ws-client callbacks, app.main, `hull:_task` tasks, async and multipart
  resumes, `hull test` cases). An entry that merely forgot to COMMIT keeps its outcome; the
  rollback is now logged. Regression legs in `tests/e2e_js_runs.sh`.
- **A `db.async` result of zero-column rows was freed twice** (audit 32). Postgres accepts an
  empty select list (`SELECT FROM generate_series(1, 20)`), and the worker's result copy grew
  its row store to `rows x 0` bytes. On glibc `realloc(p, 0)` frees `p` and returns NULL; the
  copy read that as "too many rows" and the result free freed the block again, so one
  `conn.async.query` past 16 empty rows aborted the process. A hostile server (or a MITM
  under the default `sslmode=prefer`) could send the same reply to any `db.async` query.
  A row now takes at least one cell of room, and that size is charged to the result's byte
  cap and the op's budget, so empty rows are no longer free up to the 100k-row cap.
- **DuckDB `IMPORT DATABASE` / `EXPORT DATABASE` / `COPY FROM DATABASE` got past the `_hull_*`
  check** (audit 32). With a DuckDB file policy (a directory granted in both `fs.read` and
  `fs.write`), `EXPORT DATABASE` wrote every table - `_hull_*` ones included - to CSV files
  `read_csv` read back, `IMPORT DATABASE` ran the `schema.sql` / `load.sql` an app had written
  with `fs.write` (forging sessions or roles), and `COPY FROM DATABASE` copied every table
  into an attached file, none of them naming a table. The DuckDB backend now refuses these
  statements, and the `import_database` / `export_database` / `copy_database` pragma names,
  under every file policy (`hl_cap_db_sql_reads_whole_database`, read like the audit-25
  dynamic-SQL check: any case, comments between the words, any statement of the text).
- **A losing connect attempt left its watcher on a closed descriptor** (audit 33, `hull/ssh`
  and every other `cap/net_stream.c` stream). If a name resolved to several addresses and a
  later one won while an earlier attempt was still in flight (for example, a dual-stack host
  with IPv6 blackholed past the 250 ms stagger), the losing descriptor was closed without
  removing its `attempt_ready` watcher. The registration stayed on a closed fd number. On
  epoll, the next socket to get that number could not be watched (`EPOLL_CTL_MOD` failed), so
  a connect through it failed or a connected stream's reads and writes never woke. Each race
  poisoned one more number for the life of the process. On the poll backend, that socket's
  readiness went to a callback for the old stream. The watcher is now removed before the
  descriptor closes. `test_net_stream_race` drives the race with a fake resolver (blackhole
  first, loopback second) on both backends.

- **JS: an un-awaited chain that waited holding a transaction resumed later** (audit 34).
  When a fire-and-forget async chain (`(async () => { ... })()` from a handler) stopped
  with a transaction open, Hull rolled it back and failed only the chain's run record, not
  its run tag, whenever the chain had a record - it had been wired by an earlier resume
  (`await hull.sleep(5); await hull.sleep(5); BEGIN; ...; await gate`), or it started a
  Hull op beside the BEGIN. The record stops only Hull ops' resumes: the reaction left on
  `gate`, a promise Hull does not drive, kept the tag and ran in the drain of the later
  request that settled it, so the code after the `await` went on in autocommit and its
  writes were committed (a stray COMMIT succeeds on Postgres / MySQL). The run's tag is now
  failed as well. The tag is the entry's, so other un-awaited chains of the same entry stop
  with it - they are that run's code; an entry still waiting (an async handler, app.main, a
  timer, a ws callback) is failed as one that waited holding a transaction. Regression legs
  in `tests/e2e_js_runs.sh`.

- **`hull agent eval` / MCP `hull_eval` printed whatever a Lua app's `hull.json` returned**
  (audit 35). The Lua result was encoded by `json.encode` inside the app's own VM - with
  `require`, `pcall` and `tostring` taken from app globals - and the string copied into the
  output unchecked. A cloned app that replaced `require("hull.json").encode` at top level
  made `hull agent eval "1+1" <app>` write raw ESC / OSC sequences to the terminal (screen
  and clipboard injection) and append forged JSON records an agent read as the real result;
  through MCP the client decoded the forged result. The snippet now runs as its own chunk
  under a C `lua_pcall`, and its value is encoded in C with raw accessors only (no Lua code,
  no metamethods; depth 32, 4 MiB, a cycle refused; a function / userdata as `"<function>"`;
  an error that is not a string named by its type). The eval output - JS included - also
  escapes DEL and the C1 controls (`\u009b` CSI) a JSON encoder leaves raw in a string. An
  app no longer has to declare `hull/json` for eval to work. Regression legs in
  `tests/e2e_agent.sh` and `tests/e2e_mcp.sh`.

- **`ws.connect` clients were uncounted, each holding up to 20 MiB off the heap** (audit 32).
  Every Keel client connection keeps an outbound drain (up to 4 MiB) and a message reassembly
  buffer (up to 16 MiB) on Keel's allocator until it is freed, and nothing limited how many a
  VM kept: a loop of `ws.connect` to a peer that stops reading held gigabytes beside a 64 MB
  heap. A VM now holds at most `HL_WS_CLIENT_MAX_LIVE` (64) clients - past it `ws.connect`
  raises (after one garbage collection) - and each client's frame limit is 4 MiB (or the heap
  limit, if lower). An ended client's Keel connection is freed on the next loop turn, not when
  its object is collected, so a closed client the app still references gives its slot back.
  Both runtimes. A message over 4 MiB from a peer now fails the connection (1009).

- **`hull build` linked the app through a symbolic link at its output** (audit 32). The tool
  layer's path check canonicalised a path it could not `realpath` through its nearest existing
  ancestor, so a dangling link in the not-yet-existing tail - the leaf included - passed as its
  nominal spelling, while the linker (`cc -o`, ld64 / GNU bfd) wrote through it to the target. A
  cloned app committing `app -> ~/.local/bin/git` had a plain `hull build` drop an executable
  there; nothing else stopped it where the tool mode has no kernel sandbox (macOS, Windows,
  Linux without Landlock). The check now refuses any symlink in that tail, and `hull build`
  refuses an output path that is a symbolic link with a message saying so.
- **Tool commands printed app text raw** (audit 32). The error an app's top level raised while
  its manifest was read, and file names from the app's tree, reached the terminal unescaped
  under `hull build`, `manifest`, `check`, `modules`, `analyze` and `deploy`, so an ESC / OSC 52
  sequence in them rewrote the terminal or set the clipboard - the `hull test` fix of audit 31,
  for the tool VM. `tool.extract_manifest_lua` / `_js` now return that error escaped, and
  `build.lua` prints file names through the new `tool.escape` (the `hl_log_escape_line` rules:
  CR / LF as `\r` / `\n`, other controls but tab as `\xNN`).
- **A Lua coroutine whose async wait was abandoned could still be resumed** (audit 33). When
  the runtime could not build an op's result (out of memory) it failed the handler without
  resuming it, and a cancelled op (its request gone, the pool torn down) did the same; either
  way the op was freed while the coroutine stayed suspended inside the binding's
  `lua_yieldk`, and it was only unparked. An app that had kept `coroutine.running()` could
  `coroutine.resume` it from a later request: `db.async` / `compute.async` /
  `instance:async_call` continuations then read the freed op (a use-after-free, seen under
  ASan in `lua_db_async_k`), and the dead handler ran on inside another request's run. Such a
  coroutine is now marked abandoned, and `coroutine.resume` / `coroutine.close` refuse it for
  good; every binding continuation also checks that it is the runtime's own resume before it
  reads its context.
- **The fetch cap turned the pwned-password check off and made outbox drop work** (audit 35).
  Audit 31 capped each VM at 64 in-flight fetches, but the stdlib recognised the runtime's
  refusals by their message text and did not know the new one: `hull/web/pwned` took "too
  many requests in flight" for an HIBP outage and failed open ("not pwned"), so a client
  holding ~64 slow fetches made register / reset / password change accept breached
  passwords; `outbox.flush` counted it as a failed delivery for every claimed row, and after
  `max_attempts` the rows were marked failed without ever being sent. Every runtime refusal
  to start or wait on an async op now carries a stable code - `busy` for an admission cap
  (the fetch cap, the VM's off-heap copy total for `http.fetch` / `db.async` /
  `worker.dispatch` / `smtp.send` / `compute.*` / `gpu.*`, the `ws.connect` and blob handle
  caps), `wait_refused` for a wait the runtime refuses (a transaction open, middleware, a
  parked multipart read, outside a handler, shutdown): JS errors get an own `e.code`, Lua
  error strings start with `"[hull:<code>] "` (raised without the `chunk:line:` prefix). The
  messages are otherwise unchanged. The stdlib tests the code (`hull._refusal` /
  `hull:_refusal`), never the text: pwned re-raises a refusal, and `outbox.flush` gives the
  claim back without counting an attempt - on `busy` it stops and returns `busy = true`,
  leaving the rest pending. Codes documented under "Runtime refusal codes" in
  docs/app_api_reference.md; covered by `tests/e2e_refusal_codes.sh` (both runtimes).

- **GPU uploads with no dispatch grew device memory without bound; a stalled device still
  took reads, copies and writes** (audit 34). wgpu copies each `gpu.buffer` / `gpu.texture`
  upload into a staging buffer it frees only when a later queue submission completes, and no
  write path submitted: a loop of 16 MB `gpu.buffer` writes held 4.8 GB of device memory in
  one run under the default budget, with a 64 MB VM heap. The cap layer now counts every
  write's staging copy per device and, once 64 MiB are queued
  (`HL_GPU_MAX_QUEUED_WRITE_BYTES`), has the backend submit an empty command buffer and wait
  for it (bounded, asking the run); any dispatch, pipeline, read or copy that completes
  releases them too. The same loop now peaks at ~255 MB. And audit 33's stall gate covered
  only dispatch / pipeline: on a device a timed-out call left running, every
  `gpu.buffer_read` / `texture_read` waited out its 5 s timeout behind the hung work, holding
  the device mutex (and the event loop on the sync path), and left a staging buffer nothing
  counted. Reads, copies and writes are now refused at once with `gpu_busy`
  (`HL_GPU_ERR_BUSY`); a read, copy or write flush that times out marks the device stalled and
  keeps its staging bytes, and the queued writes it carried, on the device's live total until
  the queue drains; a timed-out map counts as a timeout; and a dispatch's read-back waits
  with the dispatch's own clamped timeout.

- **`--verify-sig` loaded bytecode from the shared code cache** (audit 31). The signature and
  the disk gate hash an app's SOURCE, but the Lua / QuickJS bytecode and compiled-template
  caches are sealed with `~/.hull/cache.key`, which every app process of the user holds: a
  compromised sibling app could seal forged bytecode that a `--verify-sig` app then ran after
  passing every check. Both app entry points (the server and the `app.main` runner) now turn
  the code caches off under `--verify-sig`, before any VM exists; such a run compiles from
  source and neither reads nor writes the cache.

- **`hull test` printed app text raw** (audit 31). A test name, an error message or a test
  file name holding ESC / OSC sequences or a newline could forge PASS lines, rewrite the
  terminal or write the clipboard (OSC 52). They are printed escaped now (`\x1b`, `\n`), as
  log records are; so are `hull migrate status`'s migration names.
- **Compute AOT built by `hull build` could read and write host memory** (audit 31). `hull build`
  ran wamrc without `--bounds-checks`, and on a 64-bit target wamrc then drops the software
  bounds checks and relies on a guard region past linear memory. Hull's WAMR runtime reserves
  none (it has no hardware bound check), so a module's loads and stores at `mem_base + addr`
  went unchecked. `hull build` now passes `--bounds-checks=1 --stack-bounds-checks=1` (the flag
  set is in the AOT cache key, so no entry compiled without them is reused); WAMR patch 0007's
  stamp (now version 2) records both checks from what wamrc generated, and the runtime and
  `hull build` refuse AOT without them - the module's `.wasm` runs in the interpreter instead.
  Rebuild apps with AOT compute after updating Hull and wamrc (`hull tools install wamrc`, or
  `make wamrc`); AOT from an older wamrc is refused. The runtime's lack of hardware bound
  checks is now pinned (`WASM_DISABLE_HW_BOUND_CHECK=1`). `hull build` also hashes the wamrc
  binary by streaming it: a ~90 MB wamrc failed the build with "out of memory".
- **A failed GPU dispatch leaked its uploads** (audit 35, both runtimes, sync and async).
  `gpu.dispatch` / `gpu.pipeline` queue their inline buffers, uniforms and inline textures,
  then could fail before submitting (an unknown texture, a binding-kind mismatch, too many
  bindings in a later stage, a bind group or encoder that could not be made). Audit 34's
  flush counted no write for a failed call, so the staging copies stayed in wgpu's pending
  writes: a loop of failing dispatches with an 8 MiB inline buffer held 1.7 GB of device
  memory at 200 calls and 5.9 GB at 700 (Apple Metal), with a Lua heap under 64 MB. The
  wgpu backend now submits the pending writes itself, with a bounded wait, when a call that
  queued uploads fails before its submit (a wait that times out reports the call as a
  timeout and stalls the device); the cap layer also counts a failed call's transient bytes,
  and a refused `gpu.buffer` / `gpu.texture` write's, as queued writes, so the 64 MiB flush
  comes for any backend. The same loop now peaks at ~220 MB, as a loop of successful
  dispatches does.
- **Tool commands printed a cloned tree's text raw** (audit 33). Audit 32 escaped app text at
  the call sites it found in `hull build` and the manifest-extraction error, but most tool
  commands still wrote file names, manifest strings and signature fields verbatim: `hull verify`
  (unsigned / missing / modified file names, the signed build info), `hull inspect` (every
  `package.sig` / `hull.sig` field, signed or not), `hull deploy` (the env names and hosts it
  rejected as unsafe), `hull modules list` / `hull check` / `hull modules analyze` (declared
  specs, source file names), `hull analyze` (diagnostic paths), `hull compute build` (an invalid
  module name, a dependency path), `hull eject` (copied file names), the AOT step of
  `hull build`, and the dispatcher's report of a plugin's error (a `json.decode` error quotes the
  byte it stopped at). An ESC / OSC sequence there rewrote the terminal, set its title or (OSC 52)
  the clipboard. The tool VM's `print`, `tool.stderr` and `tool.stdout` now escape every control
  byte but LF and tab (CR as `\r`; ESC, NUL, the other C0 controls and DEL as `\xNN`; UTF-8
  unchanged), as does the dispatcher's error line; `tool.stderr_raw` / `tool.stdout_raw` write
  verbatim and are used only for JSON the plugin encoded itself. Covered by
  `tests/e2e_tool_escape.sh` (`make e2e-tool-escape`).

- **The live-database check could still lose a race to a connection being opened** (audit 34,
  both runtimes). Audit 33 registered a SQLite connection's file only after
  `hl_cap_db_init` returned, but its `PRAGMA journal_mode=WAL` had already opened the WAL and
  mapped the `-shm`; and the fs capability matched the file and then truncated it in two steps.
  A connection opened in between - `db.async` on a `db.open` DSN reopens per op, an evicted
  worker connection reopens - while the event loop wrote in place through an in-root symlink to
  `<db>-shm` was truncated under, and the process died of SIGBUS (reproduced within a second).
  The backend now registers the file right after `sqlite3_open`, which maps nothing (and
  unregisters only once the connection is closed); the agent's read-only connections register
  too; and the stat, the match and the one destructive syscall - `ftruncate` in
  `hl_cap_fs_open_write_fd`, `renameat` in the atomic `fs.write`, `unlinkat` in `fs.delete` -
  run under the registry's lock, which registration takes, so a connection either registered
  first (refused) or maps nothing until the change is done. An in-place write is also refused
  when the leaf an alias actually reaches is named like a sidecar (`hl_fs_open_at_leaf`).
  Covered by `test_fs` (a VFS shim asserting the file is registered before its WAL opens, and a
  forked race of opens against in-place writes).

- **`hull agent errors` / `logs` served a cloned repo's `.hull/` files as Hull's own output**
  (audit 36). `hull agent errors`, MCP `hull_errors` and the agent API's `/errors` passed
  `.hull/last_error.json` through as is (checked only for being a JSON object with no control
  bytes), and `hull agent logs` / `hull_logs` tailed `.hull/dev.log`, which nothing in Hull
  writes. A repo could commit either, and an agent following AGENTS.md read its text - "run
  this pipeline to fix the build" - as Hull diagnostics. `hull dev --agent` now makes its
  session nonce before the first spawn and hands it to the served child (`HULL_DEV_SESSION`,
  unset before app code runs), which stamps it into `last_error.json`; a stale record is removed
  when the session starts and ends. The record is served only when its nonce matches the live
  session's (dev.json's nonce equal to the copy under `$HOME/.hull/dev-sessions/<pid>`, the pid
  alive - the binding `hull agent inspect` already used, now `hl_agent_dev_session`), and then
  only `error` (which must be Hull's `failed to load ...`) and `timestamp`, re-encoded;
  otherwise the result is `{"errors":[],"ignored":"..."}`. `hull agent logs` reads no file:
  it returns no lines, a note that `hull dev` logs to stderr, and `ignored` when a
  `.hull/dev.log` exists. `hull agent status` takes dev.json's port only from a bound session.
  Covered by `e2e_agent` and `e2e_mcp` (planted records and logs not served; a real
  `hull dev --agent` load failure still reported, and no longer once its session ended).

- **Concurrent first failures deadlocked the lockout counters on MySQL** (audit 31, both
  runtimes). The audit-30 re-audit ran the counter's UPDATE first and the INSERT-if-absent
  only when it matched no row; but an UPDATE matching no row takes a gap lock under
  REPEATABLE READ, so two requests creating different new keys in one gap - the per-(account,
  IP) login keys are adjacent - deadlocked on their INSERTs (ERROR 1213). The losing request
  answered 500, and since the per-IP row was bumped before the account row, its guess counted
  against neither. Every counter (auth-flows login, TOTP per-user, per-IP and per-IP share,
  and the TOTP success credit) now asks with a plain, non-locking `SELECT` whether its row
  exists, inserts only a new key, then updates; a counter picked as a deadlock victim at top
  level is run once more. A failed password check bumps the account row before the per-IP
  row, so a failure on the second cannot leave the guess uncounted against the account.
- **A TOTP failure could undo another instance's per-IP lock** (audit 31, both runtimes).
  The per-IP count's UPDATE always wrote `locked_until = 0`, so a failure that passed the
  lock check just before another instance locked the IP unlocked it again. A lock still
  running is now kept. (The audit-30 note that these counters were serialised so that none
  was lost was true of the counts, not of the lock.)
- **PNG encodes were charged ~30-40x below their cost** (audit 35, both runtimes).
  `image.encode` charged 1 unit per 8 RGBA bytes whatever the format, but stb_image_write's
  PNG writer runs five filters over every row and then deflates with 16-deep hash chains:
  a 4000 x 4000 PNG encode took 1.4-3.5 s for 8M units, so ~12 fit the default budget
  and held the event loop ~40 s, under the 60 s run deadline. JPEG was ~4x under. Encodes
  are now charged per format (`hl_image_encode_units`): 1 unit per 8 bytes the encoder
  works over, times 32 for PNG and 4 for JPEG (`HL_IMAGE_ENCODE_{PNG,JPEG}_FACTOR`),
  measured on incompressible pixels, the worst case. A budget's worth of encodes now
  runs ~1.2-1.4 s, as a plain script loop does. A lower zlib level was tried and barely
  helps on noise, so the PNG level is unchanged. A single PNG encode of more than ~3M
  pixels is now over the default 100M budget; raise `--max-instructions` for that.
- **`/login` response time told known addresses from unknown ones** (audit 31, both
  runtimes). An unknown address ran the dummy PBKDF2 verify but skipped the lockout reads
  and, on failure, the counter writes a known address did. An unknown address is now counted
  under an internal key derived from it (SHA-256 of its ASCII-lowercased form, under a
  prefix no account key can have), with the same reads and writes, and throttled the same
  way; a locked unknown address gets the same 401 as a locked account.

- **A Lua `require` with no caller frame skipped the module declaration gate** (audit 33).
  Before `app.manifest()` is applied, a top-level require is recorded by the import tracker
  and checked against the declared modules once the manifest is in; a require with no Lua
  frame above it was taken for the runtime's own `hull.json` preload and neither recorded nor
  refused. Calling `require` as a coroutine body - `coroutine.wrap(require)("hull.db")` or
  `coroutine.resume(coroutine.create(require), "hull.crypto")` - therefore loaded any
  first-party module with an empty `modules` list, and the app used it (queried the default
  database) with the startup check passing. Such a require is now the app's import: tracked
  before the manifest, refused after it. The runtime's preload goes through the trusted entry,
  and a stdlib frame exempts a require only when it calls `require` by name (a stdlib helper
  calling a function the app handed it no longer lends it that exemption).

- **A network stream whose send failed spun the event loop at 100% CPU** (audit 34). When a
  stream's peer reset the connection with bytes still queued (a `hull.ssh` session, say), the
  failed flush kept the queue and `io_rearm` - which never looked at the stored error - kept
  the write watcher on the dead socket; and since the poll and Keel backends report a hangup
  as READ readiness, a stream that had never read ignored it. Either way the socket was
  reported ready on every tick (~200k ticks in 0.3 s) until the binding freed the stream. A
  failed send now drops the queue, a stream with a stored error watches nothing, and READ
  readiness on a watcher that did not ask for it is taken as a write attempt that finds the
  error (no progress counts as one). An op parked on the stream is woken with the error.

- **DuckDB app SQL could ATTACH a second catalog and read `_hull_*` cross-catalog** (audit 33).
  Hull's `_hull_*` guard on a DuckDB connection is the lexical text check, which audit 25 / 32
  reinforced by refusing dynamic-SQL table functions and whole-database file statements. But
  `ATTACH 'file.duckdb' AS c` (permitted in the fs-grant mode-B policy, within an allowed
  directory) opens a second catalog, and a stored view or macro inside an attacker-authored
  `.duckdb` file - one an app with `fs.write` of that directory may hold, e.g. an upload -
  reads `app._hull_sessions` cross-catalog with no `_hull_` ever appearing in the app's SQL,
  so the lexical check never sees it; `USE` and `SET search_path` / `SET schema` reach the
  same result by changing which catalog an unqualified name resolves to. The DuckDB backend
  now refuses ATTACH, DETACH, USE, and SET / RESET / PRAGMA of `search_path` or `schema` in
  app SQL and migrations (`hl_cap_db_sql_switches_catalog`, read with the same budgeted,
  linear, comment-aware scan as the audit-32 classifier; any statement of a multi-statement
  text counts). No stdlib SQL uses them, and cross-catalog queries are not a documented Hull
  DuckDB feature (file I/O stays available through `read_csv` / `read_parquet` / `COPY`).

- **Live SSH streams are capped per process** (audit 35). `hull.ssh` byte streams had no
  count: each holds a socket (up to 8 while its connect races addresses) until closed or
  collected, and a coroutine parked on one anchors its handle, so app code with an `ssh`
  grant pacing `hull.async` connects to a host that accepts and stalls filled the
  descriptor table under `ulimit -n 1024`, after which Keel logged "accept: out of
  descriptors" and stopped serving (SQLite temp files and TLS lost descriptors too).
  `hl_cap_ssh_open` now takes one of `HL_SSH_MAX_LIVE_STREAMS` (32) process-wide slots,
  after the grants and before anything is dialled, and the stream gives it back from its
  release hook (`HlNetStreamConfig.on_release`) once its sockets are closed; past the cap
  the connect returns `(nil, message, "too_many_streams")` (`HL_NET_E_LIMIT`), which
  `ssh.connect` reports as `{ code = "too_many_streams" }`. The Lua binding collects once
  and retries on that refusal, as `kv.open` and `blob` do, so streams dropped without
  `close()` free their slots. The dial's own deadline is clamped to 10 min
  (`HL_SSH_CONNECT_MS_MAX`, as `http.fetch`'s async ceiling); `ssh.connect`'s handshake
  deadline keeps the caller's `timeout_ms`. The other socket users already had caps
  (`db.open` / `kv.open` 16, SMTP in-flight, `http.fetch` in-flight, `ws.connect` per VM).

- **MySQL: a COMMIT tripped in its wait is finished; insert_if_absent refuses a truncated
  value** (audit 31).
  - A run whose budget or deadline tripped while the OK of a lone, wholly sent COMMIT was
    awaited closed the connection with the transaction still marked open: the commit the
    server made was reported failed, the stale-transaction guard's ROLLBACK was answered done
    without being sent, and a retry applied the batch twice. As on Postgres since audit 29, the
    reply is now read on with no budget asked, bounded by 2 s (or a shorter `read_timeout`),
    and its status taken; past the bound the connection is abandoned as before. The trip stays
    sticky.
  - `insert_if_absent` was `INSERT IGNORE`, which turns every strict-mode error into a warning
    and stores the value truncated: `'evt_1\xff\xfe'` was stored as `'evt_1'`, so the genuine
    `evt_1` then read as an inbox duplicate, and a jobs payload over a TEXT column's 64 KB lost
    its tail. It is a plain `INSERT` whose duplicate-key error (1062) means "already there";
    any other error fails the call and stores nothing. A foreign-key or NOT NULL violation that
    IGNORE skipped now fails too. The inbox also refuses a message id or source that is not
    valid UTF-8 (in JS, one with a lone surrogate).
- **In-flight `http.fetch` responses are bounded per VM** (audit 31). Keel buffers each async
  response (the decompressed body included, up to `max_response_size`, 4 MiB by default) off the
  VM heap, and nothing counted the fetches in flight, so `Promise.all` of 2000 large fetches
  could hold about 8 GiB beside a 64 MB heap limit. A VM now keeps at most 64 fetches in flight;
  past that, `http.fetch` raises ("too many requests in flight") before starting anything. The
  response buffers a VM holds are bounded by in-flight count x `max_response_size` (256 MiB by
  default). Both runtimes.
- **TUI only after the manifest admits it** (audit 31). The `hull.tui` bindings acquired the
  terminal without checking the manifest, so an app's top level - which runs before the manifest
  is applied, and in the manifest-extraction VM behind `hull build` / `manifest` / `check` /
  `modules` / `deploy` - could switch the screen or write an OSC 52 clipboard sequence, even
  without declaring `hull/tui` (a cloned repo could paste-jack the developer's clipboard).
  `hl_cap_tui_acquire` now refuses unless the runtime has applied a manifest declaring
  `tui = true` and `hull/tui`, or the command is one of hull's own `--tui` tools; manifest
  extraction never allows it. Both runtimes.

- **Blob writers and readers could exhaust the process's file descriptors** (audit 34). A
  `blob.writer()` holds two descriptors and a `blob.reader()` one until it is finalized,
  aborted, closed or collected, and opening one cost the app almost nothing: 61433 writers
  exhausted macOS's per-process limit, and with it `accept()`, SQLite's temp files and every
  TLS connection. At most `HL_BLOB_MAX_LIVE_HANDLES` (256) are open in the process now,
  counted in the capability layer where each handle is made and consumed, so a slot is given
  back exactly once (finalize, abort, close, a finalizer, teardown). An open past the cap
  collects garbage once - a handle the app dropped holds its slot until collected - and
  retries, as `fs.mmap` does, then fails with "too many open blob writers / readers". Opening
  a handle is charged `HL_BLOB_OPEN_UNITS` (1024) to the run's budget. Both runtimes.

- **Postgres `COPY ... FROM STDIN` through `conn.exec` / `conn.query` hung until the run
  deadline** (audit 35). Those calls use the extended protocol (Parse / Bind / Describe /
  Execute / Sync in one write). In copy-in mode the server ignores that Sync, so after Hull's
  CopyFail it sent its ErrorResponse and then waited for another Sync, while Hull waited for
  ReadyForQuery: 60 s for a request, 10 min for `app.main` or a `db.async` op (holding a pool
  thread), for ever with `--max-run-ms 0` and `?read_timeout=0` - and then the connection was
  dropped (inside a transaction, refused until ROLLBACK). The extended path now sends a Sync
  right after the CopyFail, so the statement fails at once and the connection stays in step;
  the simple-query path (scripts, migrations), which already got its ReadyForQuery, sends
  none. Verified against postgres:16.

- **A JS GPU call could hold the event loop for ~49.7 days, and a timed-out one let device memory
  pass its ceiling** (audit 33, `--with=gpu`).
  - **Timeout clamp.** `gpu.dispatch` / `gpu.pipeline` and their `async` forms read `timeout`
    with `JS_ToUint32` and passed it straight through, so `timeout: -1` waited 2^32-1 ms (Lua
    already clamped it). The ceiling, `HL_GPU_TIMEOUT_MAX_MS` (60 s), is now enforced in
    `hl_cap_gpu_dispatch` / `hl_cap_gpu_pipeline` for every caller, and the JS bindings read the
    option as Lua does: non-positive means the default, at most 60 s.
  - **Run deadline.** A GPU wait also asks the calling run every 10 ms, so a sync dispatch stops
    at the run's deadline (`--max-run-ms`) or a tripped budget instead of waiting out its
    timeout.
  - **Memory held by a timed-out call.** A timed-out dispatch or pipeline is still running on
    the device, and the transient buffers / textures it made are freed only when it finishes.
    Their bytes were given back at once, so repeated timed-out calls held device (on unified
    memory, host) memory past `HL_GPU_MAX_LIVE_BYTES`. They now stay reserved, and the device
    refuses new dispatches / pipelines with `gpu_busy` until a non-blocking poll
    (`queue_idle`) shows the queue drained.
  - Covered by `test_gpu` (mock backend: the clamp, and the stall and drain).

- **The audit-30 fixes, re-audited** (audit 30 re-audit).
  - The DuckDB dynamic-SQL scan was still quadratic for bare names: each `query` /
    `query_table` token skipped the comment after it, and an unterminated `--` or `/*` ran to
    the end of the text, so `query--query--...` (1 MB) held the event loop for about 100 s
    before any budget charge. The skips of one scan now share a byte budget (twice the text
    plus 64 KiB), past which the statement is refused.
  - The atomic lockout counters (auth-flows login, TOTP per-user and per-IP) ran their
    INSERT-if-absent before the UPDATE. On MySQL an `INSERT IGNORE` of an existing row takes a
    shared lock, so two concurrent wrong guesses for one key deadlocked on the UPDATE, and
    the victim's guess went uncounted (a 500). The UPDATE now runs first; only a key with no
    row yet is inserted, then counted.
  - TOTP `credit_success_ip` locked the per-user share before the IP row, the reverse of the
    failure path, so a success and a failure from one IP could deadlock on Postgres / MySQL. It
    now locks the IP row first, which also keeps the share it reads from changing under it.

- **JS: a transaction held across a wait by code no entry waits on** (audit 31). The wait check
  ran only for runs an entry waits on. A continuation nobody awaits
  (`hull.sleep(5).then(async () => ...)` from a synchronous handler), or the code of an ended
  handler resumed inside another request's job drain, could run `const p = hull.sleep(20);
  BEGIN; INSERT; await p; INSERT; COMMIT`: the first write was rolled back unseen, the second
  autocommitted - and in the cross-request case the request whose drain ran that code had its
  own acknowledged write rolled back with the other handler's transaction. A transaction left
  open where such code stops (the end of an unwired resume, or the end of another run's jobs in
  a drain, read from the new QuickJS `JS_GetPendingJobTag`) is now rolled back before anything
  else runs, logged, and that code is not continued past its await. The reverse is closed too:
  while a run is inside a transaction across a microtask yield (`BEGIN; await null; ...`),
  another run's queued jobs no longer run inside it; they wait until it closes (QuickJS
  `JS_ExecutePendingJobTagged` runs the active run's own jobs first).
- **JS: `conn.close()` inside a ws client's `onClose` / `onError` was a use after free**
  (audit 30). The connection was not yet marked closed while the callback ran, so `close()`
  armed the 5 s close-handshake deadline after the callback's caller had disarmed it; the
  client object was then released and freed with the timer still armed, and the deadline fired
  into freed memory (and could call `onClose` again through it). The connection is now marked
  closed before `onClose` / `onError` run (in both runtimes: `close()` there is a no-op, `send()`
  throws as on any closed client), and the JS finalizer cancels any deadline still armed, as the
  Lua `__gc` already did.
- **A failed Valkey read kept its receive buffer, up to 64 MiB outside the VM heap** (audit 35).
  The audit-30 trim drops an oversized receive buffer only once a reply has been used, from the
  next command. A read that failed part-way - a tripped budget, the command deadline, a
  malformed or oversized reply, a server hanging up - left the unparsed bytes behind and marked
  the connection dead, and a dead connection never reads again, so the buffer stayed until the
  store was closed or collected: 16 stores (`HL_KV_OPEN_MAX`) could hold ~1 GiB. The buffer is
  now scrubbed and freed as soon as a command fails on a dead connection. Lua `kv.get` copies
  the value with a protected push and gives the buffer back before it raises an out-of-memory
  error (a plain `lua_pushlstring` raised past the release); the JS binding already released it
  whether or not its copy succeeded.
- **JS: an ed25519 / HMAC message larger than the heap was copied off it** (audit 34).
  `crypto.ed25519Sign` / `ed25519Verify` / `auth` / `authVerify` take a MappedBuffer or
  WasmBuffer, which is not on the VM heap, and the capability layer copied the whole message
  into a malloc'd buffer - twice over for a verify (TweetNaCl's `crypto_sign_open` wants
  `sig || msg` and a same-size output): a 600 MB mapping under a 64 MB heap held 1.27 GB.
  `ed25519Sign` / `ed25519Verify` now refuse a message larger than the heap limit before the
  copy (RangeError), and HMAC-SHA512/256 (`auth`, `authVerify`) hashes the message in place
  with TweetNaCl's `crypto_hashblocks` instead of copying it behind the key block, so it takes
  any size and is charged as before. The Lua twins take Lua strings, already on the heap.
- **JS: an ended handler's op could hold another request's response** (audit 30). A handler that
  awaited a promise Hull does not drive is ended at once; when a later request settled that
  promise, the first handler resumed inside the later request's job drain, and an op it then
  started (`hull.sleep`, `http.fetch`, `db.async`, ...) was taken for the later request's: it
  suspended that connection and the client waited for the first handler's op (a 2 s
  `hull.sleep` delayed an unrelated response by 2 s). QuickJS now runs each promise job with
  the tag of the code that awaited it (HULL PATCH 0008, docs/quickjs_patches.md), every entry
  is a run with its own tag, and an op made by another run's code runs detached, attached to no
  request and part of no run (app.main's own continuations stay main's). The same holds for a
  timer's or ws callback's run resumed inside another entry.

- **`jobs.cron` at startup dropped a tick that had come due while the app was down** (audit
  30, both runtimes). Re-registering a schedule recomputed `next_run_at` from now even when
  the spec and tz were unchanged, so a restart after a tick came due - but before a worker
  fired it - pushed it into the future and it never fired, breaking the documented "missed
  ticks fire once". The stored `next_run_at` is now kept for an unchanged spec / tz (the
  payload, type, queue, priority and max_attempts still update); a new schedule, a changed
  spec or tz, or re-enabling a disabled one computes it afresh. One INSERT-if-absent plus
  an UPDATE whose `next_run_at` CASE is assigned first, so it holds under MySQL's
  left-to-right SET.

- **The login lockout counter lost updates on Postgres / MySQL** (audit 30, both runtimes).
  auth-flows read the failed-login count, computed the next one in the stdlib and wrote it
  back inside `db.batch`, whose plain `BEGIN` takes no row lock: concurrent attempts from
  several instances all wrote N+1, so a lockout allowed several times the configured
  guesses. The count is now incremented by the database (`failed_count = CASE ... THEN 1
  ELSE failed_count + 1 END`), and the lock set by a second statement from the new count -
  no statement reads a column it assigns earlier in its own SET (the audit-29 MySQL issue).
  Semantics are unchanged: an expired lockout restarts the count at 1, reaching the
  threshold locks for `lockout_duration`. TOTP's per-user and per-IP attempt counters had
  the same read-modify-write and are fixed the same way; a per-IP credit is applied once,
  by the transaction whose DELETE took the user's share.

- **DuckDB's POINTER-taking table functions are refused by name** (audit 35, `duckdb`
  feature, defence in depth). Of DuckDB 1.5.4's table, table-macro and pragma functions only
  `arrow_scan` and `arrow_scan_dumb` take POINTER arguments, and both passed Hull's SQL
  checks. Checked against the engine under Hull's configuration: no SQL value reaches a
  non-NULL POINTER - the type cannot be named (`1::POINTER` is a catalog error), no bindable
  value (integer, text, double, blob, boolean) converts to it, no function returns one and no
  table has such a column, and NULL fails at bind ("pointers cannot be null") - so they were
  not reachable with an address. They are refused anyway, so the guarantee does not rest on
  that. No other listed function reads files, takes a table name as a string, or takes a raw
  pointer outside the existing refusal lists (`read_duckdb` and `parquet_scan` are covered by
  the `read_*` / `parquet_*` file-function rule).
- **JS `rbac.hasAnyRole(uid, "admin")` checked each character as a role** (audit 30). A
  string is now one role in both runtimes (Lua's `has_any_role` returned false for one);
  `hasAnyPermission` / `has_any_permission` likewise.
- **App and request text reached stderr raw on paths outside the log** (audit 34, both
  runtimes). app.main's error, a JS app.main whose promise rejected, a non-numeric app.main
  return, a JS handler failing after its multipart body parked (whose error can quote a field
  of the remote request body), the app-context load errors `hull test` / `hull check` /
  `hull agent` print (module names, `$VAR` names, fs paths, import names from the cloned tree)
  and SQLite open / reopen messages naming a DSN were written with plain `fprintf`, so an ESC /
  OSC sequence or a newline in them rewrote the operator's terminal or forged a log line. The
  multipart error now goes through `log_error` (escaped by the installed callback) and the rest
  through `hl_eprintf_line` (`shared/term_escape.h`), which escapes the whole line as log
  records are escaped. Covered by the audit-34 legs of `tests/e2e_tool_escape.sh`.
- **A MySQL statement of exactly 16 MiB - 1 byte hung the connection** (audit 30). A packet
  length of `ff ff ff` means "continued in the next packet", so the server waited for an
  empty terminator packet Hull never sent, holding the event loop until the read timeout (for
  ever for a migration with `read_timeout=0`). The writer now refuses that length, as the
  reader already did, with the "larger than one MySQL packet" error.

- **DuckDB's dynamic-SQL check was quadratic in a run of double quotes** (audit 30, `duckdb`
  feature only). It scanned to the closing quote from every `"`, so 1 MB of quotes held the
  event loop for minutes before any SQL budget charge. The scan of a quoted name now stops as
  soon as the name is too long to be `query` / `query_table` / ...; every byte is still
  scanned for a bare name.

- **DuckDB refused ordinary SQL that contained the database's file name, or used `show` /
  `histogram`** (audit 35, `duckdb` feature; regression from audit 34). The database-file
  check refused any statement holding the file's name as a substring, anywhere: with
  `duckdb://data/hull` every `_hull_migrations` statement was refused and the app could not
  boot, `duckdb://sessions` broke the session store, `duckdb://app` refused `approvals`,
  `duckdb://data` refused `SELECT data FROM user_data`. The name is now read only in
  literals - the text between every two consecutive `'`, `"` or `$`, whatever the parity, so
  a quote inside a comment cannot hide one - and in file-function arguments and COPY targets,
  as a path component (preceded by the start or a separator, followed by the end, a separator
  or a `.wal` / `.tmp` sidecar suffix). `read_blob` / `read_text` / `COPY` of the file and its
  sidecars stay refused in every spelling (relative, `./`, `..`, any case, backslash,
  `"..."`, dollar-quoted), and a `U&'...'` literal with a backslash is refused like an `E'...'`
  one. The table-name check refused `"show" = 1`, `WHERE "show" = ?` and
  `histogram(lower(name))`: `show` / `table_info` take their `= value` spelling only after
  `PRAGMA` now, and `histogram` is the table macro only after a table-position word (FROM,
  JOIN, CALL, PRAGMA, `,`, `(`, ...) and with more than one argument.
- **A libhull embedder that skipped the optional phase-1 pledge was killed on its first WAL**
  (audit 30). The phase-2 pledge did not record that a pledge was in force, so SQLite's
  `fchown` ran a real `chown` under it and a root process died on "pledge chown". Phase 2 now
  records it too.

- **`kv.open` had no limit on live connections, and each kept its biggest reply's buffer**
  (audit 30, Valkey / Redis). Forty handles after one 40 MB `GET` each held 2.4 GB. At most 16
  KV connections are open at once now (as for `db.open`; a refused open collects garbage once
  and retries, so dropped stores free their slot), and a receive buffer past 1 MiB is freed
  once its reply has been copied.

- **Named and internal SQLite file DSNs could leave the app tree** (audit 34, both runtimes).
  A `databases.named` / `databases.internal` file DSN was opened as written: a relative one
  resolved against the working directory, and nothing checked containment or links. So a cloned
  app that committed `cache.db -> ../secret.db` with `databases.named.cache = "cache.db"` read and
  wrote a database outside its tree wherever there was no kernel sandbox (`--no-sandbox`,
  degraded Linux, a cosmo APE on Windows / macOS / BSD). Such a DSN (scheme-less, `sqlite://`,
  `file://` or `duckdb://`, and a `$VAR` one once it is resolved) is now resolved against the app
  directory and must stay inside it. It may have no `..` component and no symbolic link below
  the app directory, and its leaf and `-wal` / `-shm` / `-journal` sidecars must be absent or
  regular files, the same rule as `db.open` (audit 29) and the implicit `data.db` (audit 25). An
  absolute path outside the app directory is refused. The kernel sandbox never granted one, so
  sandboxed deployments see no change. `:memory:`, network DSNs and the operator's `-d` are
  unaffected. The worker pool is handed the same checked absolute path, and a connection whose
  DSN no longer passes gets a DSN no backend takes, so `async` fails instead of falling back to
  the default database. Covered by `test_db_registry` and `tests/e2e_named_connections.sh`.

- **`time.now()` could lag the other clocks by a second on Linux.** It read `time()`, which
  glibc serves from the coarse clock, a few milliseconds behind `CLOCK_REALTIME`. Just after a
  second boundary it still named the previous second while `time.now_ms()` / `nowMs()` and
  SQLite's `'now'` named the new one, so a timestamp taken by one and compared with the other
  was off by one (a `jobs.reap` claim stamped two seconds ago read as one). `time.now()`,
  `time.date()` and `time.datetime()` now read `CLOCK_REALTIME`.

- **A Lua coroutine that yielded inside a transaction could be resumed by a later request**
  (audit 35). Waits refuse while a transaction is open, but a coroutine the app made and
  stored could `BEGIN`, write, `coroutine.yield()`, and be resumed from the next request:
  the stale-transaction guard had rolled the first writes back when the first request
  ended, so the rest of its statements autocommitted one by one and its `COMMIT`
  "succeeded". Such a coroutine can now be resumed, closed or called through
  `coroutine.wrap` only by the run that yielded it (a generator inside `db.batch` keeps
  working); from a later run the call raises. A coroutine that yields with no transaction
  open is unaffected. Covered by `test_lua` (`lua_audit35`).

- **A build that linked mbedTLS for a database without HTTP used none of it** (e.g.
  `make HL_ENABLE_HTTP=0 HL_ENABLE_POSTGRES=1`, or MySQL / Valkey). The mbedTLS crypto backends
  were compiled only under `HL_ENABLE_HTTP`, so that binary carried mbedTLS yet ran the
  fail-closed stubs: no AES-CTR or ChaCha20-Poly1305 (`crypto.aes256ctr` and the AEAD calls
  failed), no RSA / ECDSA sign or verify. Its `hull sbom` also left out mbedTLS and Keel, both
  linked. The Makefile now defines `HL_LINK_TLS` for C code whenever it links the TLS stack, and
  the backends and the SBOM entries test that. Builds with HTTP, and pure-compute builds, are
  unchanged.
- **`gpu.load(name)` compiled on the default device only**, in both runtimes: it took no
  options, so a shader from `shaders/` could not be dispatched on another device
  (`shader_not_found`). It now takes `{ device = N }` / `{ device: N }` as `gpu.compile` does.
- **JS `gpu.compile`, `gpu.bufferRead` and `gpu.textureRead` ignored `{ device }`**: a shader
  compiled on the default device only (a dispatch on another device failed `shader_not_found`),
  and both reads returned the default device's object of that name. They now take the device
  as their Lua twins do; a throwing getter fails the call. Found by the new runtime GPU tests
  (`lua_gpu` / `js_gpu`), which run both bindings over an in-memory backend on every runner.
- **Streaming-multipart routes had no header, buffer, part or total cap unless they set one
  (audit 34)**: such a route skips the server's `-b` body limit, and every multipart cap left
  unset was Keel's `0` = unlimited. A route that set only `max_part_size` (the documented
  pattern, and the photos example) let an unauthenticated client send a part header that
  never ends: it was buffered without bound and rescanned for its terminator on every chunk,
  holding a core at 100% and growing memory for the 30 s body deadline, per connection. Hull
  now fills every unset cap (`hl_cap_multipart_effective_config`, both runtimes and the
  in-process test harness): `max_headers_size` 16 KiB, `max_input_buffer` 1 MiB, `max_parts`
  1000, and `max_total_size` the server body limit (`-b`, 1 MB by default) raised to fit one
  `max_part_size` part. Unlimited is no longer offered; a route that needs more sets an
  explicit value (one taking several big files sets `max_total_size`).
- **A GPU dispatch or pipeline stage whose bindings did not match its shader aborted the
  process (audit 35)**: wgpu-native reports a bad bind group through its error callback but
  sends a `wgpuQueueSubmit` / `wgpuQueueWriteBuffer` / `wgpuQueueWriteTexture` error through
  a Rust panic that cannot unwind out of its C API ("fatal runtime error: failed to initiate
  panic, error 3, aborting"), so a `gpu.pipeline` stage passing one buffer more than its WGSL
  declares killed the whole Hull process. `hull build --with=gpu` / `HL_ENABLE_GPU` builds
  only. Every binding a `gpu.dispatch` / `gpu.pipeline` call makes (sync and async; uniforms
  at 0, then buffers, then textures) is now checked against the shader's
  `@group(0) @binding(N)` declarations, read from the WGSL at compile
  (`hl_gpu_wgsl_layout` / `hl_gpu_bindings_check`): count, index, kind (uniform / storage /
  sampled texture / sampler / storage texture) and a storage texture's format, refused as
  `binding_mismatch` before wgpu sees it. As defence in depth the wgpu backend never makes a
  queue call after an error was reported - an invalid bind group, layout, buffer, texture or
  command buffer is refused (`binding_mismatch` / `gpu_validation_error`) instead of
  submitted - and refuses the device limits Hull's own caps allow past before wgpu does:
  workgroup counts over `maxComputeWorkgroupsPerDimension` (`workgroups_exceed_limit`),
  uniforms over `maxUniformBufferBindingSize` (`uniforms_too_large`), a storage binding over
  `maxStorageBufferBindingSize` (`buffer_too_large`), a persistent buffer over
  `maxBufferSize` or a texture over `maxTextureDimension2D` (an invalid object that
  "succeeded", whose first write aborted), and a persistent buffer bound larger than it is
  (`buffer_size_mismatch`). A zero-size inline buffer is now made one word long. The
  `gpu_texture` example's shader now uses the sampler it declares (WGSL drops an unused one
  from the layout, so the example's dispatch was one of these aborts).
- **`gpu.texture(name, nil, { device = N })` / `gpu.texture(name, null, { device: N })`
  destroyed the texture on the default device**, and so did JS `gpu.buffer(name, null,
  { device: N })`: the destroy branch ran before `opts.device` was read. A multi-GPU app
  freed the wrong device's texture of that name and leaked the one it meant. Both runtimes now
  read `device` first (a throwing JS getter fails the call); Lua `gpu.buffer` was already right.

### Security - build and release (audit 30)

- **The release workflow no longer hands the platform signing key to a downloaded binary.**
  The `sign-platform-manifest` job fetched v0.1.2's `hull-linux-x86_64` with `curl`, with no
  hash pin or signature check, and ran it with `HULL_PLATFORM_KEY`: anyone able to replace that
  old release asset would have captured the platform private key on the next release. The job
  now builds hull from the tagged source (plain `make`, which needs no platform signature) and
  signs with that. Both signing keys are now scoped to the one step that uses them (not the job
  environment) and written to a private `mktemp -d` under `$RUNNER_TEMP`, removed on exit. See
  docs/release_signing.md "Signer binaries".
- **`make fetch-wgpu` / `fetch-duckdb` / `fetch-cosmocc` / `fetch-pwned-blocklist` download into
  a private directory.** They used fixed names in the shared `/tmp`, so on a multi-user host
  another account could pre-create the name as a symlink (curl then overwrote a file of the
  victim's) or swap the archive between the SHA-256 check and the unzip. Each now downloads,
  verifies and unpacks inside `mktemp -d "${TMPDIR:-/tmp}/hull-fetch.XXXXXX"`, removed on every
  exit path. `scripts/build_musl_platform.sh`'s default work dirs and
  `scripts/build_pwned_blocklist.sh`'s default input no longer name fixed `/tmp` paths either.

### Security - build and release (audit 31)

- **The Zig bundle's input is pinned.** `scripts/build_zig_bundle.sh` (release `build-zig`)
  downloaded `zig-<triple>-<ver>.tar.xz` from ziglang.org with no hash check, and the repacked
  `hull-zig-*.tar` was then signed into `hull.sha256`. It now verifies the tarball against a
  per-triple SHA-256 pin in `scripts/zig.sha256` before extracting and fails on a mismatch or a
  missing pin. CI's Zig installs read the same file (one source of truth). The pins match
  ziglang.org's `index.json`, and each tarball was checked against Zig's minisign key.
- **Least-privilege workflow tokens.** Every workflow now declares `permissions: contents: read`
  at the top (ci, dco, deploy-site, release and the manual 1 GiB bench had none, so they ran
  with the repository's default token permissions); the release publish job keeps its own
  `contents` / `id-token` / `attestations: write`. Every `actions/checkout` sets
  `persist-credentials: false` (no job pushes or authenticates git after checkout), so the token
  is not left in `.git/config` for later steps.
- **The musl release assets build in a digest-pinned Alpine.** `build-floor-musl` and
  `build-platform-musl` ran `alpine:3.20` by tag; they now run
  `alpine:3.20@sha256:<index digest>` (workflow-level `ALPINE_IMAGE`), so a re-pushed tag cannot
  change what the release-signed `hull-libc-musl-*.tar` / `hull-platform-musl-*.tar` are built
  from. (apk package versions are not pinned: Alpine drops superseded versions from its
  repositories, so a pin would break the release on the next security update.)
- **The browser E2E installs from a lockfile.** `tests/e2e_htmx_playwright.sh` ran
  `npm install` over caret ranges with no lockfile. The dependency set is now the committed
  `tests/playwright/package.json` + `package-lock.json` (exact versions and integrity hashes,
  playwright 1.61.1, @axe-core/playwright 4.13.0), installed with `npm ci --ignore-scripts`; the
  CI cache is keyed on the lockfile's hash.

### Security - vendored dependencies

- **Vendored mbedTLS 3.6.2 -> 3.6.7** (3.6 LTS; official release tarball, SHA-256 checked
  against the published `mbedtls-3.6.7-sha256sum.txt`). No Hull-local changes to mbedTLS; the
  trimmed `include/` + `library/` layout and `hull_config.h` are unchanged. The SBOM entry now
  carries the exact version (`3.6.7`, CPE `cpe:2.3:a:arm:mbed_tls:3.6.7`). Upstream security fixes
  in between that reach code Hull's `hull_config.h` compiles in (TLS 1.2 client + server,
  ECDHE-RSA / ECDHE-ECDSA, X.509, PEM, RSA, ECC, ChaCha20, CBC):
  - X.509: a malformed basicConstraints extension could parse an end-entity certificate as a CA
    (CVE-2026-49300); buffer underflow in `x509_inet_pton_ipv6()` (CVE-2026-25833).
  - TLS 1.2: extended-master-secret transcript-hash errors ignored (CVE-2026-50581); Finished
    message miscalculated after an allocation / hardware failure (CVE-2025-27810); the client
    accepted a ServerKeyExchange signed with an algorithm it had disallowed (CVE-2026-25834);
    `mbedtls_ssl_conf_sig_algs()` restrictions are not applied to the certificate chain, now
    documented (CVE-2026-54441).
  - A verifying TLS client that never called `mbedtls_ssl_set_hostname()` now fails the
    handshake (CVE-2025-27809). Keel's client and `shared/tls_client.c` already set the name
    (and fail closed when it does not take), so no Hull behaviour changes.
  - RSA: PKCS#1 v1.5 decryption timing side channel (Bleichenbacher; CVE-2026-50587); timing
    side channel in key / prime generation.
  - ECC / bignum: `mbedtls_ecdh_calc_secret()` buffer overflow (CVE-2026-35336); 1-byte overread
    parsing a malformed ECC public key (CVE-2026-50583); optimized-modp side channel
    (CVE-2026-54435); modular inversion / GCD timing side channel (CVE-2025-54764).
  - Ciphers: ChaCha20 32-bit block-counter wrap reused keystream (CVE-2026-50584); CBC-PKCS7
    padding timing side channels (CVE-2025-49087, CVE-2025-59438).
  - PEM: integer underflow parsing malformed encrypted PEM keys (CVE-2025-52497).
  - AES on x86/amd64: a thread-scheduling adversary could force the software AES / GCM fallback
    over AES-NI at startup and time it (CVE-2025-52496).
  - RNG: random state duplicated across `fork()` after `psa_crypto_init()`, now reseeded in the
    child (CVE-2026-25835).
  Fixed upstream in modules Hull does not compile in (TLS 1.3, DTLS, PSK, EC J-PAKE, session
  tickets, PKCS#7, LMS, FFDH, X.509 writing): CVE-2025-48965, CVE-2026-25832, CVE-2026-50640, CVE-2026-50713, CVE-2026-50586,
  CVE-2026-50588, CVE-2026-50579, CVE-2026-50585, CVE-2025-49600, CVE-2025-49601, CVE-2025-47917,
  the TLS 1.3 early-data and ECDHE-PSK overflow fixes, and the PKCS#7 weak-hash rejection.
- Hull's own mbedTLS translation units (`cap/crypto_*_mbedtls.c`, the TLS client and transport)
  are now compiled with `MBEDTLS_CONFIG_FILE=hull_config.h`, like the library and Keel; they
  were compiled against mbedTLS's default configuration, so the two halves could disagree on
  struct layouts. `MBEDTLS_HAVE_ASM` is left off under MemorySanitizer (3.6.3+ refuses it).

### Behaviour changes (security audits 24-28)

Audits 24-26 tightened stdlib inputs; some apps will see different output or a new error.
Audit 27 walked back the changes that broke ordinary calls (CSV column order, search paging) and
kept the rest. Check the list below before upgrading.

**Stdlib - output**
- **Template dot paths read own properties only.** `{{ user.name }}` reads a field the value
  itself holds: a JS getter defined on a class (its prototype), or a Lua table that gets the field
  through an `__index` metatable, now renders empty, and a function value renders nothing. Pass
  plain data (`{ name: user.name }`) to the template.
- **`search` snippets and highlights are HTML-escaped.** The indexed text in `snippet` /
  `highlight` is escaped and only your `before` / `after` / `ellipsis` markers are inserted as
  given, so render the field raw (`{{{ r.snippet }}}`); `{{ r.snippet }}` now escapes it twice.
- **CSV header order (`csv.encode` with `headers`).** Without `opts.columns`, the header is the
  union of every row's keys (a key missing from the first row no longer drops its column). JS
  keeps first-seen order - the first row's keys in their order, then each new key as a later row
  brings it - so `[{title, id, done}]` still gives `title,id,done` (audit 24 had sorted it,
  `done,id,title`; audit 27 restores the order). Lua sorts the keys by their text, as Lua tables
  have no key order. Pass `opts.columns` for a fixed order in both.

**Stdlib - inputs that used to fall back to a default now raise**
- `jobs.enqueue`: `throttle` that is not a finite number of seconds >= 0 (`false`, negative,
  NaN); `delay` / `at` / `run_at` that is not a finite number or numeric string (`false`,
  `"soon"`, infinity); `max_attempts` (`maxAttempts`) outside 1..10000 (0 dead-lettered the job
  before its first run). `nil` / JS `null` / `undefined` still mean "use the default", in
  `init`, `enqueue` and `cron` alike (JS `enqueue` / `init` threw on `null` until audit 27).
- `jobs.cleanup`: `before` that is not a finite unix timestamp; `older_than` (`olderThan`) that is
  not a finite number of seconds >= 0. `older_than = 0` deletes every terminal job in both
  runtimes (JS took 0 as unset and kept seven days until audit 27).
- `jobs.enqueue` / `jobs.cron`: `priority` that is not an integer in the INTEGER range (Lua
  `priority = false` used to mean 0; a JS string went into the column as text); `concurrency`
  that is not a whole number >= 0. `nil` / `null` still mean 0 / no cap.
- `inbox` and `idempotency` `ttl`: must be a whole number of seconds in 1..31622400 (366 days),
  so the expiry fits a 32-bit INTEGER column. Checked at `init` and on every call that takes one
  (`inbox.mark_processed`, `inbox.check_and_mark`, `idempotency.middleware`); a fraction or a
  longer ttl used to be accepted.
- `audit-log` `retain_days` (`retainDays`): must be a finite number in (0, 36500]. A non-number
  disabled pruning, and JS `null` / `false` computed a cutoff of "now" and deleted every row.
  Omit it (Lua `nil`, JS `undefined`) for the default 365.
- `attachment.init` `max_size` (`maxSize`): must be a finite number > 0, or `false` (JS also
  `null`) for unlimited. A non-number raised mid-upload, and a NaN disabled the cap.
- `totp.init`: `max_lockout_duration` (`maxLockoutDuration`) must be a finite number >= 0 (a
  string one disabled the lockout in JS and raised at the first failure in Lua), and
  `pending_ttl` (`pendingTtl`) a finite number or `false` (a string or NaN never expired a
  pending enrolment).

**Stdlib - clamped, not refused**
- **`search.query` paging clamps.** `limit` / `offset` accept a number or a numeric string
  (`req.query.offset`) and clamp to 1..1000 / 0..2147483647; anything else takes the default
  (20 / 0). Audit 26 raised on them, so `{ limit = 5000 }`, `{ limit = 0 }` or a query-string
  offset answered 500. `max_query_len` / `max_query_terms` still raise on a bad value.
- **Count options read numbers the way Lua's `tonumber` does in both runtimes** (jobs `limit` /
  `offset` / `batch`, outbox `flush` `limit`, search paging): JS `"Infinity"`, `"0b11"` and
  `"0o7"` now take the default instead of a clamped or parsed value. `outbox.flush({ limit = 0 })`
  processes one item in both runtimes (JS used to fall back to 50 before audit 24).

**Stdlib - other audit 27 fixes**
- `jobs` `throttle`, `cleanup`'s `before` / `older_than` and the attempt-history retention are
  rounded up to whole seconds, as `run_at` is: a fraction went to Postgres as a double, which it
  refuses for an INTEGER column.
- `attachment.init { max_size = false }` resets the per-upload cap to unlimited (JS: `null` or
  `false`); Lua had no way to lift a cap on re-init.
- `auth-flows` verify with `new_password` runs the pwned-password lookup before it reads the user
  and checks the link's address binding, as password-reset confirm does, so it never acts on a
  record that changed during the network wait.
- Lua `auth-flows` and `email` refuse an address that is not valid UTF-8, as the JS twins
  (whose strings cannot hold invalid bytes) effectively do.

**Stdlib - audit 28 fixes**
- **TOTP per-IP lockout counts each account's failures.** Audit 26 stopped a successful verify
  from clearing the IP's failure count (one IP could spread wrong codes over many accounts and
  reset its count with its own), so behind a proxy or NAT ordinary typos from every user added up
  to `max_failed_attempts_per_ip` and locked 2FA for everyone there. A success now takes back only
  that user's own failures from the IP's count; other accounts' failures still count toward the
  lockout. The per-account shares live in a new `_hull_totp_attempts_by_ip_user` table, created by
  `totp.init` like the others (nothing to migrate).
- JS `auth-flows` and `email` refuse an address with an unpaired UTF-16 surrogate: it has no
  UTF-8 form (QuickJS writes it as invalid bytes, which the Lua twins already refuse).
- `safe_url` percent-encodes a backslash (`%5C`): browsers read it as `/` in an http(s) URL, so a
  relative `/\evil.test` was the protocol-relative `//evil.test`.
- JS `jobs` duration options (`visibilityTimeout`, `reapInterval`, `throttle`, `olderThan`) read a
  string the way Lua's `tonumber` does: `"0b11"`, `"0o7"` and `"Infinity"` now throw, as in Lua.
- `hull dev --agent`'s discovery publish refuses a `.hull` it cannot inspect, as every other
  symlink check does.

**Stdlib - audit 29 fixes**
- **`auth-flows` login lockout locks at the threshold on MySQL.** The failed-login UPDATE
  incremented `failed_count` and then compared `failed_count + 1` with the limit; MySQL evaluates a
  single-table `SET` left to right, so it saw the new count and locked one attempt early (after
  `max_failed_logins - 1`, both the per-(account, IP) and the account-wide row). The new count and
  lock are now computed in the stdlib and written under `db.batch`, the same on every backend.
  "Not locked" is now stored as `0` rather than `NULL` (read the same way).
- **`jobs` keys are stored as strings and fit their column.** `dedup_key` / `concurrency_key`
  (`dedupKey` / `concurrencyKey`) were bound as given: on MySQL an integer key compared numerically
  with the `VARCHAR` column, so the dedup read-back could return another job's id. An integer is
  now its decimal string, any other non-string raises, and a key over 255 bytes raises instead of
  being truncated or failing in the backend. `ctx.step` names are held to 255 bytes and signal
  names (`ctx.wait_signal` / `waitSignal`, `jobs.signal`) to 200, which leaves room for the wait's
  memo prefix. JS counts UTF-8 bytes, as Lua does.
- Lua `csrf` answers 413 for a body with more than 256 form pairs (counted on every `&`, as JS
  splits), as JS and the documentation do; it stopped scanning quietly, reporting a later `_csrf`
  field as missing. An empty token header is no token, so the body field is read, as in JS.
- `auth.jwt_middleware` / `jwtMiddleware` read `Authorization` by one rule: the `Bearer` scheme in
  any case (RFC 7235), spaces / tabs around the token dropped, an empty token a 401. Lua took only
  `Bearer` / `bearer` and any whitespace, JS only `Bearer ` with one space.
- JS `totp` guards the whole lazy re-key after a successful verify, as Lua's `pcall` does: a throw
  from re-encrypting the secret turned a success whose code (or recovery code) was already spent
  into a 500.

**Runtime - audit 28 fixes**
- A built app without HTTP (the `app.main` runner) logs one escaped line per record, as the
  server does: it used log.c's default output, which printed a message's newline raw, and its
  records no longer carry the C `file:line`.
- Running as root, SQLite hands a new WAL / journal / shm file to the database file's owner again
  when no kernel sandbox is in force (`hull migrate`, `--no-sandbox`, macOS); only a pledged
  process keeps the creator as owner. A root `hull migrate` killed mid-run had left a root-owned
  hot journal the app's own user could not recover.
- SIGTERM no longer waits on a `worker.dispatch` job's SQL wait (`pg_sleep`, a lock wait, a silent
  server with `read_timeout=0`): while the worker pool is freed every bound SQL charge stops, as a
  `db.async` op's already did.

**Runtime (JS) - audit 28 fixes**
- **A JS `tui.run({ tickMs })` clock under 50 ms no longer dies on the instruction budget.**
  `tui.pollSync` re-armed only when one call waited 50 ms, so every idle tick of a 16 ms clock
  piled into one run. The idle time is now added up since the last re-arm and a new run starts
  once it reaches 50 ms (Lua re-arms every `tui.poll` resume). A `pollSync(0)` busy loop still
  never re-arms, and a hangup never does.
- **JS `gpu.bufferCopy` charges no more than the source holds.** An explicit `size` was charged
  as given, so `size: -1` tripped the budget (uncatchable) instead of failing with the catchable
  `out_of_bounds`; it now charges what Lua charges.
- **JS `WasmInstance.call` without `maxOutput` is clamped to the VM heap limit.** The instance's
  own default skipped the clamp, so an oversized output was copied off the heap before it was
  refused; the guest now fails `output_too_small` first, as in Lua.
- **An unchanged TUI frame writes nothing.** `flush` moved the cursor on every call even when no
  cell changed, so a `tickMs: 0` redraw loop flooded the terminal and, on one that read slowly,
  sat blocked in `write` where neither the run deadline nor the budget could stop it.

**Runtime - audit 29 fixes (caps / DB)**
- **App SQL cannot write SQLite's bookkeeping tables.** `UPDATE sqlite_sequence SET seq =
  9223372036854775807` (or an INSERT naming `_hull_jobs`) made every later AUTOINCREMENT insert
  into a `_hull_*` table fail `SQLITE_FULL` without the SQL naming one, and `DELETE FROM
  sqlite_sequence` made the `_hull_jobs` / `_hull_job_events` ids restart after a purge, so a jobs
  event subscriber whose cursor was past the reused ids missed events. INSERT / UPDATE of
  `sqlite_sequence` and `sqlite_stat*` in `main` / `temp` is refused, and so is an app DELETE of
  either (in any database, from a CTE, or from a trigger the app defines on its own table), as is
  `DROP TABLE sqlite_stat1`. The DELETEs SQLite runs itself stay legal - those of DROP TABLE / DROP
  INDEX, which follow the statement's own DROP callback, and of `ANALYZE <table>`, which precede
  its ANALYZE callback, so every app statement is now prepared through `hl_cap_db_prepare`, which
  refuses a statement that ends its prepare with a `sqlite_stat*` DELETE and no DROP / ANALYZE (a
  multi-statement script runs statement by statement through it, no longer through
  `sqlite3_exec`) - as does the rename of a sequence row an `ALTER TABLE ... RENAME` makes, and
  VACUUM's copy.
- **The agent's read-only SQL gate checks every pragma, ATTACH, DETACH and ANALYZE.** An
  argument-less `PRAGMA optimize` (which runs ANALYZE and writes) passed; so did `ATTACH ':memory:'`
  and DETACH. Bare pragmas pass only when they report a value (`user_version`, `page_count`, ...)
  or describe a table; the strict authorizer now stays installed through step and finalize, where
  SQLite re-prepares a statement whose schema changed.
- **MySQL implicit-commit classifier:** MariaDB's `ANALYZE SELECT / UPDATE / DELETE` (an EXPLAIN
  that runs the statement) commits nothing and is no longer resumed as DDL; only `ANALYZE
  [NO_WRITE_TO_BINLOG | LOCAL] TABLE` commits. `CREATE OR REPLACE TEMPORARY TABLE` is temporary.
- **Postgres: a trip on any frame of a lone COMMIT's reply reports the commit.** The audit-18
  exemption covered only a trip on CommandComplete; a trip on ParseComplete / BindComplete /
  NoData, or during the wait for the reply, dropped the connection with the transaction still
  open although the server committed. Once a COMMIT is sent whole, a stop finishes the reply in
  the bounded (2 s) finishing mode instead.
- **Postgres SCRAM:** the server nonce must extend the client's (an equal one was taken), and an
  iteration count below 4096 is refused unless the certificate was verified or
  `sslmode=disable` - over `prefer` / `require` a MITM could ask for `i=1` and brute-force the
  password from the proof. The PBKDF2 state and the derived keys are scrubbed on every path.
- **Postgres read buffer shrinks back** to 8 KiB after a frame over 1 MiB is consumed; a pooled
  connection that once read a big row kept up to ~1 GiB until it closed.
- **A `worker.dispatch` job's end-of-job ROLLBACK can be stopped.** It ran unbound, and an
  unbound charge never stops, so a Postgres / MySQL server silent under `read_timeout=0` pinned
  the pool thread and the SIGTERM join. It now runs under a cleanup budget that stops after 2 s or
  when the pool shuts down; a stopped rollback drops the connection.
- **`db.open` of a file DSN refuses a path through a symbolic link** - a directory component, the
  database file, or its `-wal` / `-shm` / `-journal` sidecar. Containment and the fs.write match
  were checked on the path, then SQLite opened it following links: a planted
  `data/x.db -> ../private/ref.db` opened a file fs.write would have refused.
- **Ed25519 verification refuses a non-canonical `S`.** TweetNaCl alone accepted `R || (S + L)`,
  a second valid encoding of one signature (malleability for anything keyed on signature bytes).
- **Valkey: `sslmode=require` with a password is refused** (use `verify-full`): require verifies
  no certificate, and the handshake sends `HELLO 3 AUTH <user> <password>` over it.
- **The TLS client refuses a verified handshake with no server name**, rather than skipping
  `set_hostname` and checking the chain only (defence in depth for CVE-2025-27809).
- **A WASM instance that grew its memory is not pooled.** Pooling went by the `heap_size` asked
  for, so a guest that grew to ~4 GiB was kept holding it (8 per module, outside the live-instance
  bounds); past its initial memory plus its heap it is now destroyed, on the zero-copy release
  too.

**Runtime - audit 28 fuzzing fixes** (five new libFuzzer harnesses: `fuzz_log_escape`,
`fuzz_http_guard`, `fuzz_sql_kw`, `fuzz_dsn_redact`, `fuzz_mysql_stmt`)
- A SQL `--` comment ends at a carriage return as well as a newline, as SQLite, Postgres and
  MySQL end it. The shared transaction-keyword reader ran it to the next `\n`, so
  `-- x\rCOMMIT` read as a comment where the server ran a COMMIT.
- DuckDB's dynamic-SQL refusal (`query(...)`, `query_table(...)`, `json_execute_serialized_sql`)
  reads past a quoted identifier one byte at a time: a quote inside a string or comment made it
  skip the call that followed.
- Audit-log URL redaction finds the userinfo `@` only inside the authority (before the first
  `/`, `?` or `#`): a password holding `?` or `#` was printed. A URL with an `@` past the
  authority is logged as `scheme://[redacted]`.
- Log escaping writes every control byte but tab (and DEL) as `\xNN`, not only CR / LF: an
  ESC sequence in app text rewrote what a terminal showed.
- A guest log message truncated to fit the log ends with `...` after a whole escape; it
  could cut one short (`a\x0...`). Found by the new `fuzz_wasm_log_escape`; `fuzz_migrate_name`
  covers the embedded-migration filter. The escaper moved to `cap/wasm_log.c`.

**Tooling / sandbox - audit 29 fixes**
- **The CA bundle gets no kernel sandbox grant.** The anchor is read into memory before the
  sandbox applies, but the resolved source was still unveiled read-only (and granted in the
  macOS Seatbelt profile) - including the embedded bundle's label `(embedded)`, a relative path
  that resolved against the working directory: a cloned app shipping a `(embedded) -> /` symlink
  widened the read grant to the whole filesystem. `hl_sandbox_apply` no longer takes a CA path.
- **`hull build` / `hull compute build` refuse inline assembly in a compute module.** The
  assembler's own macro language (`.macro` with `\()` concatenation) builds an `.incbin` at
  assembly time, past every scan of the preprocessed text, so the auto-rebuild could embed any
  file the developer can read. An `asm` / `__asm` / `__asm__` token outside a string literal now
  fails the build; a freestanding `hull_process` module has no use for it.
- The generated `app_registry.c` escapes `?` (`\077`) in entry names: it is compiled with
  `-std=c11`, where a file name holding the `??/` trigraph became a backslash that escaped the
  literal's closing quote.
- DuckDB's read-only `/proc`, `/sys` and `/etc/localtime` grants are made only when a linked
  backend claims the `duckdb://` scheme; a `--with=postgres` or `--with=mysql` app (any composed
  backend) got them too.

**Runtime (Lua) - audit 29 fixes**
- **Lua `print` in an app is a log record now (behaviour change).** It wrote its arguments
  straight to stderr, unescaped - a CR / LF / ESC in app text forged log or terminal lines - and
  charged nothing, so a loop of huge prints was free. It now writes one INFO record tagged
  `[app]` (`HH:MM:SS INFO  [app] ...`, like `log.info` and JS `console.log`), control bytes but
  tab escaped, each argument charged 1 unit per 8 bytes before the write. Scripts that parsed a
  bare `print` line from stderr should match past the prefix, or write to `ctx.stdout`. The
  tool VM (the CLI plugins) keeps the raw print.
- No wait and no `hull.async` task starts once the runtime is shutting down. A `__gc` finalizer
  run at teardown could start a task whose `hull.sleep` armed a timer on a loop that never runs
  again and tracked its ctx after the teardown sweep; every Lua wait (sleep, `http.fetch`,
  `db.async`, compute, gpu, ssh, smtp, `worker.dispatch`, `tui.poll`) now refuses then, and
  `hl_lua_free` marks the runtime shutting down before `lua_close` on every path.

**Runtime (JS) - audit 29 fixes**
- **`db.udf.register` / `unregister` refuse a `db.open()` connection.** Called with a `db.open`
  handle as `this` (`c.udf.register.call(d, ...)`), they registered a UDF on the dynamic handle,
  where `udf` is meant to be absent, and an options getter that closed it left the call using a
  freed handle. Both now throw a TypeError for such a `this`, and resolve the handle again after
  the last argument conversion.
- **GPU buffer data, uniforms and texture data take typed arrays and buffers as bytes.** Only an
  ArrayBuffer was taken as bytes: a `Float32Array` uploaded its text `"1,2,3"` and a WasmBuffer /
  MappedBuffer `"[object WasmBuffer]"`. Typed arrays, WasmBuffer, MappedBuffer and images are now
  bytes in `gpu.dispatch` / `gpu.pipeline` (sync and async) and inline textures; any other object
  is a TypeError. A string still uploads its text.
- **Compute inputs take typed arrays as bytes.** `compute.call` / `compute.async.call` /
  `WasmInstance.call` / its async call / `compute.stream` / `compute.buffer` (and
  `compute.segment` data) read a typed array's own bytes; any other object (a plain object, an
  array, a detached buffer) is a TypeError instead of its `toString`.
- **`ws.broadcast` sends an ArrayBuffer or typed array as bytes**, as `conn.sendBinary` does:
  relaying an `onMessage` ArrayBuffer sent `"[object ArrayBuffer]"`. Any other object is refused.
- **Route `opts.multipart` caps are read once, when the route is registered.** The app's object
  was kept and its getters read when the server wired the routes: one that threw left the cap at
  0 (Keel's "unlimited") with the exception pending. A getter that throws, or a cap that is not a
  non-negative number, now fails `app.post(...)` itself.
- `app.use` / `app.usePost` throw the app's own error when the method or pattern conversion
  throws, instead of a TypeError over it.
- `tui.async(fn)` logs an error `fn` throws (it was left pending), and re-raises a tripped
  instruction budget.
- **`gpu.async.dispatch` reads its options as `gpu.dispatch` does:** `output: false` means no
  read-back (it read buffer 0), `textures` / `outputTexture` are refused rather than ignored, and
  a buffer `name` is kept whatever its length (one over 63 bytes was dropped).
- **`gpu.async.pipeline` with more than one output** resolves to an array of ArrayBuffers in JS
  and a table of strings in Lua (`result`), as the sync form returns; it resolved to an empty
  buffer.
- **Async GPU read-backs and async compute outputs count toward the in-flight result total**
  (both runtimes): each is taken on the VM's `async_result_bytes`, under its heap limit, on the
  worker thread before the copy is made, and the op fails `result too large` past it - as
  `db.async` results and `worker.dispatch` returns already did (audit 23).
- `db.async` exec results, `gpu.devices()`, the multi-output `gpu.pipeline` array and the
  `smtp.send` result object fail with an exception when a value cannot be made, instead of
  returning a half-built object.
- `key.secretboxOpen` zeroes its plaintext scratch buffer before freeing it.

## [0.17.1] - 2026-10-18

Full PR list:
[GitHub release v0.17.1](https://github.com/artalis-io/hull/releases/tag/v0.17.1).

This release carries security audit round 17 (#758-#770). No public API was added or removed, but
some calls that used to pass silently now refuse or are charged to the instruction budget. Check
the list below before upgrading.

### Upgrade notes
- **Reinstall tool bundles once.** `hull tools install` now records which release a bundle came
  from, and `hull build` checks it against the running hull. A bundle installed by an earlier
  release (`zig`, the `libc-musl-<arch>` floors) fails that check: run `hull tools install <name>`
  again after upgrading.
- **A built binary reads nothing from disk beside it.** Templates and relative `require` /
  `import` modules that are not embedded in the binary are no longer read from its working
  directory (migrations and static files already worked this way). A built app's entry now
  resolves its relative requires against the embedded copies; before, they were silently read
  from the disk beside the binary. Development (`hull app.lua`, `hull dev`) is unchanged.

### Behaviour changes

**Runtime**
- **Middleware numeric returns mean the same in both runtimes.** The value is truncated toward
  zero and only its sign counts: 0 continues, positive stops, negative answers 500. `NaN`, `nil`
  and `undefined` continue. So Lua `1.5` now stops (it continued) and `2^32` stops in both (it
  wrapped to 0). A JS `valueOf` that throws answers 500 instead of continuing.
- **A `hull.async` task that trips the instruction limit while `hull._spawn` runs it inline now
  raises the trip in its spawner too.** Before, the spawner ran on until its own hook came round.

**Stdlib**
- **`validate` `min` / `max` on strings count code points in both runtimes.** Lua counted bytes,
  JS UTF-16 units. JS `validate` also reads only the data's own properties, not its prototype.
- **`etag` sends no ETag for an empty body, and never answers it 304.** Lua raised (a 500) and JS
  sent `ETag: null`.
- **`hull.jobs` / `hull:jobs` no longer export `_tick`, `_cron_next` / `_cronNext`,
  `_events_drain` / `_eventsDrain` or `_config`.** They wrote `_hull_*` rows with a caller-chosen
  clock. Drive jobs through the public API (`jobs.work`).
- **JS `http.fetch` & co fail when a header's getter or `toString` throws.** The header used to be
  skipped and the request sent.

**Database**
- **Postgres and MySQL replies are charged frame by frame** (1 unit plus 1 per 64 bytes), every
  frame type included, so a `LOOP RAISE NOTICE` stream or the rows of a 2nd+ result set stop at
  the run's limit. This also bounds `db.async` ops.
- **Postgres refuses `COPY ... TO STDOUT`** by dropping the connection.
- **The connection startup is charged.** Postgres startup frames are charged like reply frames,
  and a SCRAM login 16 units per round before the key derivation (the default 4096 rounds cost
  65536 units). The MySQL auth exchange takes at most 16 packets and charges each one.
- **MySQL refuses more parameters than placeholders** (`parameter count mismatch`), as SQLite and
  Postgres do. The surplus used to be dropped.
- **Valkey `scan` / `clear` charge every SCAN step** and stop at the run's limit.

**GPU**
- **Each device holds at most 1 GiB of GPU memory.** This covers persistent buffers and textures
  and the buffers, uniforms and inline textures a `gpu.dispatch` / `gpu.pipeline` creates for the
  call, sync or async; past it the call fails with `device_memory_limit`. The creates, uploads and
  inline buffers are charged 1 unit per 64 bytes.
- **A GPU read-back larger than the VM heap limit is refused** with `output_too_large`.

**Files, tar, mime, encoding**
- **`tar.parse` / `tar.extract` charge the whole input up front**, 1 unit per 64 bytes.
- **Content sniffing:** a window of nothing but whitespace has no type, and an SVG or HTML
  document after leading whitespace is sniffed as such (it came out `application/xml`).
- **JS `hull:encoding` codecs refuse an output larger than the heap limit** and charge input and
  output 1 unit per 8 bytes.

**Tooling**
- **Tool spawns never run a program from the working directory.** A bare tool name (`nm`,
  `clang`, ...) is resolved through absolute `PATH` entries only and refused when not found;
  `hull dev` re-runs hull by its own path. Before, an empty or `.` `PATH` entry let a repo's own
  `nm` or `hull` run during `hull build` / `hull dev`.
- **`hull verify` without `--developer-key` reports the app layer `VALID (self-consistent)`** and
  warns that the signer is not pinned. Exit status unchanged.
- **A `"$VAR"` `databases.internal` must be declared** in `env` or `secrets` before it is read,
  also under `hull test` and `hull agent`.
- **The audit log writes outbound URLs as scheme://host/path** (no userinfo, query or fragment).

### Fixes worth knowing
- **Shutdown no longer crashes when a Lua `__gc` finalizer uses a database connection.** The
  connection registry now outlives the runtime; a finalizer's `conn.query`, `db.default()`,
  `db.connect` or `db.open` is refused.
- **A JS handler resumed by `req.multipart()` that fails keeps the headers earlier middleware
  set** on its 500.
- **Lua binding lifetime:** a kv `scan` closed by a finalizer mid-call, an `image.encode` closed by
  its options' `__index`, and image / tar views closed during argument conversion no longer touch
  freed memory. `conn.data` set on a closed WebSocket no longer leaks.
- **A cache-sourced feature archive is copied, then verified**, and recorded in the release trust
  domain.

## [0.17.0] - 2026-10-16

Full PR list:
[GitHub release v0.17.0](https://github.com/artalis-io/hull/releases/tag/v0.17.0).

This release carries security audit rounds 14, 15 and 16, Keel v3.3.1, and fixes for the Windows
source build. No public API was removed, but some calls that used to succeed or pass silently now
raise, refuse, or rate-limit. Check the list below before upgrading.

### Behaviour changes

**Stdlib**
- **ttl options must be positive finite numbers.** This covers:
  - session `ttl` / `absolute_ttl`, and the per-call ttl of create / load / update;
  - idempotency init and middleware;
  - inbox init and mark / check;
  - totp `lockout_duration_per_ip`.

  A string ttl, `0` or a non-finite value now raises or throws in both runtimes. Before, JS
  concatenated a string onto the expiry and Lua coerced it. An invalid `absolute_ttl` now raises
  instead of switching the cap off with a warning; `false` still disables it.
- **Email change: the confirmation mail is rate-limited**, per recipient and per account. An
  over-limit request is answered with **429**. Before, the endpoint could be used to mail-bomb an
  address.
- **TOTP: new secrets are sealed bound to the user id.** Legacy unbound seals still open, and are
  re-sealed bound on the next verify or `rekey()`. The new option `allow_unbound_secrets`
  (default `true`) can turn the legacy fallback off once every secret has been re-sealed;
  `rekey_status` reports the remaining unbound count.
- **User ids such as `42.0` are accepted as the integer `42`** by the shared user-id helper.
- **The `_test` exports are gone** from inbox, totp and auth-flows. Test-only helpers now register
  with the stdlib-internal `hull._test_hooks` / `hull:_test_hooks`, which app code cannot import.

**Database**
- **`db.async` results are bounded.** A result larger than the submitting VM's heap limit (or
  256 MB) fails with `result too large`, and the copy is charged to the run's budget. On Postgres
  and MySQL an over-cap result now stops reading the reply and closes the connection.
- **`conn.wait_notify` raises on a backend without LISTEN/NOTIFY**, instead of taking a pool
  thread. Its timeout is clamped to 0..60 s.

**Files, blobs, tar**
- **Whole-object reads larger than the VM heap limit are refused before anything is allocated:**
  `fs.read`, `blob.get` and a blob reader's `read(n)`. `read(n)` is clamped to 16 MB. These reads
  are now also charged to the instruction budget, at 1 unit per 8 bytes.
- **`tar.create` / `tar.pack`** take at most 65536 entries and refuse an archive larger than the
  heap limit. `tar.extract` is now charged.
- **The tar reader refuses names it would read differently from `tar`:**
  - GNU long-name / long-link records (`L` / `K`);
  - a non-empty ustar name prefix;
  - pax records that set `path`, `linkpath`, `size` or `GNU.sparse.*`.

  Other pax headers (`x` / `g` / `X`, e.g. mtime or uid) are skipped and the archive is accepted.
  Tool bundles with paths over 100 bytes need `tar --format=ustar`.

**Compute (WASM)**
- **Persistent instances can no longer grow memory past their heap.** This applies to
  `compute.instance`, `compute.stream` and WASM-backed `db.udf`. `memory.grow` is capped at the
  module's initial pages plus `heap_size`, and a grow beyond that returns -1 in the guest. A module
  that relies on growing its memory needs a larger `heap_size`. A memory the module neither
  imports nor exports gets no growth.
- **A `compute.segment` over 1 GiB is refused** with `segment_too_large`. A failed add or replace
  leaves the old segments loaded.
- **`hull compute build` is stricter about what a module may read.** It refuses:
  - a dependency outside `compute/<name>/`;
  - an `#embed`;
  - an `incbin` or asm `.include`;
  - any directive left after preprocessing other than line markers and a short pragma allowlist,
    which also catches directives assembled by macros.

**WebSocket, SSE, shutdown**
- **SSE and WebSocket writes are charged to the instruction budget.** SSE `event` / `comment` cost
  1 unit per 8 bytes plus 1 per line. ws `send` / `send_binary`, `ws.broadcast` and the ws-client
  sends cost 1 unit per 8 bytes plus 1 per frame, per destination. The charge is taken before the
  write.
- **A WebSocket still open at shutdown gets no `on_close`.**
- **A Lua `ws.connect` during teardown raises.** This includes one called from a `__gc` finalizer.
  Before, it crashed the process.

**Keel v3.3.1**
- **SSE events go out as one HTTP chunk and one transport write each**, instead of three chunks per
  line. Output bytes are unchanged.
- **A second `end_stream` no longer emits another terminating chunk.**
- **HTTP/2 streams refuse the streaming entry points** instead of writing HTTP/1.1 framing.

### Fixes worth knowing
- **HTTPS shutdown no longer double-frees the server TLS context.** The context belongs to Keel
  once the server is initialised.
- **Shutdown and teardown order was corrected** for parked handlers, detached fetches, held
  WebSockets and app.main leftovers. Covered by `make e2e-teardown`.
- **Windows: `hull compute build` works again.** The tool sandbox now admits the clang that Hull
  itself resolves on `$PATH`.


## [0.16.0] - 2026-10-13

Not itemised here. Headline work: the `hull/net` byte stream (connect, TLS) and
the `hull/ssh` module (SSH transport, Ed25519 / ECDSA / RSA keys, exec channels,
SFTP, a known_hosts trust store, WebSocket tunnels), `hull.kv` encryption at
rest, `hull.encoding` as the one home for hex / base64 / base32, joined
concurrent work (`hull.async` tasks, `hull.gather`, `hull.map`),
`databases.internal`, a configurable `http.fetch` timeout, the per-run
wall-clock watchdog, and a signed manifest that names its release. Two breaking
changes: `hull.crypto` takes and returns bytes, with no hex anywhere, and JS has
one shape for checking untrusted input. Full PR list:
[GitHub release v0.16.0](https://github.com/artalis-io/hull/releases/tag/v0.16.0).

## [0.15.0] - 2026-09-20

Windows first-run developer experience. Installing Hull, scaffolding an app,
building it, and launching the result now works end to end on a clean Windows
box without knowing anything about PowerShell PATH refresh, Cosmopolitan
toolchain setup, APE filename conventions, Unix package managers, or Makefiles.

### Added

- **`hull doctor --fix`.** Installs the missing *Hull-managed* prerequisite
  doctor just reported - on a cosmo build, `hull tools install cosmocc`. It only
  ever runs a command doctor would have printed, through the ordinary
  `hull tools install` path (same Ed25519-signed-manifest trust chain, same
  `~/.hull/tools/` destination), never touches PATH / the registry / system
  directories, and never installs optional capabilities. Mutually exclusive with
  `--json`.
- **`hull build --keep-build-artifacts`.** Leaves cosmocc's debug sidecars beside
  the produced binary instead of relocating them (see below).
- **Host-aware output rendering** (`src/hull/shared/host.c`, exposed to the tool
  VM as `tool.host_os()` / `tool.exe_suffix()` / `tool.render_exec()`). One place
  owns the executable suffix a host requires and how a runnable command is
  spelled, instead of per-platform string literals scattered through the CLI.
- **`hull doctor --json`** gains `host_os`, `exe_suffix`, `build_compiler`,
  `build_compiler_required`, `fix_command`, and `ca_bundle.effective` /
  `ca_bundle.ok`.

### Changed

- **Keel upgraded to v3.0.0** (from the `3.0.0-rc.3`+3 pin). Hull inherits the
  release's headline server fix without any code change: an early final response
  is no longer destroyed by its own teardown. Previously a 413 / 431 / 415 / 500
  - or an auth rejection on a `multipart` route, which is a documented Hull
  pattern - left the request body unread, and closing a socket with unread
  received data makes TCP send RST, discarding the response the client should
  have seen. Keel now flushes the response, half-closes its send side, and drains
  inbound bytes asynchronously, bounded by `reject_drain_max_bytes` (64 KiB) and
  `reject_drain_timeout_ms` (500 ms); Hull leaves both at their defaults. Also
  picked up: `kl_async_complete` sends the response when `on_resume` leaves the
  connection processing, a connection pool no longer claims its old capacity
  after being freed, and several IOCP correctness fixes that matter on Windows.
  No API Hull uses changed - the only public removals are `max_align_t` from a
  header (so MSVC consumers can compile it) and the prerelease version macros.

- **`hull build` names the produced binary for the host that must run it.** The
  default output is now `app.com` on Windows (`app` on Linux/macOS, unchanged).
  Windows will not execute an extensionless file, so the APE previously written
  as `app` could not be started at all until it was manually renamed. An explicit
  `-o` / `--output` is still honoured verbatim.
- **A normal build leaves one obvious shippable executable in the app root.**
  cosmocc's debug sidecars (`<base>.com.dbg`, `<base>.aarch64.elf`) move to
  `<output dir>/.hull/build/`. The move happens after the post-link symbol
  verification and is non-fatal; signing, verification, and inspection all
  address the output path and are unaffected.
- **`hull build` prints how to start the binary**, spelled for the host
  (`./app` / `.\app.com`) - neither PowerShell nor a POSIX shell searches the
  current directory.
- **`hull doctor` gives platform-aware, actionable guidance.** It distinguishes
  required-and-missing from optional-and-absent from working-fallback, asks for
  the compiler *this* binary can actually link with (cosmocc on a cosmo build,
  cc/gcc/clang on a native one), recommends Hull's own toolchain bootstrap over
  an external package manager where one exists, names a Windows route for the
  optional WASM source toolchain, and presents composable features via
  `hull feature install` rather than a Makefile. `make ...` now appears only
  labelled as the developer/source-build route.
- **An absent system CA store is no longer rendered as a failure** when the
  embedded Mozilla bundle is present - that is the designed fallback and the
  normal state on Windows.
- **Default CLI output no longer exposes Hull's own C source coordinates.**
  `hull new` / `hull build` and friends previously printed lines like
  `INFO src/hull/sandbox_tool.c:243: [sandbox] tool mode applied`, because
  nothing configured the logger on the CLI path. Warnings and errors still
  print; `--verbose` restores every level plus `file:line`. `--verbose` and
  `--json` are now recognised after the subcommand as well as before it.
- **`install.ps1` updates the PATH of the session it runs in**, so `irm ... |
  iex` leaves `hull` immediately resolvable with no new terminal. When the
  installer cannot prove it reached the caller's shell (run from a file, where a
  same-process `.\install.ps1` is indistinguishable from a separate-process
  `powershell -File`), it prints the exact one-line command instead of advising
  a restart.
- Scaffolded `.gitignore` files now ignore the built binary (`/app`,
  `/app.com`) and Hull's per-project scratch directory (`/.hull/`).

### Fixed
- **A Windows toolchain path leaked into `package.sig`.** The compiler and
  linker vtables took the tool's name by splitting the invocation on `/` only,
  so a Windows-shaped path stayed whole. `hull build` then printed
  `compiling with C:\msys64\ucrt64\bin\gcc.exe` and recorded that absolute path
  as the `cc` field inside `package.sig` - a developer's directory layout baked
  into a build artifact, and a different recorded value on two machines with
  the same toolchain. The name is also matched against (`cosmocc`, `tcc` in
  `build.lua`), and a path matches nothing.

  Both now derive the name once, through a shared `hl_host_tool_name`:
  basename on either separator, minus a trailing `.com` / `.exe`, so
  `C:\tools\gcc.exe`, `/usr/bin/gcc` and a bare `gcc` all yield `gcc`. The
  spawn allowlist's inline copy of that rule (hull#471) now uses the same
  helper, leaving one implementation rather than two that must agree.

- **`hull doctor` could not find any compiler on Windows.** Its PATH search split
  on `:` and joined with `/`, which shreds the drive letters in a `;`-separated
  Windows PATH, so every probe reported "not found" regardless of what was
  installed. It now splits on the host separator and also tries the `.exe` /
  `.com` forms.
- **`hull doctor` reported a multi-arch build as `single-arch`.**
  `hl_build_get_platforms()` returned 0 whenever its optional out-parameter was
  NULL, which is how doctor called it. This is what made doctor's "single-arch"
  and `hull build`'s "dual-arch" look contradictory; both messages now also say
  which layer they describe (platform archives vs. the APE link).
- **`hull doctor` could report `hull build  ready` for a build that then failed
  at the link step** - a cosmo hull with a stray gcc on PATH. Readiness now
  requires the compiler that binary can actually link with, matching
  `hl_driver_resolve_native()`.
- **An app's own top-level logging no longer pollutes build output.**
  `hull build` executes the app entry to capture `app.manifest()` (it is a call,
  not static data, and it decides which feature archives get composed). That
  window is now explicitly marked: the app's log output is suppressed by default
  and labelled `[build-eval]` under `--verbose`. The window's existing bounds -
  the kernel sandbox, stripped dynamic-code loaders, no-op capability stubs,
  top-level only, once per process - are now documented at the call site.
- **An interrupted cosmocc build could not be resumed, only `make clean`ed.**
  cosmocc compiles every translation unit twice - `foo.o` beside
  `.aarch64/foo.o` - and pairs the archives that collect them the same way, but
  only the first of each pair is ever named as a make target. A build stopped
  part-way (Ctrl-C, or the Windows source-build watchdog killing a wedged cc1)
  therefore left a half-written pair that every later `make` skipped as up to
  date, and the build died at the fat link on a message that named the archive
  rather than the cause (`linker input missing concomitant
  vendor/keel/.aarch64/libkeel.a file`). A parse-time repair now removes a
  half-written pair so the ordinary rules rebuild both halves. This is also what
  makes the Windows source build's retry-after-stall work: it resumes from the
  objects already built, and one of those was the pair its own kill had broken.
- **`hull build`'s no-compiler hint pointed at a flag that no longer exists.** It
  advised "rebuild hull with `HL_ENABLE_TCC=1`", which was removed with the rest
  of the tcc toolchain, so following it could only fail. The hint now names a
  route that exists on the running binary, as doctor's do: a cosmo hull (the
  build that reaches Windows) is told to run `hull doctor --fix` or
  `hull tools install cosmocc`, since it can only link with cosmocc and Hull
  installs that itself; a native build is told to install gcc or clang or point
  at one with `--compiler=<path>`.
- **`hull doctor --tui` no longer reserves a row for embedded tcc.** `doctor.c`
  has emitted no `tcc_embedded` field since tcc was retired, so the row was
  unreachable.
- **No toolchain on PATH was findable on Windows.** `hl_host_find_in_path` -
  the resolver introduced to fix exactly this - inferred the list separator
  from the host and split on `;`. But a Cosmopolitan APE, the only Hull build
  that runs on Windows, is handed a POSIX-shaped PATH by its own runtime
  (measured on Windows 11: `/C/Users/...:/C/Program Files (x86)/...`), and an
  MSYS2 or Git-Bash shell exports the same shape, so the whole list collapsed
  into one nonsense component and every probe reported "not found" however much
  was installed. The separator is now taken from the list (`;` when one is
  present, or a single drive-prefixed entry; `:` otherwise) and each hit is
  composed with its own component's separator, so a POSIX-shaped entry stays
  POSIX-shaped. On a POSIX host both rules are exact no-ops. Measured on this
  machine: `hull doctor` went from reporting no compilers to resolving the
  `cc.exe` that had been on PATH all along.
- **`hull doctor` and `hull tools list` could disagree about the same tool.**
  `hl_tools_lookup_path`'s PATH step kept a second, private walker that split
  on `:` and joined with `/` - the copy hull#459 replaced in doctor, left
  behind here - so the two surfaces used different rules and neither tried the
  `.exe` / `.com` forms. It now delegates to the shared resolver, leaving one
  PATH walker in the tree.
- **`hull build --compiler=<name>` failed on Windows for a compiler that was
  installed.** Selection spawned the bare name and left the search to exec,
  which does not search the PATH Windows hands an APE. A bare name is now
  resolved by Hull first (`hl_driver_resolve_name`), falling through to the
  bare name on a miss so exec still gets its turn and POSIX behaviour is
  unchanged. The same resolution now applies to `--linker=<name>` and to the
  automatic cc / gcc / clang / cosmocc probes.
- **cosmocc could not be driven from a shell whose `$HOME` differed from the
  install's.** cosmocc ships a `#!/bin/sh` driver that Windows cannot execute
  directly, so Hull routes it through the busybox in the same bundle - but
  looked for that busybox only under `$HOME`. Under MSYS2 `$HOME` is
  `/home/<user>` (`C:\msys64\home\<user>`) while `hull tools install cosmocc`
  from PowerShell installs under `C:\Users\<user>`, so a perfectly good busybox
  sitting next to the resolved cosmocc was missed and no compile could start.
  It is now looked for beside the driver first, with the `$HOME` locations as
  fallbacks.
- **Running a lint gate deleted your build.** The build-fingerprint purge and
  the fat-cosmo pair repair are parse-time `$(shell)` hooks, so they fire on
  *any* `make` invocation whose configuration differs from the last build - and
  a gate is normally run without the flags the tree was built with. Measured:
  after `make CC=cosmocc HL_OPT=-O0 HL_ENABLE_WASM=0`, a plain
  `make check-keel-flags` removed 429 objects and `build/hull`. `make -n` did
  the same, because `-n` does not suppress parsing. Both hooks are now skipped
  when the tree must not change: under `-n`, and for goals that build nothing
  (`lint`, `help`, and every `check-%` except `check-hardening`, which depends
  on `build/hull` and so genuinely does build). New gate
  `make check-dry-run-inert` pins it, exercising the MAKEFLAGS detection - the
  fragile part, where a naive `findstring n` reads `--no-print-directory` as a
  dry run - in a temp directory where nothing can be lost.
- **`check-keel-flags` reported six failures on any machine that had built
  Hull.** It dry-ran `make -n vendor/keel/libkeel.a` and read the compile
  lines, but that archive's only prerequisites are Hull's mbedTLS objects, so
  once it exists make answers "up to date", never enters Keel's sub-make and
  emits no compile lines at all. The gate then reported "no Keel compile lines
  found (recipe changed?)", which reads as a regression in the thing being
  gated rather than as the gate mis-firing. It now asks with `-B` what the
  commands *would* be, which is the actual question.
- **`check-keel-flags` asserted LTO propagation on toolchains that cannot do
  LTO.** Hull's own `HL_LTO_CFLAG` comes from a compiler probe and stays empty
  when neither `-flto=thin` nor `-flto` works, so there was no flag to
  propagate and the gate was testing the probe. It now skips that
  configuration, mirroring what it already did for CFI, and the negative
  self-test skips its matching probe for the same reason (that probe is
  observable only through the LTO configuration).
- **`make platform-cosmo` built a corrupt aarch64 platform archive on Windows.**
  Keel picks per-platform translation units (`socket_posix.c` vs
  `socket_winsock.c`, `platform_posix.c` vs `platform_win.c`, and four more) and
  its `clean` removes the set for the configuration it was *parsed* with. Hull
  invoked that clean without passing `CC`, so on Windows - where Keel sets
  `WINDOWS := 1` from `uname` - it deleted the Windows objects and left every
  POSIX object a cosmo build had made. Measured under MSYS2:
  `make -n clean` names 17 win objects and 0 posix; `make -n clean
  CC=x86_64-unknown-cosmo-cc` names the 6 posix ones.

  The aarch64 pass then found `platform_posix.o`, `socket_posix.o`,
  `udp_cmsg.o` and three siblings already present and up to date, and archived
  those **x86_64** objects into the aarch64 library. Every `hull build` against
  it then died in the app link with `ld.bfd: i386:x86-64 architecture of input
  file ... is incompatible with aarch64 output` - naming binutils rather than
  the stale object. Each keel clean is now given the same `CC`/`AR` as the build
  whose objects it removes, and asserts that no object survived it; `make clean`
  passes its own `CC` through for the same reason. On Linux both configurations
  select the same POSIX set, so this worked there by coincidence.

  The sweep also covers `.aarch64/` residue under `integrations/`, which Keel's
  own clean does not reach (it sweeps `src/`, `src/protocols/*/` and
  `vendor/llhttp/`) - found by the new assertion on its first run.

### Security

- **Hull announced a kernel sandbox on Windows that was not there.** A
  Cosmopolitan APE is the only Hull build that reaches Windows, and
  `sb_supported()` returned true for every cosmo host unconditionally - so
  startup logged `[sandbox] phase 1 pledge applied (exec/proc/fork blocked)`
  and `[sandbox] applied (... pledge: stdio rpath wpath cpath flock fattr
  inet)`, which a reader can only take as "this process is confined". It was
  not. Cosmopolitan's `pledge()`/`unveil()` enforce only where the host gives
  them a mechanism (seccomp-bpf plus Landlock on Linux, the native syscalls on
  OpenBSD); everywhere else both return 0 and do nothing. Measured with cosmocc
  4.0.2 on Windows 11: `pledge("stdio", NULL)` returns 0 and a following
  `socket(AF_INET, SOCK_STREAM, 0)` succeeds, though `stdio` grants no `inet`.
  The same false claim covered a cosmo APE on macOS and the BSDs, where the
  cosmo branch is selected ahead of the `__APPLE__` one and Seatbelt is never
  reached.

  `sb_supported()` is now true only on the hosts that enforce, and any other
  host emits one warning naming what is and is not protecting it:

  ```
  [sandbox] NO kernel sandbox on this host: syscall and filesystem
  confinement, and W^X, are not enforced. Hull's capability layer
  (manifest fs / env / hosts) is the only boundary.
  ```

  Startup is **not** refused there. W^X normally fails closed without a kernel
  sandbox, but that rule exists for a backend that is incomplete - Linux
  without Landlock, which still refuses unless `--allow-degraded-sandbox`. On a
  host with no backend at all there is no partial sandbox to opt into and no
  setting that would produce one, so demanding a flag on every run would add
  friction without offering an actionable decision. Behaviour on Linux, macOS
  (native) and OpenBSD is unchanged.

- **`hull doctor` reports the sandbox.** A new `Sandbox` section says whether
  pledge/unveil are enforcing on THIS host, or that the capability layer is the
  only boundary - the fallback state, not a failure, so the exit status is
  unaffected. `--json` carries `kernel_sandbox`. Whether a backend exists is a
  host fact rather than a build one for the cosmo binary, so it could not be
  read off the build before.

## [0.14.0] - 2026-08-27

Windows stabilization. A produced Cosmopolitan APE web app now serves on Windows
(and every host); manifest extraction gains project-rooted local-module
resolution with hardened security; `hull update` self-replaces safely on a
locked Windows executable; and the tool-resolution surfaces (`doctor` /
`tools list` / `agent tools`) agree. Several fixes are security-relevant.

### Upgrade notes (Windows)

- **One-time manual step to reach 0.14.0 on Windows.** The Windows self-update
  fix ships *in* 0.14.0, so a 0.13.0 binary cannot install it: 0.13.0's updater
  tries to overwrite the running `.exe` in place, which Windows refuses, and it
  fails with an `atomic_write: rename ... failed` error, leaving the old binary
  intact. To move from 0.13.0 to 0.14.0 on Windows, **download `hull-cosmo` from
  the 0.14.0 release and replace your `hull` executable manually while Hull is
  not running** (close any running instance first). From 0.14.0 onward `hull
  update` self-updates normally on Windows via the deferred rename-aside swap.
  macOS and Linux are unaffected (their atomic-rename self-update already
  works). (#429)

### Added

- **Unified tool resolution.** `hull tools list`, `hull agent tools`, and
  `hull doctor` now report tool install/resolution through one shared resolver,
  so they agree on whether a tool is a hull-managed install, where it resolves
  (managed / beside hull / on `PATH`), and its accurate on-disk size - recursive
  for a directory bundle. (#428)

### Changed

- **Produced Cosmopolitan APE web apps serve on Windows (and every host).** A
  `hull build` of a cosmo web app previously linked the strong server entry but
  a *weak* route-wiring stub, so the app started and exited without listening.
  The per-runtime HTTP bridge is now force-anchored at compose, with a post-link
  verification. (#422)
- **`hull tools list` / `hull agent tools`** now surface a system/`PATH`- or
  sibling-resolved tool (not only `~/.hull/tools`) and report accurate bundle
  sizes; the prior behavior reported every directory-bundle tool (cosmocc, zig,
  the musl floors) as not-installed with size 0. (#428)
- **`hull tools install cosmocc`** on a native hull now fails fast (before any
  network round-trip) with a clear message that cosmocc is the cosmo/APE
  toolchain for Windows app builds, installed from the cosmo `hull`. (#428)

### Fixed

- **One project-rooted local-module resolution rule** across dev, test, manifest
  extraction, and built binaries (Lua + JS): a nested `require`/`import` resolves
  relative to the requiring module and fails closed on escape past the app root.
  A manifest-extraction failure that controls feature composition is now fatal
  rather than silently building with defaults. (#423)
- **Non-admin Windows cosmocc extraction.** Archive links prefer a validated
  hardlink (Windows can't create symlinks without elevation / Developer Mode),
  copying only from a verified regular target inside the extraction root; the
  trimmed bundle's producer fails on any dangling member, so an incomplete
  toolchain is never shipped. (#425)
- **JS manifest extraction is fatal on a deferred top-level throw or failed
  import** (parity with Lua), via a bounded, fail-closed settle so a runaway
  microtask or a never-resolving top-level await can neither hang the build nor
  silently mis-compose it. A permissive, non-thenable lenient stub lets a
  feature-module import during extraction still reach `app.manifest()`. (#426)
- **`hull update` on Windows.** A running `.exe` can't be overwritten in place,
  so self-update now falls back to a deferred rename-aside swap (`<self>.old`)
  with rollback on failure and a next-startup sweep of the aside copy; the POSIX
  atomic-rename path is unchanged. (#429)

### Security

- **Module loading is descriptor-relative and symlink-contained to the app
  root** - the shared virtual-root resolver (`openat2` / `RESOLVE_IN_ROOT` with a
  held-fd userspace fallback), replacing a realpath-then-open check: no TOCTOU,
  and a module symlink cannot escape the app root. (#423)
- **Dynamic code evaluation is removed from the manifest-extraction window** -
  `load` / `loadfile` / `loadstring` / `dofile` (Lua) and `eval` / `Function`
  (JS) are unavailable while extracting a manifest and cannot be recovered after
  importing an approved helper. (#423)

### Known issues

- A `hull build` of an app that schedules a self-referential-closure infinite
  microtask can intermittently abort in QuickJS's teardown GC (no binary is
  produced; loader liveness and the manifest outcome are unaffected). Tracked in
  #427; the preferred fix is subprocess-isolated extraction, post-0.14.0.

## [0.13.0] - 2026-08-08

Stdlib maturation: five new plumbing modules, a house style guide that codifies
the stdlib error convention, and a sweep of cross-runtime (Lua/JS) drift fixes -
several security-relevant - each now guarded by a byte-for-byte parity or
interop test so the two runtimes cannot silently diverge again.

### Added

- **`hull/cache`** - in-memory key/value cache with TTL and get-or-compute
  memoization: `cache.fetch(key, ttl, fn)` / `get` / `set` / `has` / `delete` /
  `clear` / `size`, plus `cache.new(opts)` for an isolated instance. Bounded
  (LRU eviction at a `max_entries` cap), lazily expiring, process-local; stores
  any value with no serialization.
- **`hull/config`** - typed configuration over the env capability:
  `config.get(name, { type, default, required })` / `config.require`, with
  `string` / `number` / `integer` / `boolean` coercion that throws on a missing
  required value or an uncoercible one. `config.load_dotenv([path])` loads a
  `.env` behind two independent gates - the `fs` capability for the file AND the
  `manifest.env` allowlist per key - so `.env` can never widen what config
  exposes beyond the manifest.
- **`hull/uuid`** - RFC 9562 UUID v4 (random) and v7 (time-ordered, lexically
  sortable - good for database primary keys).
- **`hull/retry`** - generic retry-with-backoff for any fallible operation:
  `retry.run(fn, opts)` + `retry.backoff`. `fn` is arbitrary (HTTP, WASM/GPU
  compute, DB, a plain function); it retries on a thrown error or a
  `retry_on`-flagged result, with exponential backoff and optional jitter.
- **`hull/logx`** - contextual logging over `hull/log`: `logx.with({fields})`
  returns a child logger that appends the bound fields (logfmt) to every line;
  children compose via `.with()`.
- **`env.allowed(name)`** capability - reports manifest env-allowlist membership
  (membership only, never a value), letting code distinguish "declared but
  unset" from "not declared". Used by `hull/config`'s `.env` loader.
- **House style guide** (`docs/stdlib_style.md`) codifying the stdlib error
  convention (failure throws with a stable `.code`; absence is a value;
  validators return data), naming rules, footguns, and the DRY/parity policy.
- **Shared internal helpers** so load-bearing logic lives in exactly one place
  per runtime: `hull.crypto._hex` (raw-byte hex), `hull.web._request` (client-IP
  under a `trust_proxy` policy), and `hull.web._logfmt` (logfmt value formatting,
  shared by the logger middleware and `logx`).
- **Cross-runtime parity / interop test guards** - byte-for-byte Lua/JS e2e
  tests for hex, client-IP, template escaping, `validate`, `config`, `uuid`,
  `cache`, `ratelimit`, `logx`, the option-name aliases, csrf cookie-fallback,
  and `email` error codes, plus a jwt / envelope / csrf mint-in-one-runtime,
  verify-in-the-other interop suite.

### Changed

- **`email.send` throws a coded error** (`invalid_argument` /
  `unknown_provider` / `delivery_failed`) instead of returning
  `{ ok = false, error }`, and returns `true` on success - aligning it with the
  stdlib throw-on-failure convention. **Breaking:** callers must `pcall` /
  try-catch. The JS side also gained the `from`/`to` address validation the Lua
  side already had.
- **Option-name reconciliation.** Modules configured side by side now share one
  canonical option name, with the old spelling kept as a back-compat alias: the
  session cookie is **`name`** (was `cookie_name` in `auth.*`), the auth HMAC
  key is **`secret`** (was `state_secret` in `oauth` / `auth-flows`), and the
  CSRF token lifetime is **`ttl`** (was `max_age`). `cookie.max_age` /
  `cors.max_age` keep `max_age` (a real HTTP `Max-Age`).
- **`ratelimit`** now backs its per-key bucket store with a bounded `hull/cache`
  instance (dropping the hand-rolled sweep + cap). Core rate-limiting is
  unchanged; at the memory cap a new key is now admitted by evicting the
  least-recently-used idle bucket instead of being rejected, so a unique-key
  flood can no longer 429 a genuine new client.
- **`jwt.sign` / `jwt.verify` throw on precondition failures** (missing
  payload/secret, missing verification key) rather than returning a
  `(nil, err)` tuple; an invalid-but-well-formed token still returns nil.
- **`validate`'s email check is stricter and identical across runtimes** (length
  cap, rejects `..` and edge dots, requires a letter TLD).
- **`csrf`** gained a session-id cookie fallback plus `name` / `require_session`
  options, bringing the Lua behavior in line with JS; `session.create` /
  `load` / `update` honor a per-call `ttl = 0` (immediate expiry) in Lua too.
- The htmx widget tier is repositioned in the docs as an optional, opinionated
  component pack, distinct from the frontend-agnostic core (`hull/web/htmx`).

### Fixed

- **jwt HS256 tokens are now interoperable across runtimes (security).** The JS
  side base64url-encoded the signature via a byte-string, UTF-8-inflating every
  byte >= 0x80, so a JS-Hull could not verify a Lua-Hull HS256 JWT (and
  vice-versa). It now encodes the raw digest bytes directly.
- **`logx` / logger log-injection escaping (security).** `logx` escaped only
  `"`; both now share `hull.web._logfmt`, which escapes `\` / CR / LF / `"` (a
  field containing a newline could otherwise forge a second log line), and a
  latent Lua/JS quoting divergence in the logger middleware is fixed too.
- **Template XSS drift (security)** between the Lua and JS engines - a backtick
  and the `json` filter's `<` were left unescaped on the Lua side.
- **`csrf` JS module resolution** - a JS app declaring only
  `hull/web/middleware/csrf` now resolves its `hull/web/cookie` dependency.
- Three Lua-only convention/footgun bugs where the JS sibling was already
  correct (a `csv.parse` doc, the `cookie.secure` doc default, and
  `idempotency`'s `ttl = 0` handling).

## [0.12.0] - 2026-08-07

`hull/jobs@1` grows a nervous system. On top of 0.11.0's durable queue and
workflows, this release adds a durable, subscribable **fleet-wide event log**, a
**strict** (hard-cap) per-key concurrency mode alongside the existing soft
limiter, and **low-latency worker wakeup** over PostgreSQL LISTEN/NOTIFY so a
newly enqueued job is picked up in milliseconds instead of at the next poll.
Every new surface runs unchanged on SQLite, PostgreSQL, and MySQL, at full
Lua/JS parity, and is exercised against real Postgres 16 and MySQL 8 in CI.

### Added

- **Durable fleet-wide events.** `jobs.init({ events = true })` records every job
  lifecycle transition (completed / retried / dead / cancelled / ...) into a
  durable `_hull_job_events` log. `jobs.events(opts)` tails it newest-first
  (filter by `since` / `types` / `limit`) for a dashboard or ad-hoc inspection;
  `jobs.subscribe(name, handler, opts)` / `jobs.unsubscribe(name)` register
  durable named subscriptions with **at-least-once** delivery and a durable
  cursor - a crash between a handler succeeding and the cursor advancing
  re-delivers, so handlers must be idempotent, the same contract as a job
  handler.
- **Strict per-key concurrency.** `enqueue({ concurrency_key, concurrency,
  concurrency_strict = true })` enforces the per-key cap with a hard,
  counter-backed gate that never lets an over-limit job run - complementing
  0.11.0's soft claim-then-reconcile limiter (which favors throughput by letting
  a claimed over-limit job yield and requeue). Pick strict for a hard external
  quota, soft for maximum drain.
- **`conn.wait_notify` (db capability).** A new primitive - `conn.wait_notify`
  (Lua) / `conn.waitNotify` (JS) - that blocks until a `NOTIFY` arrives on a
  channel, backed by a LISTEN/NOTIFY consumer path added to the pure-C PostgreSQL
  wire client. This is the plumbing under the low-latency wakeup below, and is
  usable directly for any app-level pub/sub over a Postgres connection.
- **`examples/workflows`** - a runnable durable workflow-as-code example wired to
  the jobs observability surface.

### Changed

- **Low-latency job wakeup (PostgreSQL LISTEN/NOTIFY).** Workers and durable
  subscriptions on a Postgres backend now wake within milliseconds of a new job
  or event via LISTEN/NOTIFY instead of waiting out the poll interval; a dropped
  LISTEN connection reconnects transparently. SQLite and MySQL keep the poll
  loop. Pure latency win - claim semantics and at-least-once delivery are
  unchanged.
- **`db.exec` returns the affected-row count on PostgreSQL**, matching SQLite and
  MySQL, so an `UPDATE` / `DELETE` reports rows changed uniformly across all
  three backends.

### Fixed

- Hardening pass over the new LISTEN/NOTIFY surface (Low-severity findings from a
  code audit of the events consumer + reconnect path).

## [0.11.0] - 2026-08-06

`hull/jobs@1`: a durable, DB-backed background job queue. Enqueue a unit of work
and process it later with retries, exponential backoff, and a dead-letter path.
It runs unchanged on SQLite, PostgreSQL, or MySQL through the `hull/db`
capability, and `jobs.enqueue` is a plain `INSERT`, so a job commits atomically
with the business row inside the same `db.batch()` (no external broker, no
lost-job window). The module grows from a simple queue all the way to durable
**workflow-as-code** with signals, sagas, and deterministic replay, plus a
DB-derived observability surface. Both runtimes (Lua + JS) at full parity.

### Added

- **`hull/jobs@1` durable job queue.** `jobs.init` / `jobs.enqueue` /
  `jobs.handler` / `jobs.work`; at-least-once delivery with idempotent handlers;
  a concurrency-safe atomic claim (`SKIP LOCKED` on PostgreSQL/MySQL, single-
  writer-serialized on SQLite, with a `FOR UPDATE` fallback for older servers);
  retries with exponential backoff, a `dead` dead-letter status, and a
  visibility-timeout reaper. Enqueue options: `delay` / `at` / `run_at`,
  `priority`, named `queue`, `dedup_key` (idempotent enqueue), `throttle`,
  `max_attempts`.
- **Two execution models, one claim.** An in-process `app.every` poller and a
  dedicated `hull jobs worker` process share the same claim. `run_worker` adds
  intra-process `concurrency = N` (N in-flight handlers) and multi-queue draining
  via `opts.queues` (a **list** for strict priority, a **map** for weighted
  fairness).
- **Ops surface:** `jobs.stats`, `jobs.get`, `jobs.dead`, `jobs.retry`,
  `jobs.cancel`, `jobs.cleanup`, `jobs.heartbeat` (claim-token lock extension for
  long jobs), `jobs.pause` / `jobs.resume` / `jobs.purge` (durable, fleet-wide
  queue control).
- **Durable cron.** `jobs.cron(name, spec, data, opts)` / `jobs.uncron` - 5-field
  UTC schedules in the DB, fired exactly once across the fleet via compare-and-
  set; missed ticks advance to the next occurrence. Fixed-offset `opts.tz`.
- **Fleet-wide rate limiting.** `jobs.limit(queue, { rate, per })` - a shared DB
  counter so K workers share one budget (claim-then-reconcile, nothing dropped).
- **Workflows (job dependencies).** `enqueue({ depends_on = {...} })` - a job
  starts `blocked` and unblocks when its dependencies complete; each dependency's
  return value is injected as `job.deps`. A dead dependency cascade-fails the
  dependent unless `on_dep_failure = "run"`.
- **Result backend + more.** `jobs.result(id)` / `jobs.await(id, { timeout })`
  (yielding wait for a job's value); `jobs.enqueue_many` (bulk); lifecycle hooks
  `jobs.on("completed" | "retried" | "dead", fn)`; `jobs.progress`.
- **Durable workflows (workflow-as-code).** `jobs.workflow(name, fn)` /
  `jobs.start` - a workflow instance IS a job, so it inherits claim / reaper /
  retry / worker / result. Inside the body: `ctx.step(name, fn)` (memoized, so a
  crash or retry resumes past completed steps), `ctx.sleep` (durable timer),
  `ctx.wait_signal` + `jobs.signal` (durable signals with a new non-terminal
  `waiting` status), saga compensation (`ctx.step(name, fn, { compensate })`,
  run in reverse on terminal failure), and the deterministic-replay primitives
  `ctx.now` / `ctx.random` / `ctx.uuid` (memoized, byte-identical across
  replays). `jobs.workflow_status(id)` for DB-derived introspection.
- **Workflow versioning (`ctx.patched`).** Temporal-style patching so a changed
  workflow definition keeps in-flight instances correct: an instance already past
  a patch point keeps the old branch, a new one adopts the patch. Derived from
  the durable step store, no schema change.
- **Soft per-key concurrency.** `enqueue({ concurrency_key, concurrency })` caps
  how many jobs sharing a key run at once, fleet-wide (claim-then-reconcile;
  released over-limit claims keep their full retry budget).
- **Observability.** `jobs.metrics()` - DB-derived, fleet-correct gauges
  (pending / running / waiting / blocked / dead, oldest-pending age) plus, with
  opt-in attempt history (`jobs.init({ history = true })`), latency percentiles
  (wait/run p50/p95/p99) and throughput. `jobs.history(id)` timeline; W3C trace
  propagation (`enqueue({ trace })` → `job.trace` / `ctx.trace`, carried to child
  jobs).

### Fixed

- **TLS teardown crash on long-lived connections.** `hl_tls_client_handshake`
  passed a stack-local `KlAllocator` by address, but Keel's `KlTls` captures the
  allocator by pointer and frees through it at destroy - a dangling read that
  crashed (SIGSEGV) when a long-lived TLS connection (e.g. a `hull/db` PostgreSQL
  or MySQL registry connection) was torn down. Now uses a `pthread_once`-initialized
  static allocator (handshakes run on `db.async` worker threads, so the init is
  thread-safe and TSan-clean).

## [0.10.1] - 2026-08-05

### Fixed

- **`tar` extraction** wrongly rejected a symlink whose `..`-containing target
  still resolved *within* the extraction root, which broke `hull tools install
  cosmocc`; the traversal check now resolves the target against the root before
  rejecting.

## [0.10.0] - 2026-08-05

Self-contained cross-platform builds. A cosmo `hull` on a stock machine with no
pre-installed toolchain can now produce a working APE, capped by the self-hosted
Windows story: `hull tools install cosmocc` side-loads a trimmed, signed
compiler+shell bundle and `hull build` drives it transparently. Rounds out with
in-process `hull test` harness parity, the Makefile modularization, and a sweep
of stdlib audit follow-ups.

### Added

- **Self-contained `hull build` on stock Windows.** A cosmo `hull` plus `hull
  tools install cosmocc` (a trimmed cosmocc + busybox-w64 bundle, published as
  `hull-cosmocc.tar` and Ed25519-signed in the release) builds a working APE on
  a stock Windows box with no pre-installed toolchain. The `cosmocc` tool is
  registered in the side-load registry on the same trust chain as `hull update`
  (fetch the release-signed `hull.sha256`, verify the asset's SHA-256, atomic
  install to `$HOME/.hull/tools/`).

### Changed

- **In-process `hull test` harness parity.** The test harness now wires the fs
  capability from the manifest, so `blob.*` / `fs.*` / `fs.mmap` work under `hull
  test` exactly as they do under `hull dev`; and it supports streaming multipart
  bodies (`req:multipart()` / `req.multipart()`) by pre-feeding the whole
  synthetic body. Both runtimes.
- **Build modularization.** The monolithic Makefile is split into `mk/`
  fragments (features / vendor / platform / …), ~40% smaller, gated by
  resolved-value parity so the split is behavior-preserving.

### Fixed

- **Windows:** `blob.init` / `hl_mkdir_p` failed on Windows because a `mkdir` of
  a drive-root component (`/C`, `/D`) returns `EIO` (not `EEXIST`) under
  Cosmopolitan; `hl_ensure_dir` now stat-checks for an existing directory on any
  `mkdir` errno. (Was mis-filed as "spaces in the project path break the blob
  sandbox.")
- **Stdlib audit follow-ups:** `csrf.js` per-pair body cap (parity with Lua); a
  `build.lua` musl SQLite-engine-probe fix; auth-flows nil-guards (both
  runtimes); `i18n.js` own-property interpolation guard; `template.lua`
  `__`-prefix identifier rejection.

## [0.9.0] - 2026-08-01

The fully-composable base. Every remaining reducible subsystem - the WASM
interpreter, the SQLite engine, the image codecs, mbedTLS, and the Keel event
loop itself - becomes an auto-composed feature, and the distributed `hull`
embeds the SLIM base that drops all of them. `hull build` composes back only
what the app declares, statically at build time (no `dlopen`, no runtime
plugins). A stock build of an `app.main` compute or CLI tool links zero Keel,
mbedTLS, SQLite, and WASM (~2.1 MB, versus ~6.5 MB for a full web app). Flavors
collapse into build.lua presets on that base. This completes the composable-base
arc begun in 0.8.0.

### Added

- **WASM as an auto-composed feature (#118).** The native base is compute-less;
  `hull build` composes the WAMR core + the per-runtime `compute` binding back
  on a two-signal gate (a declared `hull/compute` cap, or a shipped
  `compute/*.wasm` that catches a WASM-backed `db.udf`). A compute-free app
  drops ~256 KB of WAMR. See `docs/wasm_feature.md`.
- **Image codecs as an auto-composed feature (#138).** The base is image-less;
  it composes `libhull_feature-image.a` + the per-runtime `image` binding only
  when the app declares `hull/image` (the `needs_image` gate), so a compute app
  drops ~146 KB of vendored stb. The subtractive `HL_ENABLE_IMAGE=0` knob still
  drops it entirely. See `docs/image_feature.md`.
- **SQLite as an auto-composed feature (#123-#132).** The base ships the DB core
  (the `HlDbBackend` vtable + registry) but not the SQLite engine; it composes
  `libhull_feature-sqlite.a` + the per-runtime udf bridge only when the app uses
  `db` (the `needs_sqlite` gate). A db-free app links zero SQLite. See
  `docs/sqlite_feature.md`.
- **mbedTLS / TLS as an auto-composed feature (#139-#147).** The crypto HMAC and
  asym-verify backends move behind weak hooks and the TLS transport behind a
  seam; the base is TLS-less and composes `libhull_feature-tls.a` only when the
  app needs TLS - an HTTP module, or a `--with=postgres`/`mysql` net-DB
  connection (the `needs_tls` gate, #158). A plaintext app links zero mbedTLS;
  `crypto.*` stays fully available via the portable backend. See
  `docs/tls_feature.md`.
- **The Keel event loop as an auto-composed feature (#148-#153).** The base is
  Keel-free: an `app.main` runner (`serve_cli.c`) on a Keel-less async backend
  (`async/poll.c`). `hull build` composes `libhull_feature-keel.a` (the KlServer
  loop + `libkeel.a`) only when the app needs HTTP (the `needs_http` gate). A
  compute app runs `app.main` / `compute.async` and links zero Keel. See
  `docs/keel_feature.md`.

### Changed

- **The distributed `hull` now embeds the SLIM app-build base** (SQLite-less +
  TLS-less + Keel-less), so a stock `hull build` composes every subsystem up
  from the minimum. Nothing to install or flag: the runtime, HTTP, WASM, image,
  SQLite, TLS, and Keel archives are embedded in `hull` and auto-composed per
  app. Cosmo is exempt (a fat APE cannot force-load native feature archives), so
  its base keeps everything compiled in.
- **`--flavor=pure-compute` is now a build.lua preset (#154), not a pre-built
  platform library.** Since the base already composes, the preset only
  *validates* that the app declares no HTTP/TLS. The pre-built per-flavor
  platform libs and their release matrix were removed. `hull flavor install
  pure-compute` reports "preset flavor, nothing to install"; `hull flavor list`
  shows it as `preset (default base)`.
- **Composed-feature signing extended.** `package.sig.gethull.composed`'s
  `platform_domain` now attests the TLS, Keel, WASM, image, and SQLite archives
  in addition to the runtime + HTTP + tui archives; runtime `--verify-sig` (§5c)
  refuses to boot on any tamper. `HL_PLATFORM_PUBKEY_HEX` is a real key, so §5c
  enforces on every released binary. See `docs/composed_feature_signing.md`.

### Fixed

- **serve:** resolve the default DB path at runtime, not compile time (#155), so
  an HTTP app on the SQLite-less SLIM base initializes cleanly.
- **build/flavor:** default the Keel compose probe to skip (fail-safe) and make
  the flavor asset NULL-safe (#157, audit follow-up).
- **release:** drop the deleted per-flavor SBOM globs left over from the
  flavor-to-preset move (#156).
- Rolled-in pre-arc fixes: udf ctx double-free on `sqlite3_create_function_v2`
  failure (#134), tool-plugin / stdlib / cap audit hardening (#135), agent
  manifest extraction for DB-requiring apps (#136, #137).

## [0.8.0] - 2026-07-27

The composable runtime + HTTP base. The two largest always-present layers - the
script interpreters and the HTTP stack - move out of the app-build base and
compose back only when the app needs them.

### Added

- **Runtimes as composable features (#113).** The base is runtime-less; `hull
  build` composes exactly one interpreter (Lua or JS), auto-inferred from the
  entry extension (`app.lua` -> lua, `app.js` -> js) and embedded in `hull`. A
  single-runtime app no longer carries the other VM.
- **HTTP as a composable feature (#114, Phases A-D).** The HTTP core caps and the
  per-runtime web bindings (routes, `res:*`, ws/sse/smtp, the in-process test
  harness) split behind a weak seam into `libhull_feature-http.a` +
  `libhull_feature-http-<rt>.a`, composed only when the manifest declares an HTTP
  module (the `needs_http` gate). A genuine `app.main` CLI / compute app links
  only the pure runtime. The `server-only` / `client-only` flavors were removed
  (the HTTP axis is `full` vs `pure-compute`).
- **Composed-feature signing (#114).** `hull build` records every composed
  archive into `package.sig.gethull.composed`, split into a platform-key-attested
  `platform_domain` and a release-key-attested `release_domain`; runtime
  `--verify-sig` (§5c) verifies the platform domain against the embedded platform
  key. See `docs/composed_feature_signing.md`.

## [0.7.0] - 2026-07-24

The composable-features release. Optional heavy subsystems (analytics,
extra SQL backends, GPU, terminal UI) move OUT of the base binary and
become **composable features** added at build time with `hull build
--with=<name>` and installed by end users with `hull feature install
<name>`. The base stays lean; you pay only for what you compose in.

### Added

- **Composable features (`hull build --with=<name>`).** A large optional
  subsystem now ships as its own signed static archive
  `libhull_feature-<name>-<arch>.a` and is composed into an app binary at
  build time, orthogonal to the subtractive `--flavor` axis. Selection is
  inferred from the app manifest (a `duckdb://` connection, a `hull/tui`
  module) or forced with `--with=`. Five features ship, all native-only:
  `duckdb`, `postgres`, `mysql`, `gpu`, `tui`. End-user tooling: `hull
  feature install <name>` / `feature list` / `feature uninstall <name>`,
  the same Ed25519-signed `hull.sha256` trust chain as `hull update` /
  `hull flavor install` (no new keys). A cache-sourced feature lib is
  re-verified against its signed manifest before linking (closes the
  install-to-build TOCTOU). See `docs/features_and_flavors.md`.
- **PostgreSQL backend** (feature `postgres`). Pure-C PostgreSQL wire
  client, no libpq: SCRAM-SHA-256 auth, `sslmode=disable..verify-full`
  TLS over the shared TLS client, typed decode by OID, `?`-to-`$n`
  placeholder rewrite with params always bound, dialect-portable
  migrations. Reached by a `postgres://` / `postgresql://` DSN on
  `hull/db`. Real-Postgres-16 E2E in CI (auth + TLS + migrations +
  `db.async` + stdlib).
- **MySQL / MariaDB backend** (feature `mysql`). Pure-C MySQL/MariaDB wire
  client, no libmysql/libmariadb: `mysql_native_password` +
  `caching_sha2_password` auth, TLS, text `COM_QUERY` for param-less and
  binary prepared statements for parameterized queries (no value ever
  touches SQL text), backtick dialect, `INSERT ... ON DUPLICATE KEY
  UPDATE` upsert. One backend serves both `mysql://` and `mariadb://`.
  Real-MySQL-8 E2E in CI.
- **DuckDB OLAP backend** (feature `duckdb`). Embedded columnar analytics
  engine, reached by a `duckdb://` DSN. Composed with `hull build
  --with=duckdb` (needs `--compiler=system`; a C++ engine can't link
  under the embedded TinyCC).
- **GPU compute** (feature `gpu`). The base ships the generic GPU dispatch
  layer; the `gpu` feature fills the concrete wgpu-native backend, reached
  by `gpu.*`. `hull build --with=gpu`.
- **Terminal UI** (feature `tui`). The `hull.tui` subsystem moved out of
  the native base (a TUI-free base links ~80-150 KB leaner); an app that
  declares `hull/tui` composes it back automatically (auto-inferred, so a
  plain `hull build` of a TUI app just works). The hull toolchain keeps
  its own `--tui` commands by force-loading the feature archive.
- **Optional module declarations (`"hull/gpu@1?"`).** A trailing `?` on a
  manifest module spec marks it optional: when the build lacks the
  capability and no matching feature is composed, the resolver skips it
  and the require/import returns a soft-absent value (`nil` / `null`)
  instead of failing app load, so an app can use a capability when present
  and fall back when not.
- **`crypto.constant_time_eq` / `crypto.constantTimeEq`** (Lua / JS): a
  constant-time byte-string equality primitive compared in C. `hull/jwt`
  (HS256) and `hull/web/middleware/csrf` now route their HMAC-signature
  compare through it instead of an in-interpreter loop, matching
  `hull/crypto/envelope` and removing interpreter-timing variance.
  Functionally identical (both were already non-early-exit); a
  defense-in-depth consistency change.

### Changed

- **TinyCC is no longer embedded in the binary; it is a side-loaded
  tool.** `hull tools install tcc` (or a `tcc` on `PATH`); resolved at
  build time via `hl_tools_lookup_path` (`~/.hull/tools` -> `dirname(hull)`
  -> `$PATH`). The binary is ~404 KB lighter; macOS now defaults
  `HL_ENABLE_TCC=0` (its ELF output was unusable there anyway). Same
  signed trust chain as every other side-loaded tool.
- **DSN-scheme backend routing.** `hl_db_backend_select` picks the backend
  per connection by DSN scheme; each backend declares the schemes it
  claims. A reserved-but-uncompiled scheme (`postgres://` / `mysql://` /
  `duckdb://` on a base binary) now fails with a specific `hull feature
  install <name>` hint instead of a generic error.
- **`HlDbDialect` descriptor.** Dialect facts (identifier quoting,
  placeholder style, upsert style, `RETURNING` support, autoincrement DDL)
  consolidated into one backend-owned struct in `.rodata`, exposed to
  app/stdlib code as `conn.dialect`. Groundwork for a future stdlib query
  builder; `quote_identifier` / `autoincrement_id_ddl` stay as
  descriptor-sourced back-compat aliases.

### Security

- **`network_outbound` sandbox grant for declared network databases.** The
  kernel-sandbox outbound-socket grant was gated ONLY on the `http.fetch`
  `hosts` allowlist, ignoring the databases config, so a sandboxed DB-only
  app (a `postgres://` / `mysql://` connection, no `http.fetch`) was
  pledge-SIGKILLed the moment it dialed its own database. Now also granted
  for a declared network DB connection (manifest `databases.named` /
  `databases.dynamic` net scheme or `$VAR` env-ref, or a `-d` net DSN).
  Outbound-only (no bind/inbound). Linux CI now exercises the grant under
  the real sandbox (the DB E2Es no longer use `--no-sandbox`).
- **Post-feature C / Lua / JS audit hardening.** Case-insensitive DSN
  scheme match in the sandbox (an uppercase-scheme named DSN was
  under-granted -> connect-time self-DoS); fail-closed re-verification of a
  cache-sourced feature lib (a missing verify binding must abort, not link
  unverified); plus the earlier A-H cross-surface audit batch.
- **CSV formula-injection sanitizer.** Opt-in on `csv.encode`: prefixes a
  cell beginning with `= + - @` (and tab/CR) so a spreadsheet does not
  execute exported data as a formula.
- **Build-time re-verify of installed per-flavor platform libs (closes the
  install-to-build TOCTOU).** `hull flavor install` now caches the signed
  manifest (`hull.sha256` + `.sig`) in `~/.hull/platform/`, and `hull build
  --flavor` re-verifies any lib it pulls from that cache before linking, fully
  offline, via `hl_release_io_verify_local_asset` (`tool.platform_verify`): the
  manifest signature is re-checked against the EMBEDDED release pubkey (the
  trust anchor, not the writable cache dir), then the lib's SHA-256 is matched
  to the signed manifest. A tampered cached lib or a swapped/absent manifest
  fails the build with a reinstall hint; a lib the developer built locally
  (`make platform-<flavor>`) is trusted as-is. Verified end-to-end against the
  v0.6.0 release (valid lib links, tampered lib rejected).
### Fixed

- JS `csrf` middleware session-cookie fallback defaulted to `"hull.sid"`
  instead of the session/auth middleware's `"hull_session"`, so the fallback
  could never find the cookie. Aligned to `"hull_session"`.
- GPU: destroy the GPU context on shutdown in composed `--with=gpu` builds
  (leak on graceful exit); resolve `HlJS` via `JS_GetContextOpaque` rather
  than a phantom global (JS GPU binding); zero-init a pipeline stage before
  the non-table skip in the Lua path (uninitialized read).
- `health.js` ReferenceError + Lua/JS stdlib parity hardening (audit
  follow-ups).
- DB E2E flakes: poll for real app-server / TCP+SCRAM readiness in the
  Postgres and MySQL suites instead of a fixed sleep.

### Docs

- The **feature / flavor / tool / stdlib** extension taxonomy and its
  classification decision-procedure (`CLAUDE.md`, `docs/roadmap.md`); a full
  **Redis / Valkey** connection-feature spec (`docs/roadmap_next.md`, the
  first non-SQL connection seam); composable-features + optional-module
  documentation; the ORM-vs-query-builder scope note (`hull/query` compiler
  in scope, generic ORM out).

## [0.6.0] - 2026-07-10

### Build flavors

- **Signed published cosmo per-flavor platform libs + cosmo `hull platform
  install`.** The release pipeline now builds each cosmo flavor lib
  (server-only / client-only / pure-compute) on its **own fresh runner** and
  publishes the dual-arch pair
  `libhull_platform-<flavor>.{x86_64,aarch64}-cosmo.a`, covered by the
  Ed25519-signed `hull.sha256`. `hull platform install <flavor>` on a cosmo
  hull fetches + verifies the pair into `~/.hull/platform/`, and `hull build
  --flavor` finds it there (previously cosmo flavor libs were build-from-source
  only). Isolated producer: a second cosmo platform build in one CI job
  corrupts the dynamic loader (system cc/sh can't mmap libc; not a resource
  limit), so each flavor builds alone. `platform.c` was already cosmo-ready;
  `build.lua`'s cosmo `--flavor` branch gained the `~/.hull/platform` lookup.

## [0.5.0] - 2026-07-10

### Build flavors

- **All four HTTP build flavors now link clean and are CI-covered.**
  Fixed the `HL_ENABLE_HTTP_CLIENT=0` (server-only) and `HL_ENABLE_HTTP=0`
  (pure-compute) link breaks and added both to the `flavors` matrix in
  `.github/workflows/ci.yml` (each builds, runs `hull version`, and runs
  an `app.main` exit-code smoke). Server-only: `release_io.c`'s offline
  helpers (used unconditionally by `hull verify-self`) are no longer gated
  out alongside the HTTPS fetch path. Pure-compute: with both HTTP halves
  off, mbedTLS is dropped entirely, so Hull's own hashing now runs on
  in-tree implementations (hand-rolled SHA-1, the cap layer's
  self-contained SHA-256, TweetNaCl SHA-512, and a portable HMAC backend)
  instead of `mbedtls_sha*` / `mbedtls_md_hmac`. Default and HTTP-on builds
  are byte-for-byte unchanged.
- Documented the pure-compute flavor in `CLAUDE.md` ("Pure-compute builds")
  with the mbedtls-free hashing path, the contributor invariant (core code
  must hash via the cap layer, never mbedTLS directly), and refreshed the
  flavor binary-size table to measured values.
- **`hull build --flavor=full|server-only|client-only|pure-compute`** builds
  a flavored app binary from a full hull, linking a locally-built
  `libhull_platform-<flavor>.a` (`make platform-<flavor>`) and validating the
  app's manifest against the TARGET flavor's caps (a module needing a dropped
  subsystem is rejected at build time). **`--flavor=auto`** infers the
  minimal flavor from the declared modules. Design: `docs/build_flavors.md`.
  Along the way: fixed the no-keel platform archive (missing `sh_seal_arena`),
  a `tool.modules_resolve` parser that ignored the canonical array module
  form, and `serve_cli.c` not detecting a built binary's embedded app.
- **`hull platform install <flavor>` + signed published per-flavor platform
  libs.** Releases now publish `libhull_platform-<flavor>-<arch>.a` for the
  three native arches, covered by the Ed25519-signed `hull.sha256`. `hull
  platform install` fetches + verifies them into `~/.hull/platform/`, and
  `hull build --flavor` uses the cached lib, so end users no longer have to
  build the platform lib from source. Same trust chain as `hull tools
  install`, factored into one shared `hl_release_io_fetch_verified_manifest`.
  Cosmo flavor libs stay build-from-source for now (`make
  platform-cosmo-<flavor>`).

## [0.4.0] - 2026-06-17

HTMX widget tier completed (8 widgets total: toast, confirm, form,
search, inline-edit, sort, pagination, table) and shipped with
browser-driven Playwright E2E coverage. Three production-affecting
bug fixes in the `hull build` + sandbox pipeline: any app declaring
`manifest.fs.write` was silently broken when shipped via standalone
binary, and `static/vendor/*` assets weren't getting embedded.
Confirm widget fixed twice (open only on `hx-confirm`; resume via
`htmx.ajax` to work around htmx 2.0.9's deferred-`issueRequest`
no-op). 31 commits since v0.3.0.

### Added - HTMX widget tier (§1.5.g)

- **`hull/web/htmx/toast@1`** (Lua + JS). Server-fired toasts via
  `HX-Trigger` header - `toast.success(res, msg)` /
  `toast.info(res, msg)` / `toast.warning` / `toast.error` /
  `toast.show(res, msg, opts)`. Auto-dismiss with configurable
  duration, deduplication by id (server-controllable), single-
  instance container appended lazily on first toast. ~150 LOC of
  structural CSS + client JS.
- **`hull/web/htmx/confirm@1`** (Lua + JS). `confirm.attrs(question,
  opts?)` produces `hx-confirm` + `data-confirm-*` attribute string
  for splicing into delete buttons. Replaces the browser's
  `window.confirm()` with a styled native `<dialog>`. ~140 LOC.
  Single-instance dialog (second prompt overwrites first), Escape
  + backdrop click both map to cancel.
- **`hull/web/htmx/form@1`** (Lua + JS). Validation-error rendering
  + auto loading state. `form.errors(validation_result) -> errs`,
  `form.field_error(errs, "name") -> "<small class=...>" or ""`,
  `form.field_attrs(errs, "name") -> "aria-invalid=true ..." or ""`.
  Client JS toggles `aria-busy="true"` + `disabled` on submit
  buttons during in-flight htmx requests; restores on
  `htmx:afterRequest`. ~75 LOC.
- **`hull/web/htmx/search@1`** (Lua + JS). `search.input_attrs(opts)`
  produces a debounced `<input>` attribute string
  (`hx-get` + `hx-trigger="input changed delay:Nms"` +
  `hx-target` + optional `hx-push-url`).
  `search.results_attrs(opts)` for the result container.
- **`hull/web/htmx/inline-edit@1`** (Lua + JS). Click-to-edit single
  fields: `inline_edit.cell(opts) -> read view`,
  `inline_edit.editor(opts) -> edit form`. Two routes + one PATCH
  pattern; ~30 LOC of server helpers.
- **`hull/web/htmx/sort@1`** (Lua + JS). Sortable column headers
  driven by `?sort=col[:asc|desc]`. `sort.parse(req, opts)` reads
  + allowlist-validates the query param;
  `sort.header_attrs(col, current, opts)` returns the htmx attrs
  for a `<th>`. Configurable `opts.swap` for the swap target.
  Client JS (sort.js) adds Enter / Space keyboard activation
  without relying on htmx trigger filters (which need
  `unsafe-eval` and conflict with the `csp = "htmx"` preset).
  Native `<th>` semantics + `tabindex="0"` + `aria-sort` - no
  `role="button"` override (would invalidate `aria-sort`).
  ~30 LOC.
- **`hull/web/htmx/pagination@1`** (Lua + JS). `pagination.nav(total,
  opts)` wraps existing `hull/web/pagination@1` page-math and
  emits htmx-attributed nav HTML. Replaces hand-written nav
  partials.
- **`hull/web/htmx/table@1`** (Lua + JS). `table.render(rows,
  schema, opts)` composes sort + inline-edit per column via a
  schema array. Custom `render(value, row)` callback for cells.
  ~155 LOC; the grid pattern of every admin / data UI in one
  helper.
- **CSP preset `csp = "htmx"`.** Manifest sugar - expands at
  startup to a known-good policy for SSR htmx apps with stdlib
  JS from `/static/`: `default-src 'none'; script-src 'self';
  style-src 'self' 'unsafe-inline'; img-src 'self' data:;
  connect-src 'self'; form-action 'self'; frame-ancestors 'none'`.
  Cheaper than the nonce-based `hull/web/middleware/csp@1` for
  apps that don't need per-request nonces.
- **Stdlib static + template VFS.** `stdlib/static/hull/<module>/*`
  files are embedded in the platform library and served at
  `/static/hull/<module>/*`. Widget CSS and client JS ship through
  this path so apps just add one `<link>` and one `<script>` per
  widget - no vendoring, no per-app bundling.
- **`htmx.trigger(res, name, payload, opts?)`** with `opts.timing`
  to pick `HX-Trigger` / `HX-Trigger-After-Settle` /
  `HX-Trigger-After-Swap`. Replaces the older
  `trigger_after_settle` / `trigger_after_swap` variants.
- **`docs/htmx_widgets.md`** - long-form usage guide for the eight
  widgets, the handler-pre-render pattern (templates can't call
  functions; widget helpers run once in the handler and the
  resulting strings flow into template data), setup checklist
  with htmx-config requirements.
- **`examples/htmx_widgets_register`** - exercises every widget in
  one CRUD page (asset register). Doubles as the UX-test surface
  and the playwright suite's primary target.

### Added - Browser E2E suite (Playwright + axe-core)

- **`tests/e2e_htmx_playwright.sh`** + **`tests/e2e_htmx_playwright.mjs`**.
  Headless Chromium driven by Playwright; tests
  `examples/htmx_widgets_register` (all 8 widgets) and
  `examples/hypermedia_photos` (both Lua AND JS runtimes -
  runtime-parity gaps surface as concrete FAIL lines). Catches
  things curl can't: CSS actually applies, htmx swaps fire,
  widget JS executes under the `csp = "htmx"` strict preset,
  sort widget Enter / Space keyboard activation works, CRUD
  round-trip with CSRF + session, confirm-yes-deletes-row.
- **Two modes.** `make e2e-htmx-playwright` (dev) launches
  `hull <app.lua>` so files come off disk;
  `make e2e-htmx-playwright-build` (build) runs `hull build`
  first and launches the standalone binary, exercising the
  embedded-VFS code path. 33 assertions in either mode
  (widgets 15 + photos[lua] 9 + photos[js] 9). Caught
  the `static/vendor/*` and `fs.write` resolution bugs.
- **`@axe-core/playwright` WCAG scan** on initial page load,
  per suite. FAILs on `critical` / `serious` violations; logs
  `moderate` / `minor` as informational. Caught three real
  bugs (sort widget `aria-allowed-attr`, two `color-contrast`
  cases, one `heading-order`) - all fixed in stdlib + the
  example app.
- **Failure artifacts.** Each suite writes a Playwright
  `trace.zip` (per-action DOM snapshots, network log, console,
  screenshots) and a `final.png` to `build/playwright-artifacts/`
  on failure only (no spurious uploads on success). CI uploads
  via `actions/upload-artifact@v4` gated on `if: failure()`.
  Local runs print the `npx playwright show-trace` invocation.
- **CI matrix.** New `htmx-browser` job runs both dev and build
  modes on Linux + macOS. Playwright + Chromium install cached
  via `actions/cache@v4`; skips cleanly when node / npm absent.

### Added - Build / runtime infrastructure

- **`hl_tool_find_files_ex(..., include_vendor)`** in the C tool
  cap. `hl_tool_find_files` kept as a back-compat wrapper. Lua
  binding accepts `opts.include_vendor = true` (default false)
  so static-asset walks no longer silently drop `static/vendor/*`.
- **`sandbox_resolve_manifest_path(app_dir, relpath, ...)`** -
  shared cross-platform helper that joins app_dir + relpath,
  pre-mkdirs, then `realpath()`s. Used by both seatbelt SBPL
  build (macOS) and the unveil setup (Linux / OpenBSD / Cosmo)
  so the sandbox allows the SAME absolute directory the
  capability layer (`hl_cap_blob_init`, `hl_cap_fs_validate`)
  resolves to.
- **`hl_serve_resolve_entry` canonicalizes `app_dir` to an
  absolute path** when the entry point has no slash (the standard
  case for `hull build` standalone binaries). Downstream
  `HlFsConfig.base_dir` is now always absolute, which
  `hl_blob_store_open` requires.

### Fixed

- **`hull build` silently dropped `static/vendor/*` assets.**
  `hl_tool_find_files`'s recursive walker hardcoded a skip on
  any directory named `vendor` - correct for source-file walks
  but wrong for `static/`, where apps put vendored CSS/JS
  (pico, htmx, etc.). Standalone binaries returned 404 for
  every `/static/vendor/*` request; pages rendered unstyled.
  `hull dev` was unaffected (reads from disk, bypasses the
  recursive walk). Caught by the new MODE=build playwright pass.
- **Standalone binaries with `manifest.fs.write` failed
  `blob.init`** (and any other capability that resolves paths
  via `HlFsConfig.base_dir`). The sandbox processed
  `policy->fs_write[i]` (e.g. `"data/"`) as cwd-relative for
  unveil / realpath / seatbelt, but `hl_cap_blob_init` resolved
  the same name against `app_dir`. When `cwd != app_dir` (always
  true in CI / production), the two interpretations disagreed
  and the app's writes were blocked at syscall time, with a
  confusing "blob.init: failed (check fs.write declares
  'data/blobs')" error that pointed at the wrong layer. Affects
  every app that declares `manifest.fs.write` - including
  `examples/hypermedia_photos` - when shipped via `hull build`.
  Fixed by resolving manifest paths against `app_dir` in the
  sandbox setup (both macOS seatbelt and Linux unveil paths).
- **Confirm widget popped its dialog on every htmx request.**
  `handleConfirm` did not check whether the triggering element
  actually had `hx-confirm`, so any htmx swap (search keystroke,
  form submit, sort header click) triggered the delete-confirm
  dialog. Early-return when `evt.detail.question` is empty
  (htmx sets it only when `hx-confirm` is present). Caught by
  the playwright suite while typing into the search input.
- **Confirm widget couldn't resume hx-delete requests.** In
  htmx 2.0.9, `evt.detail.issueRequest()` is a silent no-op
  for verbs like `hx-delete` on a standalone `<button>` - both
  sync inside the event AND deferred after the dialog closes.
  Verified by capturing the event externally and calling
  `issueRequest()` at various points: no path makes it fire.
  Workaround: `close()` now uses `htmx.ajax(verb, path, {source,
  target})` with the same config the original click would have
  produced, temporarily stripping `hx-confirm` from the source
  element so the resumed request doesn't re-trigger the dialog.
- **Sort widget broke keyboard accessibility under
  `csp = "htmx"`.** Used `hx-trigger="click, keyup[key=='Enter']
  from:this, keyup[key==' '] from:this"`. The `[expr]` filter
  syntax goes through `new Function()` eval, which the strict
  preset rejects (`allowEval:false`). htmx then dropped the
  entire trigger string - so Enter / Space AND the bare `click`
  both stopped working. Moved keyboard activation to a new
  `sort.js` client file that listens for keydown Enter / Space
  on `.hull-sort-header` and dispatches a real click. The
  `hx-trigger` collapses to just `click`.
- **Sort widget hard-coded `hx-swap="outerHTML"`.** When
  `opts.target` was a wrapping container (e.g. `#grid` holding
  table + nav + actions), outerHTML replaced the container
  itself with the response, destroying the target for any
  subsequent request that named `#grid`. Made `hx-swap`
  configurable via `opts.swap`; omitted by default so htmx
  uses its own default (`innerHTML`), which is correct for
  the common container-wrapping pattern.
- **Sort widget invalidated `aria-sort`.** Put `role="button"`
  on `<th>`, which overrode the implicit `columnheader` role.
  `aria-sort` is only valid on `columnheader` / `rowheader` /
  `gridcell`, so axe-core flagged it as `aria-allowed-attr`
  critical. Worse, screen readers said "Name, button" instead
  of "Name, column header, sorted ascending". Dropped
  `role="button"`; `tabindex="0"` + `sort.js` keyboard handler
  preserves activation.
- **`examples/hypermedia_photos/app.js` was never migrated to
  the §1.5.g widget tier.** Lua sibling had been; JS rendered
  `<input id="search-input">` with no `hx-*` attrs and delete
  buttons without `hx-confirm`. Mirrored the Lua wiring
  line-for-line: imports, manifest entries, pre-rendered
  attribute constants, plumbing through `rowData()` /
  `feedData()` / form re-render path.
- **A11y polish in `examples/htmx_widgets_register`** (all
  axe-surfaced): explicit text color on table headers so contrast
  stays WCAG-AA in both light and dark Pico; `.badge-retired`
  bumped from grey-500/grey-100 (~3.4:1, fail) to grey-700/grey-200
  (~7.5:1, AAA); `_form.html` `<h3>` → `<h2>` so heading order
  doesn't skip levels.

### Changed

- **`htmx.trigger(res, name, payload, opts?)`** with
  `opts.timing` is the canonical surface for firing client
  events. The earlier
  `trigger_after_settle` / `trigger_after_swap` /
  `triggerAfterSettle` / `triggerAfterSwap` variants are
  removed. Migration is mechanical:
  `htmx.trigger_after_settle(res, "foo", {x=1})` →
  `htmx.trigger(res, "foo", {x=1}, {timing="after-settle"})`.

## [0.3.0] - 2026-06-15

Production-grade authentication & authorization stack, streaming multipart upload, content-addressed blob storage with runtime caches, and asymmetric crypto. The auth stack alone went through **13 iterative security-review rounds** (rounds 1–13); the final round returned zero findings across three independent reviewers, marking convergence. Eight new stdlib modules ship in this release: `hull/web/auth-flows@1`, `hull/web/middleware/totp@1`, `hull/web/middleware/oauth@1`, `hull/web/middleware/audit-log@1`, `hull/web/auth-health@1`, `hull/web/pwned@1`, `hull/qrcode@1`, `hull/attachment@1`, `hull/blob@1`, `hull/mime@1`. (132 commits since v0.2.0.)

### Added - Authentication & authorization stack

- **`hull/web/auth-flows@1`** (Lua + JS). End-to-end auth flows on top of HMAC-signed envelopes: registration, email verification, login, password reset, magic link, email change (with revoke from old address), optional TOTP 2FA. Owns `_hull_auth_used_tokens` (single-use replay protection), `_hull_auth_pending_email_changes`, `_hull_auth_login_attempts` (per-user lockout). Pluggable user-storage callbacks; turnkey `standard_users` adapter for the common schema. Per-recipient email-storm rate limit, `public_origin` / `trusted_hosts` URL-origin gate (host-header injection class closed), strict user-sanitize allowlist with optional `user_sanitize(user)` hook. Settled API after 13 audit rounds.
- **`hull/web/middleware/totp@1`** (Lua + JS). RFC 6238 TOTP. Enroll → confirm → verify lifecycle with dual-row staging (pending vs confirmed) so re-enrollment doesn't lock out users who lost the new recovery codes mid-flow. Recovery codes single-use, PBKDF2-hashed, normalized (case + non-alphanumerics) so "ABCD-EFGH" and "abcdefgh" match. Constant-time digest compare. At-rest secrets stored under a versioned NaCl-secretbox scheme with multi-key rotation (`encryption_keys = {[1]=OLD, [2]=NEW}`); lazy-rekey on verify, batch `totp.rekey()` for active migration. Per-user brute-force lockout (`max_failed_attempts=5`, `lockout_duration=15min`); per-IP lockout opt-in via `trust_xff` flag. Auto-daily prune of orphaned pending rows via `app.daily`.
- **`hull/web/middleware/oauth@1`** (Lua + JS). OIDC Authorization Code + PKCE. Verifies ID-token signatures against the IdP's JWKS (RS256/384/512, PS256, ES256/384). Presets for Google and Microsoft Entra; Microsoft `tenant=common` now passes via `issuer_pattern` (regex match) since the actual ID-token issuer is `/{tenant-guid}/v2.0`, not `/common/`. HMAC-signed state cookie binds (provider, state, nonce, PKCE verifier, return_to). Owns `/auth/:provider/login`, `/auth/:provider/callback`, `/auth/logout`. `on_login(req, res, provider, claims, tokens) -> path?` and `on_logout(req, res) -> path?` callbacks.
- **`hull/web/middleware/audit-log@1`** (Lua + JS). Append-only sign-in / auth-event log with per-device fingerprint (HMAC-salted hash of UA + IP-prefix, deployment-private salt). Records `login_success`, `login_failure`, `password_reset_completed`, `email_changed`, `email_change_revoked`, custom kinds. `audit_log.list(user_id, opts)` and `list_devices(user_id)` for /devices UI. `is_new_device(user_id, req)` for new-device notifications. Auto-daily cleanup (`opts.retain_days = 365`); manual `audit_log.cleanup()`. Migration helper `recompute_fingerprints()` for salt rotation (paged + bounded by `max(id)` snapshot, in-process mutex with 1h staleness, breaks only on empty page). `cleanup_status() -> "scheduled" | "external" | "missing"` tri-state probe.
- **`hull/web/auth-health@1`** (Lua + JS). Runtime health probes for the auth stack: session table presence + count, audit-log cleanup status, pwned reachability (`not_yet_checked` / `reachable` / `fail_open` tri-state), TOTP enrollment count, RBAC tables. `auth_health.check({include_counts = false})` for the JSON output (counts gated since enumeration is recon material). `auth_health.routes(app, {auth_check})` mounts `/admin/auth-status` behind a required gate (admit on literal `true` only; thenable returns 500; throw returns 401; falsy returns 403). Backs the `hull agent auth-status` CLI.
- **`hull/web/pwned@1`** (Lua + JS). HIBP k-anonymity check (`api.pwnedpasswords.com/range/<first-5-hex-of-SHA1>`). 80KB embedded SecLists-10K blocklist for offline / air-gapped operation (binary-searched). Single-flight request cache, fail-open on network error with a one-shot warn. `pwned.health()` surfaces `{ok, status, last_check_at, last_error}` for the auth-health probe. Apps must add `api.pwnedpasswords.com` to `manifest.hosts`.
- **`hull/qrcode@1`** (Lua + JS). Pure-Lua / pure-JS QR Code generator (ISO/IEC 18004). SVG output with EC level + scale options. Used by `totp.enroll()` to render the authenticator-app QR. Color allowlist on `opts.dark` / `opts.light` so user-input wiring doesn't break out into arbitrary SVG.

### Added - Multipart upload, attachments, blob storage

- **Streaming multipart iterator (`req:multipart()` Lua / `req.multipart()` JS).** Routes opt in via `app.<verb>(..., { multipart = {...} })`. Iterator-shaped: each part yields `{name, filename, content_type}` plus `part:chunks(n)` / `part.chunks(n)` for byte-streaming reads and `part:read()` / `part.read()` for whole-part buffering. Binary-safe - Lua returns byte strings, JS returns `ArrayBuffer`. Caps (`max_part_size`, `max_total_size`, `max_parts`, `max_headers_size`, `max_input_buffer`) surface as parser errors the handler can `pcall` / try-catch to write structured 4xx responses. 94/94 e2e cases in `tests/e2e_multipart.sh` across both runtimes. See [docs/multipart.md](docs/multipart.md). Roadmap §1.5.b-2.
- **Incremental SHA-256 hasher.** `crypto.create_sha256()` / `crypto.createSha256()` returns an object with chainable `:update(bytes)` / `.update(bytes)` and a one-shot `:digest()` / `.digest()` (lowercase hex). Update-after-digest and double-digest both raise. Designed for streaming multipart digests but works for any incremental hashing use.
- **`hull/attachment@1`** (Lua + JS). Store, retrieve, delete file attachments backed by `hull/blob@1`. `attachment.store(part)` ingests a multipart `Part` via `blob.writer()` (content-addressed, dedupes identical bytes). `attachment.metadata(id)`, `attachment.read(id)`, `attachment.read_to_file(id, path)`, `attachment.delete(id)`. Companion `hull/web/attachment-serve@1` for serving with Content-Type + ETag + Range. Reference apps: `examples/hypermedia_photos` (photo gallery), `examples/hypermedia_todo` (photo uploads). Roadmap §1.5.b-4.
- **`hull/blob@1`** (Lua + JS). Content-addressed blob storage. `blob.writer()` returns a streaming writer (`:write(bytes)` / `.write(bytes)` + `:finalize()` / `.finalize()`) that returns the content-hash id. `blob.reader(id)` for streaming reads. Used as the low-level CAS layer for attachments, runtime bytecode caches, template caches, AOT compute caches, and `hull tools install`. Per-blob hard-link layout so duplicate writes are free. See `docs/blob.md`. Roadmap §1.5.b-3.5.
- **`hull/mime@1`** (Lua + JS). MIME type sniffer (magic-bytes + extension fallback). Used by attachment-serve. Roadmap §1.5.b-3.

### Added - Crypto cap + asymmetric verification

- **Asymmetric signature verification in the crypto cap.** RS256 / RS384 / RS512 / PS256 / ES256 / ES384 via mbedTLS. `cap/crypto_asym_mbedtls.c` backend behind a vtable; surfaces in stdlib as `crypto.verify_asym(alg, pem, message, sig)`. JWT module (`hull/jwt@1`) now dispatches on alg: HS256 (HMAC) AND RS/ES (asymmetric). Allowlist enforced BEFORE key resolution to defeat alg-confusion (e.g. token claims RS256 against an HS256-only allowlist → rejected before the key is ever hashed). Used by OAuth's OIDC ID-token verification.
- **`crypto.x509_pubkey_pem(cert_der_or_pem)` helper.** Extracts the SPKI public key in PEM from an x509 cert. Used to convert IdP JWKS `x5c` entries to PEM for the asymmetric verifier.
- **HMAC-SHA256 routed through a backend vtable** (`HlHmacBackend`). Same shape as the asymmetric and database vtables; mbedTLS is the default. Closes the round-1 audit finding that crypto primitives should follow Hull's vtable convention.
- **HMAC-SHA1 added** (HOTP/TOTP prerequisite).
- **`crypto.hex_encode` / `crypto.hex_decode` in the cap layer.** Lifted from per-module ad-hoc implementations; now a single binary-safe helper. (Stdlib modules that need byte-safe hex for non-ASCII bytes still use a local `bytesToHex` / `bytes_to_hex_local` because `crypto.hex_encode` for `string` input goes through QuickJS's UTF-8-inflating C boundary; documented in the relevant modules.)
- **`crypto.sha256` SHA-NI runtime dispatch** on Linux/Cosmo for arm64 + x86_64 (`cap/crypto_sha256_*`). Falls back to mbedTLS when the CPU doesn't support the extensions.

### Added - Runtime caches + CAS infrastructure

- **`hull/blob@1` low-level CAS** (`hl_blob_store_*`) powering everything content-addressed in Hull: attachment storage, Lua + JS bytecode caches, Lua + JS template caches, compute AOT cache (`wamrc` output memoization), `hull tools install`. Per-blob hard-link layout so duplicate writes are free; LRU/FIFO eviction via `hull cache prune`.
- **`hull cache` CLI surface.** `hull cache list` (per-kind entry count + size), `hull cache prune --max-size=N --max-age=N --kind=K [--dry-run]`, `hull cache clear --yes [--kind=K]`, `hull cache verify`. Plus `--json` output. Cache registry is a single source of truth shared between `hull doctor` and `hull inspect`.
- **`HULL_CACHE_DIR` env var** for per-app cache isolation in multi-tenant deployments. The `tools/` store stays in `$HOME/.hull/` regardless (signed durable downloads).
- **Persistent Lua + JS bytecode caches** at `~/.hull/blobs/runtime/{lua-bytecode,js-bytecode}/`. Reduce cold-start time on `hull dev` / re-init.
- **Persistent Lua + JS template caches** at `~/.hull/blobs/runtime/{lua-template,js-template}/`. Templates compile once, reuse across processes.
- **Compute AOT cache** keyed on the WASM bytes + `wamrc` invocation hash.
- **`hull tools install`** now routes through `blob_store` with a symlink-as-tool layout so a single binary backs multiple symlink names.
- **`stdlib/context/`** task-discoverable docs: `multipart.md`, `blob.md`, plus existing entries surface via `hull agent context --task=<name>`.

### Added - Streaming-handler routes (Keel v2.1+ pre-body dispatch)

- **`kl_server_route_streaming_async()` integration** lets Lua/JS handlers run BEFORE the body is fully buffered, enabling the multipart iterator and mid-stream early-exit on parser caps. Hull's body cap (`HL_ENABLE_HTTP_SERVER`'s 16 MiB default) is no longer the only knob - per-route multipart caps surface as structured 4xx without consuming server memory.

### Added - Build / agent / CI

- **`hull agent auth-status`** subcommand backs the auth-health probe over the CLI. Used by ops dashboards.
- **`hull agent attachment`**, **`hull agent blob`**, **`hull agent mime`** subcommands surface the new modules.
- **`hull modules available`** four-section grouping (Intrinsic / Core / Web / Web middleware) updated with the eight new modules.

### Changed (BREAKING)

- **`hull/web/auth-flows@1` API converged after 13 audit rounds.** Apps written against pre-v0.3.0 snapshots of any of `auth-flows` / `audit-log` / `oauth` / `totp` / `session` should re-read the module docstrings; option names and shapes were unified across runtimes during the audit cadence. Notable changes:
  - `auth_flows.init` now REQUIRES one of `public_origin` (single canonical URL), `trusted_hosts` (allowlist array of bare hostnames), or `trust_request_host = true` (explicit dev/test escape, init-time warn). Pre-v0.3.0 silently honored `req.headers.host` - host-header injection class. See round-9 HIGH-1.
  - `session.init({absolute_ttl = 86400})` is the new default (24 h hard cap on top of sliding TTL). Pass `false` to opt out; non-positive numeric values are coerced to nil with a warn.
  - `audit_log.init({fingerprint_salt = ...})` is REQUIRED (was silently `""` if missing). Run `audit_log.recompute_fingerprints()` ONCE during deploy if you had pre-v0.3.0 rows. Round-6 hardening.
  - `totp.verify(user_id, code, req)` now accepts an optional `req` for the per-IP gate. Apps wiring `totp_verify` callback in `auth-flows.init` should pass `req` through.
  - `strip_user_secrets` flipped from 1-field denylist (`password_hash`) to strict allowlist (`{id, user_id, email, email_verified}`) with optional `user_sanitize(user) -> safe_user` callback. Apps reading custom user fields in templates / `on_login` must wire the callback. Round-9 MEDIUM-6.
  - `is_email_ish` rejects bytes < 0x20 or == 0x7f.
- **Vendored Keel bumped through v2.0.0 → v2.1.0 → v2.1.1 → v2.1.2 → v2.2.0.** Streaming handler routes, mid-stream early-exit on parser errors, streaming-async pre-body dispatch, state-honoring fix on leftover-rc<0 / KL_PARSE_ERROR branches, synchronous-completion keep-alive force-off. Closes the previously-documented single-read `max_total_size` known limitation end-to-end.
- **DB SQL dialect helpers moved into the `HlDbBackend` vtable.** `db.table_columns()` and other dialect-aware queries dispatch via the backend rather than per-call SQL strings. SQLite is the only shipped backend today; PostgreSQL backend lands behind the same vtable.

### Fixed

- **Auth-stack: 13 rounds of iterative fixes.** Full per-round breakdown lives in the `git log --grep='^auth: round-'` history. Highlights:
  - Round 9 HIGH-1 - host-header injection in 5 click-through URL builders. Phishing-grade takeover via attacker-controlled `Host`. Pre-existing since auth-flows shipped.
  - Round 9 HIGH-2 - lockout counter never reset after window expiry; user got 1 attempt per 15min for 24h, not 5.
  - Round 9 HIGH-3 - `session.absolute_ttl = 0` killed every session on Lua (`0` is truthy in Lua, falsy in JS).
  - Round 10 HIGH-3 - Microsoft `tenant=common` preset rejected every login; strict `iss` equality failed against per-tenant issuer.
  - Round 10 HIGH-4 - `extract_ip` unconditionally honored X-Forwarded-For; trivially spoofable. `trust_xff` opt-in (default false).
  - Round 10 HIGH-5 - round-9 timing-collapse dummy PBKDF2 turned lockout into a CPU amplifier. Reverted.
  - Round 11 HIGH-1 - async `userSanitize` returning a Promise bypassed the round-10 allowlist (Promise is truthy object). Explicit thenable reject.
  - Plus ~50 MEDIUM / LOW items across rounds 5-12 covering parity gaps, operator-visibility, edge cases.
- **`hull manifest` and `hull inspect` on JS apps.** Previously the JS-runtime manifest-extractor was only reachable via the JS-bindings TU, so `hull inspect` (a build-tool command that doesn't link the JS runtime) failed on `app.js`. Extractor now lives in a runtime-neutral TU (`src/hull/manifest_extract_file.c`) reachable from both runtimes and from the build-tool commands.
- **`cap/crypto` swapped K constants in SHA-NI rounds 16-19.** SHA-256 produced wrong digests on Linux/x86_64 with SHA-NI enabled. Caught by the SBOM `binary_sha256` cross-check.
- **Multipart `multipart_config` zero-init on every route alloc** (Lua + JS). Stale config from a previous route could leak in if the struct wasn't zeroed. Caught by static analysis.
- **`blob_store` O_NOATIME on Linux for `track_access=0` reads.** Cache reads no longer perturb atime → no spurious LRU touches → eviction policy behaves as specified.
- **`hull/web/middleware/outbox` `cleanup({alsoFailed=true})` actually deletes failed rows** (JS only). Pre-fix `WHERE delivered_at <= ?` never matched failed rows (delivered_at IS NULL); now branched into `(state='delivered' AND delivered_at <= ?) OR (state='failed' AND created_at <= ?)`.
- **`hull/web/middleware/health` `/ready` actually emits the `stats` block** (Lua). Pre-fix `if server and http_server.stats then` - `server` was an undefined free global, branch was dead since the code was written.
- **`hull/qrcode` rejects malformed `opts.dark` / `opts.light`** so user-input wiring (e.g. `opts.dark = req.query.color`) can't break out into arbitrary SVG. Validates against a small color literal allowlist.
- **`hull/web/cookie` rejects `;` / CRLF / NUL in `opts.path`** to defeat Set-Cookie directive injection.
- **`hull/web/middleware/csrf` per-form-pair cap** (JS, parity with Lua). 256 pairs max.
- **`auth-flows.js`: avoid for-of destructuring** (`for (const [k, b] of map)`). Triggers a MSan use-of-uninit in QuickJS's `js_parse_destructuring_element`; surfaced repeatedly on CI MSan job. Workaround: `.forEach((b, k) => ...)`. Same pattern dropped in totp.js, qrcode.js.
- **MSan + UBSan CI job timeout** bumped from 10 → 15 min to fit the expanded auth-stack test surface.
- **14 scan-build / `-Wformat-truncation` findings** closed in one analysis pass.
- **2 cppcheck findings** closed; `HL_QJS_VERSION` propagated to cppcheck so QuickJS-version-dependent code stops being flagged.
- **JSON serialization moved to `sh_json` writer** in `cache`, `sbom`, `doctor`, `tools`, `version`, `serve` for streaming-safe output (no buffer-the-world).
- **Various e2e fixture fixes** for Linux Landlock (`data/` pre-creation), dash bashisms, and CI flakes exposed by first-time runs of new test suites.

### Security

- **13 iterative rounds of auth-stack security review** ran against `auth-flows` + `session` + `audit-log` + `totp` + `oauth` + `pwned` + `auth-health` between rounds 5 (initial) and 13 (convergence). Round 13 returned zero findings across three independent reviewers; the surface has converged. See `git log --grep='^auth: round-'` for the full per-round history.
- **Single-pass `/c-audit` + `/js-audit` + `/lua-audit` ran on the non-auth surface** after round 13. Two MEDIUM findings (outbox cleanup no-op; health.lua dead branch) shipped along with five LOW hardening items.
- **`/auth-audit` skill** (local, in `.claude/skills/`) encodes the iterative audit pattern: parallel-agent fan-out over three slices, git-log round-awareness, explicit "drop weak findings; zero is OK" instruction, report-only output. Not committed (`.claude/` is gitignored, matching `/c-audit` etc.); see the existing skill files for the convention.

## [0.2.0] - 2026-05-31

Hypermedia profile + web stdlib namespace reorganization. `hull init --profile htmx` now produces a complete HTMX + Pico app with CSP nonce, CSRF, session, flash, pagination, search, inline edit, loading indicator, form re-population, and idempotency wiring out of the box. Strictly-web stdlib modules moved under `hull/web/*` to parallel the existing `hull/middleware/*` precedent (full release notes: the GitHub release page or the GitHub release).

### Added

- **HTMX hypermedia profile.** `hull init --profile htmx` scaffolds a complete working app: HTMX 2.0.9 + Pico v2.1.1 classless (vendored, SHA-pinned), per-request CSP nonce, CSRF, session, flash messages, pagination, search-with-debounce, inline edit, loading indicator, form re-population, idempotency. 8/8 scaffold tests pass.
- **`hull/web/flash@1`** (Lua + JS). One-shot user notifications. Session-backed `flash.set` / `flash.consume` for POST/redirect/GET; `flash.trigger` for HTMX `HX-Trigger: {flash:...}` events. Hard dep on `hull/web/middleware/session@1`.
- **`hull/web/pagination@1`** (Lua + JS). Offset-based pagination. `pagination.from_query(req)` parses `?page=N&per_page=M`; `pagination.render(total, opts)` produces a windowed-links nav structure with ellipses for big page counts. Snake_case keys in both runtimes so the same template works for both.
- **`idempotency.respond_html(req, res, status, html, headers?)`** parallels existing `respond()` for HTML fragments. Replay path now reads content-type from cached headers blob instead of hardcoding `application/json` - HTMX double-clicks against the cached-response path now actually work for HTML.
- **`docs/htmx.md`** (~700 lines). Long-form pattern guide: architecture, CSP, CSRF, fragment-vs-page rendering, flash, delete confirmation, search + debounce, inline edit, loading indicator, form re-population, idempotency, empty states, testing.
- **`stdlib/context/htmx.md`** - agent-discoverable task-context doc; `hull agent context --task=htmx` returns it.
- **Resolver fix-it hint for renamed modules.** Apps declaring old names get an explicit `module 'hull/X@1' was renamed to 'hull/web/X@1' in v0.2.0` message before the fuzzy-suggest path. `hull_module_registry_suggest` also strips `web/` from candidates so typos of old short names still resolve to the new canonical.
- **`hull modules available`** now groups output into four sections (Intrinsic / Core / Web / Web middleware). `--json` output unchanged.
- **README + completions** surface `hull sbom`, `hull verify-self`, `hull verify-release`, `hull sign-release`, `hull modules` (with subcommands) - these existed but were missing from the README table and the bash/zsh/fish completion files.

### Changed (BREAKING)

- **20 strictly-web stdlib modules moved under `hull/web/*`.** Full move table in the GitHub release page. Modules that stay flat: `hull/jwt`, `hull/http-server`, `hull/http-client`, `hull/template`, `hull/email`, `hull/smtp`, plus all general utilities. Migration: see the perl one-liner in the release notes - idempotent, run from repo root. Old names emit a fix-it hint at app load.

### Fixed

- **Pre-existing template scoping bug in `examples/hypermedia_todo`** caught by the new pagination tests: `partials/todo_row.html` used `{{ id }}` as a bare name, which resolved to outer-scope nil when included inside `{% for t in todos %}{% include %}{% end %}`. The fragment-render path worked because POST handlers passed `{id, title, done}` directly. Now uses `{{ t.X }}` consistently; both contexts work.
- **Stale module-name references** in `src/hull/commands/doctor.c` and `stdlib/context/quickstart-cli.md` - `hull/http`, `hull/ws`, `hull/server` (deprecated names that pre-dated v0.2.0) cleaned up alongside the rename.
- **HTMX dev-mode session cookies** across `examples/{auth, todo, hypermedia_todo}`: `secure = true` was browser-dropping the cookie on plain-HTTP `hull dev`, breaking the CSRF binding. Now explicit `secure = false` with a doc comment about flipping for HTTPS.
- **CSRF placeholder warning** at app startup if scaffold-generated apps still have the literal `"CHANGE-ME-IN-PRODUCTION"` secret.
- **JS sessionBootstrap dead-code removal** - `req.headers["cookie"] = ...` patch was load-bearing before v0.1.8's `csrf.sessionKey` and became dead afterwards.

## [0.1.6] - 2026-05-29

Trust chain end-to-end verifiable, three independent ways. Closes the cheap and medium-cheap items from `roadmap_next.md` §0.3 (trust-chain hardening). After this release, anyone can prove a hull binary came from a specific commit, was built by a specific workflow, hasn't been tampered with, and that its SBOM was signed by the same release process. No single-party trust required.

### Added

- **`hull verify-self` subcommand.** One-shot replacement for the three-step `sha256sum` + `grep` + `hull verify-release` manual flow. Resolves running binary path (`/proc/self/exe` / `_NSGetExecutablePath` / argv[0] fallback), streams SHA-256, fetches manifest from cwd or alongside argv[0], verifies Ed25519 signature against the embedded release pubkey, extracts the expected hash for this platform's asset, constant-time compares. Flags for explicit / offline / custom-release use: `--manifest`, `--signature`, `--asset`, `--pubkey`. Roadmap §0.3.8.
- **Sigstore + Rekor transparency log.** Every release publishes `(release_tag, hull.sha256_digest, signature)` to `rekor.sigstore.dev` via `cosign sign-blob`. Two new release assets: `hull.sha256.cosign.sig` (96 bytes, Sigstore signature) and `hull.sha256.cosign.pem` (3.3 KB, Fulcio short-lived cert). Verifies independently of `HULL_RELEASE_KEY`: anyone runs `cosign verify-blob hull.sha256 --certificate hull.sha256.cosign.pem --signature hull.sha256.cosign.sig --certificate-identity-regexp='^https://github.com/artalis-io/hull/' --certificate-oidc-issuer='https://token.actions.githubusercontent.com'`. Roadmap §0.3.4.
- **SLSA build provenance.** Every shipped binary now has a Sigstore-signed attestation tying it to the specific commit, workflow run, and runner image. Generated by `actions/attest-build-provenance@v2` (SHA-pinned), covers all 7 binaries + 4 manifest/SBOM files. Verifiable with `gh attestation verify hull-linux-x86_64 --repo artalis-io/hull`. Independent of `HULL_RELEASE_KEY`. Roadmap §0.3.5.
- **Signed SBOM as release artifact.** Three new release files: `hull.sbom.json`, `hull.sbom.cdx.json` (CycloneDX 1.5), `hull.sbom.spdx.json` (SPDX 2.3). Generated by the linux-x86_64 binary before `hull.sha256` is computed, so all three are covered by the existing Ed25519 manifest signature AND the new Rekor signature AND a SLSA attestation. The SBOM is now triply attested. Roadmap §0.3.9.
- **`hull sbom` output includes `binary_sha256`.** New top-level field in JSON, `metadata.component.hashes[0]` in CycloneDX, `documentDescribes` + `SPDXRef-Package-hull-binary.checksums` in SPDX. The running binary's SHA-256 is computed lazily on first format call (cached, invalidated when `hl_sbom_set_binary_path` is called with a different path). Lets consumers cross-reference SBOM output against the signed release manifest in one comparison. Roadmap §0.3.11.
- **`docs/fork_playbook.md` (new, 259 lines).** Authoritative guide for forking Hull to ship with your own trust root: when to fork, what it gets you and doesn't (including trademark caveat), a substantial "why most organisations shouldn't fork" section (AGPL §13 propagation, security posture inheritance, super-linear divergence cost, trust non-transitivity, three lighter-weight alternatives), the five-step procedure, verification checklist, maintenance strategies, honest threat-model notes. Roadmap §0.3.15.

### Changed

- **All GitHub Actions invocations pinned to commit SHA.** Workflows previously used `actions/checkout@v4`-style mutable tags. Now pinned with format `actions/checkout@34e114876b0b11c390a56381ad16ebd13914f8d5  # v4`. Closes the "compromised action publisher slips code under existing tag" attack path. Four distinct actions pinned across `ci.yml`, `release.yml`, `deploy-site.yml`, `dco.yml`. Dependabot supports SHA-pinned actions natively and will PR updates. Roadmap §0.3.6.
- **Release workflow grants `id-token: write` and `attestations: write`** to the Create Release job (required for Sigstore OIDC and SLSA attestations).
- **`hull.sha256` manifest now covers SBOM files** in addition to binaries; the existing Ed25519 manifest signature transitively signs them.
- **`docs/roadmap_next.md` §0.3** updated to mark 7 of 15 trust-chain hardening items shipped. Remaining 8 are either organisational (Tier 1: pin CI environment, signing-key custody, key rotation) or polish (Tier 3: CPE strings, vendored archive repro check, build-env manifest).

### Fixed

- **`src/hull/sbom.c` magic-constant cleanup** during the §0.3.11 work: `HL_SHA256_DIGEST_BYTES` / `HL_SHA256_HEX_LEN` / `HL_SHA256_HEX_BUF` / `HL_SBOM_BINARY_MAX_BYTES` named instead of raw 32 / 64 / 65 / 256\*1024\*1024 sprinkled through the file.
- **`test_sbom`'s `binary_sha256_present_when_path_set` test** uses `__has_feature(memory_sanitizer)` to skip cleanly under MSan, where MSan's stdio interception silently short-circuits the `fopen` + `fread` + `mbedtls_sha256` chain. The feature is verified on every other CI variant (Linux, macOS, ASan, UBSan, cosmocc, GPU, reproducibility) plus a local smoke check that compares `hull sbom`'s `binary_sha256` to `shasum -a 256 ./build/hull` byte-for-byte. Not a Hull bug; a test-method incompatibility with MSan's libc-stdio wrappers.

## [0.1.5] - 2026-05-29

Live verifiable SBOM, macOS reproducibility CI gate, canonical positioning surfaced across every Hull-mentioning surface, site self-hosted with no third-party CDN dependencies, em-dash sweep across all prose.

### Added

- **`hull sbom` and `hull agent sbom`.** Every Hull binary self-describes its bill of materials via four output formats: human (default table), JSON (flat array; same shape as `hull agent sbom`), CycloneDX 1.5 (NTIA-aligned), SPDX 2.3. Per-build auto-refresh via Makefile `-DHULL_VENDOR_*_COMMIT=$(git -C vendor/<name> rev-parse HEAD)` injects vendored submodule SHAs into the binary at compile time. Build-flag gated: `make HL_ENABLE_DB=0` correctly omits SQLite. Runtime SHA-256 over the embedded Mozilla CA bundle (via mbedTLS, cached) exposed as `embedded_blob_sha256`. Orthogonal to the rest of the runtime: src/hull/sbom.c depends only on `cacert.h` + mbedTLS; test_sbom links against the minimal surface and would fail loudly if the SBOM module accidentally pulled in anything else. 16 test cases including format-determinism (two calls produce byte-identical output) + defensive error paths. Documented in `docs/sbom.md`.
- **Byte-reproducible builds, three layers, CI-gated.** Same source tree → byte-identical `build/hull` between rebuilds. Same source + same hull version → byte-identical app binary from `hull build`. Plus `make self-build` proves hull is self-hostable across all platforms. New CI job `reproducibility` enforces all three on every commit. Mechanism: deterministic ar archives via `ZERO_AR_DATE=1`; `-ffile-prefix-map=<srcdir>=.` in `sys_compile` to strip per-build tempdir paths from .o file content; same-path methodology for the `hull build` test (macOS `ld64` hashes output path into LC_UUID, so end-user same-target-name behavior is what's tested). Documented in `docs/MANIFESTO.md` "Reproducible builds" and `docs/POSITIONING.md` precision notes; full investigation arc preserved in `docs/roadmap_next.md §0.2` as lessons-learned.
- **`docs/POSITIONING.md` (new, 153 lines).** Standalone messaging-style guide encoding the canonical thesis, descriptor, target audiences, tone, vocabulary table, geographic positioning, typography rules, surface-update cadence, and authoring checklist. Lives separately from `MANIFESTO.md` (which is narrative WHY); POSITIONING is operational reference for anyone writing about Hull (humans or AI agents).
- **New canonical thesis surfaced across every Hull-mentioning surface:** *"Code became disposable. Trust is not."* Hero on gethull.dev, README opener, MANIFESTO callout, INVESTORS callout, GitHub repo About. Canonical descriptor unified to *"Hardened, capability-secure runtime infrastructure for AI-native systems."*
- **Self-hosted site assets.** gethull.dev no longer fetches from third-party CDNs. Tailwind, Lucide, Inter + JetBrains Mono fonts all vendored under `site/vendor/` and `site/fonts/`. Verify.html CSP tightened to `default-src 'none'` with `'self'` for everything except `connect-src https:` (for the dev-key fetch path).
- **OG card PNG variant.** `site/og-card.png` (1200×630, rendered from the SVG via `rsvg-convert`) added so LinkedIn and Facebook social previews render correctly (those platforms don't render SVG OG images).
- **OG metadata on `verify.html`.** Previously had only `<title>` + `<meta name="description">`; social shares of the verifier URL showed blank/scraped previews. Now has full `og:*` + `twitter:*` block with the canonical thesis.
- **`Engineered in Europe for sovereign, deploy-anywhere AI infrastructure.`** Imprint-style line added to site footer and README license area. Restrained provenance, no flag.
- **LICENSING.md vendored-dependency table.** Every static-linked third-party library listed with license (mostly MIT/BSD/Apache/ISC/PD). Explicit TinyCC note: LGPL-2.1+ is the only non-permissive dependency; compliance with §6 is satisfied by Hull shipping full vendored source under AGPL.

### Changed

- **README, MANIFESTO, AGENTS, INVESTORS positioning aligned** to the new canonical thesis. README's lead tagline + Why section rewritten. MANIFESTO title vocabulary updated (`Agent-Native, Local-First Applications` → `Hardened, Capability-Secure Runtime for AI-Native Systems`). AGENTS.md opening line uses canonical descriptor. INVESTORS adds thesis callout at top and refreshes Status section (was stuck at "approaching v0.1.0" - now correctly states v0.1.4 shipped 2026-05-28 with detailed shipped-features list). All `agent-native` → `AI-native`, `secure runtime` → `hardened, capability-secure runtime`. "Zero system dependencies" wording corrected: embedded TinyCC removes the system-compiler requirement, but the system linker is still used.
- **`docs/release_signing.md` status line** updated from `Design draft - pre-v0.1.0` to `Shipped (as of v0.1.3). The embedded gethull release key is live; every release manifest carries an Ed25519 signature verified by hull verify-release and by hull update before atomic install.`
- **Em-dash sweep across all prose files.** 1100+ em-dashes replaced with period + capitalize for between-clause breaks, parentheses for parentheticals, colons for elaborations. Same pattern as commit 6b6f394 precedent. Applies to README, AGENTS, CLAUDE, MANIFESTO, INVESTORS, PERSONAS, POSITIONING, all `docs/*.md`, and site HTML. Two UI-placeholder em-dashes in `site/verify.html` status badges became middle dots (`·`).
- **Deploy-site workflow CF-invalidation bug fixed.** Conditional was `if: ${{ env.CF_DISTRIBUTION_ID != '' }}` but `env:` was set in the same step (applied after `if:`), so the invalidation step never ran. Promoted to job-level env; now fires correctly when the secret is configured.
- **Documentation archive cleanup.** Five superseded docs moved to `docs/archive/`: three Phase-6 audits, `ASSESSMENT.md` ("approaching v0.1.0" snapshot), `api_review.md` (pre-v0.1.0 public-surface review). Archive index (`docs/archive/README.md`) updated with new entries and reorganized sections.

### Fixed

- **macOS sandbox claim precision.** Hero/site tech-stack chip clarified to `pledge + unveil (Linux/OpenBSD/cosmo) · Seatbelt (macOS)` so the platform split is visible without restructuring copy. Aligns the marketing claim with the truth disclosed deeper in the "Hull is not" section.

## [0.1.4] - 2026-05-28

§3.1, §3.2, §3.3 from `docs/roadmap_next.md`. Closes the v0.1.4 batch from the platform-sign-chain follow-ups roadmap.

### Added

- **§3.1 - Cosmo APE `hull build` works on Linux.**
  - Tool-mode sandbox unveils widened with `/opt` + `$HOME/.cosmocc` + `$HOME/cosmocc` so the cosmocc toolchain's install locations aren't sealed off (`src/hull/sandbox.c`).
  - `hl_compiler_select()` auto-detects cosmocc when running as a cosmo APE: checks `$HOME/.cosmocc/bin/cosmocc`, `$HOME/cosmocc/bin/cosmocc`, `/opt/cosmo/bin/cosmocc`, then `$PATH`. No need to pass `--compiler cosmocc` explicitly (`src/hull/compiler.c`).
  - Makefile `WAMR_INVOKE_SRC` selection reordered: target-compiler checks (cosmocc / `*-unknown-cosmo-cc`) precede `UNAME_S=Darwin` so cross-building a cosmo APE from a macOS host picks the correct asm invoker (was previously failing with "missing elf symbol table").
  - Known limitation: cosmo platform-sig E2E smoke test in `release.yml` stays SKIPPED. The jart pledge polyfill on Linux rejects child `mmap(PROT_EXEC)` independently of unveil policy, so spawning `/bin/sh` or `/usr/bin/cc` from inside the polyfill always fails on Linux runners. Native build jobs (darwin-arm64, linux-x86_64, linux-aarch64) gate the gethull signature chain end-to-end.

- **§3.2 - `hull eject` and `hull sign-platform` work on installed binaries.**
  - Both commands auto-extract the embedded `libhull_platform.a` to a tempdir when no `build/` tree is present. End-users who only have the binary can now eject or sign-platform without first cloning the repo (`stdlib/cli/lua/hull/{eject,sign_platform,build}.lua`).
  - New Lua bindings: `tool.extract_platform()` (single-arch) and `tool.extract_platform_cosmo()` (multi-arch).

- **§3.3 - Platform-sign chain polish (four follow-ups).**
  - Variant A (`--no-sandbox`) + Variant B (with sandbox active) for the native platform-sig E2E smoke test in `release.yml`. Variant A is the primary gate; Variant B re-runs the same `myapp` with the full pledge/unveil sandbox active and confirms `--verify-sig` still works under it.
  - Key-rotation story added as a new section in `docs/security.md` §2. Covers scheduled rotation, post-compromise rotation, the non-cross-validity property (intentional), and the impersonation gate.
  - `hull verify --gethull-key <file>` parity in Lua (`stdlib/cli/lua/hull/verify.lua`). CROSS-CHECK semantics: the file's pubkey must match this hull's embedded `HL_PLATFORM_PUBKEY_HEX` AND the signature must verify against it. Documented asymmetry with `verify.js` (JS uses override semantics because it can't reach the embedded key from QuickJS).
  - `tests/release_smoke.sh` extended with platform-sig E2E section. Post-publish on the actually-uploaded artifact, runs sign-platform + build --sign + --verify-sig. Catches CDN-tampering / upload-integrity surprises the in-CI smoke can't see.

### Fixed

- **Cosmo platform-sig E2E re-disabled in `release.yml`.** Round-1 audit attempt re-enabled it under the §3.1 sandbox/compiler fixes; CI demonstrated the deeper polyfill mmap restriction (`/bin/sh: libc.so.6: failed to map segment from shared object`) is independent of unveil policy. Re-skipped with honest comment naming the actual mmap-vs-unveil distinction. Native build jobs continue to gate the gethull signature chain.

## [0.1.3] - 2026-05-27

> The first published v0.1.3 build was found to ship with a broken
> gethull layer (the `libhull_platform.a` embedded into the hull
> binary had different bytes than the .a `sign-platform-manifest`
> hashed, so `hull build --sign` against that binary failed the
> cross-check). The original tag + GitHub release were deleted and
> v0.1.3 was retagged with the release-process fix described under
> "Fixed - release process" below. End-users who installed the
> first v0.1.3 should `hull update` to pick up the corrected build.

### Added

- **Platform-sig chain end-to-end (v0.1.3).** The
  `HL_PLATFORM_PUBKEY_HEX` placeholder is replaced with the real
  gethull.dev platform Ed25519 public key
  (`2a5461235aa51bbbe1e9cbc462e6a63f37d099f5ad17646a8f3a67db2f3a4fad`),
  and the full release → build → verify chain is wired:
  - **Release CI:** `release.yml` reorg splits platform building
    into a matrix, then a single Linux job
    (`sign-platform-manifest`) downloads the per-arch `.a`
    artifacts, computes SHA-256s, signs the canonical text
    manifest (`<64-hex>  <arch>\n`, LC_ALL=C sorted by arch) with
    `HULL_PLATFORM_KEY`, and emits
    `build/embedded_platform_sig.h` for the downstream
    `build-native` / `build-cosmo` jobs to embed.
  - **`hull build`:** computes SHA-256 of the
    `libhull_platform.a` being embedded, cross-checks against the
    inherited signed manifest, hard-rejects on mismatch unless
    `--no-verify-platform` is set. Writes the inherited
    `{manifest, signature, arch_hashes}` into
    `package.sig.platform.gethull`.
  - **`--verify-sig` runtime:** verifies the gethull manifest
    signature against `HL_PLATFORM_PUBKEY_HEX`; hard-rejects on
    missing block unless `--no-verify-platform`. Apps built by
    dev hulls (placeholder pubkey) skip the check silently with
    a one-line warning - same bootstrap shape as `hull update`'s
    release-pubkey placeholder.
  - **`hull verify` (CLI):** matching `--no-verify-platform`
    flag, new `tool.platform_pubkey()` Lua binding exposing the
    embedded pubkey, gethull-layer block in the report.
  - **Browser verifier (`site/verify.html`):**
    `verifyGethullLayer()` checks `platform.gethull.signature`
    against the hardcoded gethull pubkey; renders the layer
    explicitly. Per-app platform layer is now self-consistency
    only (the canary scanner is gone - the signed per-arch
    SHA-256 does all the integrity work it was hypothesized for).
- **`--no-verify-platform`** flag on both `hull build` and the
  runtime serve path. Documented escape valve for dev-built hulls
  and forks signing with their own key.
- **`tool.platform_pubkey()`** Lua tool-mode binding - returns the
  embedded `HL_PLATFORM_PUBKEY_HEX` or `nil` when the all-zeros
  placeholder is active.
- **`hull sign-release` / `hull verify-release`** still cover the
  release-binary path; the new gethull layer is signed by a
  separate key (`HULL_PLATFORM_KEY`) so a release-key compromise
  cannot forge platform-sigs and vice-versa.

### Changed

- **`signature.c` §5 reduced to self-consistency.** The v0.1.2
  per-app platform layer no longer pins
  `platform.public_key_hex` against `HL_PLATFORM_PUBKEY_HEX` -
  that role moved to the new §5b gethull layer. Forks signing
  platform with their own developer key continue to work without
  conflicting with the upstream gethull pubkey. `hull verify`
  matches the same shape; the
  `Platform layer: WARNING - key mismatch` path becomes a soft
  comparison against `--platform-key` only.
- **Docs/security.md §2 and §6** rewritten to reflect the
  three-layer structure (gethull, per-app, app developer) and
  the v0.1.3 status flip from "wired but inactive" to "shipped
  and enforced." Section 3.A and 3.B replace the canary-based
  prevention story with the manifest-signature prevention story.
- **Honest scorecard on `gethull.dev`** moves the platform-sig
  bullet from "Not yet" to "Ships," and replaces the old gap
  text with an explicit out-of-scope note for post-install
  binary integrity (OS layer's job).

### Removed

- **Platform canary scanner** in the browser verifier. The
  canary was the placeholder integrity signal while the
  signed-manifest path was missing; v0.1.3's per-arch SHA-256s
  cover the same property without Makefile post-link gymnastics.

### Fixed - release process

- **Platform-sig embed drift in `release.yml`.** `make EMBED_PLATFORM=1`
  was rebuilding `libhull_platform.a` from source during the
  `build-native` / `build-cosmo` jobs, replacing the downloaded
  artifact (the one `sign-platform-manifest` had hashed) with a
  freshly-built .a that produces different bytes on a different
  GH Actions VM. Result: the hull binary embedded one set of bytes
  and a manifest signature pointing at a different set, breaking
  the gethull cross-check for every app built with v0.1.3.
  - Fixed by switching to `make -o build/libhull_platform.a` -
    GNU make's `--assume-old` flag tells make to treat the .a as
    pristine and never rebuild it.
- **Verify-before-embed step** in every build job: sha256sum the
  downloaded .a, compare against its line in the signed manifest,
  fail loudly on mismatch. Defense in depth if the rebuild guard
  ever regresses.
- **Platform-sig E2E smoke test inside CI** (`release.yml`): each
  `build-native` / `build-cosmo` job now does `hull keygen` +
  `hull sign-platform` + `hull build --sign` + run with
  `--verify-sig` on the freshly-baked hull binary. Both signature
  layers (per-app v0.1.2 + gethull v0.1.3) are exercised end-to-end
  before the artifact is uploaded. Tests `tests/release_smoke.sh`
  still cover the release-sig path (post-publish, manual); the new
  in-CI test covers the gap that let v0.1.3-rc ship broken.

### Fixed

CI matrix repairs - four jobs that had been red since v0.1.2 went
green during the v0.1.3 stabilization pass:

- **Cosmopolitan (APE)** - test fixtures used `system("rm -rf ...")`
  for cleanup, which Cosmo's toybox `rm` rejects (the `-r` flag fails
  in any combined form: `-r`, `-rf`, `-fr`, `-Rf`). The failed cleanup
  left tmpdirs intact and the next `mkdir` hit `EEXIST`, cascading-
  failing the rest of the suite. Replaced with in-process
  `nftw(FTW_DEPTH | FTW_PHYS)` recursive delete - POSIX, works on
  Linux/macOS/cosmo uniformly, no flag-parsing dependency. Gated the
  `_XOPEN_SOURCE` define on `__linux__` so the change doesn't hide
  Darwin extensions on macOS.
- **Static Analysis (scan-build + cppcheck)** - scan-build flagged
  `agent/compute.c`'s `const char *platform = "unknown"` as a
  dead-store on Linux x86_64 because subsequent `#ifdef` branches
  reassigned it. Restructured as one `#if/#elif/#else` chain so
  exactly one assignment runs per build. cppcheck flagged four false
  positives - `agent/overview.c` `knownArgument`, three
  `commands/tools.c` `knownConditionTrueFalse` on version-string
  prefix-stripping - added suppressions for the defensive code
  cppcheck couldn't model.
- **Code Coverage** - `test_lua`'s link line referenced `$(PLEDGE_OBJS)`,
  `$(BUILDDIR)/tool.o`, `$(BUILDDIR)/sandbox.o`, and several other
  objects without listing them as prerequisites. Serial builds picked
  them up via earlier test binaries' chains; the coverage and ASan
  parallel builds didn't. Added explicit prereqs so make schedules
  them unconditionally.
- **MSan + UBSan** - vendor `*_CFLAGS` carried
  `-fsanitize=memory,undefined`; UBSan flagged well-known
  "technically UB but works on every target" patterns in
  `tweetnacl` (left shift of negative values in field arithmetic)
  and `quickjs` (function-pointer casts in `cutils`, signed-overflow
  left shifts). Dropped `,undefined` from QJS/LUA/SQLITE/LOG/
  SH_ARENA/SH_JSON/TWEETNACL/STB CFLAGS so vendor TUs get MSan
  shadow tracking (the actual point of the job) but not UBSan.
  Added MSan instrumentation to `MBEDTLS_CFLAGS` so MSan tracks
  mbedtls writes to caller buffers (e.g. `mbedtls_sha256` →
  caller-provided `digest[32]`); without it MSan reported every
  subsequent read as use-of-uninitialized. Hull's own CFLAGS still
  carry `-fsanitize=memory,undefined`, so Hull-code UBSan errors
  still fail the build loudly.

## [0.1.2] - 2026-05-26

Two themes: (1) side-loaded Hull-native tools (`hull tools install`),
opening the door for optional executables like `wamrc` without bloating
the main binary; (2) a complete agent-onboarding surface so AI coding
agents can bootstrap against a fresh hull install without scraping
docs from elsewhere.

### Added

- **`hull tools install <name> [--all]` / `tools list [--json]` /
  `tools uninstall <name>`** - Side-load optional tools (currently
  `wamrc`, the WAMR AOT compiler) from GitHub releases into
  `$HOME/.hull/tools/`. Reuses the same Ed25519-signed `hull.sha256`
  manifest that protects `hull update` - no new keys, no new
  ceremonies. Version-coupled: pulls from the SAME release as the
  running hull binary so e.g. wamrc stays at the WAMR commit hull was
  compiled against. Cosmo unsupported for tools that need LLVM. Full
  design: [`docs/tools_install.md`](docs/tools_install.md).
- **CI `build-wamrc` matrix** (linux-x86_64, linux-aarch64,
  darwin-arm64) in the release workflow - produces signed
  `hull-wamrc-<platform>` assets listed in the same `hull.sha256`
  manifest as the hull binaries.
- **`tool.find_tool(name)` Lua binding** (build-tool VM) - generic
  4-step lookup (`~/.hull/tools/` → sibling-of-hull → `$PATH`).
  Replaces the inline `find_wamrc()` heuristic in `build.lua`.
- **`hull --help` / `hull help` / `hull -h`** - Top-level usage
  printer grouping every registered subcommand by purpose. Suppresses
  build-time-gated commands when their `HL_ENABLE_*` flag is off so
  help only advertises what this binary can do. Symmetric with
  `--version`/`-v`/`version`.
- **`hull agent tools`** - Generic JSON dump of the tool registry
  crossed with the install state on this host. Independent of
  `HL_ENABLE_HTTP_CLIENT` - CLI-flavor builds without the installer
  can still enumerate what's registered.
- **`hull agent compute` gains a `wamrc` block** - installed/path/
  managed/available_for_platform/install_hint, so agents seeing
  modules with no AOT artifacts can recommend
  `hull tools install wamrc` in one call.
- **`hull agent context --list`** - Enumerate every embedded topic
  doc plus which level markers (minimal/compact/full) each populates.
  Cold-start agents call this first to discover the topic registry.
- **`hull agent overview [app_dir]`** - Single-shot composite
  summary (runtime, routes, compute, gpu, migrations, declared
  modules, tests, build_ready). One call to orient an agent dropped
  into an unfamiliar tree.
- **Six new context docs** in `stdlib/context/`: `orientation.md`
  (the AI-agent cold-start primer), `quickstart-{web,cli,tui}.md`
  (opinionated bootstraps per app shape), `gpu.md` (WGSL shaders +
  GPU vs WASM AOT crossover guidance), `tools.md` (`hull tools
  install` from the app-dev perspective). All three levels.
- **Discoverability breadcrumbs** in `hull --help`, bare-`hull`
  usage, and `install.sh` postscript - every entry point now points
  at `hull agent context --task=orientation --level=minimal` so an
  agent can bootstrap by reading any one of them.
- **Shared `release_io.{c,h}` helpers** extracted from
  `commands/update.c` - HTTPS GET, JSON-string extraction, SHA-256
  hex, manifest-line lookup, atomic install. Used by both `hull
  update` and `hull tools install` so the trust chain is implemented
  once.
- **`tests/release_smoke.sh`** - Post-`gh release create` smoke
  script that exercises the live install path: download wamrc from
  the just-published release, verify SHA-256, run `wamrc --help`,
  uninstall. The only end-to-end test of the live install codepath
  (the rest is covered by unit + e2e suites).
- **Shell completions** (bash, zsh, fish) for the new surface:
  `tools list/install/uninstall`, `agent overview`, `agent context
  --list`, all 19 context tasks.

### Fixed

- **`hl_release_io_find_checksum` OOB-read defense** - Reordered the
  exact-match guard's OR chain so the bounds check runs before the
  byte dereference. Dormant in practice (Keel allocates body+1 and
  writes a trailing NUL), but the helper is now safe regardless of
  caller. Surfaced by the post-implementation audit.
- **`atomic_write` checks `fsync` + `close` return values** - Aborts
  + unlinks the `.new` sidecar on either failure. Prevents a
  power-loss window where rename could happen over a half-flushed
  file (ENOSPC, EIO on network mounts).
- **SHA-256 hex compare is now constant-time** in both `hull update`
  and `hull tools install` paths (via `mbedtls_ct_memcmp`). The
  compared values are public, so the practical timing leak is nil,
  but the codebase now uniformly uses constant-time comparison for
  hash equality.
- **JSON escaper for `hull tools list --json`** - Tool descriptions
  now flow through a proper RFC 8259 escaper. Today's static
  registry is clean (no embedded quotes/backslashes), but future
  tool descriptions can't accidentally produce malformed JSON.
- **Makefile xxd hyphen handling** - The stdlib registry generator's
  sed expression now translates hyphens to underscores in symbol
  names, matching xxd's own normalization. Without this, context
  docs with hyphens in their filenames (`quickstart-web.md`) would
  fail to link.

### Changed

- **`build.lua` wamrc lookup simplified** - Replaces the inline
  `find_wamrc()` (PATH → `./build/wamrc` → sibling-of-hull) with a
  call to `tool.find_tool("wamrc")` plus a single `./build/wamrc`
  dev fallback. Same effective lookup order, implemented once in C.
- **`hull doctor` wamrc row** - Reports `installed` /
  `managed via hull tools` / `not installed` states. Hint text now
  recommends `hull tools install wamrc` (signed download) over
  `make wamrc` (build from source) as the first option.
- **`hull agent context` bare-command error** - Now points users at
  `--list` instead of hard-coding a stale topic enumeration.
- **`hull update` refactored to use `release_io`** - Same external
  behavior; the HTTPS + SHA-256 + manifest plumbing it relied on is
  now shared with `hull tools install`. Net diff: 252 lines deleted
  from `update.c`, same lines added to `release_io.{c,h}` plus
  reused in `tools.c`.

## [0.1.1] - 2026-05-25

Patch release. Two reproducible v0.1.0 bugs that broke first-time
user experience on Linux:

### Fixed

- **`hull init` scaffolded app crashed at load.** The generated
  `app.lua` / `app.js` declared `modules = ["hull/time@1"]` and
  imported `hull.log` + `hull.time`, but never declared
  `hull/http-server@1`. The runtime gate refused `app.get` (which
  the http-server module installs on the `app` intrinsic) and
  emitted "module 'hull/log' was imported at top-of-file but is not
  declared". Templates now declare every module they import
  (`hull/http-server@1`, `hull/log@1`, `hull/time@1`); the JS
  template also gains its missing `import { app, log } from
  "hull:..."` lines.
- **Released native binaries shipped without the embedded platform
  library.** `hull doctor` on a freshly-installed v0.1.0
  `hull-linux-x86_64` / `hull-linux-aarch64` / `hull-darwin-arm64`
  reported `Platform library: embedded no` and `hull build: not
  ready`. Cause: the release-workflow native-build step ran plain
  `make` while the Cosmopolitan job correctly ran `make CC=cosmocc
  EMBED_PLATFORM=cosmo`. Now the native step runs `make platform`
  followed by `make EMBED_PLATFORM=1`, with a smoke-test check that
  fails the release if doctor doesn't report `embedded yes`. Binary
  size grows ~5.5 MB → ~12 MB per native platform; still well under
  the cosmo APE.

### Improved

- `HL_PLATFORM_PUBKEY_HEX` and `HL_RELEASE_PUBKEY_HEX` macros are
  now `#ifndef`-guarded in their headers so tests, self-hosted
  forks, and future rotation tooling can override them at compile
  time via `-DHL_*_PUBKEY_HEX=...`. Production builds inherit the
  embedded values unchanged.

### Reverted

- `HL_PLATFORM_PUBKEY_HEX` is back to the all-zeros placeholder.
  v0.1.0 shipped a real value, but the rest of the platform-sig
  chain (canary emission in `libhull_platform.a`, release-time
  signed manifest, `hull build` pass-through, runtime
  enforcement - tracked as item 2 in `docs/roadmap_next.md`)
  isn't in place yet. With a real pubkey embedded and no actual
  signed platform artefact, the pinning bypass in
  `hl_verify_startup` turns off and rejects every `hull build`
  output. The placeholder restores the documented "wired but
  inactive" state until the full chain ships. The platform key
  itself is still backed up offline and in the
  `HULL_PLATFORM_KEY` GitHub secret - no key regeneration needed
  when the chain is ready.

### Trust model

No changes. Existing v0.1.0 binaries continue to verify their own
updates against the same embedded release pubkey; v0.1.1 is a
drop-in replacement.

## [0.1.0] - 2026-05-25

First publicly tagged release. The goal of `v0.1.x` is to lock the
distribution and capability model so apps and tooling built against
`v0.1` can rely on a stable shape; performance, internal architecture,
and the WASM / GPU surface continue to evolve under that contract.

### Distribution

- **One binary per platform.** `hull` ships as a self-contained
  executable with no runtime dependencies. Bundled: Lua 5.4, QuickJS
  (ES2023), SQLite, mbedTLS, WAMR, TweetNaCl, the embedded Mozilla CA
  bundle, the platform standard library, and the build toolchain.
- **Four release artifacts per tag.**
  - `hull-linux-x86_64` - native ELF (~5 MB).
  - `hull-linux-aarch64` - native ELF for Graviton / NVIDIA DGX /
    Ampere / Raspberry Pi 4+ (~5 MB).
  - `hull-darwin-arm64` - native Mach-O for Apple Silicon (~5 MB).
  - `hull-cosmo` - Cosmopolitan APE (fat x86_64 + aarch64, ~30 MB)
    that runs on Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD from
    a single file.
- **Signed releases.** Every release manifest (`hull.sha256`) is
  Ed25519-signed (`hull.sha256.sig`). The release public key is
  embedded in every Hull binary as `HL_RELEASE_PUBKEY_HEX`; `hull
  update` verifies the signature against the embedded key before
  atomically replacing itself via `rename(2)`. See
  [docs/release_signing.md](docs/release_signing.md).
- **One-line installer.** `curl -fsSL https://gethull.dev/install.sh | sh`
  detects OS+arch, downloads the matching native binary (or `hull-cosmo`
  on unsupported combinations), verifies SHA-256 against `hull.sha256`,
  and installs to `~/.local/bin/hull`.
- **Self-update.** `hull update` fetches the latest release via
  `api.github.com`, verifies both the signature and the SHA-256, and
  atomically replaces the running binary.
- **Shell completions.** Bash, zsh, fish completions ship in
  `completions/`.

### Runtimes

- **Lua 5.4** sandbox: `io` and `os` removed, custom `require` resolves
  only from the embedded stdlib registry, 64 MB memory cap, configurable
  per-request instruction limit (default 100 M instructions).
- **QuickJS** sandbox (ES2023): `eval` removed, `std`/`os` not loaded,
  64 MB memory cap, 1 MB stack cap, configurable per-request instruction
  limit.
- **One runtime per app** - entry point extension (`app.lua` or
  `app.js`) selects.
- **Polymorphic vtable.** Both runtimes go through the same
  `HlRuntimeVtable`; the capability layer (`hl_cap_*`) is shared C and
  is the only path to system resources.

### Capabilities

All system access is mediated by the C capability layer:

- **Database** - SQLite, parameterized queries, transactions, batch,
  user-defined functions (Lua/JS or WASM), async pool.
- **Filesystem** - manifest-allowlisted read/write paths, traversal
  rejection, symlink-escape rejection, mmap.
- **HTTP client / server** - separately gateable
  (`HL_ENABLE_HTTP_CLIENT`, `HL_ENABLE_HTTP_SERVER`). Server: routes,
  middleware, SSE, WebSockets, body limits, ETag, static files. Client:
  host-allowlisted `http.fetch`, SMTP, follow-redirects, mTLS via
  embedded mbedTLS.
- **Crypto** - SHA-256/512, HMAC, PBKDF2, Ed25519, NaCl box/secretbox,
  random. Constant-time comparison helpers.
- **Time / env / WebSockets / static files / image codecs**.
- **Compute (WASM)** - WAMR runtime, AOT compilation, gas metering,
  persistent instances, shared read-only data segments, streaming I/O.
  ~256 KB binary cost, gateable.
- **Compute (GPU)** - wgpu-native (Vulkan / Metal / DX12), WGSL
  compute shaders, pipelines, persistent buffers, GPU↔CPU and GPU↔GPU
  copies, textures. Opt-in (`HL_ENABLE_GPU=1`).
- **Tool mode (Lua-only)** - `hull build`, `hull deploy`, etc. run in
  a sandboxed Lua VM via `hl_tool_spawn` with a compiler allowlist.
  No `system()` / `popen()`.

### Capability sandbox

- **Linux / Cosmopolitan** - `pledge(2)` syscall filter + `unveil(2)`
  filesystem cap. Violation → SIGKILL.
- **macOS** - Seatbelt SBPL profile built dynamically from the
  manifest. Violation → EPERM.
- **OpenBSD** - native pledge/unveil.

### Module declaration

- `manifest.modules` is the canonical declaration mechanism. Apps list
  every first-party stdlib module they use, with API version
  (`"hull/crypto@1"` etc.).
- Two independent gates: **module resolver** (build-time / declaration
  check) and **per-call capability cap** (manifest `fs`/`env`/`hosts`
  allowlist enforced at the C boundary).
- `hull/app` is the only intrinsic - `app.manifest`, `app.get`,
  `app.post`, `app.use`, `app.router`, `app.ws`, `app.sse`, `app.main`.
  Other modules (timers, db, fs, http, …) must be declared.

### Build modes

- `make` - default build, ~5 MB on darwin/arm64.
- `make HL_ENABLE_DB=0` - compute-only build, ~3.9 MB on darwin/arm64
  (no SQLite, no DB-dependent stdlib modules).
- `make HL_ENABLE_HTTP_SERVER=0` - CLI-only build (no inbound HTTP).
  Apps may use `app.main(fn)` plus `http.fetch` outbound.
- `make HL_ENABLE_HTTP=0` - pure compute/CLI, no Keel, no mbedTLS,
  smallest possible build.
- `make CC=cosmocc EMBED_PLATFORM=cosmo` - fat APE binary with multi-arch
  embedded platform library.
- `make HL_ENABLE_GPU=1 WGPU_LIB_DIR=vendor/wgpu` - adds GPU compute.

### Application surface

- **Modular layouts** for REST / CLI / TUI apps: `hull new --type rest`
  / `--type cli` / `--type tui` scaffold a `routes/` + `models/` +
  `middleware/` + `lib/` skeleton instead of a single `app.lua`.
- **Background timers** - `app.every(ms, fn)` and `app.daily(hhmm,
  fn)` run on the event-loop thread, support full async runtime,
  return `false` to self-cancel.
- **Migrations** - versioned SQL migrations in `migrations/*.sql`,
  embedded in built binaries, auto-applied on startup, tracked by
  checksum.

### Standard library

Embedded `hull/*` modules (Lua + JS, identical semantics where the
languages allow):

- **Middleware** - cors, ratelimit, csrf, auth (session + JWT), session,
  logger, transaction, idempotency, outbox, inbox, rbac, health, etag.
- **Helpers** - cookie, jwt, template (with inheritance + filters),
  validate, form, i18n, csv, search (SQLite FTS5), image, db.udf.
- **CSRF wire format** is fixed and identical across Lua and JS
  (`tsHex.hmac_hex` over `session_id ":" tsHex`); a cross-runtime
  reference token is asserted by unit tests in both runtimes.

### Tooling

- **`hull agent`** - JSON introspection commands for AI coding agents:
  `routes`, `db schema`, `db query`, `request`, `status`, `errors`,
  `test`, `context`, `migrate`, `deploy`, `manifest`, `endpoint`,
  `middleware`, `capabilities`, `modules`, `validate`, `vfs`, `compute`,
  `gpu`, `perf`, `logs`, `eval`, `template`, `schema_diff`, `sql`.
- **`hull mcp`** - MCP server exposing the same introspection surface
  to MCP-aware clients (Claude Code, Cursor, OpenCode).
- **`hull doctor`** - environment-readiness report (platform embedded,
  compilers available).
- **`hull check`** - manifest + module-graph validation before tests.
- **`hull dev`** - watch-and-reload server with structured error
  sidecars (`--agent` writes `.hull/dev.json` + `.hull/last_error.json`).
- **`hull init`** - in-place project initialization (git-init-style;
  doesn't overwrite existing files).

### Distribution site

- `https://gethull.dev/` - landing page + `install.sh`. Hosted on S3
  behind CloudFront, deployed automatically from `site/**` changes via
  `.github/workflows/deploy-site.yml`.

### Licensing

Dual-licensed:
- **AGPL-3.0** - free for AGPL-compatible use.
- **Commercial** - closed-source embedding and proprietary
  distribution. Contact `licensing@artalis.io`.
- See [LICENSING.md](LICENSING.md) for which license applies to your
  use case.

### Known limitations in 0.1.0

- macOS x86_64 (Intel) is not a native target - Intel Mac users get
  `hull-cosmo` via the installer.
- Windows native binary not in the matrix yet; Windows users get
  `hull-cosmo` via WSL or directly (APE runs natively on Windows).
- HTTP/2 is supported via Keel, but production hardening of HTTP/2
  server is still maturing (see `vendor/keel/CLAUDE.md`).
- PostgreSQL backend for `hull/db` is on the post-0.1 roadmap; v0.1
  ships SQLite only.

[Unreleased]: https://github.com/artalis-io/hull/compare/v0.1.1...HEAD
[0.1.1]: https://github.com/artalis-io/hull/releases/tag/v0.1.1
[0.1.0]: https://github.com/artalis-io/hull/releases/tag/v0.1.0
