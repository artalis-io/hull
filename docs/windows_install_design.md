# Windows-native installation and trust UX (design record)

Design and audit for making Hull natural to install, upgrade, verify, and remove
on Windows without admin rights or Developer Mode. This is Slice A of a
separately-reviewed initiative; it freezes the trust model, the install
directory / PATH convention, the `install.ps1` CLI contract, the Winget/Scoop
shape, and the Authenticode experiment plan. No implementation lands in this
slice.

Scope guard: this initiative does not begin or modify BuildContext, does not
create tags/releases/external submissions/secrets/certificates/tokens, does not
modify the published v0.14.0 assets, and does not weaken checksum/signature
verification. Broad permanent Windows product CI is a separate next initiative;
only installer-specific Windows tests belong here.

## 1. Audit evidence (current behavior)

### 1.1 `install.sh` (the POSIX installer, 309 lines)

- **Knobs** (all env vars): `HULL_VERSION` (default `latest`), `HULL_PREFIX`
  (default `$HOME/.local/bin`, or `/usr/local/bin` as root), `HULL_FORCE`,
  `HULL_DRY_RUN`, `HULL_FLAVOR` (`auto` / `cosmo` / `native`), `HULL_REPO`
  (default `artalis-io/hull`).
- **Latest resolution:** GETs `api.github.com/repos/<repo>/releases/latest` and
  extracts `tag_name`. The `/releases/latest` endpoint already excludes drafts
  and prereleases, so "latest" is implicitly the newest stable release. The
  installer does not re-filter, because the endpoint guarantees it.
- **Verification:** SHA-256 only. It fetches `hull.sha256`, greps the line for
  the selected asset (` <asset>$`), compares to a locally computed digest, and
  aborts on mismatch. It does not fetch or verify `hull.sha256.sig`. This is
  deliberate: `install.sh` is SHA-256-only over TLS, and the Ed25519 signature
  is what `hull update` verifies on subsequent self-updates once the user
  already trusts the first install.
- **Install:** downloads into a unique `mktemp -d` dir with a cleanup `trap`,
  then `mv` into place + `chmod +x`. Refuses to overwrite an existing `hull`
  unless `HULL_FORCE=1` (interactive prompt when a tty, hard refuse otherwise).
- **PATH:** it only HINTS. It prints an `export PATH=...` / `fish_add_path`
  suggestion when the prefix is not already on `PATH`; it never edits any rc file
  or environment.
- Resolves the latest release dynamically, so it does not pin a version and does
  not need editing per release.

### 1.2 Release assets, `hull.sha256`, `hull.sha256.sig`

- The v0.14.0 release publishes 58 assets. The four first-class binaries are
  `hull-linux-x86_64`, `hull-linux-aarch64`, `hull-darwin-arm64`, and
  `hull-cosmo` (the Cosmopolitan APE, the Windows artifact).
- `hull.sha256` is a standard checksum file: one line per asset,
  `<64-hex-lowercase><two spaces><asset-name>`. It covers all 54 hashable
  assets (not only the four binaries), so a consumer must select the exact line
  for its asset by an anchored name match, reject a missing entry, and reject
  duplicates.
- `hull.sha256.sig` is a 128-hex-char Ed25519 signature (64 raw bytes) over the
  bytes of `hull.sha256`, with a trailing newline (129 bytes on disk).
- The release also carries `hull.sha256.cosign.sig` + `.cosign.pem` (Sigstore
  keyless / Fulcio) and SBOMs. Sigstore/Rekor + SLSA provenance are produced by
  `release.yml`.

### 1.3 Self-update / self-replacement (v0.14.0)

- `hl_release_io_self_replace` (`src/hull/release_io.c`): tries an atomic
  `rename(new, self)` first (works on POSIX); on Windows a locked running `.exe`
  makes that fail, so it falls back to a deferred rename-aside swap: move the
  running binary to `<self>.old`, install `<self>.new` at the original path, and
  roll back from `<self>.old` on failure. `<self>.old` lingers until the process
  exits and is swept on the next startup by `hl_release_io_cleanup_stale_self`.
- The Windows upgrade limitation this creates: a v0.13.0 Windows binary predates
  this fix and cannot self-update (its updater fails at the atomic replace of the
  running image). v0.13.0 Windows users must download `hull-cosmo` from a
  v0.14.0+ release and replace `hull` once manually while it is not running;
  updates work normally from v0.14.0 onward. This is recorded in the changelog
  and [`release_acceptance_v0.14.0.md`](release_acceptance_v0.14.0.md).

### 1.4 Release public key + verification commands

- The embedded release public key `HL_RELEASE_PUBKEY_HEX`
  (`include/hull/release.h`) is a real key today:
  `31ac1195c1be4030d45275256808b3e4c7468257cd255bc44c2ea2ef6c59886b`.
- `hull verify-release <manifest> <signature> [--pubkey <hex>]` verifies an
  Ed25519 signature over a manifest against the embedded key (or an explicit
  override). Exit 0 = valid, 1 = invalid / placeholder. `hull update` uses the
  same verification internally before installing a downloaded release.

### 1.5 Current Windows docs + shell surface

- Windows appears in [`cosmocc_install.md`](cosmocc_install.md) (the
  build-toolchain side) and [`agent_guide.md`](agent_guide.md), plus the README
  note that "as of v0.10.0 a cosmo `hull` + `hull tools install cosmocc` builds
  on stock Windows." There is no Windows install quick-start and no `install.ps1`
  / Winget / Scoop today (only a roadmap mention).
- Shell completions ship for bash / zsh / fish (`completions/`); there is no
  PowerShell completion. That is out of scope here but noted as a later UX gap.

### 1.6 APE execution on Windows (learned during v0.14.0 acceptance)

- Windows will not execute a PE/APE saved without a recognized executable
  extension. The download asset is named `hull-cosmo` (no extension); to run it
  as `hull`, it must be installed as `hull.com` (or `hull.exe`). With the install
  directory on `PATH`, `hull` then resolves via `PATHEXT`. Both the Windows
  acceptance and the release smoke install/run the APE as `hull.com`.

## 2. Trust model (explicit)

Each layer provides a specific, bounded guarantee. The design must not conflate
them, and in particular must not claim that a checksum downloaded from the same
channel is an independent authenticity proof.

| Layer | What it proves | What it does NOT prove |
|-------|----------------|------------------------|
| **HTTPS + GitHub** | The bytes were served by GitHub for this repo/release over an authenticated TLS channel; transport was not tampered in flight. | Nothing about the artifact across time, and nothing if the GitHub account/release itself is compromised. It authenticates the channel, not the publisher's intent. |
| **SHA-256 (`hull.sha256`)** | The downloaded asset matches the digest in the manifest fetched from the same release. Integrity of the asset relative to the manifest. | NOT independent authenticity. The manifest travels the same channel as the asset, so a channel/release compromise can swap both consistently. SHA-256 alone is an integrity check, not a root of trust. |
| **Ed25519 release signature (`hull.sha256.sig`)** | The manifest (hence every hash in it) was signed by Hull's offline release key, whose public half is embedded in the source (`HL_RELEASE_PUBKEY_HEX`) and is NOT hosted on GitHub. This is the channel-independent root of trust. | It is only as good as the verifier: verifying it requires a hull binary you already trust or the pubkey obtained from a trusted source. A freshly-downloaded hull verifying its own release is circular and is not an independent proof. |
| **Sigstore / Rekor (`*.cosign.*`)** | A keyless (Fulcio) signature over the artifacts, publicly logged in the Rekor transparency log; independently and offline verifiable, binding the artifact to the CI identity that built it. | Requires the user to run `cosign verify-blob` against Rekor; not performed by the installer. |
| **SLSA provenance** | Build provenance (source commit, builder identity) attesting how the artifact was produced. | Supply-chain provenance, not a user-facing execution-trust signal. |
| **Authenticode (possible, Slice E)** | OS-level "this executable is signed by X", and, only with an EV certificate or accrued reputation, reduced SmartScreen friction. | Not an integrity proof of Hull's content, and a self-signed cert yields no SmartScreen reputation. Feasibility on an APE is unproven (see 6). |

Consequences the installer must honor (ratified):

- SHA-256 verification against the exact `hull.sha256` entry is mandatory and
  fail-closed on missing / duplicate / malformed / mismatched entries. A
  same-channel SHA-256 protects against corruption and a mismatched asset, not
  against a compromised release channel.
- The installer never treats a newly-downloaded executable as its own root of
  trust. No novel cryptography in PowerShell: SHA-256 uses the .NET
  `System.Security.Cryptography` primitives; Ed25519 / Sigstore verification is
  delegated to `hull verify-release` (a pre-existing trusted hull) and to
  `cosign`, never re-implemented in the script.
- **Upgrade path (three explicit outcomes, no silent downgrade):**
  - a pre-existing `hull` that supports `verify-release` is invoked to check
    `hull.sha256.sig` BEFORE replacement, and its verification SUCCEEDS -> proceed;
  - the verifier is present and its verification FAILS (bad signature, or the
    invocation itself errors) -> ABORT the install; never fall back to
    checksum-only after a failed verifier invocation;
  - the pre-existing `hull` is older and lacks `verify-release` -> proceed, but
    CLEARLY REPORT that the install is under bootstrap trust (GitHub HTTPS plus a
    matched SHA-256), not an Ed25519-verified upgrade.
- On a first install there is no in-band independent root: the guarantees are
  HTTPS + GitHub + the mandatory SHA-256 (bootstrap trust), and the docs point
  the user at the out-of-band Ed25519 (`hull verify-release`) and Sigstore/Rekor
  verification for stronger assurance.

## 3. Install directory + PATH convention (frozen)

- **Install directory (frozen):** `%LOCALAPPDATA%\Programs\Hull\` with the binary
  as `hull.com`. This is the Windows per-user convention (writable without admin,
  Control-Panel-trackable), mirrors the POSIX `~/.local/bin` intent, and is
  consistent with how Winget portable and Scoop place per-user tools. The binary
  is `hull.com` so `hull` resolves via `PATHEXT`. `-Prefix` overrides it.
  - Alternatives considered and rejected: `%USERPROFILE%\.hull\bin` (Hull keeps
    state, not binaries, under `~/.hull`, and a hidden dot-dir is less
    Windows-idiomatic); `%USERPROFILE%\.local\bin` (exact POSIX mirror,
    non-idiomatic on Windows).
- **PATH (frozen).** Add ONLY the install directory to the CURRENT USER's
  `HKCU\Environment\Path`. The mutation must:
  - compare PATH components case-insensitively using normalized separators and
    normalized trailing separators, and add the directory only if absent
    (idempotent);
  - preserve unrecognized entries and expandable values (`%VAR%`) verbatim, and
    never reorder or truncate existing entries;
  - preserve the registry value type where practical (an existing
    `REG_EXPAND_SZ` stays `REG_EXPAND_SZ`; do not flatten expandable values);
  - never read, write, or rewrite the machine PATH (`HKLM`), and never elevate;
  - broadcast the environment-change notification (`WM_SETTINGCHANGE`, `Environment`)
    only after a successful mutation, so new shells pick it up;
  - make NO PATH change during `-DryRun` or when `-NoPath` is supplied.
  - Paths containing spaces are handled by exact-component comparison and by never
    quoting the directory into a single component.

## 4. `install.ps1` CLI contract (proposed, frozen at review)

Entry points:

```
irm https://gethull.dev/install.ps1 | iex
# or download-then-run:
Invoke-WebRequest https://gethull.dev/install.ps1 -OutFile install.ps1
.\install.ps1
```

Parameters:

| Parameter | Meaning | Default |
|-----------|---------|---------|
| `-Version <tag>` | Install a specific release (e.g. `v0.14.0`). | latest stable non-prerelease, non-draft |
| `-Prefix <path>` | Install directory. | `%LOCALAPPDATA%\Programs\Hull` |
| `-Force` | Overwrite an existing install (subject to the overwrite rule). | off |
| `-DryRun` | Print the plan; perform no writes (no download-to-final, no PATH edit). | off |
| `-NoPath` | Do not modify user PATH. | off |
| `-Uninstall` | Remove the installer-managed Hull executable + the exact PATH component this installer added; retain all `~/.hull` state. | off |

Frozen behavior:

- The upstream repository is FIXED to `artalis-io/hull`. The public installer
  exposes NO arbitrary repository override (unlike `install.sh`'s `HULL_REPO`).
- Validate `-Version` against a strict release-tag grammar (for example
  `^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$`) BEFORE constructing any URL, and
  reject anything else. `latest` resolves to the newest stable release.
- Resolve the requested official GitHub release; reject drafts and prereleases
  when resolving "latest".
- Download only the official `hull-cosmo` asset plus `hull.sha256` (+
  `hull.sha256.sig` when an upgrade check is possible) into a unique temp dir.
- Verify the asset SHA-256 against the exact `hull.sha256` entry BEFORE
  installation; fail closed on missing / duplicate / malformed / mismatched
  entries. SHA-256 is computed via .NET.
- On upgrade, if a safe existing `hull` can verify `hull.sha256.sig`
  (`hull verify-release`), use it as an additional check; never treat the newly
  downloaded binary as an independent root of trust.
- Windows PowerShell 5.1 must force TLS 1.2+
  (`[Net.ServicePointManager]::SecurityProtocol`); respect the default system
  proxy; never log credentials, proxy secrets, or temporary URLs.
- Install atomically (write `hull.com` to the temp dir, verify, then move into
  the prefix) with rollback; a failed install must leave any previous `hull`
  runnable. Clean temp files on success and failure.
- Overwrite rule: refuse to replace an existing install unless it is the same
  version or `-Force` is given.
- PATH edit is user-scoped, exact, idempotent, never truncating (section 3).
- Print concise next steps + verification commands, and state the v0.13.0 ->
  v0.14.0 one-time manual-replacement note.

`-Uninstall` behavior (frozen). No `-Purge` is added in this initiative; `~/.hull`
state, downloaded tools, caches, and application data are always retained.

- Remove only the known installer-managed Hull executable (`<prefix>\hull.com`).
- Remove the install directory only when it is empty after the executable is
  gone; never recursively delete an arbitrary custom `-Prefix`.
- Remove only the exact PATH component the installer manages (the resolved
  prefix), using the same case-insensitive normalized-separator comparison as
  insertion; leave every other PATH entry untouched.
- Be idempotent (uninstalling when nothing is installed is a clean no-op).
- Report that Hull state under `~/.hull` was retained and how to remove it
  manually.
- `-Uninstall` is mutually exclusive with the installation-only parameters
  (`-Version`, `-Force`, `-NoPath`); only `-Prefix` (to locate the managed
  install) and `-DryRun` compose with it. Combining it with an installation-only
  parameter is a usage error.

Installer-specific tests (read-only w.r.t. GitHub releases), run on a non-admin,
Developer-Mode-off Windows runner under both Windows PowerShell 5.1 and
PowerShell 7: parser/static checks; denied control-symlink precondition; default
install; explicit `-Version`; `-Prefix` with spaces; `-DryRun` writes nothing;
existing-install-without-`-Force`; forced replacement; checksum mismatch;
missing / duplicate checksum entry; interrupted-install rollback leaves the prior
binary runnable; PATH insertion user-scoped / exact / idempotent / non-corrupting;
`-Uninstall`; and the installed `hull` reports the expected version and runs a
minimal command.

## 5. Winget + Scoop readiness (shape only; built in Slice D)

Both consume the official immutable release URL and the pinned SHA-256, generated
from the release tag + `hull.sha256` (a generator, not hand-copied hashes, that
fails closed if metadata and hashes disagree). Neither is submitted to an
external index/bucket without explicit authorization.

- **Winget (portable):** `hull-cosmo` parses as a valid PE, so the `portable`
  installer type applies (Winget 1.3+; per-user, placed under
  `%LOCALAPPDATA%\Microsoft\WinGet`, alias on PATH via `%LOCALAPPDATA%\Microsoft\WinGet\Links`).
  Manifest pins `InstallerUrl` (immutable release URL) + `InstallerSha256`, sets
  a `Commands` / `PortableCommandAlias` of `hull`, and declares architecture
  honestly. **Architecture stays evidence-gated:** `Architecture: neutral` is
  preferred because the fat APE carries both x86_64 and aarch64, but this is NOT
  frozen until a real Winget install proves client compatibility (the fallback is
  duplicate x64/arm64 entries pointing at the same URL). **Do not claim** the
  extension-less `hull-cosmo` automatically becomes a runnable `hull.exe`; Slice D
  must test the actual portable-package alias/rename behavior. If Winget cannot
  reliably rename or shim the extension-less asset, the clean future solution is
  an ADDITIONAL immutable Windows-named release asset (for example
  `hull-windows.com`) produced and signed during a FUTURE release, never a
  packaging-time byte mutation of the published `hull-cosmo`. Validate with
  `wingetcreate` / `winget validate`.
- **Scoop:** a JSON manifest with `"url": "<immutable>/hull-cosmo#/hull.com"`
  (the `#/` rename saves the extension-less APE as `hull.com` so it runs),
  `"hash": "<sha256>"`, and `"bin": [["hull.com", "hull"]]` for the `hull` shim.
  This shape is appropriate subject to real Scoop install/invoke/uninstall tests
  in Slice D. No `persist` (Hull keeps mutable state under `~/.hull`, not the
  install dir). `autoupdate` only with a hash-checked url/hash template. No
  external bucket is created or mutated without authorization.

## 6. Authenticode feasibility (experiment plan; GO/NO-GO/DEFER in Slice E)

Strong prior from the audit + research, to be confirmed empirically on throwaway
copies (never on published assets):

- **Structural conflict.** Authenticode places the certificate table at the end
  of the file (verification expects it last). A Cosmopolitan APE also relies on
  structure at the end of the file (the ZIP end-of-central-directory that Cosmo
  scans backward from EOF for its embedded filesystem and multi-arch selection).
  A cert table appended after the ZIP EOCD, especially a large one, can push the
  EOCD outside Cosmo's backward-scan window and break APE self-load / arch
  selection on ALL platforms, or conversely the APE's trailing structure can make
  the signature non-canonical. This is the central risk.
- **Self-modification.** If the APE assimilates / self-modifies on first
  execution, any byte change invalidates the Authenticode signature.
- **Cross-platform execution.** The SAME signed bytes must still run on Linux and
  macOS (via the APE shell stub / loader); appended cert bytes may or may not be
  tolerated there.
- **Reproducibility.** Authenticode embeds the signer certificate and, if
  RFC-3161 timestamped (needed to outlive cert expiry), a timestamp
  countersignature, which is non-deterministic and breaks byte-for-byte
  reproducibility.
- **Ordering.** Authenticode mutates the file bytes, so it must be the innermost
  step: sign with Authenticode BEFORE computing `hull.sha256`, before the Ed25519
  manifest signature, and before Sigstore/SLSA over the artifact. It changes the
  published bytes.
- **SmartScreen.** A self-signed Authenticode certificate earns no SmartScreen
  reputation and still warns/blocks; only an EV certificate or accrued reputation
  reduces friction. Self-signed is not a production trust solution.

Experiments (throwaway copies only; no published-asset mutation, no production
cert, no CI secret):

1. Sign a copy of `hull-cosmo` with a temporary self-signed test certificate;
   confirm `signtool verify` / PowerShell `Get-AuthenticodeSignature` recognizes
   it.
2. Run the signed APE on Windows (standard user).
3. Run the identical signed bytes on Linux and macOS; confirm both x86_64 and
   aarch64 APE slices still select and run where runners exist.
4. Re-verify that Hull's own structures survive: the APE still loads its embedded
   VFS, and (independently) recompute its SHA-256 to characterize the byte delta.
5. Characterize reproducibility: sign twice (with and without timestamping) and
   diff the outputs.
6. Document the signing-order constraint against SHA-256 / Ed25519 / Sigstore /
   SLSA / publication.
7. Compare operationally and financially: EV certificate vs. Azure Trusted
   Signing vs. no Authenticode (rely on SHA-256 + Ed25519 + Sigstore + docs).

Deliverable: a GO / NO-GO / DEFER recommendation. **DEFER or NO-GO is the
expected outcome** unless the experiments prove ALL of the following:

1. Windows recognizes the signature.
2. Windows, Linux, and macOS all still execute the same signed APE.
3. APE architecture selection remains valid.
4. ZIP / APE structure remains valid.
5. The signing order integrates cleanly with Hull's SHA-256, Ed25519, Sigstore,
   and SLSA.
6. Reproducibility claims can be amended honestly (the byte-for-byte
   reproducibility policy is reconciled with whatever Authenticode does).
7. The chosen certificate provides meaningful SmartScreen value (a self-signed
   cert does not qualify).

Authenticode production signing is NOT added in Slice B, and no production
signing is added anywhere until certificate custody, CI authorization, ordering,
provenance, and a reproducibility policy are separately approved.

## 7. Slice plan and review gates

- **Slice A (this record):** audit + frozen trust model + install dir/PATH +
  `install.ps1` contract + Winget/Scoop shape + Authenticode experiment plan.
  Stop for review before any implementation.
- **Slice B:** `install.ps1` + installer-specific Windows tests only. It does NOT
  begin Winget, Scoop, or Authenticode execution, and adds no production signing.
  Stop for review.
- **Slice C:** installation-only docs/website delivery (README Windows quick
  start, agent/install/security docs, the v0.13.0->v0.14.0 note, verification
  instructions). Do not edit `install.sh` merely to create a diff. Stop for
  review.
- **Slice D:** repository-owned Winget + Scoop metadata + a generator + non-admin
  install/invoke/uninstall tests. No external submission without authorization.
  Stop for review.
- **Slice E:** Authenticode experiments + GO/NO-GO/DEFER. Design-only unless
  proven safe.

Each material change is its own branch/PR. Every slice honors: standard-user /
Developer-Mode-off, no machine-wide PATH, no elevation by default, spaces in
paths, Windows PowerShell 5.1 + PowerShell 7, contextual hyphens (no em-dashes),
and the existing documentation/lint and applicability-aware CI gates staying
green. Linux/macOS install behavior and the official release assets remain
unchanged.
