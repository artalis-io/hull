# Windows e2e: what runs, what does not, and why

Before September 2026 no `tests/e2e_*.sh` had ever run on a Windows runner.
`windows-source-build.yml` stopped at the unit suites, and
`cosmocc-windows-e2e.yml` built on Ubuntu and only *consumed* the result on
Windows. 93 suites, none of them exercised on the platform whose path handling,
process spawning and filesystem semantics differ most.

This file records the last full sweep so the next reader starts from the map
rather than rebuilding it from run logs, which expire in 14 days.

## Last full sweep

**2026-09-15, against `main` @ `684476c5`** - all 93 targets measured, none
skipped by the harness.

| verdict | count |
|---------|------:|
| ok      | 81 |
| FAILED  | 10 |
| TIMEOUT |  2 |

The previous sweep (2026-09-13, before any fixes) was **58 ok / 34 FAILED /
1 TIMEOUT**. 24 suites were fixed in between; one regressed (see `e2e-linker`).

## Still failing

| suite | verdict | cause |
|-------|---------|-------|
| `e2e-agent` | FAILED | `hull agent request` cannot reach a dev server it believes it started (`failed (error 14)`). Undiagnosed. Its exit-status noise was removed separately, so this is now the only signal in the suite. |
| `e2e-build` | FAILED | Undiagnosed. ~650s, so it needs a dispatch of its own. |
| `e2e-cache-cosmo` | FAILED | Undiagnosed. Runs `make clean` + a full `EMBED_PLATFORM=cosmo` rebuild, so it MUST be probed alone - anything after it in the same job pays a from-scratch rebuild inside its own timeout. |
| `e2e-compiler-free` | TIMEOUT | Was FAILED at 19s before clang was installed on the runner; now does not finish in 180s. |
| `e2e-compute` | FAILED | Dies on `Error 143` (SIGTERM) after its assertions pass. Undiagnosed. |
| `e2e-feature-valkey` | FAILED | Probably the docker-cannot-run-linux-containers cause its sibling `e2e-valkey` had, but its log has not been read. |
| `e2e-linker` | FAILED | **A regression.** See below. |
| `e2e-project-discovery` | FAILED | Undiagnosed. |
| `e2e-project-discovery-lua` | FAILED | Undiagnosed. ~350s. |
| `e2e-smtp` | TIMEOUT | Did not finish in 600s. The only TIMEOUT in the first sweep too. |
| `e2e-smtp-link-seam` | FAILED | Regressed with `e2e-linker`, same root cause. |
| `e2e-tui` | FAILED | Needs a pty (`forkpty`). |

## Green but empty

Several suites are `ok` because they SKIP wholesale, and it is worth not
counting them as coverage:

- the five `feature-*` suites and `e2e-build-flavor` - cosmo keeps every
  composable subsystem in-base, so there is no feature/flavor axis to test on a
  fat APE;
- `e2e-musl`, `e2e-valkey` - docker on the Windows runner serves WINDOWS
  containers and cannot pull a linux image;
- `e2e-htmx-playwright-build` - the probe's hull has no embedded platform, so it
  cannot link the standalone binaries the suite drives.

Each names its reason in its own output.

## What broke things, in order of how often

1. **An APE's exit status is unreadable from a POSIX shell on Windows.** It
   arrives shifted left by 8 and MSYS2 / Git Bash / Cygwin keep the low byte, so
   `$?` is 0 for every outcome (jart/cosmopolitan#1521). This accounted for most
   failures, and for a quieter problem: assertions written `[ "$rc" -eq 0 ]` were
   PASSING without proving anything. `tests/lib/hull_rc.sh` recovers the real
   status; `hull_run2` additionally keeps stdout and stderr apart for checks that
   assert one is empty.
2. **A path hull prints is not spelled like the one the shell built.** The MSYS
   shell rewrites `/d/a/x` to `D:/a/x` (canonical UPPERCASE drive) on the way in,
   and `hl_host_normalize_path` faithfully returns `/D/a/x`. Hull is correct.
   `tests/lib/hull_path.sh` compares on the verbatim spelling first and only
   retries the drive-canonical one where an APE is involved.
3. **A fixture the host refuses to build.** `ln -s` silently COPIES without
   symlink privilege, so a containment test finds no violation to detect and
   reports containment BROKEN - the inverse of what happened. `chmod 000` does
   not deny reads. Guard on the fixture, not on a proxy like `id -u`.
4. **A probe that asks the wrong question.** `file(1)` calls an APE a "DOS/MBR
   boot sector", so a `*cosmo*|*APE*` match never fires (`hull_is_ape` reads the
   magic instead). `command -v docker` is not "docker can run a LINUX container"
   (`hull_docker_runs_linux` asks `docker info` for the daemon's OSType).

## The regression, and the lesson in it

`e2e-linker` went `ok -> FAILED`, and the cause was a change to the shared
runner rather than to the suite:

1. clang + lld were installed on the Windows runner so the wasm32 compute suites
   would stop skipping;
2. `e2e-linker` resolves lld from PATH and SKIPS when there is none - it had been
   green BY SKIPPING, and now it runs;
3. the lld link fails there and hull prints a hint containing BACKTICKS
   (`src/hull/linker_system.c`: ``use the self-contained `--linker=zig` ...``);
4. the suite interpolated that captured output into a nested `sh -c`, whose shell
   evaluated the backticks and executed them:

       sh: line 1: --linker=zig: command not found
       sh: line 1: hull: command not found

`e2e-smtp-link-seam` regressed the same way: its guard skipped when no native
`cc/gcc/clang` was on PATH, on the reasoning that a cosmo host carries only
cosmocc. Installing clang satisfied that check on a cosmo host.

**Adding a toolchain to a shared runner changes behaviour for every suite that
branches on its presence.** Six do - `compute_dev`, `compute_memops`,
`cross_build`, `linker`, `musl`, `smtp_link_seam`. Three were the target; all six
moved.

## Running a sweep

`windows-source-build.yml` -> Run workflow, with `probe_e2e` set to a
space-separated list of make targets. It never gates the job; the verdicts are in
the step output and the full per-suite logs upload as `hull-windows-e2e-probe-logs`.

Batching that works: ~15 fast suites per dispatch at a 180s cap (the job itself
is capped at 25 minutes, most of which is the hull build), `e2e-build` and
`e2e-cache-cosmo` each alone with 900s, and `e2e-smtp` + `e2e-project-discovery-lua`
together at 600s.

<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
