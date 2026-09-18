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
| `e2e-build` | 22/22, build half SKIPPED | Steps 4-18 (build/sign/verify/inspect/eject) skip because a cosmo hull needs a compiler it can SPAWN and the cosmocc.zip driver on PATH is a `#!/bin/sh` script an APE cannot exec. **This is a DUPLICATION gap, not a coverage hole** - and an earlier version of this row implied otherwise. `hull build` on Windows IS tested, by the separate `windows-app-build` job: it fetches cosmocc *with busybox* (required to spawn it), builds the multi-arch platform, builds hull with it embedded, then runs `hull new` + `hull build` and EXECUTES the produced APE, asserting on its output. What e2e-build would add on top is sign/verify/inspect/eject, tamper detection, multi-file apps and the error cases. Closing it here was tried and reverted: installing the bundle alone flips `CAN_BUILD=1` and turns an honest skip into 42 failures, and adding the `make platform-cosmo` Step 0 that would fix those took **1200s without finishing** - which is exactly why `windows-app-build` is a separate job ("three builds where the sibling job does one... a wedge here turns the fast build+test signal red even though both had already passed"). If those extra assertions are wanted, they belong in THAT job, where the archives already exist. |
| `e2e-cache-concurrent` | **now PASSES 11/11, ENFORCED** | Was 8/9 with its premise unmet: of the 8 workers it races, 2 started, and in the mixed section 1 of 8 - the rest died on `failed to open database connection`. Two separate defects. (1) The suite could not SEE it: both sections defined success as "none of `Segmentation|Abort|panic|fatal` appeared", so dead workers scored zero matches and every cache invariant (one file per key, no zero-sized blobs, no leftover tmp) passed on a single writer's output while reporting 8 - precisely the properties that only mean something under real concurrency. It now asserts the workers reached `listening on`. (2) The real cause was a **Hull bug**, not a Windows one: `hl_cap_db_init` aborted the app if ANY pragma failed, and `PRAGMA journal_mode=WAL` takes an exclusive lock that concurrent openers of one fresh database contend for. The loser gets `SQLITE_PROTOCOL` - not a busy condition, so `busy_timeout` never covered it and retrying never cleared it. A tuning pragma losing a race killed the process. Pragmas are now classified by what failure means (required / WAL / WAL-only / tuning), so a lost race costs performance instead of startup. Measured: N=1 1/1, N=2 2/2, N=4 4/4, N=8 8/8 three times over. Windows only surfaced it - its SQLite VFS is stricter about the window, and hull's default DSN is the RELATIVE path `data.db`, so processes sharing a working directory share a database without choosing to. |
| `e2e-cache-cosmo` | **now PASSES (679s), ENFORCED** | The target deleted its own prerequisite, and not only on Windows: `mk/tests.mk` ran `$(MAKE) platform-cosmo` (which leaves `build/libhull_platform.{x86_64,aarch64}-cosmo.a`) and then `$(MAKE) clean` (`rm -rf $(BUILDDIR)`), so the next line died on `No rule to make target 'build/libhull_platform.x86_64-cosmo.a'`. The clean was redundant anyway - `platform-cosmo` already cleans between its two arch passes. Fixed during the sweep, but NOTHING RAN IT afterwards, on any platform, so 'fixed' stayed an untested claim until this measurement. Now gated on Windows, where cosmo is the native case. It carries a NAMED 900s per-suite budget rather than the shared 300s: it rebuilds platform-cosmo and a cosmo hull before asserting anything, so it is a toolchain rebuild wearing a suite's clothes, and raising the cap globally would give back the hang protection the 300s buys. |
| `e2e-compiler-free` | TIMEOUT | The `sh -c` injection below was what HUNG it; with that fixed it is a clean FAILED at 13s, and its dead cosmo guard (see cause 4) is why it ran at all. |
| `e2e-compute` | FAILED | Dies on `Error 143` (SIGTERM) after its assertions pass. Undiagnosed. |
| `e2e-feature-valkey` | FAILED | Probably the docker-cannot-run-linux-containers cause its sibling `e2e-valkey` had, but its log has not been read. |
| `e2e-linker` | FAILED | **A regression.** See below. Now guarded on `hull_is_ape`: a cosmo hull cannot link through a native lld. |
| `e2e-project-discovery` | 3 of 66 | **Solved, and it was a Hull bug, not a test bug.** The suite injects a sidecar `session_pid` and expects hull's `kill(pid, 0)` gate to call it live. On Windows hull is a NATIVE process and the shell an MSYS one, numbering processes differently (measured in one shell: `$$`=640, which Windows does not know; its WINPID 36772 it does), so hull was right to call that pid dead. Passing the WINPID instead made the suite stop 17 assertions early, printing no summary and exiting 0. The cause: on Windows `kill(pid, 0)` is **not** the inert existence check POSIX specifies - it TERMINATES a live pid. Proven by control: mismatched pids short-circuit before the kill and the victim lives; matching pids and it dies. The pid a live-session sidecar names is the `hull dev --agent` supervisor, so `hull agent inspect` was killing the dev server whose generation it was about to serve, returning 0 with correct output. Never caught because the fast path needs dev.json and discovery.json to carry the same pid the host agrees is alive - unreachable on Windows until the suite built it. Fixed by declining the fast path on Windows (a safe probe needs a direct Win32 `OpenProcess`, which nothing in Hull has); inspect falls back to a standalone analysis there - correct output, freshly analysed, just not tagged `source: dev`. **These three still fail on Windows, and now do so honestly** (standalone instead of `source: dev`) rather than silently stopping the suite. |
| `e2e-project-discovery-lua` | 6 of 57 | **Not undiagnosed - the same cause as its sibling above.** Measured on main after the `kill(pid, 0)` fix: 51 pass, 6 fail, and all six are live-session assertions (`source=dev` served, generation carries a `session_pid`, no new generation after reload, sidecars linger after dev exit, live matching PID IS served, JS-less live gen). Those are exactly the fast path `hull agent inspect` now DECLINES on Windows by design, because `kill(pid, 0)` terminates its target there - see the `e2e-project-discovery` row. It carries 6 rather than that row's 3 because it also exercises the JS-less live generation. **Not gated, and not for cost reasons:** the target opens with `$(MAKE) clean` and rebuilds `RUNTIME=lua` into an isolated binary, so it CLOBBERS `build/` - it is designed to run as its own CI job, and dropping it into the shared enforced tier would destroy the tree the other 88 suites run against. ~368s. |
| `e2e-smtp` | **now PASSES (33s), ENFORCED** | Was the only TIMEOUT in either sweep. One command was responsible: the `wait $HULL_PID` after `kill -INT` consumed 299s of a 300s traced budget while every other command was sub-second. MSYS process control does not cross to a NATIVE child, and an APE is native here - `kill -INT` is DROPPED (SIGTERM/SIGKILL fall back to TerminateProcess; SIGINT has no fallback), so the wait never returned. A second, quieter defect rode along: `kill -0` is not a liveness test for such a child either - it stays unreaped, so `kill -0` keeps succeeding after the process is gone, which would have turned every shutdown check into a false 'hung'. Both now live in `tests/lib/hull_proc.sh`. The two graceful-shutdown legs are SKIPPED here, not retargeted: Keel does stop gracefully on Windows (a console control handler - `CTRL_C_EVENT`/`CTRL_BREAK_EVENT`/`CTRL_CLOSE_EVENT` -> `kl_http_server_stop`), but MSYS cannot generate a console event for another process without hitting every process on the shared console. Substituting SIGTERM would make 'shutdown within 15s' trivially true - a vacuous guard. Linux CI now runs the suite so those legs are still asserted somewhere. |
| `e2e-smtp-link-seam` | FAILED | Regressed with `e2e-linker`, same root cause. |
| `e2e-tui` | **now PASSES 11/11, ENFORCED** | The earlier account here - that the pty works and only MID-SESSION rendering fails to arrive - was wrong on both halves. **There is no pty.** Without `forkpty` the driver prints `SKIP: no forkpty on this platform` (33 bytes, every time) and returns 77, the automake skip convention. The suite guards for exactly this (`pty_driver_works`, whose comment says to drive a trivial command through the driver and *require the expected text back*) - but the implementation discarded stdout and tested only the exit status, and `e2e_tui_drive` is built by cosmocc, so it is an APE and 77 arrived as 0. The probe reported success and 26 interactive cases ran against a driver that never drove anything. Worse, the 8 picker cases also assert by exit status, so they **passed vacuously**: the real state was 10 genuine passes, 8 fake, and 19 failures that were all just the driver's SKIP text. Also ruled out, so the old account is not retried: the driver captures INCREMENTALLY (`drain_for` polls, reads and echoes each chunk - `e2e_tui_drive.c:181,272`), and the TUI writes to `STDOUT_FILENO`, not `/dev/tty` (`cap/tui.c:510`). Fixed by making the probe read the captured text, which no status shift can fake, and by lifting the six ENOTTY refusal checks out of the pty gate - they drive hull with stdin closed and need no terminal, so gating them cost six real assertions. **Known gap:** 26 interactive cases skip on Windows, so nothing about TUI RENDERING is asserted there. Closing that needs a ConPTY-based driver (Cosmopolitan has no `forkpty`); it is a real hole, not a solved problem. |

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

## Audit: do the PASSING suites actually assert anything?

Every defect this sweep found was discovered while chasing a FAILURE, so the
suites that pass on Windows had never been examined for the same problem - and
three of the ones that did fail turned out to be passing assertions that tested
nothing. The passing set was audited statically for the four shapes that
produced every broken guard here. All four are PRESENT in the tree; none of
them produces a silent false pass on Windows today.

| shape | found | live on Windows? |
|---|---|---|
| Symbol-ABSENCE guard (`nm BIN \| grep -c SYM`, assert 0) | 9, in 4 suites | **no** - all four skip first |
| Status-gated pass (`<hull ...> && pass`) | 10, in 5 suites | **no** - those paths skip |
| Bad-string guard (`grep -q "Segmentation\|panic\|fatal"`) | 3 | fixed (#529) |
| `kill -0` as liveness | 20 verdict-bearing | vacuous, but harmless - see below |

**The symbol guards are the ones that would have mattered.** They assert the
invariants Hull's composable base rests on: pure-compute links zero Keel and
zero mbedTLS, the base defines zero `sqlite3_open`, a non-SMTP app links none
of the SMTP objects. And `nm` genuinely cannot read a cosmo APE - measured:

    $ nm build/hull
    nm: build/hull: no symbols

so a guard pointed at one counts 0 of 0 symbols and passes having inspected
nothing. It does not happen today because all four suites refuse to run on
cosmo BEFORE reaching the guard, each with an accurate reason ("cosmo keeps
SQLite in-base (a fat APE can't force-load a feature archive)"). That is the
pattern done correctly: establish that the platform can support the claim
before making it.

**`kill -0` is vacuous here and it does not matter.** A backgrounded NATIVE
child stays unreaped, so `kill -0` keeps succeeding after it is gone. Fourteen
sites read `if ! kill -0 $PID; then fail "server failed to start"`, which
therefore cannot fire; the remaining six only GATE a real assertion that
follows. In both cases the verdict comes from a later check on actual output
(a `curl` response, a computed value), so a dead server still fails the suite -
it is merely reported at the wrong line. Worth fixing opportunistically, not
worth a sweep.

### The coupling to watch

These guards are dormant because of the skips, not because they are sound. If
a skip is ever lifted, the guard behind it goes live and asserts nothing. The
concrete case is already on the roadmap: giving the Windows job a drivable
`cosmocc` (see the `e2e-build` row) would let `hull build` produce app
binaries there - and six of the nine symbol guards inspect a hull-BUILT
binary. They would start passing on APEs `nm` cannot read, and the composable-
base invariants would be retired silently on the day the coverage gap is
closed.

Anyone lifting one of those skips should make the guard prove the tool read
something first - assert a non-zero total symbol count beside the zero match -
rather than trusting silence. Silence meaning "the tool could not do its job"
is the single mistake behind every broken guard recorded in this document.


**Known coverage gap: TUI rendering is not asserted on Windows.**
`e2e-tui` passes there because 26 of its 37 cases SKIP - Cosmopolitan has no
`forkpty`, so the pty driver cannot drive a terminal at all and prints
`SKIP: no forkpty on this platform`. What still runs is real: the ENOTTY
refusal paths, the manifest/module gates, and the picker's not-a-tty
behaviour. What does NOT run is everything that asserts on rendered output -
titles, panes, streamed child output, SGR mouse enable/disable.

Closing it needs a ConPTY-based driver for Windows (`CreatePseudoConsole`),
which is a real piece of work and a separate decision. Recording it because a
green enforced suite would otherwise imply coverage that is not there - which
is exactly how this suite came to claim 8 passes it had not earned.

**Not gated, deliberately**: `e2e-build` and
`e2e-project-discovery-lua`. Together ~17 minutes for two suites whose causes

**A guard in `e2e-project-discovery-lua`'s recipe asserts nothing on Windows.**
`mk/tests.mk` proves the lua-only binary carries no QuickJS symbols with
`if nm $(BUILDDIR)/hull-lua-only 2>/dev/null | grep -E 'hl_js_gen_|JS_...'`.
On Windows plain `nm` is native binutils handed a fat APE, and `2>/dev/null`
swallows whatever it says about that: no output means the grep matches
nothing, which the recipe reads as success. That is the broken-guard shape
this sweep keeps finding - `file(1)` calling an APE a "DOS/MBR boot sector"
so a cosmo exemption never fired, `find_clang` spawning a bare name that
could not resolve. It is not why the suite fails, but it proves nothing
here. The fix is the usual one: require the tool to produce output before
trusting its silence, or use the cosmo-aware `x86_64-linux-cosmo-nm` that
sits in `cosmo/bin` beside the compiler.
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
