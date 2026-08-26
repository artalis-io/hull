# hull.fs application design

Status: **PARTIALLY IMPLEMENTED.** The application-facing `hull.fs` **resolver,
compiled path-authorization policy, `stat`, and `list` are SHIPPED** (checkpoints
1-3, merged: descriptor-relative virtual-root resolver → per-op platform parity +
race-resistance → compiled SUBTREE/EXACT/CREATE/PATTERN authorization + `stat`/
`list`). **Checkpoint 4 - BuildContext - is next** (the narrower, Hull-owned plugin
surface built ON these hardened primitives). Checkpoint sequencing + the shipped
vs remaining split live in §9 (resolver-first) and §10 (non-scope + checkpoints)
below, and the parity ratification record
[hull_fs_resolver_parity.md](hull_fs_resolver_parity.md).

This is the dedicated design of the APPLICATION-facing `hull.fs` capability - the
general filesystem surface an app author uses. It is a peer of, and a PREREQUISITE
for, the active BuildContext design
([buildcontext_design.md](buildcontext_design.md)). BuildContext is the narrower,
Hull-owned plugin surface built ON the hardened primitives designed here - so
`hull.fs` was designed first. The earlier inventory and sequencing record remains
in [hull_fs_buildcontext_audit.md](hull_fs_buildcontext_audit.md) as historical
input, not as the implementation contract.

Design rule carried in from `hull.path` (#392, #394): **`hull.path` manipulates
NAMES lexically; `hull.fs` exercises AUTHORITY.** `hull.fs` is where real
filesystem containment is enforced against the RESOLVED object - never by lexical
checks alone.

Both load-bearing decisions are SETTLED during review: symlink policy (§5) =
**virtual-root follow** (in-sandbox symlinks followed; absolute targets re-rooted,
excess `..` clamped, `base_dir` a hard ceiling - NO "escape" error, because a
virtual root makes escape impossible, not rejected); sequencing (§9) =
**resolver-first** (fix the existing `realpath` TOCTOU + migrate the current
surface before any plugin work). Everything else is a recommendation with
rationale.

This revision corrects SEVEN review findings across two rounds. Round 1: the
`RESOLVE_IN_ROOT`/`outside_root` contradiction (§3/§5/§6), the over-stated
allowlist claim (§1.3/§6), the `write` implicit-parent-creation back-compat
(§1.4/§4.1), and under-specified resolver result shapes (§3 mode table). Round 2:
(1) `WRITE` must also `O_TRUNC` to match today's truncating `fopen("wb")` (§3/§4.1,
tested §7); (2) virtual-root is NOT behavior-preserving for ABSOLUTE symlink
targets - now documented as an intentional compatibility change with a migration
note (§2/§5/§9), not "unchanged behavior"; (3) path authorization must follow the
RESOLVED path, not the caller path (`allowed/link -> ../secret`) - realized by
rooting resolution at the authorized grant so confinement IS authorization,
race-free (§3/§6, tested §7). Round 3: a manifest grant is not necessarily an
existing directory (Hull grants exact files and not-yet-existing write targets),
so §6 now compiles grants into AUTHORIZATION ENTRIES (held anchor dir fd + a
`SUBTREE`/`EXACT`/`CREATE` constraint), with deterministic most-specific selection,
independent read/write sets, and a per-kind symlink rule: `SUBTREE` follows
symlinks (virtual-root confined), `EXACT`/`CREATE` REFUSE any symlink
(`symlink_denied`) - so an exact-file grant never becomes sibling-directory
authority and needs no race-prone cross-policy symlink resolution. An independently
granted target is reached via its own grant, not through an exact-file alias.

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

### 1.3 Authority model (corrected - the cap layer confines, it does not authorize paths)

Gates: a build-time module gate (`hull/fs@1`) and a per-call capability gate. But
the per-call gate is NARROWER than the audit implied. `HlFsConfig`
(`include/hull/cap/fs.h`) carries ONLY `base_dir` + `base_len` - it does NOT carry
the manifest's individual `fs.read` / `fs.write` path lists, and
`hl_cap_fs_validate` checks ONLY containment under `base_dir` (via `realpath`
prefix), NOT a per-path allowlist. The manifest's read/write path restrictions are
materialized primarily by the KERNEL SANDBOX (unveil / seatbelt), not by the cap
layer. So today there are effectively two separable things already: ROOT
CONFINEMENT (cap layer, `base_dir`) and PATH AUTHORIZATION (kernel sandbox). §6
makes that split explicit for the new resolver + `stat`/`list`.

### 1.4 The load-bearing findings

**(a) Resolution is TOCTOU-susceptible.** `hl_cap_fs_validate` resolves with
**`realpath()`** (three call sites in `src/hull/cap/fs.c`) and the actual
`open`/`read`/`write` happens later: `realpath -> check -> open`. Between the check
and the open a path component can be swapped (a directory replaced by a symlink),
so the check does not bind the `open` target. In-tree precedent for the fix:
`src/hull/shared/blob_store.c` opens with `O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC`.

**(b) `write` creates parent directories** - `hl_cap_fs_write` mkdir-p's the
leaf's parents (a `mkdir()` loop over reconstructed absolute path prefixes). This
is EXISTING behavior the design must preserve (§4.1), and the absolute-path
`mkdir()` is itself part of the TOCTOU surface to move descriptor-relative.

**(c) `write` is NOT atomic** - it `fopen(full, "wb")` + `fwrite`, truncating in
place. Any header/comment implying atomicity is stale. Atomic write is a possible
FUTURE API change (§4.3), not part of this prerequisite.

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
6. **Backward-compatible where it can be, with ONE documented exception.**
   `read`/`write`/`mmap` keep their signatures; regular paths and RELATIVE
   in-sandbox symlinks resolve exactly as today, and `write` still creates parent
   dirs (§4.1). The ONE intentional compatibility change: ABSOLUTE symlink targets
   are re-interpreted under the virtual root (§5) instead of as host-absolute paths
   - `link -> /app/releases/1` no longer means the host path `/app/releases/1`. This
   is called out as an intentional change (not "unchanged behavior") with a
   migration note in §5; it affects only symlinks whose stored target is absolute.
7. **Two distinct authorities** - ROOT CONFINEMENT (descriptor resolver keeps every
   op under `base_dir`) is separate from OPERATION/PATH AUTHORIZATION (which paths
   an app may read/write). The resolver provides the first; the second is an
   explicit policy + the kernel sandbox (§6). Conflating them is the current gap
   this design must not repeat.

## 3. Resolution model (the shared foundation) - virtual-root, contained

A single resolver underpins every `hull.fs` op AND (later) BuildContext. Its
contract is **true virtual-root (chroot-like) semantics**: `base_dir` is the
resolution ROOT, symlinks are followed, and NOTHING can escape - with NO
resolve-then-open (TOCTOU) window.

**Semantic contract (this is the whole point of §5's decision).** Within
resolution:
- an ABSOLUTE symlink target is RE-ROOTED at `base_dir` (`foo -> /etc/x` resolves
  to `base_dir/etc/x`);
- excess `..` is CLAMPED at `base_dir` (`foo -> ../../../x` resolves to
  `base_dir/x`);
- these are NOT errors - a virtual root does not "reject escapes," it makes escape
  impossible by construction. There is therefore **no `outside_root` outcome for
  symlink targets or `..` encountered during resolution** (this is the correction
  to the earlier draft: `RESOLVE_IN_ROOT` clamps, it does not reject, and the
  manual fallback clamps identically - so the two cannot disagree).

The only rejection is a LEXICAL pre-check on the CALLER-supplied path (reject an
absolute path, reject `..` components) via `hull.path` - an API input contract
(`invalid_path`), distinct from resolution. The caller declares clean relative
paths; symlink DATA discovered on disk is virtual-rooted, not errored.

**The resolver performs the TERMINAL operation itself (no returned re-openable
path).** A single "opened object fd" is insufficient and would let the manual
fallback reintroduce a final-component race, so the resolver is MODE-parameterized
and each mode's terminal syscall is issued relative to a held fd:

| mode | terminal op | result | leaf symlink |
|---|---|---|---|
| `READ` / `MMAP` | open leaf `O_RDONLY` | leaf fd | FOLLOWED (contained) |
| `WRITE` | mkdir-p parents (contained) + open leaf `O_WRONLY\|O_CREAT\|O_TRUNC` | leaf fd | FOLLOWED (contained) |
| `STAT` | `fstatat(parent_fd, leaf, AT_SYMLINK_NOFOLLOW)` | stat record | NOT followed (link-own) |
| `LIST` | open dir `O_DIRECTORY` | dir fd | FOLLOWED (contained) |

The "leaf symlink FOLLOWED" column applies under a `SUBTREE` grant (§6). Under an
`EXACT` or `CREATE` grant, an intermediate or terminal symlink is REFUSED
(`symlink_denied`), never followed - see §6.

For `WRITE` and `STAT` the resolver walks to the leaf's PARENT fd (contained) and
issues the create/stat relative to that held fd - the final component is never
re-resolved from a path string, so there is no leaf race. `WRITE` carries
`O_TRUNC` because today's `fopen("wb")` truncates: overwriting `abcdef` with `xy`
must yield `xy`, not `xycdef` (a shorter-replacement test is mandatory, §7).

**The resolver is rooted at the SELECTED grant's anchor, not always `base_dir`.**
`root_dfd` below is the held ANCHOR fd of the compiled authorization entry the
caller path selects (§6) - an existing-directory grant's own fd (`SUBTREE`), an
exact-file grant's PARENT fd (`EXACT`, terminal-name-constrained), or a
not-yet-existing target's nearest-existing-ancestor fd (`CREATE`, component-
constrained). When the app declares no granular grants, the anchor IS `base_dir`
(= today's confinement). Rooting resolution at the anchor + applying the entry's
constraint is what makes authorization race-free (§6). The mode table below
describes the terminal op; §6 defines which anchor/constraint applies.

**Implementations:**
- **Linux >= 5.6:** `openat2(root_dfd, subpath, { flags, resolve: RESOLVE_IN_ROOT
  | RESOLVE_NO_MAGICLINKS })` for `READ`/`WRITE`/`LIST` leaf/dir opens (one
  race-free syscall; the returned fd IS the object). `WRITE` first mkdir-p's the
  parents (each `mkdirat` relative to a held, contained fd); `STAT` uses the
  contained parent fd + `fstatat(...AT_SYMLINK_NOFOLLOW)`. `RESOLVE_NO_MAGICLINKS`
  blocks `/proc` magic-symlink escapes.
- **Platforms without `openat2` (macOS, older Linux, cosmo where unavailable):** a
  manual walk reproducing `RESOLVE_IN_ROOT` EXACTLY. Hold a STACK of directory fds
  rooted at `root_dfd`; each component `openat(top, comp, O_NOFOLLOW | O_DIRECTORY
  | O_CLOEXEC)`; when a component is a symlink (`ELOOP` / `fstatat` probe),
  `readlinkat` and SPLICE its target into the walk (ABSOLUTE target restarts at
  `root_dfd` = re-root; RELATIVE continues from the current dir); a `..` component
  POPS the fd stack, CLAMPED at `root_dfd` (never `openat(dfd, "..")`). Bound total
  symlink expansions (e.g. 40, the kernel `ELOOP` limit) -> `symlink_loop`. Every
  step is relative to a HELD fd, so containment is structural and there is no
  TOCTOU window. The leaf is just the terminal component, handled per the mode
  table above.
- **Fail closed** where a platform offers neither primitive - never downgrade to
  `realpath`.

This is the userspace equivalent of what container runtimes settled on
(`RESOLVE_IN_ROOT` / Go's `securejoin`): a hard virtual root, symlinks followed
inside it.

**Confinement and authorization are UNIFIED by the root choice.** The resolver
guarantees every op stays under `root_dfd`. By setting `root_dfd` to the
applicable AUTHORIZED root (§6), that single guarantee delivers BOTH root
confinement (the authorized root is always within `base_dir`) AND path
authorization (the resolved object cannot leave the authorized root) - race-free,
with no separate resolved-path re-check. The outer `base_dir` remains the ceiling
for the no-granular-roots case.

**Platform notes** (resolve at implementation): `openat2` is Linux-only (>= 5.6);
confirm whether the Cosmopolitan target exposes it (raw syscall) or must use the
manual walk on its Linux host. `openat` / `O_NOFOLLOW` / `readlinkat` / `fstatat` /
`mkdirat` are POSIX (Linux, macOS, cosmo shim). A native Windows target (not
today's cosmo APE) needs the reparse-point-aware equivalent; scope that when such a
target exists.

## 4. Operations

### 4.1 Existing, hardened (no signature change)
- `fs.read(path)` -> bytes | (nil,err) / throw. Now resolved per §3; `fs.read`
  authority.
- `fs.write(path, bytes)` -> true | (nil,err) / throw. Resolved per §3 (`WRITE`
  mode); `fs.write` authority. **Preserves today's implicit parent-directory
  creation** (the current `hl_cap_fs_write` mkdir-p's parents) - now done
  descriptor-relatively with `mkdirat` under the contained walk instead of
  `mkdir()` on reconstructed absolute paths. Behavior unchanged; only the
  mechanism hardens. Whether writes should become ATOMIC (they are NOT today -
  see §1.4) is a SEPARATE API decision, out of this prerequisite scope.
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
| explicit `mkdir` | `write` ALREADY creates parents implicitly (§1.4b, §4.1) and that is preserved; a STANDALONE `mkdir(path)` op (create an empty dir with no write) is rarely needed and deferred until a concrete case appears. |
| `copy` / `rename` / `move` | `rename` is the ATOMIC-PUBLISH primitive - it belongs to the Hull-owned BuildContext.outputs (audit §3.5), NOT general app `hull.fs`, precisely so staging/publication authority stays separate. |
| ATOMIC `write` / `tempfile` | Today's `write` is non-atomic truncate-in-place (§1.4c); making it atomic (tmp + `rename`) is a SEPARATE API decision. The transactional staging + atomic PUBLICATION machinery specifically is a BuildContext.outputs concern by the audit's HARD boundary; exposing it on app `hull.fs` would blur the two authorities. |

The through-line: `hull.fs` gets its EXISTING contracts hardened
(`read`/`write`-with-implicit-parents/`mmap`) + discovery (`stat`/`list`); the
transactional staging + atomic-publication machinery stays in BuildContext.

## 5. DECIDED: in-sandbox symlinks are FOLLOWED (contained), not refused

**Decision (owner: user, during review): the app surface MUST follow symlinks
that resolve within `base_dir`.** Real apps rely on in-sandbox symlinks -
atomic-deploy `current -> releases/N`, asset trees, config indirection - so
refusing them would break legitimate layouts. The prior draft's deny-all
recommendation is REJECTED. The chosen path follows in-sandbox symlinks while
fixing the TOCTOU (via §3, not `realpath`) - which preserves RELATIVE-symlink
behavior exactly but INTENTIONALLY changes ABSOLUTE-symlink behavior (see the
compatibility note below).

Semantics = the **true virtual-root** model of §3 (chosen deliberately over a
reject-escapes / `RESOLVE_BENEATH` model, which would conflict with re-rooting
absolute targets and could not use the one-call `RESOLVE_IN_ROOT` implementation):
- A symlink whose target resolves within `base_dir` is FOLLOWED transparently for
  `read` / `write` / `mmap`.
- An ABSOLUTE symlink target is RE-ROOTED at `base_dir` (`foo -> /bar` ->
  `base_dir/bar`); excess `..` in a target is CLAMPED at `base_dir`.
- Re-rooting and clamping are **not errors** - a virtual root makes escape
  impossible by construction, so there is **no `outside_root` outcome** for a
  symlink target or `..` met during resolution. (This is the correction to the
  earlier draft, which wrongly promised both re-rooting AND `outside_root`:
  `RESOLVE_IN_ROOT` clamps rather than rejects, and the manual fallback clamps
  identically, so the contract is virtual-root, single and consistent.)
- The only rejection is the LEXICAL caller-path pre-check (absolute path or `..`
  in the app-supplied path -> `invalid_path`), an input contract, NOT a resolution
  outcome.
- `stat` / `list` report a symlink's OWN type WITHOUT following (`lstat`
  semantics), so an app can enumerate links as links; only path RESOLUTION for
  `read`/`write`/`mmap` follows.
Still NOT a lexical authorization - `hull.path` never authorizes; the `openat2` /
descriptor walk is the confinement authority.

**COMPATIBILITY CHANGE (intentional, not "unchanged behavior").** Virtual-root is
behavior-preserving for regular paths and RELATIVE symlinks, but it CHANGES what an
ABSOLUTE symlink target means. With `base_dir = /app`:

| symlink target | today (`realpath`) | virtual-root (new) |
|---|---|---|
| `link -> releases/1` (relative) | `/app/releases/1` | `/app/releases/1` (same) |
| `link -> /app/releases/1` (absolute, in base) | `/app/releases/1` (resolves) | `/app/app/releases/1` (re-rooted -> usually `not_found`) |
| `link -> /outside/x` (absolute, out of base) | REJECTED | `/app/outside/x` (re-rooted, no longer an error) |

So an absolute in-sandbox symlink that happened to include the `base_dir` prefix
BREAKS, and an absolute symlink pointing outside is silently redirected inside
rather than refused. This is an accepted consequence of the virtual-root decision,
documented here rather than hidden.

**Migration note.** In-root symlinks should use **sandbox-root-relative** targets
(`/releases/1`, which under the virtual root means `base_dir/releases/1`) or
**relative** targets (`releases/1`), NOT host-absolute targets that embed
`base_dir` (`/app/releases/1`). Checkpoint 1 includes an audit for absolute
symlink targets inside app trees + this migration guidance in the release notes.
Affected surface is narrow: only symlinks whose STORED target string is absolute.

## 5a. DECIDED: a READ/WRITE/MMAP leaf must be a REGULAR file

`fs.read`, `fs.write`, and `fs.mmap` resolve to a terminal FILE leaf. The resolver
opens that leaf **non-blocking** (`O_NONBLOCK`) and then **type-gates** it: only a
regular file is accepted. A FIFO, socket, character/block device, or directory is
rejected with the single stable token **`not_a_regular_file`**; the `O_NONBLOCK` is
cleared on the accepted regular fd so ordinary blocking read/write semantics are
preserved. (`HL_FS_OPEN_DIR`, used by `list` and to resolve a `SUBTREE` grant
anchor, keeps its own `S_ISDIR` contract and is unaffected - a directory never
blocks on open.)

**Why.** A special-file leaf is both a correctness and an availability hazard.
`open(O_RDONLY)` on a FIFO with no writer, or `open(O_WRONLY)` on a FIFO with no
reader, **blocks indefinitely** - on Hull that would hang the event-loop thread (a
self-DoS), and for a build plugin's declared inputs a stray `mkfifo` in a workspace
would hang the whole pipeline. Character devices can carry open-time side effects,
and reading a directory as a file is meaningless. The interior components of a path
and the `DIR`-mode terminal were already classify-before-open protected against the
FIFO hang; this extends the same guarantee to the READ/WRITE **leaf**, which was
the one remaining path that could block. The two implementations (Linux `openat2`
fast path and the portable manual walk) apply identical behavior.

**Intentional tightening (migration note).** An authorized special-file leaf that
was previously readable/writable is now rejected. In practice `fs.read`/`fs.write`
targets are regular files; an app that legitimately needs to read a FIFO/device is
out of scope for the sandboxed filesystem capability. The token surfaces to app
code (Lua `nil, "not_a_regular_file"` / JS throw) exactly like the other stable
resolver tokens. Enforcement lives in `src/hull/cap/fs_resolve.c`
(`finalize_regular_leaf` + `leaf_type_errno`); covered by the special-file suite in
`tests/hull/cap/test_fs_resolve.c` (FIFO/socket/device/directory under READ and
WRITE, each under a no-hang watchdog, on both implementations, with fd-leak guards)
and the concurrent regular<->special leaf-swap case in
`tests/hull/cap/test_fs_resolve_parity.c`.

## 6. Two authorities: root confinement vs path authorization

The audit and the earlier draft conflated these; they are separate and this
design keeps them separate (goal §2.7).

**(a) Root confinement** - every op stays under `base_dir`. Provided entirely by
the §3 descriptor resolver (virtual-root), for every op including `stat`/`list`.
No manifest input.

**(b) Operation / path authorization** - WHICH paths an app may read vs write.
Today this is materialized by the KERNEL SANDBOX (unveil / seatbelt) from the
manifest `fs.read` / `fs.write` roots; `HlFsConfig` / `hl_cap_fs_validate` do NOT
carry or check per-path lists (§1.3).

**The rule (chosen): authority follows the RESOLVED path, enforced by
CONFINEMENT** - but a manifest grant is NOT necessarily an existing directory, so
"each authorized root is a dirfd" is too narrow. Hull grants exact files
(`fs.read = {"data.bin"}`, `{"huge.bin"}`) and write targets that do not exist yet
(`fs.write` may name `out/result.bin` with `out/` absent, which `write` must
create). Each grant is therefore COMPILED at config time into an
**authorization entry** = a HELD anchor dir fd (always an EXISTING directory) plus
a CONSTRAINT. Three entry kinds:

| grant | compiled entry | permits |
|---|---|---|
| **existing directory** (`data/`) | `anchor_fd = open(data, O_DIRECTORY\|O_NOFOLLOW)`; constraint = `SUBTREE` | virtual-root resolution rooted at `anchor_fd`; any descendant. Symlinks clamp WITHIN the subtree (§3/§5), so they cannot leave it. |
| **exact file** (`data.bin`) | `anchor_fd = open(dirname, O_DIRECTORY)`; constraint = `EXACT("data.bin")` | ONLY `openat(anchor_fd, "data.bin", ...)` - the one leaf. Siblings are unreachable (no subtree traversal is granted; the exact leaf name is a literal). |
| **not-yet-existing target** (`out/result.bin`, `out/` absent) | anchor at the NEAREST EXISTING ANCESTOR (`open(".", O_DIRECTORY)`) + constraint = `CREATE(["out","result.bin"], kind)` where kind is exact-file or subtree | `mkdirat` ONLY the constrained intermediate components (`out`) relative to `anchor_fd`, then create the terminal (`result.bin`, `O_TRUNC`). `out/other.bin` is denied (terminal is exactly `result.bin`). |

**Selection is deterministic and component-aware.** `fs.read` and `fs.write`
compile to two INDEPENDENT entry sets (a readable path need not be writable, and
vice versa) - a `read`/`mmap`/`stat`/`list` op selects from the READ set, a
`write` op from the WRITE set. When a caller path matches several entries, the
MOST SPECIFIC (deepest component-prefix) wins - grants `data/` and `data/private/`
select `data/private/` for a path beneath it. Matching is component-aware
(`data` never matches `database`). A caller path matching NO entry in the relevant
set -> `permission`. Because the terminal syscall is issued relative to the held
anchor fd (§3 mode table), selection + confinement are race-free with no separate
resolved-path re-check.

**Symlink rule per entry kind (definitive):**
- **`SUBTREE`**: symlinks are FOLLOWED with virtual-root confinement (§3/§5) - they
  clamp within the granted subtree and cannot escape it.
- **`EXACT` and `CREATE`**: an intermediate OR terminal symlink is **REFUSED**
  (`symlink_denied`), never followed. There is no granted subtree to clamp within,
  and following the leaf would require re-selecting another grant AFTER the leaf is
  opened - which the one-call `openat2` cannot do (it resolves the leaf inside the
  exact entry's parent first), and which a manual `readlinkat` + reselect cannot do
  race-free (component-replacement race) without a stable symlink handle the
  platform does not provide. So an exact/create grant follows NO symlink at all.
  This keeps authorization race-free with the documented resolver (no cross-policy
  symlink-resolution protocol).
- An INDEPENDENTLY authorized target stays reachable through ITS OWN granted path,
  NOT through an exact-file alias: `link.bin -> secret.bin` under an exact grant for
  `link.bin` is DENIED (the symlink is refused) even if `secret.bin` is separately
  granted - read `secret.bin` via its own grant instead. An exact-file grant never
  becomes sibling-directory authority. (Following exact-file aliases could be a
  later, explicitly-designed extension if a real need appears; it is NOT v1.)

**Acceptance cases (must pass, checkpoint 2):**
```
read grant  data.bin:            data.bin -> allowed;  sibling.bin -> denied
write grant out/result.bin (out/ absent):
                                 out/ created;  result.bin created+truncated;
                                 out/other.bin -> denied
grants      data/ and data/private/:   most-specific (data/private/) selected
exact grant link.bin -> secret.bin:    DENIED through link.bin (symlink refused under EXACT),
                                       even if secret.bin is separately granted; reach
                                       secret.bin only via its own grant
```

This is the user-recommended option (authority-follows-resolved-path) generalized
to Hull's real manifest contract. Consequences:
- **`stat` / `list` respect the READ set, not merely `base_dir`** - they select +
  resolve within a read entry like `read` does, so metadata / directory entries
  inside `base_dir` but outside the granted read paths are never exposed.
- The explicit policy object is the two SETS OF COMPILED ENTRIES (anchor fd +
  constraint), held on an extended `HlFsConfig` or a separate `HlFsPolicy`
  (implementation detail, checkpoint 2) - NOT a list of path strings re-checked per
  call, and NOT an assumption that grants are directories.
- The KERNEL SANDBOX remains defense-in-depth BENEATH this, but is no longer the
  sole authorization gate for the new surface.
- **Anchoring edge cases** (checkpoint 2): a `CREATE` anchor whose nearest existing
  ancestor is removed/replaced between config and op fails closed; an intermediate
  component that exists but is a non-directory is refused; any symlink component
  under a `CREATE`/`EXACT` entry is refused (`symlink_denied`), never traversed -
  consistent with the per-kind symlink rule above.

**Error model (proposed, stable tokens).** Uniform `(nil, err)` (Lua) / `throw`
(JS): `"invalid_path"` (caller path absolute or containing `..` - lexical
pre-check), `"not_found"`, `"permission"` (path-authorization policy or file
mode), `"not_a_directory"`, `"is_a_directory"`, `"symlink_loop"` (expansion bound
exceeded), `"symlink_denied"` (a symlink under an `EXACT`/`CREATE` entry, which
never follows symlinks - §6), `"too_large"`, `"io_error"`. **No `outside_root`
token** - resolution cannot escape (§5, virtual-root); an escape is impossible, not
an error. (Exact
tokens confirmed at implementation; today's messages are not a stable contract.)

## 7. Lua/JS parity

Every application op has both bindings, mirrored semantics, snake_case (Lua) /
camelCase (JS) only. `read`/`write`/`mmap` already parity; `stat`/`list` land in
both at once. A parity E2E (the `tests/e2e_path_parity.sh` model, but over a real
fixture tree because fs needs files) asserts Lua == JS for: `list` ordering +
per-entry metadata, `stat` fields + symlink typing (link reported as link), an
in-base symlink FOLLOWED identically for `read`, an absolute symlink target
RE-ROOTED at `base_dir` the same way, a `foo -> ../../../x` target CLAMPED to
`base_dir/x` the same way (NOT an error, per §5), `invalid_path` on a caller path
with `..`, a `symlink_loop` cycle bounded identically, a WRITE that OVERWRITES
longer content with shorter (`abcdef` -> `xy` yields `xy`, proving `O_TRUNC`,
correction 1), and the authorization case `allowed/link -> ../secret` resolving to
`allowed/secret` / `not_found` rather than reaching an unauthorized `secret`
(correction 3), and the §6 authorization-entry acceptance cases (exact-file grant
allows the file but denies a sibling; a `CREATE` write grant creates `out/` +
truncates `result.bin` but denies `out/other.bin`; overlapping `data/` +
`data/private/` select most-specific; an exact grant `link.bin -> secret.bin` is
DENIED through `link.bin` with `symlink_denied` even when `secret.bin` is separately
granted - `secret.bin` reachable only via its own grant). A dedicated resolver unit
test proves virtual-root parity between
the `openat2` and manual-walk paths on the same fixture tree (Linux runs BOTH to
catch drift), and covers a swapped-component TOCTOU by construction (held-fd walk)
with a deterministic case where testable.

## 8. Relationship to BuildContext

Same descriptor-relative resolver + `stat`/`list` primitives underpin both
surfaces; this design PROVIDES them.

| surface | who | authority | ops |
|---|---|---|---|
| **application `hull.fs`** | app code | root confinement (`base_dir`, resolver) + path-authorization policy (`fs.read`/`fs.write` roots, §6) + kernel sandbox | read / write / mmap / stat / list |
| **plugin `BuildContext`** | Hull-owned, handed to a plugin | declared input roots + a private staging root; NARROWER | inputs: read / stat / list (recorded + hashed); outputs: staged write + atomic publish |

BuildContext inputs reuse this design's resolver + `stat`/`list`, add observation
recording + content hashing, and drop general write authority. BuildContext
outputs use private action staging and host-owned commit; neither staging nor
publication is exposed by application `hull.fs` (§4.3). Artifact inputs and
constrained tools are defined by the active BuildContext design. A build plugin
never receives the general `hull.fs`.

## 9. DECIDED: resolver-first (migrate the existing surface before building plugins)

**Decision (owner: user, during review): resolver-FIRST.** The audit (§6) had the
app-`hull.fs` migration sequenced AFTER BuildContext; that is REVERSED. Rationale:
it fixes the EXISTING security defect (the `realpath` TOCTOU, §1.4a) before any
plugin work, and it validates the foundational resolver through the primary,
most-used surface rather than proving it first on a new, less-exercised one.
`RESOLVE_BENEATH` / reject-escape is also rejected in favor of virtual-root (§5).

Ratified ordering:
1. **Land the resolver and migrate existing `read`/`write`/`mmap` first** - onto
   the §3 virtual-root resolver, anchored at `base_dir` (the no-granular-grants
   case), so today's authorization model (base_dir confinement + kernel sandbox) is
   PRESERVED. Write keeps implicit parent creation (now `mkdirat`-based) and
   `O_TRUNC`. No new app ops, no compiled-entry policy yet; this is the TOCTOU fix +
   migration, behavior-preserving EXCEPT the intentional absolute-symlink
   re-rooting (§5), which ships with the migration note + an absolute-symlink audit.
2. **STOP and prove** platform parity (`openat2` path == manual walk) AND
   race-resistant symlink behavior (virtual-root follow, re-root, clamp, loop
   bound) on the fixture tree + resolver unit tests.
3. **Add the §6 compiled-entry authorization policy + application `stat` / `list`.**
   The policy (SUBTREE/EXACT/CREATE entries, independent read/write sets,
   most-specific selection, exact-file symlink rule) lands HERE, because `stat`/
   `list` are the new metadata-leak surface that requires it; `read`/`write`/`mmap`
   adopt the same per-grant anchoring at this point. Lua/JS parity; the §6
   acceptance cases pass - STOP.
4. **Then `BuildContext.inputs` and `BuildContext.outputs`** on the proven
   primitive. Then Build Plugin / BuildArtifact. **Not Query IR yet.**

## 10. Non-scope + checkpoints

Design only. No code, no binding changes, no resolver, no `stat`/`list`, no
BuildContext, no plugin loader, no Query IR.

Checkpoints mirror the §9 ratified ordering; every arrow is a STOP-for-review:

0. **This design + the BuildContext audit (#393)** - STOP for review. Both
   decisions are now settled: symlink policy = virtual-root follow (§5),
   sequencing = resolver-first (§9). Nothing else is open.
1. Land the resolver + migrate `read`/`write`/`mmap` (TOCTOU fix; `O_TRUNC` +
   implicit parents preserved; the one intentional change = absolute-symlink
   re-rooting, §5, shipped with a migration note) - STOP.
2. Prove platform parity + race-resistant symlink behavior - STOP.
3. Add the §6 compiled-entry authorization policy (SUBTREE/EXACT/CREATE,
   independent read/write sets, most-specific selection, exact-file symlink rule)
   + `stat`/`list`, Lua/JS parity, §6 acceptance cases pass - STOP.
4. `BuildContext.inputs` then `BuildContext.outputs` - STOP. Then Build Plugin /
   BuildArtifact. **Not Query IR yet.**
