# hull.fs resolver parity + race-resistance (checkpoint 2)

Checkpoint 2 of the hull.fs design ([hull_fs_design.md](hull_fs_design.md) §9/§10):
**prove that the two resolver implementations honor the same virtual-root
contract, and that resolution is race-resistant.** It is a proof/test checkpoint -
no new resolver features, no `stat`/`list`, no compiled authorization, no
BuildContext.

## The two implementations

`src/hull/cap/fs_resolve.c` resolves a caller path under a base-directory fd with
virtual-root (RESOLVE_IN_ROOT) semantics via one of two implementations:

- **openat2 fast path** (Linux ≥ 5.6, READ only): one
  `openat2(RESOLVE_IN_ROOT | RESOLVE_NO_MAGICLINKS)` syscall.
- **manual walk** (macOS, cosmo, older Linux, and Linux WRITE): a held-fd-stack
  walk that reproduces RESOLVE_IN_ROOT (per-component `openat` `O_NOFOLLOW`,
  symlink splice with absolute-reroot / `..`-clamp, a 40-expansion loop bound).

Both are race-free (every step binds a held fd / a single kernel-confined call);
neither uses `realpath`.

## How parity is proven

`tests/hull/cap/test_fs_resolve_parity.c` runs each case through **both**
implementations on one host: the `HL_FS_FORCE_MANUAL` env forces the manual walk,
so on Linux a case is resolved once via openat2 and once via the manual walk and
their outcomes are compared directly (success + the stable error token). On
macOS/cosmo both passes are the manual walk (a self-consistency check).

- **`read_battery`** — a fixture tree exercised for: a plain file, a nested file,
  a relative symlink, an absolute symlink (re-rooted), a `..`-escaping symlink
  (clamped), a missing path (`not_found`), a self-loop (`symlink_loop`), a
  trailing-slash path (`invalid_path`), and a caller `..` (`invalid_path`). Each
  asserts the two implementations return the **identical** success content or the
  **identical** error token.
- **`depth_boundary`** — `HL_FS_MAX_DEPTH` components is accepted (not "too deep")
  and `+1` is rejected with `path_too_deep`, identically on both (the caller-path
  component count is checked before either implementation).
- **`component_swap_race_stays_contained`** — see "Race resistance" below.

## Ratified divergence: symlink-expanded depth (a platform resource limit)

There is exactly **one** intentional, documented behavioral asymmetry, and it is
asserted explicitly by `ratified_symlink_expanded_depth` (Linux):

- A **caller path** longer than `HL_FS_MAX_DEPTH` components is rejected
  identically by both (the pre-check runs before either implementation).
- But a **symlink whose target expands the resolved path past
  `HL_FS_MAX_DEPTH`** diverges: the manual walk fails closed with `path_too_deep`
  because its held-fd stack is a finite resource (one open descriptor per level,
  kept well under `RLIMIT_NOFILE`); openat2 has no component-count limit and
  resolves the same path within the kernel's own `PATH_MAX` / `ELOOP` bounds, so
  it may succeed.

**Ratification.** This is **not** claimed as exact behavioral parity. It is a
platform-resource-limit asymmetry: the manual walk's fd-stack depth is a bounded
resource that openat2 (a single kernel call) does not consume. Both implementations
remain **fully contained** — neither can escape `base_dir`; the only difference is
that the manual walk rejects a pathological deep-symlink-expansion that openat2
accepts. The manual walk is therefore the **stricter, fail-closed** side, which is
the safe direction. Reaching this case requires a symlink whose single target has
256+ path components (a construction no normal layout produces).

Two ways this could be "eliminated" instead, both rejected for checkpoint 2:
- Post-resolution depth-check openat2 via `/proc/self/fd` readlink + component
  count — adds a per-op `/proc` dependency + readlink cost for a pathological case,
  and is Linux-container-fragile.
- Drop openat2 and use only the manual walk — removes the kernel-enforced
  containment defense-in-depth and the one-call fast path the approved design
  chose.

If a future need makes exact parity here mandatory, the `/proc` post-check is the
tracked path; until then this asymmetry is ratified as above.

## Race resistance

`component_swap_race_stays_contained` spawns a thread that continuously flips an
interior path component (`a/mid`) between a real directory and a **symlink to an
external sentinel directory** (outside `base_dir`, containing a file `f` with
contents `SECRET-SENTINEL`), while the main thread resolves `a/mid/f` up to 8000
iterations through **both** implementations. The invariants, asserted with enough
context to reproduce a failure (the swapper seed, the first failing iteration, and
which implementation):

- **no iteration ever reads the external sentinel** — a successful resolve returns
  **only** the in-base file's content (`inbase`); reading `SECRET-SENTINEL` would
  mean containment failed and the escaping symlink was followed as a raw host path;
- a failed resolve returns **only** a known *contained* token (`not_found` /
  `not_a_directory` / `symlink_loop` / `permission` / `io_error` / `path_too_deep`
  / `invalid_path`) — transient failures during a swap are distinguished from any
  unexpected error;
- the loop is **bounded** (8000 iterations and a 60 s wall-clock cap);
- **file-descriptor count is bounded** — the process's open-fd count is sampled
  before and after and must not grow, proving the resolver leaks no descriptors
  across the run.

This holds structurally: each component is opened `O_NOFOLLOW` relative to a held
fd, so a component swapped **after** it is opened binds the already-held inode, and
a component swapped **to a symlink before** it is opened is caught by `O_NOFOLLOW`
and re-rooted/clamped by the virtual-root splice — never followed as a raw host
path. openat2 gets the same guarantee from `RESOLVE_IN_ROOT` in-kernel. The test
makes that structural property observable under real concurrent mutation.

**Sanitizers.** The harness runs under ASan + MSan via the normal test discovery,
and is added to the TSan suite (`TSAN_TESTS`) so the thread race itself is checked:
the only shared memory is an `atomic_int` stop flag (not `volatile`), and the
concurrency is filesystem-level (mkdir/symlink/unlink vs `openat` resolution),
which TSan tolerates.

**Exercised, not dormant.** The ratified divergence probes `openat2` availability
(`__NR_openat2`); where openat2 is present (Linux CI) the test REQUIRES the
divergence to actually occur (openat2 succeeds while the manual walk rejects) rather
than passing vacuously — so a regression that silently stops using openat2 fails the
assertion instead of hiding.

## Scope

Checkpoint 2 is tests + this ratification record only. `stat`/`list`, the §6
compiled-entry authorization policy, and BuildContext remain out of scope (later
checkpoints). The realpath-based `hl_cap_fs_exists` / `hl_cap_fs_delete` / direct
`hl_cap_fs_validate` consumers likewise remain a tracked follow-up (recorded in
`src/hull/cap/fs.c`); the cap layer is not yet fully TOCTOU-free.
