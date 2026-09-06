# Installing Hull on Windows

Hull ships as a single Cosmopolitan APE (`hull-cosmo`) that runs natively on
Windows. `install.ps1` installs, upgrades, verifies, and removes it for the
current user, with no administrator rights and no Developer Mode.

For the design and the full trust model behind the installer, see
[`windows_install_design.md`](windows_install_design.md).

## Quick start

```powershell
irm https://gethull.dev/install.ps1 | iex
```

That form executes the script straight from the network, unverified - the same
bootstrap trust as `install.sh | sh` elsewhere. Worth being precise about what
is and is not checked: **the installer script itself** arrives over HTTPS only,
while **everything it goes on to download** - the `hull.com` binary - is checked
against the Ed25519-signed `hull.sha256` manifest, and an upgrade additionally
verifies that signature before replacing anything. See
[Trust and verification](#trust-and-verification).

To inspect the script before any of it runs, download it first:

```powershell
Invoke-WebRequest https://gethull.dev/install.ps1 -OutFile install.ps1
.\install.ps1
```

This installs `hull.com` to `%LOCALAPPDATA%\Programs\Hull`, adds that directory
to your user `PATH` for future sessions, **and updates the PATH of the session
you are in**, so `hull` works immediately - no new terminal.

The current-session update requires the installer's code to run *in* your shell,
which is exactly what `irm ... | iex` does - so that form always leaves `hull`
resolvable immediately, and says so.

Run from a downloaded **file** the picture is less certain: `.\install.ps1`
executes in the same process (so the PATH update does reach your prompt), but
`powershell -File install.ps1` is a separate process (so it cannot - Windows
gives no way for a child process to change its parent's environment). Those two
are indistinguishable from inside the script, so instead of guessing, the
installer prints the exact one-line command to paste if your shell still cannot
find `hull`:

```powershell
$env:PATH = "$env:LOCALAPPDATA\Programs\Hull;$env:PATH"
```

## Five-minute path

Copy-paste this into a fresh PowerShell. Nothing else is required: no admin
rights, no Developer Mode, no Visual Studio, no MSYS, no package manager, and no
knowledge of Cosmopolitan or APE filename conventions.

```powershell
irm https://gethull.dev/install.ps1 | iex

hull doctor                  # reports what is ready and what is missing
hull doctor --fix            # installs the missing Hull-managed toolchain
                             #   (equivalent to: hull tools install cosmocc)

hull new hello
cd hello
hull app.lua                 # serves from source on http://127.0.0.1:3000
                             #   (this one blocks - Ctrl+C before continuing)

hull build                   # produces .\app.com
.\app.com                    # the same app, now one portable binary
```

`hull build` names the produced binary `app.com` on Windows because Windows will
not execute an extensionless file, and `.com` is the Cosmopolitan APE
convention - the same reason Hull itself installs as `hull.com`. On Linux and
macOS the same command still produces `app`. The build prints the exact command
to start it, and leaves cosmocc's debug sidecars (`app.com.dbg`,
`app.aarch64.elf`) under `.hull/build/` so the project root holds one obvious
shippable executable. Pass `--keep-build-artifacts` to leave them in place.

Note that `.\app.com` needs the leading `.\`: PowerShell, like a POSIX shell,
does not search the current directory for executables.

## Building an app: the toolchain

`hull build` links a Cosmopolitan APE, so on Windows it needs **cosmocc** - not
gcc or clang. A native C compiler cannot link Hull's cosmo-format platform
archives into a portable APE, so `hull doctor` asks for cosmocc specifically and
points at Hull's own signed bootstrap:

```powershell
hull tools install cosmocc   # or: hull doctor --fix
```

That downloads a trimmed, signed cosmocc bundle (verified against the same
Ed25519-signed `hull.sha256` manifest as every other Hull download) into
`%USERPROFILE%\.hull\tools\`. It writes nothing outside `~\.hull`, needs no
admin rights, and does not touch your `PATH`.

### Building Hull itself from source (needs MSYS2)

Everything above - installing Hull, and building **apps** with the released
`hull.com` - needs no MSYS2, no Visual Studio, and no WSL. Building **Hull
itself** from source on Windows is the one path that does need MSYS2, and it is
a hard requirement rather than a preference.

Hull's `Makefile` passes its version macros as escaped-quote defines
(`-DHL_VERSION=\"...\"` and five siblings). A *native Win32* make - Chocolatey's,
mingw64's, or any other - does not hand a POSIX shell an argv; it builds a
**Windows command line**, which MSYS `sh` then re-parses under Windows rules, so
everything from the first `\"` collapses into a single argument and the compiler
rejects it. Measured: the same recipe line yields `argc=2` through a native make
and `argc=8` through `sh -c`. An MSYS-native make execs `sh` with a real argv, so
the escaping survives.

The commands below mirror what CI actually builds and verifies
(`.github/workflows/windows-source-build.yml`). Two of the flags are load-
bearing and were not obvious — see the notes after the block.

```sh
# From an MSYS2 shell (msys2.org), in the MSYS environment.
pacman -S make binutils vim diffutils git
#          make  ar/nm     xxd  cmp       version stamp

git clone --recursive https://github.com/artalis-io/hull
cd hull

# Everything below runs in ONE subshell under `set -e`, so a failed checksum
# aborts the recipe. That matters when pasting: without it, `sha256sum -c`
# would print FAILED and the very next line would extract and then RUN the
# unverified toolchain anyway. A check that gates nothing is worse than no
# check, because it reads as though it did. The subshell also means a failure
# ends the recipe, not your shell session.
(
  set -euo pipefail

  # cosmocc, at the version and SHA-256 CI pins. Verify it: this is a compiler
  # toolchain fetched over the network.
  curl -fsSLO https://cosmo.zip/pub/cosmocc/cosmocc-4.0.2.zip
  echo "85b8c37a406d862e656ad4ec14be9f6ce474c1b436b9615e91a55208aced3f44  cosmocc-4.0.2.zip" \
    | sha256sum -c
  mkdir -p cosmo && tar -xpf cosmocc-4.0.2.zip -C cosmo

  # HL_OPT does not reach Keel's sub-make (issue #461), so Keel would build at
  # its own hardcoded -O2 and wedge. Until that is fixed, shadow cosmocc with a
  # wrapper appending -O0; gcc honours the LAST -O. It MUST be named `cosmocc`:
  # Keel enables its dual-arch build only when CC is exactly that string.
  mkdir -p cosmo/wrap
  printf '#!/bin/sh\nexec "%s" "$@" -O0\n' "$PWD/cosmo/bin/cosmocc" > cosmo/wrap/cosmocc
  chmod +x cosmo/wrap/cosmocc

  # cosmo/wrap FIRST (the shim), cosmo/bin LAST: cosmo/bin ships its own `make`
  # which must not shadow MSYS2's.
  export PATH="$PWD/cosmo/wrap:$PATH:$PWD/cosmo/bin"

  make CC=cosmocc HL_OPT=-O0 HL_ENABLE_WASM=0 -j2
)
```

The `PATH` above is scoped to that subshell, which is the point — but it means
a later rebuild needs it again:

```sh
export PATH="$PWD/cosmo/wrap:$PATH:$PWD/cosmo/bin"   # from the repo root
make CC=cosmocc HL_OPT=-O0 HL_ENABLE_WASM=0 -j2
```

**`HL_OPT=-O0`** is not a preference. cosmocc's gcc wedges on Hull's largest
translation units at higher levels on Windows, so `-O0` is the only level
observed to complete. It also means this build cannot catch bugs that only
appear under optimization; those stay covered by the Linux and macOS CI jobs.

**`HL_ENABLE_WASM=0`** matches CI. WAMR under cosmocc-on-Windows is unproven
and untested on this path — building with WASM enabled here is not validated,
and is not the same configuration CI exercises.

> **This build wedges intermittently — roughly 1 run in 3.** The compiler stops
> emitting output and sits at ~0 CPU indefinitely. It is not a hang in *your*
> setup and not something the flags above avoid: it is an open defect, tracked
> in [#462](https://github.com/artalis-io/hull/issues/462), and it has been seen
> on three different translation units. If a build goes silent for several
> minutes, interrupt it and re-run — `make` resumes from the objects already
> built, so a retry is cheap. CI catches this with a watchdog that fails after
> five minutes of silence rather than waiting out the job timeout.

This path is exercised in CI by `.github/workflows/windows-source-build.yml`,
whose header carries the full evidence and history.

## What it does

- Resolves the latest official stable release from `artalis-io/hull` (drafts and
  prereleases are never selected for "latest").
- Downloads only the official `hull-cosmo` asset and the `hull.sha256` manifest
  (an upgrade with a signature-capable existing Hull also downloads
  `hull.sha256.sig` for the Ed25519 check below).
- Verifies the artifact's SHA-256 against the exact manifest entry BEFORE
  installing (a mismatch, or a missing / duplicate entry, aborts).
- Installs `hull.com` atomically (a failed install never destroys a previous
  Hull), and adds only the install directory to your user `PATH`
  (`HKCU\Environment`), idempotently and without touching the machine `PATH`.

## Options

```powershell
.\install.ps1 -Version v0.14.0     # install a specific release (default: latest stable)
.\install.ps1 -Prefix D:\tools\Hull # install elsewhere (default: %LOCALAPPDATA%\Programs\Hull)
.\install.ps1 -Force               # replace an existing install
.\install.ps1 -DryRun              # print the plan; write nothing
.\install.ps1 -NoPath              # do not modify PATH
.\install.ps1 -Uninstall           # remove the installer-managed hull + its PATH entry
```

`-Uninstall` removes only the executable this installer placed and only the PATH
entry it added; it retains your Hull state under `%USERPROFILE%\.hull` (tools,
caches, application data). Both Windows PowerShell 5.1 and PowerShell 7 are
supported, and paths containing spaces work.

## Trust and verification

A checksum downloaded from the same channel proves integrity, not authenticity:
it confirms the bytes match the manifest, but a compromised release channel could
change both together. Hull's authenticity root is its **Ed25519 release
signature** (`hull.sha256.sig`). Its public key is source-controlled in
`include/hull/release.h` and embedded in every Hull binary, so a Hull you already
trust can verify a new release WITHOUT fetching a key from the current release
channel. That continuity, not the key being secret or off-GitHub, is what makes
the signature meaningful.

- **First install** proceeds under bootstrap trust: HTTPS + GitHub plus the
  mandatory SHA-256 check.
- **Upgrades** are stronger: if the Hull you already trust supports
  `verify-release`, the installer runs that Ed25519 check before replacing the
  binary and aborts on a signature failure (or an ambiguous / timed-out
  verifier), rather than silently downgrading to checksum-only.

### Verifying the signature

The value of `hull verify-release` depends on WHICH Hull runs it:

- **A pre-existing, already-trusted Hull** verifying a new release gives
  **continuity**: the new assets are signed by the same key your trusted Hull
  already carries, independent of the candidate release's own binary. This is the
  upgrade check the installer performs automatically.

  ```powershell
  # run with a Hull you already trust, NOT the one you just downloaded:
  hull verify-release hull.sha256 hull.sha256.sig
  ```

- **The freshly downloaded Hull** verifying its own release with its own embedded
  key is **consistency evidence, not independent authentication**: a tampered
  release could ship a matching key and signature together. Treat a pass as
  "internally consistent", not "authenticated".

- **Independent first-install checks** do not rely on the candidate binary at
  all. Use Sigstore/Rekor, GitHub attestation, or compare the release public-key
  fingerprint through a separately trusted channel:

  ```powershell
  # Sigstore keyless signature + Rekor transparency log (no gethull-managed key):
  cosign verify-blob hull.sha256 --certificate hull.sha256.cosign.pem --signature hull.sha256.cosign.sig

  # SLSA build provenance for a specific asset:
  gh attestation verify hull-cosmo --repo artalis-io/hull
  ```

See [`security.md`](security.md) for the full signature-verification chain.

## Upgrading from v0.13.0 (one-time manual step)

The Windows self-update fix ships in v0.14.0, so a v0.13.0 binary cannot install
it: v0.13.0's updater tries to overwrite its own running `.exe`, which Windows
refuses. To move from v0.13.0 to v0.14.0 on Windows, close any running Hull and
run `install.ps1` once (or download `hull-cosmo` from the v0.14.0 release and
replace your `hull` manually while it is not running). From v0.14.0 onward,
`hull update` self-updates normally on Windows.
