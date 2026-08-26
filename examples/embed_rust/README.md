# embed_rust - Rust host for the libhull no-runtime flavor

Reference Rust consumer of the [`hl_embed_*` ABI](../../include/hull/embed.h).
Links only `libhull.a` + Keel (no Lua/QuickJS runtime) and drives the
runtime-free Hull core - two-phase sandbox, capability-mediated filesystem
I/O (incl. traversal rejection), crypto, and identity - via a small
`extern "C"` block. The analogue of [`../embed_c`](../embed_c/), from Rust.

## Run

```sh
make embed-rust-smoke      # from the repo root; builds libhull.a first
```

The target skips cleanly if `cargo` is absent. `build.rs` links the
archives in the order `libhull.a libkeel.a libhull.a` (the archive cycle
needs the repeat under GNU ld / lld); override the archive paths with the
`HULL_LIBHULL_A` / `HULL_LIBKEEL_A` env vars (the make target sets them to
absolute in-tree paths).

See [docs/libhull_flavor.md](../../docs/libhull_flavor.md) for the trust
boundary and the full embedding story.
