# `hull tools install cosmocc` — making `hull build` self-sufficient for APE apps

Status: **investigation + Windows experiment.** The productionized install is
**deferred** on one design decision (below).

## Goal

A `hull-cosmo` APE runs everywhere (Linux/macOS/Windows/BSD). We want it to also
`hull build` an app *on* those hosts with no separately-installed toolchain — the
symmetric counterpart of `hull tools install zig` (which gives native targets a
self-contained linker). cosmocc is the only toolchain that can produce an APE:
`obj_emit` has no APE format (only ELF/Mach-O/COFF), so the compiler-free path is
native-only, and a cosmo app is built by *spawning* cosmocc (build.lua gates the
emit path `if not is_cosmo`; cosmocc compiles `app_registry.c`/`app_main.c` and
`apelink`s the fat binary against the cosmo platform archive embedded in
hull-cosmo). cosmocc is already on hull's spawn allowlist (`cap/tool.c`).

## Why it maps onto the tool-bundle pattern

Mirrors `hull tools install zig`/`wamrc`: a signed `.tar`, Ed25519-verified into
`~/.hull/tools/cosmocc/`, resolved by `hl_tools_lookup_path`, spawned by
`hull build`. cosmocc's binaries are themselves APEs → **one arch-free bundle**
serves every host (simpler than zig's per-platform), and a *cosmo* hull can drive
it (unlike a native zig tree). Version-coupling is load-bearing: the embedded
cosmo platform archive is cosmo-libc-version-specific, so the cosmocc must match
(same as wamrc ↔ the WAMR commit); `hull tools install` pins to the running
hull's release, giving that for free. Producer: `scripts/build_cosmocc_bundle.sh`
(repacks the SHA-pinned `cosmo.zip/pub/cosmocc/cosmocc-<ver>.zip`).

## The deferred design decision: bundle size / format

`cosmocc-4.0.2` extracts to **~1.43 GB** (two full arch toolchains + cosmo libc +
apelink). That is 4× the zig bundle and far past the `release_io` 512 MB download
cap. Shipping it as a tool bundle therefore forces one of:

1. **A ~1.43 GB uncompressed `.tar`** + raise the cap to ~2 GB. Simple, but a
   punishing download.
2. **Compressed-bundle support in the trust-critical tools-install extract path**
   (gzip via the already-vendored miniz, or zip). ~150 MB download, but it adds a
   format to the signed-bundle install path — a real change to review.
3. **Prune cosmocc** to a minimal link-only subset. Fragile; both arches are
   needed for a fat APE.

This is a Hull-shaping decision (installer capability + release-asset size), so it
is left for an explicit call rather than defaulted. Until then the release does
NOT publish a cosmocc asset and there is NO `cosmocc` REGISTRY row (so the
`check_tools_registry` guard stays satisfied).

## The Windows experiment (`.github/workflows/windows-cosmocc.yml`)

Independent of the bundle decision, the *frontier* question is: does cosmocc's
nested pipeline (`cosmocc → cc1 → as → ld → apelink`) survive Cosmopolitan's
**emulated fork/exec on Windows**, driven by hull's `hl_tool_spawn`? That's
Cosmopolitan's story, not a Hull design gap — so a standalone, non-required,
`continue-on-error` workflow probes it on `windows-latest`: download the shipped
`hull-cosmo` APE, stage the pinned cosmocc at `~/.cosmocc` (a path the cosmo
compiler-resolver already checks — no bundle needed), `hull build` a minimal
`app.main`, and run the produced APE. Each step reports PASS/FAIL so the verdict
shows exactly how far the pipeline gets. See the workflow header for the model.
