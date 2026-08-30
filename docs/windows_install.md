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

Or download the script first and run it (recommended if you want to read it
before running):

```powershell
Invoke-WebRequest https://gethull.dev/install.ps1 -OutFile install.ps1
.\install.ps1
```

This installs `hull.com` to `%LOCALAPPDATA%\Programs\Hull` and adds that
directory to your user `PATH`. Open a new terminal and run `hull doctor`.

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
