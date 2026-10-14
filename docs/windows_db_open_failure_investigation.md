# Windows database-open failure investigation (2026-10-13)

The access violation observed while checking the local v0.16.0 installation
is specific to running the Cosmopolitan executable inside Codex's restricted
Windows execution sandbox. It is not evidence of a Hull app-context cleanup
crash. The initial cleanup-bug diagnosis was premature.

The installed, signature-verified release was tested with a database path
whose parent directory did not exist. SQLite reported error 14 and Hull
reported `app context init failed`. Under the restricted executor the process
status was `-1073741819` (`0xC0000005`, access violation). Outside that executor,
the identical failure returned status 256, the Windows APE representation of
exit code 1. An unknown CLI option also produced the access violation inside
the restricted executor and status 256 outside it, without initializing the
database or server.

A local Cosmopolitan build reproduced the restricted-executor behavior under
GDB, using its matching ELF symbols. GDB located the fault at
`TerminateThisProcess(dwWaitStatus=256)` in
`libc/intrin/terminatethisprocess.c:45`, after the worker threads exited.
This locates the observed fault in the platform process-exit path rather
than app-context initialization or cleanup. The precise interaction between
the restricted executor and Cosmopolitan's termination routine remains
unresolved; no production runtime fix or sandbox bypass was added.

## Regression coverage

`tests/e2e_db_open_failure.sh` checks both Lua and JavaScript with:

- A missing parent directory for the database file.
- An existing directory passed as the database file.

Each case requires exit code 1, SQLite-open and app-context diagnostics, and
no execution of app.main. The suite uses the existing Windows APE exit-status
helper so shell truncation cannot accidentally turn a crash into success.
It runs as part of `make e2e-cli` and independently via
`make e2e-db-open-failure` or:

```sh
HULL_BIN=/path/to/hull sh tests/e2e_db_open_failure.sh
```

The installed v0.16.0 release and a locally rebuilt Cosmopolitan debug binary
each passed all four cases outside the restricted executor (8/8 checks).
Run these checks in the ordinary Windows test environment; do not weaken the
exit-code assertion to accommodate the restricted-executor access violation.
