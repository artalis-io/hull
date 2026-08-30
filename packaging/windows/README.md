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

- **Scoop** - installed, invoked (`hull version`), and uninstalled as a
  freshly-created **standard (non-admin)** user with Developer Mode disabled.
  Scoop is per-user by design, so this is the complete fresh-standard-user
  package proof.
- **Winget** - the manifest is **validated** (`winget validate`) as that same
  standard user (the elevated orchestrator resolves `winget.exe` and hands the
  path down, since a fresh standard user cannot see the package to resolve it),
  and the **install / invoke / uninstall** run under the runner's own account.

  Winget boundary (precise): Winget's per-user App Installer MSIX is not
  provisionable for a freshly-created standard user in a `runas` session on the
  GitHub image (no loaded profile, so no package identity / execution alias), so
  CI does not prove a Winget *install* under a newly-created standard-user
  profile. The install path is exercised where App Installer has a valid
  identity - the runner account. Hull's Winget install is per-user portable scope
  and requests no elevation; the runner account's actual integrity (the GitHub
  image runs it **elevated / High**) is recorded in the evidence and is a
  runner-image property, **not** evidence that Hull requires administrator
  privileges (Scoop demonstrates the fully non-admin path). The runner account is
  in the Administrators group, so it is not a true non-admin account and is not
  described as one.

The whole job fails unless generator drift-check, manifest validation, hash
verification, install, invocation, and uninstall all succeed.

## Publishing (separate, authorized step - not done here)

- Winget: `wingetcreate submit` / a PR to `microsoft/winget-pkgs` (needs a
  verified publisher and maintainer authorization).
- Scoop: add `hull.json` to a Scoop bucket you control (a separate repo).
