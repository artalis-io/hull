# libhull: the no-runtime embedding flavor

`libhull` packages Hull's hardened core as a static archive
(`build/libhull.a`) that a native program (C / Rust / Zig) links directly.
There is no Lua or QuickJS runtime and no `app.main` lifecycle: the host
owns `main()` and drives the two-phase kernel sandbox and the capability
layer itself, through the stable ABI in
[`include/hull/embed.h`](../include/hull/embed.h).

It generalizes Hull's hardening beyond the script-app model - Hull as an
SDK, not only Hull as a runtime. This document covers the **trust
boundary** and the **seal lifecycle**, which are the security-relevant
parts an embedder must understand. For the build/flavor context see
[build_flavors.md §8](build_flavors.md); for the sealed-memory mechanism
see [security.md §4b](security.md).

Status: L-1 (archive + sandbox split), L-2 (`hl_embed_*` ABI), and L-3
(policy sealing + fail-closed ordering + death test), L-4 (release signing
+ SBOM for the archive, incl. the dual-arch cosmo build), and L-5 (Rust +
Zig reference embedders) have landed. The epic is complete.

## When to reach for libhull (and when not)

libhull is for an **existing native codebase** that wants one of Hull's
guarantees without adopting a scripting runtime. The sweet spot is a native
program that must (a) run untrusted compute, or (b) enforce a capability
boundary on its own I/O - and stay native.

Genuinely useful:

- **Untrusted compute in a native service.** A daemon that runs
  user-supplied transforms / scoring / dedup / SQL UDFs gets WAMR's
  gas-metered, no-I/O linear-memory isolation (and optional GPU compute)
  without embedding Lua/JS. The host keeps its own control plane; libhull is
  the compute-plane SDK.
- **A capability boundary on a native CLI / batch tool.** "pledge/unveil for
  my C program, but portable across Linux / macOS / cosmo and with a
  declared fs / host / env allowlist." A parsing bug or hostile input can't
  read or write outside what the program declared, and can't `exec`/`fork`.
- **Defense-in-depth for a Rust / Zig service.** Memory-safe languages don't
  give you the *kernel* sandbox (seatbelt / landlock / seccomp), W^X
  enforcement, or a declared network/filesystem allowlist. libhull adds that
  second, OS-enforced layer around the whole process.
- **Signed-artifact / SBOM tooling.** Link the Ed25519 verification, SBOM
  generation, and release-manifest machinery rather than reimplementing it -
  e.g. an updater, a verifier, or an air-gapped signing utility.
- **Regulated / air-gapped deployments.** A small, auditable, signed binary
  with a provable capability boundary and an SBOM, and no scripting runtime
  in the trust base, is often a compliance requirement in itself.

Reach for something else when:

- **You're writing a new app from scratch.** Use Hull's Lua/JS runtime - you
  get the same sandbox + capability layer with `app.manifest`, hot reload,
  the stdlib, and far less boilerplate. libhull is the "I can't rewrite in a
  script" escape hatch, not the default.
- **You need Hull to police code it didn't compile.** libhull sandboxes the
  *process* you link it into; it is not a way to sandbox an arbitrary
  third-party shared library you call after seal (beyond the kernel
  boundary).
- **You want the app framework.** No routing, middleware, sessions, or
  `app.manifest` - libhull is the enforcement primitives, not the framework.

Trust note: the host owns `main()` and is **trusted to sequence the
lifecycle** (phase-1 → policy → seal before any capability use). That is a
weaker, documented contract than "Hull owns main" - a host bug *before* seal
is outside Hull's enforcement. See the trust boundary below.

## Building

```sh
make libhull            # -> build/libhull.a (+ .sha256 sidecar)
make embed-c-smoke      # links examples/embed_c against it and runs it
```

`make libhull` also writes `build/libhull.a.sha256` (the raw archive hash,
one hex line). Under the fat cosmocc driver (`make libhull CC=cosmocc`) it
additionally emits `build/.aarch64/libhull.a`, the concomitant sibling
cosmocc requires to link a dual-arch APE.

A host links only the archive, Keel, and libm/pthread; the sole Hull
include is the ABI header. `build/libhull.a` is listed twice because it and
Keel are mutually dependent (libhull's cap layer calls Keel's TLS, which
calls libhull's mbedTLS), and GNU ld resolves static archives strictly
left-to-right; macOS's linker resolves the cycle on its own:

```sh
cc -std=c11 -Iinclude \
   -o my_host my_host.c build/libhull.a vendor/keel/libkeel.a build/libhull.a -lm -lpthread
```

## The trust boundary

The embedding contract is deliberately **weaker** than "Hull owns
`main`". Be explicit about which side of the line each responsibility
sits on.

### The host is trusted to

- **Sequence the lifecycle correctly.** Call `hl_embed_sandbox_phase1()`
  early (before touching untrusted input), build the policy with the
  `hl_embed_allow_*` calls, then `hl_embed_seal()` before any capability
  use. Hull provides the primitives; the host orders them.
- **Declare an honest policy.** There is no `app.manifest()` to parse - a
  native host is trusted and states its filesystem / network / GPU / TUI
  policy directly in C. A policy that grants more than the app needs
  widens the sandbox; that is the host's call, exactly as a manifest would
  be.
- **Treat a failed seal as fatal.** `hl_embed_seal()` returns non-zero if
  either the RO policy seal or the kernel sandbox could not be applied.
  The host **must** abort rather than proceed - running capabilities
  without the kernel boundary is the one outcome the design forbids.

### libhull enforces (regardless of host bugs)

- **The capability boundary.** `hl_embed_fs_*` validates every path
  (traversal / symlink-escape rejection) against the sealed base
  directory, and `hl_embed_fs_*` refuses to run at all until the handle is
  sealed. Crypto goes through the same cap layer.
- **The kernel sandbox.** After `hl_embed_seal()`, pledge/unveil (Linux /
  Cosmo) or Seatbelt (macOS) is in force: default-deny filesystem, network
  only if declared, `exec`/`fork` blocked.
- **W^X.** The seal sets `wx_enforced` and declares no dynamic code - no
  guest-controlled memory is ever executable.
- **Read-only security policy.** The one datum the capability layer reads
  on every call - the filesystem base directory - is sealed into a
  page-backed read-only mapping (see below), so an arbitrary-write
  primitive cannot repoint the app's filesystem root after seal.

Everything the host can reach through the ABI is mediated. What the host
does with its *own* code (before seal, or outside the cap layer) is
outside Hull's enforcement - the same as any library.

## The seal lifecycle (fail-closed)

```
hl_embed_new(app_dir)         state = NEW    caps refuse (return -1)
  hl_embed_sandbox_phase1()   pledge; exec/fork blocked during setup
  hl_embed_allow_read/write() accumulate policy (rejected once sealed)
  hl_embed_allow_network/gpu/tui()
hl_embed_seal(db_path):
  1. seal base_dir into a read-only sh_seal_arena   ── fail -> return -1
  2. build the resolved HlSandboxPolicy
  3. hl_sandbox_apply(...) (default-deny kernel)    ── fail -> return -1
  4. free every writable alias of the policy
  state = SEALED                                     caps live
hl_embed_free(e)              heap freed first, RO arena destroyed LAST
```

Two properties make this fail-closed:

1. **Capabilities gate on `SEALED`.** Every `hl_embed_fs_*` call checks the
   state first and returns `-1` with a message before seal. A host that
   forgets to seal gets no filesystem access, not unmediated access.
2. **`SEALED` is set only after both seals succeed.** The base_dir RO seal
   runs *before* the (on macOS irreversible) kernel sandbox, so a seal
   failure aborts before anything irreversible; and if the kernel sandbox
   fails, the state stays `NEW` and capabilities remain closed. There is
   no window where the handle reports sealed while enforcement is missing.

### Why base_dir specifically is sealed

The resolved policy's path allowlists are read exactly once - by
`hl_sandbox_apply` at seal time - and never again, so their post-seal
mutability does not affect enforcement (and libhull frees those heap
copies right after applying the sandbox). The **base directory** is
different: `hl_cap_fs_*` reads `HlFsConfig.base_dir` on *every* call to
resolve and bounds-check paths. Per the c-audit §5b rule for boot-built,
per-call-read, security-influencing state, it is copied into an
`sh_seal_arena` (RW mmap → `mprotect` RO) during seal, and the writable
heap copy is freed. After seal there is no writable alias of the app's
filesystem root anywhere in the process.

This is verified by a fork+SIGSEGV death test
(`tests/hull/test_embed.c::embed.sealed_base_dir_is_readonly`): a child
seals, then writes to the base_dir mapping and must die with SIGSEGV /
SIGBUS. Without a real RO mapping the write would silently succeed and the
test would fail.

## Signing and verification

Each release publishes `libhull.a` as signed artifacts, so an embedder can
verify the exact archive it links:

- **Native, per-arch:** `libhull-linux-x86_64.a`, `libhull-linux-aarch64.a`,
  `libhull-darwin-arm64.a`.
- **Cosmo, dual-arch:** `libhull.x86_64-cosmo.a`, `libhull.aarch64-cosmo.a`.

Their SHA-256s are lines in the release `hull.sha256` manifest, which is
signed with the Ed25519 **release** key (and Sigstore + SLSA), exactly like
the `hull` binaries and the per-flavor platform libs. This is the same
trust chain [`hull flavor install`](build_flavors.md) uses. To verify a
downloaded archive offline, cross-check its SHA-256 against the
signature-verified manifest via `hl_release_io_verify_local_asset(dir,
asset)` - it re-checks the manifest signature against the **embedded**
release pubkey (the trust anchor baked into the binary), then matches the
asset's hash to the signed manifest line. `make libhull` also drops a
`build/libhull.a.sha256` sidecar for local checks.

Note the asymmetry (same as `hull build --flavor`): `libhull.a` is covered
by the **release** signature, not the inner **platform** signature - it is
a standalone archive a foreign host links, not an embedded platform lib
cross-checked at app-build time.

## SBOM

`hull sbom --subject=libhull [--format=human|json|cyclonedx|spdx]` emits an
SBOM scoped to the embedding surface: the subject component is named
`libhull`, the script runtimes (Lua, QuickJS) are dropped, and the linked
core (Keel, mbedTLS, WAMR, SQLite, tweetnacl, the CA bundle, …) is kept. Each release also publishes
`libhull.sbom.{json,cdx.json,spdx.json}`, covered by `hull.sha256` so they
inherit the release signature. The per-component membership is the
`in_libhull` flag on `HlSbomEntry` (`src/hull/sbom.c`).

## Testing

| Surface | Where |
|---|---|
| ABI guards, policy limits, fail-closed-before-seal, crypto, identity, NULL-safety | `tests/hull/test_embed.c` (`make test`) |
| Sealed base_dir is read-only (fork+SIGSEGV) | `tests/hull/test_embed.c` (`make test`) |
| libhull SBOM scope (flags + filter + subject rename) | `tests/hull/test_sbom.c` (`make test`) |
| Full sealed integration (sandbox + cap fs I/O + traversal) | `examples/embed_c` (`make embed-c-smoke`) |
| The underlying RO-arena mechanism | `tests/hull/test_seal_arena.c` |

## Reference hosts

Three reference hosts drive the identical embedding sequence (build a
policy in C-declared terms, seal, then capability-mediated fs I/O + crypto
+ identity), each exiting non-zero on any failure. They are the link
witnesses: a runtime dependency leaking into the core would fail their
link.

| Host | Language | How it binds the ABI | Runs via |
|---|---|---|---|
| [`examples/embed_c`](../examples/embed_c/) | C | `#include <hull/embed.h>` | `make embed-c-smoke` |
| [`examples/embed_rust`](../examples/embed_rust/) | Rust | an `extern "C"` block + `build.rs` link | `make embed-rust-smoke` |
| [`examples/embed_zig`](../examples/embed_zig/) | Zig | `@cImport("hull/embed.h")` - the header directly | `make embed-zig-smoke` |

`make embed-smoke` runs all three. The Rust/Zig targets **skip cleanly**
when `cargo`/`zig` is absent (so a plain checkout is unaffected); CI
installs both toolchains and asserts each host runs to completion. The Zig
host is the strongest ABI-cleanliness proof: `@cImport` consumes
`embed.h` as-is, so a clean compile means the header is FFI-consumable with
no massaging.

All three link `libhull.a` twice around Keel (`libhull.a libkeel.a
libhull.a`) to resolve the archive cycle under GNU ld / lld; macOS's linker
handles it without the repeat.
