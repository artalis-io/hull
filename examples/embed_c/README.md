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

To link a host by hand:

```sh
make libhull
cc -std=c11 -Iinclude -Ivendor/keel/include -Ivendor/mbedtls/include \
   -Ivendor/wamr/core/iwasm/include \
   -o my_host my_host.c build/libhull.a vendor/keel/libkeel.a -lm -lpthread
```

## What the host demonstrates

`main.c` runs the embedding sequence a real native consumer would:

1. `hl_sandbox_apply_pledge()` — phase-1 syscall reduction.
2. Build an `HlSandboxPolicy` **in C** (a native host is trusted; it does
   not parse an `app.manifest` — it declares policy directly). Filesystem
   paths in the policy are **app_dir-relative**, the same contract as a
   manifest's `fs.read` / `fs.write`; absolute paths are rejected by the
   sandbox path resolver.
3. `hl_sandbox_apply()` — phase-2 default-deny sandbox.
4. `hl_cap_fs_write` / `_read` — capability-mediated I/O, plus a
   path-traversal rejection check.
5. `hl_cap_crypto_sha256` — capability-mediated crypto (verified against a
   known vector).
6. `hl_release_io_platform` / `hl_module_registry_count` — signed-artifact
   / SBOM identity.

The host exits non-zero if any capability check fails.

## What is NOT in libhull

The archive deliberately excludes `main`/`serve`/`entry`, the CLI command
dispatch, the agent tooling, the build-tool VM (`tool`/`compiler`), both
script runtimes and their manifest extractors, `app_context`,
`runtime_factory`, `static.c` (HTTP-serving middleware), and the embedded
stdlib. The host owns the process lifecycle; Hull owns the enforcement
boundary.
