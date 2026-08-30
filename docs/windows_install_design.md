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

Consequences the installer must honor:

- SHA-256 verification against the exact `hull.sha256` entry is mandatory and
  fail-closed on missing / duplicate / malformed / mismatched entries.
- The installer never treats a newly-downloaded executable as its own root of
  trust. On an UPGRADE, if a safe pre-existing `hull` can independently verify
  `hull.sha256.sig`, that is used as an ADDITIONAL check. On a first install
  there is no in-band independent root: the guarantees are HTTPS + GitHub + the
  mandatory SHA-256, and the docs point the user at the out-of-band Ed25519 and
  Sigstore/Rekor verification for stronger assurance.
- No novel cryptography in PowerShell: SHA-256 uses the .NET
  `System.Security.Cryptography` primitives; Ed25519 / Sigstore verification is
  delegated to `hull verify-release` (a trusted hull) and to `cosign`, never
  re-implemented in the script.

## 3. Install directory + PATH convention (proposed, to be frozen at review)

- **Install directory:** `%LOCALAPPDATA%\Programs\Hull\` with the binary as
  `hull.com`. This is the Windows per-user convention (writable without admin,
  Control-Panel-trackable), mirrors the POSIX `~/.local/bin` intent, and is
  consistent with how Winget portable and Scoop place per-user tools. The binary
  is `hull.com` so `hull` resolves via `PATHEXT`.
  - Alternatives considered: `%USERPROFILE%\.hull\bin` (consistent with Hull's
    existing `~/.hull` state dir for tools/blobs/feature/platform, but a hidden
    dot-dir is less Windows-idiomatic and Hull keeps state, not binaries, under
    `~/.hull`); `%USERPROFILE%\.local\bin` (exact POSIX mirror, non-idiomatic on
    Windows). Recommendation: `%LOCALAPPDATA%\Programs\Hull`; `-Prefix` overrides.
- **PATH:** add the install directory to the CURRENT USER's PATH only, via
  `HKCU\Environment` (never machine-wide `HKLM`, never elevation). Insertion is
  idempotent with exact component matching (split on `;`, compare
  case-insensitively, add only if absent), never truncates or reorders existing
  entries, and broadcasts `WM_SETTINGCHANGE` so new shells pick it up.
  `-NoPath` skips this. Paths containing spaces are handled by never quoting into
  a single component and by exact-component comparison.

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
| `-Uninstall` | Remove the installed binary + the user-PATH entry this installer added; leave `~/.hull` state unless a future `-Purge` is designed. | off |

Frozen behavior:

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
  honestly. Open item for Slice D: the fat APE is genuinely multi-arch, so decide
  between a `neutral` architecture entry vs. duplicate x64/arm64 entries pointing
  at the same URL, and confirm Winget saves/aliases the extension-less asset as a
  runnable `hull.exe`. Validate with `wingetcreate` / `winget validate`.
- **Scoop:** a JSON manifest with `"url": "<immutable>/hull-cosmo#/hull.com"`
  (the `#/` rename saves the extension-less APE as `hull.com` so it runs),
  `"hash": "<sha256>"`, and `"bin": [["hull.com", "hull"]]` for the `hull` shim.
  No `persist` (Hull keeps mutable state under `~/.hull`, not the install dir).
  `autoupdate` only with a hash-checked url/hash template. No external bucket is
  created or mutated without authorization.

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

Deliverable: a GO / NO-GO / DEFER recommendation. No production signing is added
until certificate custody, CI authorization, ordering, provenance, and a
reproducibility policy are separately approved.

## 7. Slice plan and review gates

- **Slice A (this record):** audit + frozen trust model + install dir/PATH +
  `install.ps1` contract + Winget/Scoop shape + Authenticode experiment plan.
  Stop for review before any implementation.
- **Slice B:** `install.ps1` + installer-specific Windows tests. Stop for review.
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
