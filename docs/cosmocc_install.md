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

## Epic spec (A-E): self-contained cosmocc `hull build` on Windows

This is the design for turning §0.6's CLEAN GO into a shipped capability. It is a
**design change, not a `hull tools install`** (it crosses the "no shell
invocation" spawn-allowlist invariant), so it is written out fully here for an
explicit go/no-go. **Goal:** a `hull-cosmo` on a stock Windows box, with one
`hull tools install cosmocc`, builds a cosmo-APE app. **Non-goals:** native
Windows PE output (that needs the separate Windows C port of Hull -
pledge/unveil→JobObjects, fork/exec→CreateProcess, Keel epoll→IOCP, and a
COFF `libhull_platform.a` that does not exist); changing the POSIX-host path
(Linux/macOS already work); cosmo's own `cocmd`.

The five items, sequenced by dependency (D, B are standalone and land first; A
needs busybox present; E productionizes and depends on the C decision).

**Status: D, B, and A have landed** (the C changes; A is the transparent
cosmocc-through-busybox reroute in the spawn layer - see below) and **C is
decided** (trim the tree to a ~309 MB closure, under the cap uncompressed). Only
**E (productionize)** remains - the bundle producer + registry row + release +
the Windows end-to-end validation.

### A - drive cosmocc through busybox `sh` (the invocation change)

> **AS BUILT (landed).** The primitive is `hl_tool_spawn_driver_shell` +
> `hl_tool_cosmo_shell` (`src/hull/cap/tool.c`). The reroute is **transparent in
> the C spawn layer**, not threaded through `build.lua`: `hl_tool_spawn_env` and
> `hl_tool_spawn_read` detect a `cosmocc` `argv[0]` on cosmo+Windows and, when a
> bundled busybox resolves, run it through the shell-driver form. That was chosen
> over per-call-site wrapping because cosmocc is *also* spawned by the C compiler
> vtable (`tool.compiler.compile`, which never passes through Lua) and by the
> `-dumpmachine` / `--version` probes - so `build.lua` needs **zero** changes and
> every call site is covered uniformly. All of it is `#ifdef __COSMOPOLITAN__`, so
> native/POSIX builds are byte-identical (the reroute compiles out; `cosmo_shell`
> returns -1). `cosmo_prepare` shares one `TMPDIR`/`TMP`/`TEMP` (load-bearing) and
> best-effort-plants `/bin/sh.exe = busybox` (ignored on failure). Unit tests in
> `test_tool` cover the primitive on POSIX (reject non-allowlisted driver /
> non-sh shell / dangerous args; run `cc --version` through `/bin/sh`). Windows
> end-to-end is validated at E (needs the bundle). The original per-call-site
> design below is kept for context.

**Problem.** For a cosmo target, `build.lua` always uses the compiler path (the
`if not is_cosmo` gate around the compiler-free `obj_emit`): it does
`tool.spawn({ cc, "-c", "app_registry.c", ... })`, the same for `app_main.c`, and
the apelink of the fat binary, with `cc` = cosmocc resolved by
`hl_driver_resolve_native` (`src/hull/compiler.c:131`). Each lands in
`hl_tool_spawn` → `execvp(argv[0])` (`src/hull/cap/tool.c:233`). On a POSIX host
the kernel honors cosmocc's `#!/bin/sh` shebang; on Windows there is no kernel
shebang, so `execvp("cosmocc")` fails. The invocation must become
`busybox sh -c 'cosmocc "$@"' cosmocc <args...>` (positional `$@`, so no arg
needs shell-quoting - paths with spaces stay safe).

**Touchpoints.**
- `src/hull/cap/tool.c:133` `allowed_prefixes[]` - currently `cosmocc` is
  allowed as `argv[0]`; routing through the shell makes `busybox` the `argv[0]`.
- `src/hull/cap/tool.c:233` `hl_tool_spawn_env` - the fork/execvp site.
- `stdlib/cli/lua/hull/build.lua` - every cosmo-path `tool.spawn({ cc, ... })`.

**Design (keeps the no-shell invariant closed).** Do NOT add a general `sh` to
the allowlist - that would re-open arbitrary shell execution. Instead add a
dedicated, tightly-scoped entry point:

```
int hl_tool_spawn_driver_shell(const char *shell,      /* the busybox path   */
                               const char *driver,      /* an allowlisted cc  */
                               const char *const args[],/* compiler args only */
                               const char *const envadd[]);
```

It builds the argv ITSELF as a fixed shape - `{ shell, "sh", "-c",
"exec \"$0\" \"$@\"", driver, args... }` - so the `-c` string is a **compile-time
literal** (no app-derived bytes ever reach the shell as code) and every
app-derived value arrives as a positional parameter. It validates `driver`
through the existing `hl_tool_check_allowlist` (must be `cosmocc`) and validates
`shell`'s basename is exactly `busybox`/`sh`. Result: the surface added is "run
busybox as `sh` to exec one allowlisted compiler with positional args," not "run
a shell." `build.lua` calls this only on the cosmo+Windows path (a new
`tool.cosmo_shell` accessor exposes the resolved busybox path; nil elsewhere, and
when nil `build.lua` keeps the direct `tool.spawn` call).

**Plant `/bin/sh` + share `TMPDIR` (the §0.6 setup, done in C at build start).**
Before the first driver spawn on Windows: copy the bundled busybox to a
`/bin/sh.exe` the cosmocc driver + its symlinked sub-tools resolve (the shebang
target), and set `TMPDIR`/`TMP`/`TEMP` to one real directory (passed via the
`envadd` arg above) so the driver's final `.dbg` `mv` finds what the native APE
sub-tools wrote. Planting `/bin/sh` writes outside the app dir, so it is a
build-tool action (unveiled `~/.hull` region or the cosmo bundle's own `bin/`),
never an app capability.

**Risks.** (1) Crosses the "No shell invocation" convention in CLAUDE.md - the
scoped entry point + literal `-c` is the mitigation, and it must be called out
in the security docs. (2) Windows-only code path with no product-path CI until
Windows `hull build` CI exists (the investigation workflow is the stand-in). (3)
The `/bin/sh` plant needs a writable location; if `/bin` is not writable, fall
back to putting busybox's dir on `PATH` as `sh` + relying on busybox shebang
routing (variant (a) built the APE; only the `.dbg` step needed the plant, which
the shared `TMPDIR` also addresses).

**Tests.** Extend `windows-cosmocc.yml` into a real `hull build` smoke on
`windows-latest` once A+B+D land (build the null app, run it, assert exit code);
a `test_tool` unit case asserting `hl_tool_spawn_driver_shell` rejects a
non-allowlisted driver and a non-busybox shell.

### B - symlink-aware `hl_tar_extract`

**Problem.** `hl_tar_extract` (`src/hull/cap/tar.c:133`, contract in
`include/hull/cap/tar.h:38`) handles only typeflags `'5'`/`'0'`/`'\0'` (dir /
file) and **skips symlinks** (`'2'`) and hardlinks (`'1'`). cosmocc's arch
compilers (`x86_64-unknown-cosmo-cc`, …) are symlinks → `cosmocc`; skipping them
yields a broken toolchain. `bsdtar` preserved them on the runner, so the gap is
purely hull's extractor.

**Design.** Add typeflag `'2'` handling. The ustar `linkname` is
`hdr[157..257]` (100 bytes), adjacent to the `typeflag` at `hdr[156]` the parser
already reads. Validate `linkname` with the same `tar_safe_path` gate the member
name uses (reject absolute / `..`-escaping - a malicious symlink is a
write-through primitive even in a trusted bundle: defense in depth). Then, at
extract time: `symlink(linkname, path)`; on `EPERM`/`EEXIST`/Windows failure,
**fall back to copying** the already-extracted target file
(`dirname(path)/linkname` - the targets are same-dir regular members, e.g.
`cosmocc`), which needs a two-pass extract (regular files first, deferred
symlink members second). The copy fallback removes the hard Windows
requirement; the `symlink` fast-path is used where privilege allows.

**Touchpoints.** `src/hull/cap/tar.c` (parse: surface linkname on `HlTarEntry`;
extract: the two-pass + symlink/copy), `include/hull/cap/tar.h` (add
`const char *linkname` to `HlTarEntry`, update the contract comment), the
producer `scripts/build_cosmocc_bundle.sh` (emit symlinks, see E).

**Windows note.** `symlink()` maps to `CreateSymbolicLink`, which needs
`SeCreateSymbolicLinkPrivilege` (admin or Developer Mode). The copy fallback is
why B does not hard-require either.

**Tests.** `test_tar` cases: a symlink member extracts as a link where allowed
and as a copy where not; a `..`-escaping linkname is rejected. This is a pure C
change testable without cosmocc.

### C - bundle size / format (DECIDED: trim the tree)

**Problem.** `cosmocc-4.0.2` extracts to **~1.37 GB**. A flat uncompressed `.tar`
of the whole tree is over the `release_io.c:142` `max_response_size = 512 MB`
download cap; and hull has **no in-tree inflate** (`src/hull/utils/compress.c` is
HTTP-response gzip *encode* only), so a `.tar.gz` asset can't be decompressed by
the install path today.

**Spike result (the `sectionc-trim-trace` CI job, `strace -ff` of a fat-APE
build).** The set of cosmocc-tree files an APE build actually opens is only
**~309 MB / 64 files** - and the superset upper bound (every path mentioned,
success or not) is identical, so there is no measurement gap. The closure is
dominated by, per arch:

| Kept file (per arch, x86_64 + aarch64) | Size |
|---|---|
| `libexec/gcc/<arch>-linux-cosmo/14.1.0/cc1` (the C compiler) | 79 + 75 MB |
| `<arch>-linux-cosmo/lib/libcosmo.a` (cosmo runtime) | 59 + 56 MB |
| `as`, `ld.bfd`, `collect2`, `<arch>-linux-cosmo-gcc` drivers | ~35 MB |
| `bin/{cosmocc,apelink,fixupobj,pecheck}` + arch-cc symlinks | ~2 MB |
| headers (`include/`, `<arch>-linux-cosmo/include/`) | few MB |

`cc1` and `libcosmo.a` are opened *whole* as files regardless of how many
symbols link, so this floor is faithful even though the trace program is small
(and hull's real link only adds its own embedded platform `.a`, not cosmocc
third-party libs - hull uses cosmo libc only). **~1060 MB is droppable**:
examples, docs, the bundled python, unused third-party libs, and `.dbg` debug
variants a Hull build never touches.

**Decision: option (1) TRIM.** A trimmed bundle of the ~309 MB closure (round up
to ~350-400 MB for header slack + the crt/ape link objects + margin) fits under
the existing 512 MB cap **uncompressed** - so NO gzip inflater is added to the
trust path, and no cap change. The producer (`scripts/build_cosmocc_bundle.sh`,
item E) builds the bundle from the keep-set above (+ its symlinks + parent dirs),
not the whole tree. (Rejected: option 2 gzip-inflate - unnecessary new
trust-path code; option 3 raise-the-cap - ships a needless ~1 GB.)

### D - resolver: `$HOME`→`USERPROFILE` + the installed-bundle location

**Problem.** `hl_driver_resolve_native` (`src/hull/compiler.c:143`) probes
`getenv("HOME")` for `~/.cosmocc/bin/cosmocc`, then `/opt/cosmo`, then `cosmocc`
on `$PATH`. On Windows cosmo libc populates `USERPROFILE`, not `HOME`, so none
resolve; and it never checks the `hull tools install` location.

**Design (smallest item, standalone).** (1) Home fallback:
`getenv("HOME")` ? : `getenv("USERPROFILE")`. (2) Probe the installed bundle
FIRST via `hl_tools_lookup_path("cosmocc", hull_exe, ...)` →
`~/.hull/tools/cosmocc/bin/cosmocc`, so `hull tools install cosmocc` is found
before the ad-hoc locations. Confirm `hl_tools_lookup_path`'s own home lookup
has the same `USERPROFILE` fallback.

**Tests.** `test_compiler` case with `HOME` unset + `USERPROFILE` set resolves
the bundle path.

### E - productionize (registry + producer + release + dry-run)

- **REGISTRY row** in `src/hull/tools_install.c`: `cosmocc`,
  `.is_bundle = 1`, `.bundle_entry = "bin/cosmocc"`, **not**
  `bundle_per_platform` (cosmocc binaries are themselves APEs → one arch-free
  bundle serves every host). Platform gating: publish for all native platforms
  **and** cosmo (unlike zig, cosmocc is *for* the cosmo hull). Adding the row
  keeps `check_tools_registry.sh` satisfied (one row + one matching asset).
- **Bundle busybox-w64 INSIDE the cosmocc bundle** (`bin/busybox.exe`, ~1 MB)
  rather than as its own tool. Rationale: hull's tool-platform enum is
  `{linux-x86_64, linux-aarch64, darwin-arm64, cosmo}` with no "windows" - a
  Windows-only `busybox-w64` tool has no platform key. Riding inside the
  cosmocc bundle sidesteps that (it is unused on non-Windows hosts) and keeps
  "install cosmocc" as the single user step. `build.lua` finds it at
  `<bundle>/bin/busybox.exe` via `tool.cosmo_shell`.
- **Producer** `scripts/build_cosmocc_bundle.sh` already exists (SHA-pinned
  `cosmo.zip` repack) but must be updated per B (symlink-preserving `tar`,
  `--dereference` OFF) and C (trim set or gzip), and drop `bin/busybox.exe` in.
- **Release** `release.yml` asset + `hull.sha256` manifest line +
  `tests/release_smoke.sh` step; **dry-run** first via a hyphenated pre-release
  tag (auto `--prerelease`; teardown with `gh release delete --cleanup-tag`).
- **Licensing.** busybox is **GPLv2**; it is a separate program hull *spawns*
  (mere aggregation, like the Apache-2 `wamrc` / MIT `zig` bundles), not linked,
  so redistribution alongside Hull's AGPLv3 is fine - but the GPLv2 source-offer
  obligation for the redistributed busybox binary must be honored (pin the
  upstream source + SHA, note it in the release). Flag for confirmation.

### Sequencing & what blocks what

```
D (resolver, tiny, standalone)  ─┐
B (symlink extract, pure C)     ─┤→ A (shell-driver spawn; needs busybox present)
C DECISION (trim vs gzip vs cap)─┘        │
                                          └→ E (registry + producer + release + dry-run)
```

D and B can land immediately (both are self-contained, CI-testable without
cosmocc). A needs busybox available (dev: drop it next to a locally-unzipped
cosmocc). E is gated on the C decision (it determines the producer + asset). A
Windows `hull build` smoke in `windows-cosmocc.yml` is the acceptance test, added
once A+B+D are in.

### Open decisions (need an explicit call before E)

1. ~~**Ship it at all?**~~ DECIDED: ship it.
2. ~~**C: bundle format?**~~ DECIDED: trim the tree (~309 MB closure fits the
   512 MB cap uncompressed; no inflater, no cap change).
3. **A: gate** - is Windows cosmo-build automatic when a bundled busybox is
   present, or behind an explicit `--host-shell`/opt-in flag?
4. **Licensing** - confirm the GPLv2 busybox "aggregation, not derivative"
   redistribution stance + source-offer handling.

Until these are decided, no `cosmocc` REGISTRY row / release asset is added (the
`check_tools_registry` guard stays satisfied), and `scripts/build_cosmocc_bundle.sh`
stays the producer skeleton such a decision would build on.

## Where cosmocc IS the answer today

On a POSIX host (Linux/macOS) a `hull-cosmo` already builds cosmo-APE apps by
spawning cosmocc from `~/.cosmocc` / `/opt/cosmo` (its `#ifdef __COSMOPOLITAN__`
resolver). The install-bundle only removes the "install cosmocc yourself" step
there; it does not change the Windows conclusion above. (A minor, orthogonal
Hull gap the experiment also surfaced: that resolver probes `$HOME` but not
`USERPROFILE`, so it wouldn't find cosmocc on Windows even if cosmocc were
directly runnable.)
