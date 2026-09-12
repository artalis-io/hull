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
bearing and were not obvious; see the notes after the block.

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

  # cosmo/bin LAST: it ships its own `make`, which must not shadow MSYS2's.
  export PATH="$PATH:$PWD/cosmo/bin"

  make CC=cosmocc HL_OPT=-O0 HL_ENABLE_WASM=0 -j2
)
```

The `PATH` above is scoped to that subshell, which is the point, but it means
a later rebuild needs it again:

```sh
export PATH="$PATH:$PWD/cosmo/bin"   # from the repo root
make CC=cosmocc HL_OPT=-O0 HL_ENABLE_WASM=0 -j2
```

**`HL_OPT=-O0`** is not a preference. cosmocc's gcc wedges on Hull's largest
translation units at higher levels on Windows, so `-O0` is the only level
observed to complete. It also means this build cannot catch bugs that only
appear under optimization; those stay covered by the Linux and macOS CI jobs.

**`HL_ENABLE_WASM=0`** matches CI. WAMR under cosmocc-on-Windows is unproven
and untested on this path. Building with WASM enabled here is not validated
and is not the same configuration CI exercises.

> **This build wedges intermittently, roughly 1 run in 3.** The compiler stops
> emitting output and sits at ~0 CPU indefinitely. It is not a hang in *your*
> setup, and no flag above avoids it: it is a **cosmocc bug on Windows**, not a
> Hull one, tracked in [#462](https://github.com/artalis-io/hull/issues/462).
>
> Measured: `cc1` blocks on about **12% of compiles of a large translation
> unit** (established over 250 solo compiles of `vendor/sqlite/sqlite3.c`;
> `vendor/quickjs/quickjs.c` does the same). It needs neither parallelism
> (`-j1` wedges too) nor any particular temp directory (relocating it changed
> nothing).
>
> **If a build goes silent for several minutes, interrupt it and re-run.**
> `make` resumes from the objects already built, so a retry costs a fraction of
> a build, and the odds of wedging twice in a row are small. CI does exactly
> this automatically, retrying a stall up to three times.

This path is exercised in CI by `.github/workflows/windows-source-build.yml`,
whose header carries the full evidence and history.

## What works on Windows, and what does not

Windows runs the Cosmopolitan APE build. A fat APE cannot force-load a native
static archive, so every subsystem that ships as a **composable feature** is
unavailable there. Everything the cosmo base compiles in works normally.

| Capability | Windows | Why |
|---|---|---|
| Lua and JS runtimes, `fs`, `crypto`, `http`, `time`, `env` | yes | in the cosmo base |
| SQLite (`db`, migrations, session/outbox/idempotency/rbac/search) | yes | cosmo compiles SQLite in |
| WASM compute (`compute.*`) | yes, interpreted | cosmo keeps WASM in-base |
| Terminal UI (`hull.tui`) | yes | `HL_ENABLE_TUI` defaults to 1 on cosmo |
| `hull build` of an APE app | yes | needs `cosmocc`; see below |
| PostgreSQL (`postgres://`) | **no** | native-only feature |
| MySQL / MariaDB (`mysql://`, `mariadb://`) | **no** | native-only feature |
| DuckDB (`duckdb://`) | **no** | native-only feature |
| GPU compute (`gpu.*`) | **no** | native-only feature |
| AOT-compiled compute | **no** | `wamrc` is not published for cosmo |
| Kernel sandbox (pledge/unveil) | **no** | see below |

If an app needs a network database or GPU, run it on a native build
(`hull-linux-x86_64`, `hull-linux-aarch64`, `hull-darwin-arm64`), where
`hull feature install <name>` and `hull build --with=<name>` work. Building
Hull from source on Windows with `HL_ENABLE_POSTGRES=1` or `HL_ENABLE_MYSQL=1`
also works, because those backends are pure C with no vendored engine; DuckDB
and GPU are not viable that way.

### Compute runs interpreted

`wamrc`, the WAMR AOT compiler, is not published for cosmo (it needs LLVM, which
is too large to bundle into a fat APE). `compute.call` is correct on Windows but
runs through the interpreter, so compute-heavy workloads are slower than on a
native host with `hull tools install wamrc`. Building `wamrc` from source with
`make wamrc` is the alternative.

### There is no kernel sandbox on Windows

Cosmopolitan's `pledge()` and `unveil()` return 0 and do nothing on Windows
(measured with cosmocc 4.0.2 on Windows 11: `pledge("stdio", NULL)` returns 0
and a following `socket()` still succeeds). Startup logs one warning saying so.
Hull's capability layer, the manifest allowlists for `fs`, `env` and `hosts`,
still applies in full, and it is the only enforcement boundary on this host.
Treat a Windows deployment as capability-sandboxed but not kernel-sandboxed.

## Exit codes on Windows (read this before scripting `hull`)

**On Windows, a POSIX-style shell cannot tell whether `hull` succeeded.** In
MSYS2, Git Bash and Cygwin, a *failing* `hull` command reports success: `&&`
runs the next command anyway and `set -e` does not abort.

```sh
# In Git Bash / MSYS2 on Windows. `hull build` FAILS here, and yet:
hull build ./app && echo "shipped"      # prints "shipped"
set -e; hull build ./app; echo "reached"  # prints "reached"
```

This is not a Hull bug and Hull cannot work around it. Every Cosmopolitan APE
on Windows - which is what `hull.com` and the apps it builds are - reports its
exit status **shifted left by 8**, i.e. the raw `wait()`-style status rather
than the exit code. A two-line C program built with `cosmocc` does the same
thing. Tracked upstream as
[jart/cosmopolitan#1521](https://github.com/jart/cosmopolitan/issues/1521).

| `hull` intends | PowerShell `$LASTEXITCODE` | cmd `ERRORLEVEL` | MSYS2 / Git Bash `$?` |
|---|---|---|---|
| `0` (success) | `0` | `0` | `0` |
| `1` (failure) | `256` | `256` | **`0`** |
| `2` (failure) | `512` | `512` | **`0`** |

Success is reported correctly everywhere, which is exactly why this is easy to
miss: a green run behaves normally and only failures are swallowed. The value
is always `code << 8`, whose low byte is `0` for any code below 256 - so a
shell that keeps only the low byte sees `0`.

### What to do

**PowerShell** - works, if you test `$LASTEXITCODE` rather than `$?`:

```powershell
hull build .\app
if ($LASTEXITCODE -ne 0) { throw "hull build failed" }
```

Do **not** use `$?` here. Measured on Windows 11 with cosmocc 4.0.2: after a
failing `hull`, `$LASTEXITCODE` is `256` but `$?` is still `True`.

**cmd.exe** - works, `if errorlevel 1` triggers correctly:

```bat
hull build .\app
if errorlevel 1 (echo hull build failed & exit /b 1)
```

**MSYS2 / Git Bash / Cygwin** - the status is unusable. Check for the
**artifact** instead, which is what Hull's own CI does:

```sh
hull build ./app
test -f ./app/app.com || { echo "hull build failed"; exit 1; }
```

For commands that produce no file, check the output instead - for example
`hull doctor --json` and test a field, rather than trusting the status.

### Scope

This affects every `hull` subcommand on Windows, and every app `hull build`
produces there, since both are APEs. It does not affect Linux, macOS, or the
BSDs. Windows CI that shells out to `hull` from bash should assert artifacts or
output; a bare `hull ... && ...` chain is not a check.

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
