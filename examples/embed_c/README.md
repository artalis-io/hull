# embed_c — native host for the libhull no-runtime flavor

This is the reference C host for **libhull** (Phase L-1): the runtime-free
Hull core packaged as a static archive (`build/libhull.a`) that a native
program links directly, owning its own `main()`. No Lua, no QuickJS, no
`app.main` lifecycle — just the hardened core:

- **Kernel sandbox** — two-phase pledge/unveil (Linux/Cosmo) or Seatbelt
  (macOS), driven from a policy the host builds in C.
- **Capability layer** — `hl_cap_fs_*`, `hl_cap_crypto_*`, etc., with the
  same path-traversal and allowlist enforcement app code gets.
- **WASM / GPU compute isolation** — WAMR + the wgpu backend are in the
  archive (compute-only; no I/O imports).
- **Signed-artifact / SBOM identity** — `hl_release_io_*`, the module
  registry, signature verification.

## Build and run

```sh
make embed-c-smoke
```

That target links `examples/embed_c/main.c` against `build/libhull.a` and
`vendor/keel/libkeel.a` alone and runs it. Because neither runtime is
linked, an accidental dependency on Lua/QuickJS from the core would fail
the link here — the smoke test is a standing witness that the archive is
genuinely runtime-free.

The host includes exactly **one** Hull header, `<hull/embed.h>` — the
stable, versioned embedding ABI. It never reaches into an internal Hull
header, so it is insulated from the internal sandbox / capability struct
layout. To link a host by hand you only need the include root:

```sh
make libhull
cc -std=c11 -Iinclude \
   -o my_host my_host.c build/libhull.a vendor/keel/libkeel.a -lm -lpthread
```

## What the host demonstrates

`main.c` runs the embedding sequence a real native consumer would, all
through the `hl_embed_*` ABI:

1. `hl_embed_new(app_dir)` — create the handle (app_dir is absolute; all
   capability fs access resolves under it).
2. `hl_embed_sandbox_phase1()` — phase-1 syscall reduction.
3. `hl_embed_allow_read` / `hl_embed_allow_write` — build the policy **in
   C** (a native host is trusted; it does not parse an `app.manifest`).
   Paths are **app_dir-relative**, the same contract as a manifest's
   `fs.read` / `fs.write`; absolute paths are rejected.
4. Fail-closed check: capability calls return `-1` **before** the sandbox
   is sealed.
5. `hl_embed_seal()` — phase-2 default-deny sandbox; the host treats a
   non-zero return as fatal.
6. `hl_embed_fs_write` / `_read` — capability-mediated I/O, plus a
   path-traversal rejection check.
7. `hl_embed_sha256` — capability-mediated crypto (known-vector check).
8. `hl_embed_platform` / `hl_embed_module_count` — signed-artifact / SBOM
   identity.

The host exits non-zero if any check fails.

## The ABI

`include/hull/embed.h` is the whole surface: an opaque `HlEmbed` handle,
`hl_embed_abi_version()` for version negotiation, the `hl_embed_allow_*`
policy builders, the `hl_embed_sandbox_phase1` / `hl_embed_seal`
lifecycle, and the `hl_embed_fs_*` / `hl_embed_sha256` /
`hl_embed_platform` / `hl_embed_module_count` capability calls. It depends
only on `<stddef.h>` / `<stdint.h>`.

Unit tests for the guard / limit / identity surface live in
`tests/hull/test_embed.c` (run by `make test`); the sealed integration
path is this example, run by `make embed-c-smoke`.

## What is NOT in libhull

The archive deliberately excludes `main`/`serve`/`entry`, the CLI command
dispatch, the agent tooling, the build-tool VM (`tool`/`compiler`), both
script runtimes and their manifest extractors, `app_context`,
`runtime_factory`, `static.c` (HTTP-serving middleware), and the embedded
stdlib. The host owns the process lifecycle; Hull owns the enforcement
boundary.
