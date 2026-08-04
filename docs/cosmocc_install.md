# `hull tools install cosmocc` on Windows — investigation

**Question:** can a `hull-cosmo` APE, running on Windows, `hull build` an app by
spawning cosmocc — i.e. would `hull tools install cosmocc` make `hull build`
self-sufficient on Windows (symmetric to `hull tools install zig` for native
targets)?

**Answer (proven by `.github/workflows/windows-cosmocc.yml` on `windows-latest`):
NO, not with cosmocc alone.**

## What the experiment found

1. **`hull-cosmo` runs on Windows** ✅ — it is a real fat APE; `hull version`
   works on `windows-latest`.
2. **cosmocc's driver is a `#!/bin/sh` POSIX shell script**, not an APE. The
   `cosmo.zip` cosmocc tree is: `bin/cosmocc` = an 18 KB `#!/bin/sh` script
   ("fat cosmopolitan c/c++ compiler"); the arch compilers
   (`x86_64-unknown-cosmo-cc`, …) are **symlinks → cosmocc** (7–22 byte stubs);
   only the leaf tools (`compile.ape`, `assembler`, `apelink`, …) are actual
   APEs. cosmo even bundles its own shell (`cocmd`, ~1.1 MB) and `make`.
3. Therefore **`hull build` spawning `cosmocc` via `execvp` fails on Windows**
   with `no C compiler available`: Windows has no `/bin/sh` to run the driver
   script, and a naive extraction (`Expand-Archive`, or a flat tar) **breaks the
   symlinks**.

So the "cosmocc is an APE, so it runs on Windows" premise is wrong: only the
*leaves* are APEs; the *driver* is a shell script, and the toolchain is wired
with symlinks. cosmo's intended Windows usage is *inside* its bundled shell
(`cocmd`/bash), not `execvp("cosmocc")`.

## What it would actually take (a design change, not a tool install)

To make `hull build` drive cosmocc on Windows, hull would need to:

- **Invoke cosmocc through cosmo's bundled shell** — `cocmd -c "cosmocc …"` (or
  bash) rather than `execvp("cosmocc")` — for the cosmo/Windows path only. That
  is a real change to the build's spawn layer + the compiler resolver, and it
  crosses the spawn-allowlist design (now it's "hull runs a shell that runs the
  compiler").
- **Symlink-aware bundle extraction** — the tools installer's `hl_tar_extract`
  writes regular files; the cosmocc tree needs symlinks (or a Windows-side
  reification of them).
- Plus the **bundle-size problem**: `cosmocc-4.0.2` extracts to **~1.43 GB**
  (4× zig, past the `release_io` 512 MB cap), so the asset needs either a ~2 GB
  uncompressed tar or **compressed-bundle support in the trust-critical install
  path**.

These are Hull-shaping decisions, so they are **left for an explicit call**. No
`cosmocc` REGISTRY row / release asset is added (the `check_tools_registry`
guard stays satisfied), and `scripts/build_cosmocc_bundle.sh` (the SHA-pinned
`cosmo.zip` repack) is kept only as the producer such a decision would build on —
note it does not yet preserve symlinks.

## Where cosmocc IS the answer today

On a POSIX host (Linux/macOS) a `hull-cosmo` already builds cosmo-APE apps by
spawning cosmocc from `~/.cosmocc` / `/opt/cosmo` (its `#ifdef __COSMOPOLITAN__`
resolver). The install-bundle only removes the "install cosmocc yourself" step
there; it does not change the Windows conclusion above. (A minor, orthogonal
Hull gap the experiment also surfaced: that resolver probes `$HOME` but not
`USERPROFILE`, so it wouldn't find cosmocc on Windows even if cosmocc were
directly runnable.)
