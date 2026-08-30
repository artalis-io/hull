# Windows package metadata (Winget + Scoop)

Repository-owned package metadata for installing Hull on Windows via Winget
(portable) and Scoop, plus a fail-closed generator. These are **not submitted**
anywhere: publishing to `microsoft/winget-pkgs` or a Scoop bucket is a separate,
explicitly-authorized step.

The first-party installer is [`install.ps1`](../../install.ps1)
([user guide](../../docs/windows_install.md)); Winget/Scoop are conveniences for
users who already manage their tools that way.

## Layout

- `generate.sh` - the generator (the single source of the pinned hash).
- `templates/` - the manifest templates with `@VERSION@` / `@TAG@` /
  `@SHA256_LOWER@` / `@SHA256_UPPER@` placeholders.
- `winget/` - the generated Winget manifest set (`Artalis.Hull.yaml`,
  `Artalis.Hull.installer.yaml`, `Artalis.Hull.locale.en-US.yaml`), portable
  installer type, `Architecture: neutral` (the Cosmopolitan APE runs on x86_64
  and aarch64 from one file), the immutable release URL, a pinned uppercase
  SHA-256, and a `hull` command alias.
- `scoop/hull.json` - the generated Scoop manifest: the immutable release URL with
  a `#/hull.com` fragment rename (so the extension-less APE is a runnable
  `hull.com`), a pinned lowercase SHA-256, a `hull` shim, and hash-safe
  `autoupdate` (the hash is read from the release's `hull.sha256`). No `persist`
  (Hull keeps mutable state under `~/.hull`, not the install dir).

## Regenerating

```sh
# from the published release (downloads hull.sha256 over HTTPS):
packaging/windows/generate.sh --tag v0.14.0

# or from a local hull.sha256:
packaging/windows/generate.sh --tag v0.14.0 --sha256-file ./hull.sha256
```

The generator FAILS CLOSED: it errors if the `hull.sha256` manifest is missing,
if there is not exactly one `hull-cosmo` entry, or (with `--check`) if the
committed manifests do not match the tag + its `hull-cosmo` hash:

```sh
# drift gate (CI): the committed metadata must equal the generated output.
packaging/windows/generate.sh --tag v0.14.0 --check
```

## Testing

`tests/acceptance/windows/packaging/` exercises both package forms, driven by
`.github/workflows/packaging-windows.yml`. It is read-only with respect to
releases (it downloads the official `hull-cosmo`; it never submits or mutates a
package index/bucket).

- **Scoop is the actual fresh-standard-user installation proof** - installed,
  invoked (`hull version`), and uninstalled as a freshly-created **standard
  (non-admin)** user with Developer Mode disabled, with the download hash checked.
  Scoop is per-user by design.
- **Winget** - a non-gating **standard-user boundary probe** plus the Winget gate
  under the runner account:

  1. The fresh standard-user probe is **expected to fail, and only because
     Winget/App Installer cannot execute in that unprovisioned `runas` profile**
     (no package identity / execution alias without a loaded profile; the
     `Program Files\WindowsApps\...` binary is ACL-locked, `Access is denied`).
     This is the Winget provisioning model, **not** a Hull defect and **not** an
     install failure, so the probe records it and does not gate the job.
  2. **Schema validation (`winget validate`) runs under the runner account and is
     not presented as non-admin evidence** (Scoop is the fresh-standard-user
     proof, per the point above).
  3. **Full Winget install / invoke / uninstall remains non-elevated where
     possible, with hash verification enforced.** Hull's install requests no
     elevation and is per-user portable scope; on this image the only
     Winget-capable account is the runner (a fresh standard user cannot execute
     Winget at all), which the GitHub image runs **elevated / High** - a
     runner-image property recorded in the evidence, not a Hull requirement and
     not described as a true non-admin account. The install asserts winget's
     `verified installer hash` explicitly (a mismatch also fails winget itself).
     Installing from a *local, uncommitted* manifest additionally needs the
     `LocalManifestFiles` admin setting (a test-harness detail; a published Winget
     package needs no such setting).

The whole job fails unless the generator drift-check, the Winget manifest
validation + hash-verified install + invocation + uninstall (runner), and the
Scoop non-admin install + invocation + uninstall all succeed.

## Publishing (separate, authorized step - not done here)

- Winget: `wingetcreate submit` / a PR to `microsoft/winget-pkgs` (needs a
  verified publisher and maintainer authorization).
- Scoop: add `hull.json` to a Scoop bucket you control (a separate repo).
