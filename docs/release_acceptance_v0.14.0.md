# v0.14.0 release acceptance record

Durable, self-contained record of the acceptance evidence gathered before the
v0.14.0 release was closed out. It is intended to remain useful after the linked
GitHub Actions artifacts expire; every result below is reproducible from the
release commit + the published, signed assets.

For the acceptance *design* (the read-only workflows, the staging-repo model, and
the Windows stages), see [`release_acceptance.md`](release_acceptance.md).

## Release under test

| Field | Value |
|-------|-------|
| Tag | `v0.14.0` (full release, not a prerelease) |
| Release commit | `dfdf2510b8a53e740c36dfae87358ad1de90f911` |
| Published | 2026-08-29 |
| Release repo | `artalis-io/hull` |
| Source release pubkey (`HL_RELEASE_PUBKEY_HEX`) | `31ac1195c1be4030d45275256808b3e4c7468257cd255bc44c2ea2ef6c59886b` |

The compiled `hull` binary is byte-source-identical between `v0.14.0-rc1` and
`v0.14.0`: the only changes on `v0.14.0-rc1..v0.14.0` were acceptance PowerShell
scripts, docs, and the changelog (nothing that compiles into `hull`). So the
full non-admin Windows acceptance run against the rc1 `hull-cosmo` (below) and
the artifact smoke against the published v0.14.0 `hull-cosmo` (below) exercise
the same binary source.

## Published binary SHA-256 (from the signed `hull.sha256`)

| Asset | SHA-256 |
|-------|---------|
| `hull-cosmo` | `115e8965996a2a4bda2efa5d02513c79d9063b2f08a7eb94e88f6f76acfbecda` |
| `hull-linux-x86_64` | `b2a959c15206e58277d7da13f758d8cfe0e45affd7708f769e2a680de87bb521` |
| `hull-linux-aarch64` | `b610a14185a6a219f4f5bb41acea89250fcac44bf4fbc2ae270915fb82268bed` |
| `hull-darwin-arm64` | `4e104fc98c7d27db4973072a664675f20f51b78660a23ca4e96d2549e57097f6` |

`hull.sha256.sig` verifies against the source release pubkey above
(`hull verify-release hull.sha256 hull.sha256.sig --pubkey <pubkey>` returns OK),
and each binary's SHA-256 matches its `hull.sha256` line.

## Final-asset verification (published v0.14.0 assets)

Ran against the published release assets (host-independent: Ed25519 over the
manifest, SHA-256 over each binary):

- signature: `hull verify-release ... --pubkey <source pubkey>` returns OK;
- checksum: all four binaries match their `hull.sha256` lines (table above);
- version: the binary reports `hull 0.14.0`.

Release smoke (`tests/release_smoke.sh`, live GitHub trust chain against the
published release): 51 checks passed, 0 failed - `hull tools install wamrc` /
`zig`, `hull feature install duckdb` (live HTTPS download + SHA-256 verify), the
flavor/runtime-feature surface, and a platform-signature build + `--verify-sig`
serve.

## Windows artifact smoke (published v0.14.0 `hull-cosmo`)

The minimal Windows smoke of the published artifact, run on a GitHub-hosted
`windows-latest` (Windows Server 2025) runner as a freshly-created standard
(non-admin) user, driven by
[`.github/workflows/release-smoke-windows.yml`](../.github/workflows/release-smoke-windows.yml)
(logic in `tests/acceptance/windows/run-smoke.ps1` + `smoke.ps1`).

| Field | Value |
|-------|-------|
| Workflow run | <https://github.com/artalis-io/hull/actions/runs/33279886655> (ID `33279886655`) |
| Workflow commit SHA | `9e801483fde81a1c4f245873b5a1b13d3fd5c18b` (on `main`) |
| Evidence artifact | `windows-smoke-evidence` (retained on the run while available) |
| Result | `## RESULT: ALL SMOKE CHECKS PASSED` |

Environment assertions (fail-closed preconditions):

- token user `runnervmeef0v\hullsmoke`, **not** in `Administrators`;
- integrity level Medium (not elevated / High);
- Developer Mode disabled (`AllowDevelopmentWithoutDevLicense=0`);
- control symbolic-link creation **denied** (privilege-specific: the target was
  proven writable first, so the denial is the missing
  `SeCreateSymbolicLinkPrivilege`, not an unwritable path).

Per-check results (against the published `hull-cosmo`):

| Check | Result |
|-------|--------|
| checksum matches `hull.sha256`, verified BEFORE execution | OK (`115e8965...`) |
| `hull.sha256.sig` verified against the source release pubkey | OK (`hull verify-release: OK`) |
| reported version | OK (`hull 0.14.0`) |
| non-admin `hull tools install cosmocc` | OK |
| `hull build` + serve a Lua `/ping` app | OK (`pong`) |
| `hull build` + serve a JS `/ping` app | OK (`pong`) |

The smoke is read-only with respect to releases: it downloads from the official
release only and never updates, rolls back, mutates staging, or touches any
release, asset, or `latest`.

## Full non-admin Windows acceptance (source-identical rc1 `hull-cosmo`)

The complete before/after self-update contract (six phases) passed on the same
non-admin, Developer-Mode-off Windows environment against the rc1 `hull-cosmo`
(source-identical to v0.14.0, per the note above), driven by
[`.github/workflows/release-acceptance-windows.yml`](../.github/workflows/release-acceptance-windows.yml).

| Field | Value |
|-------|-------|
| Workflow run | <https://github.com/artalis-io/hull/actions/runs/33273285534> (ID `33273285534`) |
| Result | `## RESULT: ALL PHASES PASSED` |

Phases: preconditions; old v0.13.0 update MUST FAIL (the pre-fix
`atomic_write: rename ... failed` Windows bug that v0.14.0 fixes); candidate
self-update via the deferred rename-aside swap; ACL-induced mid-swap rollback;
extras (path-with-spaces update, non-admin cosmocc, Lua/JS `/ping`, nested
local-module built parity, doctor/tools/agent agreement); downgrade to the
previous release.

## Windows upgrade limitation (recorded)

A v0.13.0 Windows binary cannot self-update to v0.14.0: the Windows self-update
fix ships *in* v0.14.0, and v0.13.0's updater fails at the atomic replace of the
running executable. Windows users on v0.13.0 must download `hull-cosmo` from the
v0.14.0 release and replace `hull` once manually while it is not running; from
v0.14.0 onward `hull update` self-updates normally on Windows. This is also
called out in the changelog's v0.14.0 upgrade notes.
