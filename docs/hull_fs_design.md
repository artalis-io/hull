# hull.fs application design (design-only)

Status: **DESIGN (awaiting review). NOTHING implemented.** This is the dedicated
design of the APPLICATION-facing `hull.fs` capability - the general filesystem
surface an app author uses. It is a peer of, and a PREREQUISITE for, the
BuildContext audit ([hull_fs_buildcontext_audit.md](hull_fs_buildcontext_audit.md),
PR #393): BuildContext is the narrower, Hull-owned plugin surface built ON the
hardened primitives designed here - so `hull.fs` is designed first.

Design rule carried in from `hull.path` (#392, #394): **`hull.path` manipulates
NAMES lexically; `hull.fs` exercises AUTHORITY.** `hull.fs` is where real
filesystem containment is enforced against the RESOLVED object - never by lexical
checks alone.

Symlink policy (§5) has been DECIDED during review: **in-sandbox symlinks are
followed, with `base_dir` a hard containment ceiling.** One DECISION-FOR-REVIEW
remains (§9, resolver-migration sequencing). Everything else is a recommendation
with rationale.

## 1. Inventory - the CURRENT application `hull.fs` (verified, not assumed)

### 1.1 What app code can actually call today

Both runtimes expose EXACTLY three operations plus the mapped-buffer methods:

| app op | Lua | JS | backing cap |
|---|---|---|---|
| read a file | `fs.read(path)` | `fs.read(path)` | `hl_cap_fs_read` |
| write a file | `fs.write(path, bytes)` | `fs.write(path, bytes)` | `hl_cap_fs_write` |
| memory-map (zero-copy RO window) | `fs.mmap(path[, {offset,length}])` | `fs.mmap(path[, {offset,length}])` | `hl_cap_fs_mmap` / `_mmap_window` |
| mapped-buffer size / release | `buf:len()` / `buf:close()` | `buf.len()` / `buf.close()` | `_munmap` / `_borrow` / `_release` |

Bindings: `src/hull/runtime/lua/mod_fs.c` (the `luaL_Reg` is `read`/`write`/`mmap`
+ `len`/`close`), `src/hull/runtime/js/mod_fs.c` (`read`/`write`/`mmap`).

### 1.2 Correction to the BuildContext audit's inventory

The audit (§1.2) listed the app surface as "read, write, exists, delete, mmap."
That is the **cap layer**, not the app surface. `hl_cap_fs_exists` and
`hl_cap_fs_delete` DO exist in `include/hull/cap/fs.h`, but **neither is exposed
as an app binding** in either runtime today. So the real application surface is
even smaller: **read / write / mmap only.** There is NO enumeration, NO
stat/metadata, NO exists probe, NO delete, NO rename/copy/mkdir at the app level.

### 1.3 Authority model (unchanged, still correct)

Two independent gates: a build-time module gate (`hull/fs@1`) and a per-call
capability gate. `hl_cap_fs_validate` enforces the manifest `fs.read` / `fs.write`
allowlists + containment under `base_dir`.

### 1.4 The load-bearing finding (same as the audit): resolution is TOCTOU-susceptible

`hl_cap_fs_validate` resolves with **`realpath()`** (three call sites in
`src/hull/cap/fs.c`) and the actual `open`/`read`/`write` happens later:
`realpath -> check -> open`. Between the check and the open a path component can be
swapped (a directory replaced by a symlink), so the check does not bind the
`open` target. In-tree precedent for the fix already exists:
`src/hull/shared/blob_store.c` opens with `O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC`.

## 2. Design goals

1. **Race-resistant resolution** - replace `realpath -> check -> open` with a
   descriptor-relative walk so the authority check binds the object actually
   opened.
2. **Deterministic enumeration + metadata** - add `list` and `stat`, the minimum
   an app (or later a build) needs to discover and describe files.
3. **Follow in-sandbox symlinks, contained race-free** - `base_dir` is a hard
   ceiling (§5, DECIDED).
4. **Keep the surface deliberately SMALL** - Hull's convention is to reach for
   stdlib / composition before widening a C capability. Conveniences
   (copy/rename/mkdir/atomic-write/tempfile) are enumerated as candidates in §4.3
   but are NOT added by default; several belong to the Hull-owned BuildContext,
   not app `hull.fs`.
5. **Lua/JS parity** across the whole app surface (the `hull.path` bar: mirrored
   semantics, snake_case vs camelCase only).
6. **Backward-compatible semantics** - `read`/`write`/`mmap` keep their contracts;
   only the resolution MECHANISM hardens and the symlink behavior tightens (§5),
   both documented.

## 3. Resolution model (the shared foundation) - follow within base, contained

A single resolver underpins every `hull.fs` op AND (later) BuildContext. It
**follows symlinks that resolve within `base_dir`** and **refuses any resolution
that would escape it** (via `..` chains or an out-of-base target), with NO
resolve-then-open (TOCTOU) window. `base_dir` is a hard ceiling - the sandbox root
IS the root for resolution purposes.

- **Lexical pre-check** with `hull.path` (reject an absolute caller path, reject
  `..` in the CALLER-supplied path) - a fast fail, NOT the authority. (Symlink
  targets encountered DURING resolution are handled below, not by this check.)
- **Linux >= 5.6:** `openat2(base_dfd, relpath, { flags, resolve: RESOLVE_IN_ROOT
  | RESOLVE_NO_MAGICLINKS })`. `RESOLVE_IN_ROOT` makes `base_dfd` the root for the
  ENTIRE resolution: interior and leaf symlinks - even absolute ones, even ones
  containing `..` - are followed but can NEVER escape `base_dir`; the kernel
  enforces containment race-free in one syscall, and the returned fd IS the opened
  object (no separate check to race). `RESOLVE_NO_MAGICLINKS` blocks
  `/proc`-style magic-symlink escapes.
- **Platforms without `openat2` (macOS, older Linux, cosmo where unavailable):** a
  manual component-wise walk that REPRODUCES `RESOLVE_IN_ROOT`. Hold a STACK of
  directory fds rooted at `base_dfd`; for each component `openat(top, comp,
  O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC)`; when a component is a symlink (detected
  via `O_NOFOLLOW`'s `ELOOP` or an `fstatat` probe), `readlinkat` its target and
  SPLICE the target's components into the walk (an ABSOLUTE target restarts at
  `base_dfd` = re-root; a RELATIVE target continues from the current dir). A `..`
  component POPS the fd stack and is CLAMPED at `base_dfd` - it is never passed to
  the kernel as `openat(dfd, "..")`, so the walk can never ascend above the base.
  Bound total symlink expansions (e.g. 40, matching the kernel `ELOOP` limit) to
  stop loops. Because every step is relative to a HELD fd (never a re-resolved
  path string), there is no TOCTOU window and containment is structural.
- The manifest `fs.read` / `fs.write` allowlist is still enforced, against the
  lexically-normalized relative caller path.
- **Fail closed** where a platform offers neither primitive - never downgrade to
  `realpath`.

This is the userspace equivalent of what container runtimes settled on
(`RESOLVE_IN_ROOT` / Go's `securejoin`): symlinks are followed, but the sandbox
root is a hard ceiling.

**Platform notes** (resolve at implementation): `openat2` is Linux-only (>= 5.6);
confirm whether the Cosmopolitan target exposes it (raw syscall) or must use the
manual walk on its Linux host. `openat` / `O_NOFOLLOW` / `readlinkat` / `fstatat`
are POSIX (Linux, macOS, cosmo shim). A native Windows target (not today's cosmo
APE) needs the reparse-point-aware equivalent; scope that when such a target
exists.

## 4. Operations

### 4.1 Existing, hardened (no signature change)
- `fs.read(path)` -> bytes | (nil,err) / throw. Now resolved per §3; `fs.read`
  authority.
- `fs.write(path, bytes)` -> true | (nil,err) / throw. Resolved per §3; `fs.write`
  authority. (Parent-dir creation is NOT implicit - see §4.3 `mkdir`.)
- `fs.mmap(path[, window])` -> MappedBuffer. Resolved per §3 for the initial open;
  the mapping's lifetime semantics are unchanged (refcounted borrow, `close`
  defers `munmap` while a borrower is alive).

### 4.2 New (the minimal additions this design proposes)

**`fs.stat(path)` -> metadata | nil**
- Returns `{ type, size, mode, mtime }` where `type` is one of `"file"`,
  `"dir"`, `"symlink"`, `"other"`. `mode` is the permission bits; `mtime` is
  exposed but carries a policy note: reproducible builds MUST NOT key on it (that
  is the BuildContext content-hash policy, audit §3.4). Returns `nil` (Lua) /
  `null` (JS) for a non-existent path - so `stat(p) ~= nil` SUBSUMES a separate
  `exists` probe.
- Implemented with `fstatat(parent_dfd, leaf, AT_SYMLINK_NOFOLLOW)` off the §3
  walk, so it reports a symlink AS a symlink WITHOUT following it (`lstat`
  semantics). Requires `fs.read` authority over the target.

**`fs.list(dir)` -> entries**
- Returns a **deterministically ordered** (byte-wise sort of `name`) array of
  `{ name, type, size }`. `.` and `..` are omitted; no entry escapes `dir`.
  `fdopendir` on a `dirfd` opened per §3. **Non-recursive** by design - an app
  composes recursion with `hull.path.join` + `fs.list`; a recursive walker is
  stdlib/BuildContext territory, not a C primitive. Requires `fs.read` authority
  over `dir`.

`exists` is deliberately NOT added as a distinct op - `stat(p) ~= nil` covers it
without a second cap surface that could disagree with `stat` about symlinks.

### 4.3 Candidates DEFERRED (enumerated, with rationale - none added in v1)

| candidate | why deferred |
|---|---|
| `delete` | Exists at the cap layer, unexposed today. A mutation with real blast radius; add only when a concrete app need appears, and gate on `fs.write`. Not needed for enumeration/hardening. |
| `mkdir` | App authors rarely need directory creation; when they do it is usually part of a WRITE that BuildContext should own transactionally. Keep `write` non-implicit-mkdir for now. |
| `copy` / `rename` / `move` | `rename` is the ATOMIC-PUBLISH primitive - it belongs to the Hull-owned BuildContext.outputs (audit §3.5), NOT general app `hull.fs`, precisely so staging/publication authority stays separate. |
| `atomic write` / `tempfile` | Same: transactional staging + atomic publication is a BuildContext concern by the audit's HARD boundary; exposing it on app `hull.fs` would blur the two authorities. |

The through-line: `hull.fs` gets read-side hardening + discovery (stat/list); the
WRITE-side transactional machinery stays in BuildContext.

## 5. DECIDED: in-sandbox symlinks are FOLLOWED (contained), not refused

**Decision (owner: user, during review): the app surface MUST follow symlinks
that resolve within `base_dir`.** Real apps rely on in-sandbox symlinks -
atomic-deploy `current -> releases/N`, asset trees, config indirection - so
refusing them would break legitimate layouts. The prior draft's deny-all
recommendation is REJECTED. Preserving today's follow-within-base BEHAVIOR while
fixing the TOCTOU (via §3, not `realpath`) is the chosen path.

Semantics (the `RESOLVE_IN_ROOT` model of §3):
- A symlink whose recursively-resolved target stays under `base_dir` is FOLLOWED
  transparently, for `read` / `write` / `mmap`.
- An ABSOLUTE symlink target is RE-ROOTED at `base_dir` (the sandbox root is the
  resolution root), so `foo -> /bar` resolves to `base_dir/bar` - followed, still
  contained.
- Any resolution that would ESCAPE `base_dir` (a `..` chain or an out-of-base
  target) is REFUSED with `outside_root`. The ceiling is hard and kernel-enforced
  (Linux) / structurally enforced (manual walk).
- `stat` / `list` report a symlink's OWN type WITHOUT following (`lstat`
  semantics), so an app can enumerate links as links; only path RESOLUTION for
  read/write/mmap follows.
- Guarantee vs today: SAME "follow within base" behavior, now enforced race-free
  (no `realpath -> check -> open` window).

Still NOT a lexical check - `hull.path` never authorizes; the `openat2` /
descriptor walk is the authority.

## 6. Capability + manifest integration

Gates unchanged in shape: build-time `hull/fs@1` module gate + per-call
allowlist. New ops require `fs.read` authority over their target (`stat` and
`list` are reads of metadata / directory contents). No new manifest section.

**Error model (proposed, stable tokens).** Uniform `(nil, err)` (Lua) / `throw`
(JS) with a small closed set of tokens so app code can branch:
`"not_found"`, `"permission"` (allowlist / mode), `"not_a_directory"`,
`"is_a_directory"`, `"outside_root"` (a symlink or `..` chain that would escape
`base_dir`), `"symlink_loop"` (expansion bound exceeded), `"too_large"`,
`"io_error"`. (The exact tokens are confirmed at implementation; today's messages
are not a stable contract.)

## 7. Lua/JS parity

Every application op has both bindings, mirrored semantics, snake_case (Lua) /
camelCase (JS) only. `read`/`write`/`mmap` already parity; `stat`/`list` land in
both at once. A parity E2E (the `tests/e2e_path_parity.sh` model, but over a real
fixture tree because fs needs files) asserts Lua == JS for: `list` ordering +
per-entry metadata, `stat` fields + symlink typing, an in-base symlink FOLLOWED
identically for `read`, an absolute in-base symlink target re-rooted the same way,
and `outside_root` on a symlink (or `..` chain) whose target escapes `base_dir`.
Where a swapped-component TOCTOU is testable deterministically it is asserted;
where it is inherently racy it is covered by construction (`openat2
RESOLVE_IN_ROOT` / the held-fd walk) plus a unit test on the resolver.

## 8. Relationship to BuildContext (#393)

Same descriptor-relative resolver + `stat`/`list` primitives underpin both
surfaces; this design PROVIDES them.

| surface | who | authority | ops |
|---|---|---|---|
| **application `hull.fs`** | app code | manifest `fs.read`/`fs.write` + `base_dir` | read / write / mmap / stat / list |
| **plugin `BuildContext`** | Hull-owned, handed to a plugin | declared input roots + a private staging root; NARROWER | inputs: read / stat / list (recorded + hashed); outputs: staged write + atomic publish |

BuildContext.inputs reuses this design's resolver + `stat`/`list`, adds read
RECORDING + content hashing, and drops write. BuildContext.outputs adds the
staging + atomic-publish (`rename`/`fsync`) that app `hull.fs` deliberately does
NOT expose (§4.3). A build plugin never receives the general `hull.fs`.

## 9. DECISION-FOR-REVIEW (sole remaining): resolver-migration sequencing

The audit (§6) said the app-`hull.fs` migration to the descriptor-relative
resolver is a follow-up sequenced AFTER BuildContext proves the primitive.
Designing `hull.fs` first inverts that emphasis:

- **Recommended: resolver lands as the app-fs foundation FIRST.** Harden
  `read`/`write`/`mmap` + add `stat`/`list` on the descriptor-relative resolver as
  checkpoint 1; BuildContext (checkpoints 3-4) then builds on a primitive already
  proven in production by the app surface. This SUPERSEDES the audit's ordering.
- **Alternative: keep the audit's order.** Implement the resolver inside
  BuildContext first, migrate app `hull.fs` afterward. Keeps the app surface
  untouched longer, but ships the same TOCTOU one release longer and duplicates
  validation.

## 10. Non-scope + checkpoints

Design only. No code, no binding changes, no resolver, no `stat`/`list`, no
BuildContext, no plugin loader, no Query IR.

1. **This design + the BuildContext audit (#393)** - STOP for review. Symlink
   policy (§5) is DECIDED (follow within base, contained); the sole remaining
   decision is resolver-migration sequencing (§9).
2. Implement the descriptor-relative resolver + harden `read`/`write`/`mmap` + add
   `stat`/`list`, with Lua/JS parity tests - STOP.
3. `BuildContext.inputs` (declared reads + dependency hashing) - STOP.
4. `BuildContext.outputs` (transactional staging + atomic publish) - STOP. Then
   Build Plugin / BuildArtifact. **Not Query IR yet.**
