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
| `e2e-agent` | fixed | Was read as "cannot reach the dev server". It reached it fine - curl fetches `/health` immediately before. Keel's `error 14` is `KL_ERR_URL`, not a connect error: the shell rewrites the bare `/health` ARGUMENT into `C:/Program Files/Git/health` before hull runs, so the URL is malformed. Scoped `MSYS2_ARG_CONV_EXCL` for those calls; the error now names the URL it tried. |
| `e2e-build` | TIMEOUT | `make: *** wait: No child processes.  Stop.` at Step 0, before any assertion. make's own job control failing on this host, not a Hull defect and not something the suite does. Undiagnosed; ~650s, so it needs a dispatch of its own. |
| `e2e-cache-cosmo` | FAILED | **The target deletes its own prerequisite**, and not only on Windows. `mk/tests.mk` runs `$(MAKE) platform-cosmo` (which leaves `build/libhull_platform.{x86_64,aarch64}-cosmo.a`), then `$(MAKE) clean` (which is `rm -rf $(BUILDDIR)`), then a build needing those archives - hence `No rule to make target 'build/libhull_platform.x86_64-cosmo.a'`. No CI job runs `e2e-cache-cosmo` (0 references in ci.yml), which is why it went unnoticed. Unverified fix, because confirming it costs two full cosmo platform builds: drop the intervening `clean`, but note the config-sentinel may wipe `build/` on the flag flip anyway. Must still be probed ALONE - it wipes the shared build tree. |
| `e2e-compiler-free` | TIMEOUT | The `sh -c` injection below was what HUNG it; with that fixed it is a clean FAILED at 13s, and its dead cosmo guard (see cause 4) is why it ran at all. |
| `e2e-compute` | FAILED | Dies on `Error 143` (SIGTERM) after its assertions pass. Undiagnosed. |
| `e2e-feature-valkey` | FAILED | Probably the docker-cannot-run-linux-containers cause its sibling `e2e-valkey` had, but its log has not been read. |
| `e2e-linker` | FAILED | **A regression.** See below. Now guarded on `hull_is_ape`: a cosmo hull cannot link through a native lld. |
| `e2e-project-discovery` | 3 of 66 | Down from 9 after the exit-status fixes. The three left all turn on a live `hull dev --agent` session and the `kill(pid,0)` liveness check binding a published generation to it: `sidecars linger after dev exit`, `live matching PID IS served`, `post-stop read is standalone`. Process-lifetime semantics, undiagnosed. NOT related to `e2e-agent`'s failure, which looked similar and was not. |
| `e2e-project-discovery-lua` | FAILED | Undiagnosed. ~350s. |
| `e2e-smtp` | TIMEOUT | Did not finish in 600s. The only TIMEOUT in the first sweep too. |
| `e2e-smtp-link-seam` | FAILED | Regressed with `e2e-linker`, same root cause. |
| `e2e-tui` | 19 of 37 | "Needs a pty" is too glib - the pty WORKS. 18 pass, including every `tui_picker` case (enter, arrows, q, escape, both runtimes) and every not-a-tty refusal. What fails is uniformly the checks that assert on MID-SESSION RENDERED content: titles, panes, streamed child output, SGR mouse enable/disable. So the driver allocates a pty and captures a program's FINAL stdout, and what the TUI draws during the session does not arrive. Undiagnosed; a blanket skip would discard 13 genuine passes. |

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
4. **A guard that asks a question its input cannot answer.** This is the most
   dangerous class, because a broken guard reads exactly like a working one
   until something makes the suite run. Five found so far, all independent:

   | guard | why it never fired |
   |-------|--------------------|
   | `case "$(file "$HULL")" in *cosmo*\|*APE*)` | `file(1)` on Windows calls an APE a "DOS/MBR boot sector" |
   | `case "$($HULL version)" in *cosmo*)` | `version.c` prints `hull <version>` and no platform string, so it could not match on ANY build |
   | `command -v docker` | docker EXISTS on a Windows host, it just serves Windows containers |
   | `command -v cc \|\| gcc \|\| clang` | premised on "a cosmo host has no native compiler", which stopped being true when one was installed |
   | `[ -x "$DRIVE" ]` (PTY driver) | forkpty COMPILES under cosmo, so the driver builds and still cannot drive a terminal |

   The fix in every case was to ask about the thing itself rather than a proxy
   for it: `hull_is_ape` reads the binary's magic, `hull_docker_runs_linux` asks
   `docker info` for the daemon's OSType. A guard phrased as "is a tool
   present" is nearly always the wrong question - what a suite needs to know is
   whether the CONFIGURATION it requires exists.

   The same lesson applies to fixtures (cause 3): build it, then check it came
   out as required, rather than testing a precondition you believe implies it.

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

## The enforced tier

`windows-source-build.yml` no longer only probes. Since the sweep above, 87
suites are ENFORCED - the job fails if any of them regresses - in the same
three-tier shape the unit suites use, with one difference: an e2e target either
exits 0 or it does not, so there is no failure count to baseline.

| tier | meaning |
|------|---------|
| ENFORCED | must exit 0. 87 suites, ~26 minutes (measured: 1191s for the 81 the sweep timed, 348s for the six confirmed after). |
| KNOWN | expected to FAIL (`e2e-project-discovery`, `e2e-tui`). A KNOWN suite that starts PASSING also fails the job - that is how it gets promoted rather than quietly drifting. |
| PROBE | `workflow_dispatch` only, never gates. How a suite earns a place in either tier. |

**Not gated, deliberately**: `e2e-build`, `e2e-cache-cosmo`, `e2e-smtp`,
`e2e-project-discovery-lua`. Together ~30 minutes for four suites whose causes
are already recorded above; gating them would cost more than it protects.

**Enforcing a skip is still enforcing something.** Several ENFORCED suites pass
by skipping wholesale (the `feature-*` group, `build-flavor`, `musl`, `valkey`,
`htmx-playwright-build`, and the `spans-*` group where no wasm32 clang exists).
That is worth keeping - it catches the skip itself breaking - but 87 ENFORCED is
not 87 suites' worth of Windows coverage, and should not be read as such.

The gate is skipped on a dispatch that supplies `probe_e2e`, so diagnosing a
specific suite does not pay for the whole tier first.

## Running a sweep

`windows-source-build.yml` -> Run workflow, with `probe_e2e` set to a
space-separated list of make targets. It never gates the job; the verdicts are in
the step output and the full per-suite logs upload as `hull-windows-e2e-probe-logs`.

Batching that works: ~15 fast suites per dispatch at a 180s cap (the job itself
is capped at 25 minutes, most of which is the hull build), `e2e-build` and
`e2e-cache-cosmo` each alone with 900s, and `e2e-smtp` + `e2e-project-discovery-lua`
together at 600s.

<!-- SPDX-License-Identifier: AGPL-3.0-or-later -->
