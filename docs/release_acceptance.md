# Release acceptance run

The acceptance run gates a release candidate before it is promoted to a final
release. It has two phases, each a checked-in CI workflow:

- **Automated, read-only stage** (`.github/workflows/release-acceptance.yml`):
  verifies the published RC and the maintainer-prepopulated staging mirror(s),
  and produces an acceptance report. It never mutates a release.
- **Windows acceptance stage** (`.github/workflows/release-acceptance-windows.yml`,
  logic in `tests/acceptance/windows/*.ps1`): runs the real Windows behavior that
  cannot be proven on other hosts, all as a freshly-created standard (non-admin)
  user with Developer Mode disabled. Fail-closed unless the child token is proven
  non-admin, Developer Mode is off, and control symlink creation is denied
  (`preconditions.ps1`). It proves the honest before/after self-update contract:

  - the **previous** APE (`v0.13.0`, which predates the deferred-swap fix) MUST
    FAIL to self-update on Windows, at the atomic-rename of the running binary
    (`old_update_fails.ps1`) - the exact bug the release fixes;
  - the **candidate** MUST self-update successfully via the deferred rename-aside
    swap, force-reinstalling the RC over itself through the RC staging repo
    (`self_update.ps1`);
  - an ACL-induced mid-swap **rollback**, run from the candidate, proven to reach
    the install-step failure (not an earlier stage) and cleanly restore the
    candidate (`rollback_acl.ps1`);
  - `extras.ps1` - a spaces-path self-update from the candidate, non-admin
    `hull tools install cosmocc`, a Lua and a JS `/ping` build+serve, a nested
    local-module build+serve, and doctor / tools-list / agent-tools agreement;
  - an optional **downgrade** from the candidate to the previous release via the
    previous staging repo (`downgrade.ps1`), proving target-version independence
    of the fixed updater. The installed `v0.13.0` lacks the startup sweeper, so
    the `<self>.old` residue is a recorded, expected pre-fix limitation and is
    cleaned explicitly rather than claimed as auto-swept.

  The whole stage is read-only with respect to releases.

Both phases must pass before the final tag is created.

## Why staging repositories

`hull update` always reads a repository's `/releases/latest` endpoint. `--repo`
changes *which* repository, but it cannot select a tag or a prerelease. So to
exercise a real Windows self-update against a candidate, the candidate's signed
assets are mirrored into a dedicated **staging repository** as *that* repo's
latest **non-prerelease**, and the Windows job runs
`hull update --repo=<staging>`.

Two small staging repositories are used so upgrade and downgrade each have a
stable `/releases/latest` without mutating release state during the run:

- `<staging_rc_repo>` - latest mirrors the RC (e.g. `v0.14.0-rc1`).
- `<staging_prev_repo>` - latest mirrors the previous signed release
  (e.g. `v0.13.0`), for downgrade.

## Maintainer preconditions (manual, auditable - not done by CI)

1. Publish `v0.14.0-rc1` as an official **prerelease** on the main repo (the
   tag-triggered release workflow builds + signs + publishes the four binaries,
   `hull.sha256`, and `hull.sha256.sig`).
2. Mirror the **exact** signed RC assets, unchanged, into `<staging_rc_repo>` as
   its latest **non-prerelease**; mirror the exact signed `v0.13.0` assets into
   `<staging_prev_repo>` likewise. The checked-in helper does this with your own
   `gh` auth (no persistent cross-repo token), verifying the official assets
   before mirroring and refusing to clobber an existing staging release:

   ```
   tests/acceptance/populate_staging.sh artalis-io/hull v0.14.0-rc1 \
       artalis-io/hull-release-rc-staging   0.14.0-rc1
   tests/acceptance/populate_staging.sh artalis-io/hull v0.13.0 \
       artalis-io/hull-release-prev-staging 0.13.0
   ```

   Both staging repositories must be **public** so unauthenticated
   `hull update --repo=` works, matching the real end-user path.
3. The acceptance workflows are **read-only**: they never create, modify, or
   delete a tag, release, or asset, and never alter "latest". `populate_staging.sh`
   is the single deliberately-mutating step and is run by the maintainer, not
   CI. Removing the staging releases after final promotion is likewise a manual
   maintainer step (`gh release delete <tag> --repo <staging> --cleanup-tag`).

## Fail-closed verification

The automated read-only stage fails unless **all** hold (per repo checked):

- every mirrored asset exists;
- each staging asset's SHA-256 **exactly** matches the official asset;
- `hull.sha256.sig` verifies (against the source-controlled release pubkey when
  extractable, else the candidate's embedded key);
- the staging release is **not** a prerelease **and** is returned by
  `/releases/latest`;
- the candidate binary reports the expected version (`0.14.0-rc1`).

It also validates the SBOM (well-formed, core components present) and reports the
delta vs the previous release, runs the `install.sh` dry-run and the signed-tool
release smoke, and confirms the RC commit's reproducible-build checks are green.

## Running the automated read-only stage

```
gh workflow run "Release acceptance (automated, read-only)" \
  -f rc_tag=v0.14.0-rc1 \
  -f expect_version=0.14.0-rc1 \
  -f staging_rc_repo=<org>/<staging-rc> \
  -f staging_prev_repo=<org>/<staging-prev>
```

The run uploads `acceptance-report-automated` (a Markdown report + SBOM delta)
for review and for the Windows stage to reference.

## Running the Windows stage

```
gh workflow run "Release acceptance (Windows, read-only)" \
  -f staging_rc_repo=<org>/<staging-rc> \
  -f staging_prev_repo=<org>/<staging-prev> \
  -f expect_version=0.14.0-rc1
```

It uploads `windows-acceptance-evidence` (per-check evidence: the non-admin
token proof, Developer-Mode state, the denied symlink, the self-update residue
sweep, and the ACL rollback reaching the mid-swap install failure).

## Promotion

Only after both phases are green: create the final `v0.14.0` tag (fresh signed
artifacts), re-run the smoke checks against the final release, update
`install.sh` / the site / README to advertise `0.14.0`, and remove the staging
releases.
