# `hull tools install cosmocc` on Windows — investigation

**Question:** can a `hull-cosmo` APE, running on Windows, `hull build` an app by
spawning cosmocc — i.e. would `hull tools install cosmocc` make `hull build`
self-sufficient on Windows (symmetric to `hull tools install zig` for native
targets)?

**Answer (proven by `.github/workflows/windows-cosmocc.yml` on `windows-latest`):
`hull tools install cosmocc` alone does NOT unlock Windows builds (cosmocc's
driver is a `#!/bin/sh` script + symlink toolchain, not an execvp-able binary),
and an APE bash cannot self-contain it (no `/bin/sh` mount, no coreutils — §0.5).
BUT the self-sufficient Windows story IS achievable, with a bounded design
change, by bundling `busybox-w64` (§0.6): a single ~1 MB native Windows PE — no
Cygwin/MSYS runtime — that supplies BOTH `sh` and every coreutil the driver
shells out to, routes `#!/bin/sh` to its own applet, and (with a planted
`/bin/sh` + a shared `TMPDIR`) drives cosmocc to a clean `exit 0` producing a
working APE that runs and returns 42.**

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
with symlinks. cosmo's intended Windows usage is *inside* a POSIX shell, not
`execvp("cosmocc")`.

## §0 go/no-go (proven): a POSIX shell CAN drive cosmocc on Windows — GO

The gating question — *given* a shell + preserved symlinks, does cosmocc's
pipeline compile on Windows at all? — is answered **GO** by the `section0` job:

- **Git Bash (which provides `/bin/sh`) drove cosmocc's full pipeline**: `cosmocc
  exit 0`, produced a 401 KB APE (`hello_bash.com`, `-rwxr-xr-x`) that runs and
  returns 42 (arriving wait-status-encoded as `10752 = 42<<8` on Windows).
- **cosmo's own `cocmd` did NOT** (`cosmocc: No such file or directory`) — it is a
  `cmd`-like shell, not a POSIX `sh` that can run the `#!/bin/sh` driver.
- **`bsdtar` (`tar.exe`) preserved the toolchain symlinks** on the runner
  (`LinkType=SymbolicLink`), so a symlink-aware extractor is sufficient.

**Conclusion: the epic below is viable** — but the "shell" hull needs is a POSIX
`sh` (bash / MSYS `sh`, ubiquitous via Git for Windows), NOT cosmo's `cocmd`.
Either hull requires a POSIX `sh` on PATH for the cosmo/Windows path, or it
bundles/points at one.

## §0.5 can hull SUPPLY that shell itself (bundle an APE bash)? — NO

The §0 GO relied on **Git Bash** providing `/bin/sh`, which is not on a stock
Windows box. The obvious next move — bundle an APE `bash` (cosmo ships one at
`cosmo.zip/pub/cosmos/bin`) alongside cosmocc so the toolchain is
self-contained — was probed by the `section0b-ape-shell-self-contained` job.
**It does not work.** The APE bash *runs* on Windows and *finds* cosmocc, but:

- There is **no `/bin/sh`** under the APE bash (`command -v sh` → none), so
  cosmocc's `#!/bin/sh` driver can't launch: **exit 127 by name**.
- Running the driver *through* the interpreter (`bash ./bin/cosmocc`) still
  **exits 127** — cosmocc's symlinked sub-tools (`x86_64-unknown-cosmo-cc` →
  cosmocc, etc.) *also* re-exec via `#!/bin/sh`, so bypassing the top-level
  shebang doesn't help; the whole pipeline needs `/bin/sh` to exist.
- You **cannot plant one**: writing `/bin/sh` from the APE bash fails
  (`cannot write /bin/sh` — no writable `/` root on Windows).

So the blocker is not "a shell binary is missing" — it is that **cosmocc
requires a real `/bin/sh` filesystem mount** (a Unix-like FS layout that
resolves `#!/bin/sh`). MSYS / Git-Bash / WSL provide that mount; a bundled APE
shell provides an interpreter but no mount and cannot create one. **A
self-contained, APE-only cosmocc toolchain on Windows is therefore not
achievable** with cosmocc as it ships today.

**What this leaves for Windows:** the cosmo/Windows build path fundamentally
depends on an external POSIX environment (MSYS2 / Git-Bash / WSL) that supplies
`/bin/sh`. `hull tools install cosmocc` could still remove the "download
cosmocc yourself" step *within* such an environment, but it cannot make a stock
Windows box self-sufficient. The friction-free Windows story stays: **build the
cosmo APE on a POSIX host (Linux/macOS/CI) and ship the APE** — it runs on
Windows directly (proven by the `characterize` job).

## §0.6 can a bundled `busybox-w64` self-contain it? — YES (CLEAN GO)

§0.5 ruled out an APE bash because it is an interpreter with (a) no `/bin/sh`
mount and (b) no coreutils — and cosmocc's `#!/bin/sh` driver shells out to
`dirname` / `readlink` / `uname` / `sed` / `mv`. **busybox-w64** (Ron Yorston's
native-Windows busybox) fixes both: it is a single ~1 MB native PE — **no
Cygwin/MSYS DLL** — that provides `sh` (ash) AND every coreutil as applets, and
its `sh` routes `#!/bin/sh` to its own applet. The
`section0c-busybox-w64-self-contained` job proved it drives cosmocc end-to-end:

- busybox runs on Windows ✅ (`busybox-ok`; `dirname` / `uname` applets resolve).
- Driving cosmocc **built a correct APE** (runs, returns 42) in every variant.
- The driver's final `mv /tmp/fatcosmocc.XXXX.com.dbg` initially failed
  (`No such file or directory`): busybox's `/tmp` and the native APE sub-tools'
  `/tmp` resolved to **different real dirs**, so the debug sidecar wasn't where
  the driver looked. Not a compile failure — a `/tmp` disagreement.
- **Aligning them fixed it**: with a real `C:\tmp` exported as
  `TMPDIR=C:/tmp TMP=C:/tmp TEMP=C:/tmp` (both busybox and the cosmo APEs honor
  `${TMPDIR:-/tmp}`), cosmocc exited **0** and the APE ran and returned 42 →
  **CLEAN GO**.

So a stock Windows box can be made self-sufficient with a **~1 MB** extra binary
(vs a ~40 MB GPLv3 MSYS runtime, and vs requiring the user to have Git-Bash/WSL).
The recipe hull would implement: extract cosmocc (symlink-aware), **plant
`/bin/sh` = busybox** (so the `#!/bin/sh` driver + its symlinked sub-tools
resolve), **share one `TMPDIR`**, and **drive cosmocc via `busybox sh -c`**.

## What it would actually take (a design change, not a tool install)

To make `hull build` drive cosmocc on Windows, hull would need to:

- **Invoke cosmocc through a POSIX `sh`** — `sh -c "cosmocc …"` rather than
  `execvp("cosmocc")`, for the cosmo/Windows path only. That is a real change to
  the build's spawn layer + the compiler resolver, and it crosses the
  spawn-allowlist design (now it's "hull runs a shell that runs the compiler").
  §0 showed cosmo's own `cocmd` is NOT a POSIX `sh`; §0.6 showed **`busybox-w64`
  IS the concrete, self-contained enabler** — a ~1 MB native PE that hull can
  bundle (no external Git-Bash/WSL dependency, no ~40 MB GPLv3 MSYS runtime).
- **Plant `/bin/sh` + share `TMPDIR`** (the two setup steps §0.6 found load-
  bearing): copy busybox to `/bin/sh.exe` so the `#!/bin/sh` driver and its
  symlinked sub-tools resolve their shebang, and export one real `TMPDIR`
  (`TMP`/`TEMP`) so the driver's final `.dbg` rename finds what the native APE
  sub-tools wrote. Without the shared temp, cosmocc builds a correct APE but
  exits non-zero on the sidecar `mv`.
- **Symlink-aware bundle extraction** — the tools installer's `hl_tar_extract`
  writes regular files; the cosmocc tree needs symlinks (or a Windows-side
  reification of them). `bsdtar`/`tar.exe` preserved them on the runner, so the
  extractor's symlink support is the gap, not the bundle format.
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
