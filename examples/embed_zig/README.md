# embed_zig — Zig host for the libhull no-runtime flavor

Reference Zig consumer of the [`hl_embed_*` ABI](../../include/hull/embed.h).
Zig `@cImport`s `hull/embed.h` **directly** — no hand-written bindings — and
links only `libhull.a` + Keel (no Lua/QuickJS runtime), driving the same
embedding sequence as [`../embed_c`](../embed_c/): two-phase sandbox,
capability-mediated filesystem I/O (incl. traversal rejection), crypto, and
identity.

Because `@cImport` consumes the header as-is, a clean compile is itself
evidence the ABI header is FFI-consumable with no massaging.

## Run

```sh
make embed-zig-smoke       # from the repo root; builds libhull.a first
```

The target skips cleanly if `zig` is absent. Built and tested with **Zig
0.13.0**; the archives are linked `libhull.a libkeel.a libhull.a` (the cycle
needs the repeat under lld).

See [docs/libhull_flavor.md](../../docs/libhull_flavor.md) for the trust
boundary and the full embedding story.
