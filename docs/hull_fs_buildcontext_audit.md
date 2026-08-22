# hull.fs + BuildContext filesystem audit / design

Status: **AUDIT + DESIGN (awaiting review). NOTHING implemented.** This is the
design/audit checkpoint that must precede any `hull.fs` change and the Build
Plugin / BuildArtifact work. Per the review scope it FIRST inventories the current
APIs and the concrete BuildArtifact needs, THEN proposes the minimal additions.
Design only - stop after this checkpoint.

Prerequisite already shipped: **`hull.path`** (pure lexical names, no authority;
#392). The rule this design preserves: **`hull.path` manipulates NAMES; `hull.fs`
exercises AUTHORITY.** BuildContext is a THIRD, narrower authority surface - not a
re-use of the application `hull.fs`.

## 1. Inventory - current `hull.fs`

### 1.1 Capability layer (`include/hull/cap/fs.h`, `src/hull/cap/fs.c`)

| function | purpose |
|---|---|
| `hl_cap_fs_validate` | reject `..`, absolute paths, and paths outside `base_dir` |
| `hl_cap_fs_read` | read a file's bytes |
| `hl_cap_fs_write` | write bytes to a file |
| `hl_cap_fs_exists` | existence probe |
| `hl_cap_fs_delete` | unlink |
| `hl_cap_fs_mmap` / `_mmap_window` / `_munmap` / `_borrow` / `_release` | zero-copy read-only file windows (the compute mapped-span path) |

`HlFsConfig` carries `base_dir` + the manifest `fs.read` / `fs.write` allowlists.

### 1.2 Application surface (Lua `hull.fs` / JS `hull:fs`)

Deliberately SMALL: `read`, `write`, `exists`, `delete`, `mmap` (+ `close`/`len`
on the mapped buffer). **There is NO enumeration (list/readdir), NO stat/metadata,
NO staging/rename, NO dependency-hash primitive.**

### 1.3 Authority model

Two independent gates (as elsewhere in Hull): a build-time module gate
(`hull/fs@1`) and a per-call capability gate. `hl_cap_fs_validate` enforces
`manifest.fs.read` / `manifest.fs.write` allowlists + containment under `base_dir`.

### 1.4 The load-bearing finding: current resolution is TOCTOU-susceptible

`hl_cap_fs_validate` resolves with **`realpath()`** and then a later call opens the
path (`realpath -> check -> open`). Between the `realpath` check and the `open`, a
path component can be swapped (e.g. a directory replaced by a symlink), so the
check does not bind the actual `open` target. A comment already claims "use
resolved base_dir to avoid TOCTOU," but resolve-then-open remains a genuine TOCTOU
window. This is exactly the pattern to NOT carry into the build-plugin surface.

Precedent for the fix already exists in-tree: `src/hull/shared/blob_store.c` opens
with `O_DIRECTORY | O_CLOEXEC` and `O_NOFOLLOW` - Hull can build on that.

## 2. Concrete BuildArtifact needs (drives every proposal below)

A build plugin, given a workspace, must be able to:
1. **read its DECLARED inputs** (and only those) - not arbitrary workspace files;
2. **enumerate** input directories DETERMINISTICALLY (stable order) with per-entry
   **metadata** (type, size, mtime policy) to discover sources;
3. **hash dependencies from the EXACT bytes** it read (+ a defined metadata
   policy) so a build is reproducible and cache-keyable;
4. **stage outputs transactionally** and **publish atomically** (all-or-nothing;
   a crashed build leaves no half-written artifact);
5. do all of the above **race-resistantly** (a workspace may be mutated
   concurrently) and with **explicit, safe symlink behavior**.

None of (2)-(4) exists today; (1) and (5) exist only via the TOCTOU-susceptible
`base_dir` path check.

## 3. Design proposals (minimal additions)

### 3.1 Descriptor-relative, race-resistant resolution (NOT realpath->check->open)

Introduce a resolver that, from a **base directory FILE DESCRIPTOR** (`dirfd`),
walks the path ONE COMPONENT at a time with `openat(dfd, comp, O_NOFOLLOW |
O_DIRECTORY | O_CLOEXEC)` for interior components and `openat(dfd, leaf, O_RDONLY |
O_NOFOLLOW | O_CLOEXEC)` (or `O_WRONLY|O_CREAT` for writes) for the leaf. Because
each step is relative to a fd that was itself opened `O_NOFOLLOW`, containment is
enforced against the OBJECT actually opened - there is no resolve-then-open
window, and a swapped component is refused, not silently followed. Lexical
pre-validation uses `hull.path` (reject absolute, reject `..` for the plugin
surface) BEFORE the descriptor walk, but the descriptor walk is the AUTHORITY.

Platform notes to resolve at implementation: `openat`/`O_NOFOLLOW`/`O_DIRECTORY`
are POSIX and available on Linux/macOS and via Cosmopolitan's shim; `O_NOFOLLOW`
semantics differ subtly on macOS (fails only on a trailing symlink - hence the
component-wise walk, which makes EVERY component a trailing check). A Windows
native build (not today's cosmo APE) would need the reparse-point-aware
equivalent; scope that when a native Windows target exists. Where a platform
cannot offer a race-resistant primitive, the operation must **fail closed**, not
downgrade to `realpath`.

### 3.2 Deterministic enumeration + metadata

A `list(dir)` that returns entries in a **defined, stable order** (byte-wise sort
of names) with per-entry `{ name, type, size }` and a **metadata policy** for
mtime (see 3.5 - hashing should not depend on mtime by default). Enumeration is
descriptor-relative (`fdopendir` on a `dirfd` opened per 3.1). No recursion in the
primitive; a plugin composes recursion with `hull.path`.

### 3.3 Declared-input reads (the BuildContext.inputs view)

Build plugins receive a **BuildContext.inputs** view, NOT the general `hull.fs`.
It exposes only `read` / `stat` / `list` over a set of **declared** input roots
(each opened as a `dirfd` per 3.1), and **records every read** (path + content
hash) for dependency tracking (3.4). A read outside a declared input is refused.
This is a strictly NARROWER authority than application `hull.fs`.

### 3.4 Dependency hashing from exact bytes + metadata policy

Every input read through BuildContext.inputs is hashed from the **exact bytes
returned to the plugin** (reuse `hl_cap_crypto_sha256`), accumulating a
`{ path -> sha256 }` dependency set. The default hash covers CONTENT only; the
**metadata policy** (whether mode/size/mtime participate) is explicit and
defaults to content-addressed (mtime excluded, for reproducibility). The
dependency set becomes the build's cache key input.

### 3.5 Transactional staging + atomic publication (BuildContext.outputs - Hull-owned)

Outputs are written to a **staging** area (a private temp dir under the workspace,
`O_DIRECTORY` dirfd) via `BuildContext.outputs`, then **published atomically** by a
single `rename(2)` (same filesystem, atomic) of the staged tree/file into place -
or discarded wholesale on failure, so a crashed build leaves no partial artifact.
`fsync` the file + the directory before the publish rename for durability.

**HARD BOUNDARY (explicit):** staging and publication primitives are **NEVER**
exposed through ordinary application `hull.fs`. They belong ONLY to the
Hull-OWNED `BuildContext` handed to a plugin. An application cannot obtain a
staging/publish handle; a plugin cannot obtain the general `hull.fs`. The two
authorities are distinct surfaces, not one surface with flags.

### 3.6 Explicit symlink behavior

Stated, not implicit: within a plugin BuildContext, symlink components are
**refused** during resolution (`O_NOFOLLOW` at every step, 3.1); a symlink is
never transparently followed out of a declared root. The application `hull.fs`
retains its current documented behavior (manifest-authorized, `base_dir`-contained)
until separately migrated to the descriptor-relative resolver - a migration
tracked but out of THIS checkpoint.

## 4. The two authority surfaces (kept separate)

| surface | who | authority | operations |
|---|---|---|---|
| **application `hull.fs`** | app code | manifest `fs.read`/`fs.write` allowlists, `base_dir` | read/write/exists/delete/mmap (today) + (later) descriptor-relative resolution + enumeration where the app API exposes it |
| **plugin `BuildContext`** | Hull-owned, handed to a build plugin | declared input roots + a private staging root; NARROWER | inputs: read/stat/list (recorded); outputs: staged write + atomic publish |

Build plugins do NOT receive `hull.fs` "because it exists." They receive a
BuildContext with strictly the authority a build needs.

## 5. Lua/JS parity - only where the application API exposes the operation

Parity (Lua == JS, like `hull.path`) applies ONLY to operations exposed on the
APPLICATION `hull.fs` surface. BuildContext is Hull-owned orchestration handed to
plugins; its inputs/outputs views are provided by Hull to the plugin runtime and
do not need a symmetric hand-written Lua/JS user API surface beyond what a plugin
actually calls. So: add descriptor-relative resolution + (if exposed) enumeration
with Lua/JS parity on `hull.fs`; provide BuildContext to plugins without duplicating
a second public user-facing fs module.

## 6. Non-scope (this checkpoint)

Design only. No implementation. No change to application `hull.fs` behavior yet, no
BuildContext code, no plugin loader, no dependency-hash wiring, no Query IR. The
application-`hull.fs` migration to the descriptor-relative resolver is a tracked
follow-up, sequenced AFTER BuildContext proves the primitive.

## 7. Checkpoints

1. **Audit + design** (this doc) - STOP for review.
2. Implement the descriptor-relative resolver + enumeration/metadata primitive
   (application `hull.fs`), with parity tests - STOP.
3. Implement `BuildContext.inputs` (declared reads + dependency hashing) + STOP.
4. Implement `BuildContext.outputs` (transactional staging + atomic publish) +
   STOP. Then Build Plugin / BuildArtifact. **Not Query IR yet.**
