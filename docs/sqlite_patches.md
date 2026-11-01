# SQLite patches

Hull vendors the SQLite amalgamation in `vendor/sqlite/` unmodified: **no Hull
patch is applied to it today.** Hull configures SQLite through compile-time
options and run-time calls in `src/hull/cap/` only. This file exists so the
invariant lint (INV-10, `scripts/check_invariants.py`,
[invariants/README.md](invariants/README.md)) has somewhere to check a future
patch against.

**Adding a patch.** Mark every changed hunk with a `HULL PATCH NNNN` comment
(numbered, within 40 lines of each changed line) and add a
`## Patch NNNN - <what>` section here saying what it changes, why, and which
audit or issue it answers. INV-10 fails a marker this file does not list, a
section no marker uses, an unnumbered marker, and an added line in
`vendor/sqlite/` with no marker near it.

**Upgrading.** Replace the amalgamation wholesale. A diff that changes
`#define SQLITE_VERSION` is read as an upgrade, so its hunks are not checked one
by one; re-apply every patch listed here, keeping its marker.
