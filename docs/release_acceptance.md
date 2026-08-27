# Release acceptance run

The acceptance run gates a release candidate before it is promoted to a final
release. It has two phases, each a checked-in CI workflow:

- **Phase 3a - automated, read-only** (`.github/workflows/release-acceptance.yml`):
  verifies the published RC and the maintainer-prepopulated staging mirror(s),
  and produces an acceptance report. It never mutates a release.
- **Phase 3b - Windows acceptance** (a `windows-latest` job, added separately):
  runs the real Windows behavior that cannot be proven on other hosts -
  non-admin cosmocc install, a real self-update, and ACL-induced rollback.

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
   its latest **non-prerelease**.
3. Mirror the exact signed `v0.13.0` assets into `<staging_prev_repo>` as its
   latest non-prerelease.
4. The acceptance workflows are **read-only**: they never create, modify, or
   delete a tag, release, or asset, and never alter "latest". Removing the
   staging releases after final promotion is likewise a manual maintainer step.

## Fail-closed verification

Phase 3a fails unless **all** hold (per repo checked):

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

## Running Phase 3a

```
gh workflow run "Release acceptance (automated, read-only)" \
  -f rc_tag=v0.14.0-rc1 \
  -f expect_version=0.14.0-rc1 \
  -f staging_rc_repo=<org>/<staging-rc> \
  -f staging_prev_repo=<org>/<staging-prev>
```

The run uploads `acceptance-report-automated` (a Markdown report + SBOM delta)
for review and for the Windows phase to reference.

## Promotion

Only after both phases are green: create the final `v0.14.0` tag (fresh signed
artifacts), re-run the smoke checks against the final release, update
`install.sh` / the site / README to advertise `0.14.0`, and remove the staging
releases.
