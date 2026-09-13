# cosmocc on Windows: cc1 blocks indefinitely on large translation units

**Filed upstream as
[jart/cosmopolitan#1522](https://github.com/jart/cosmopolitan/issues/1522)**
(2026-09-09). This file stays as the working record: the measurements have a
home here and do not have to be re-derived. Tracked on the Hull side as
[hull#462](https://github.com/artalis-io/hull/issues/462).

The filed issue carries one thing this draft did not - a **negative control**
(below), which is probably the most useful part of it - and a standalone
reproducer that needs no Hull checkout, just the cosmocc zip and the SQLite
amalgamation.

SPDX-License-Identifier: AGPL-3.0-or-later

---

## Summary

Compiling a large C translation unit with `cosmocc` on Windows intermittently
hangs. `cc1` stops making progress and sits at **~0% CPU indefinitely** while
holding an open temporary output file. It is not an ICE, not an error, and not
a slow compile: the process simply blocks and never returns.

Measured rate: **about 12% per compile of a large TU.**

## Environment

| | |
|---|---|
| cosmocc | 4.0.2 (`cosmocc-4.0.2.zip`, SHA-256 `85b8c37a406d862e656ad4ec14be9f6ce474c1b436b9615e91a55208aced3f44`) |
| Host | GitHub Actions `windows-latest` (Windows Server 2025, image `windows-2025-vs2026`) |
| Shell | MSYS2 (`msys2/setup-msys2`, `msystem: MSYS`), GNU Make 4.4.1 from MSYS2 |
| Optimization | `-O0` (the hang also motivated using `-O0`; it is not optimization-dependent) |

## Reproducer

The Hull tree vendors the SQLite amalgamation. Compiling that one file, alone,
is enough:

```sh
cosmocc -std=c11 -O0 -w -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5 \
        -Ivendor/sqlite -c -o build/sqlite3.o vendor/sqlite/sqlite3.c
```

Roughly 1 invocation in 8 never returns. Any sufficiently large TU seems to do
it: `vendor/quickjs/quickjs.c` hangs the same way.

## Evidence

A watchdog fires after 120s of no output and samples every build process twice,
5 seconds apart, reporting cumulative CPU and the delta between samples:

```
WEDGED after 120s of silence
pid=5744 ppid=7140 cc1  cpu=3.42s  dcpu=0.01s  rss=219.4MB
    cmd: ...\libexec\gcc\aarch64-linux-cosmo\14.1.0\cc1 -quiet -nostdinc
         -I vendor/sqlite -imultiarch aarch64-linux-cosmo ...
         vendor/sqlite/sqlite3.c -quiet -dumpdir build/.aarch64/
         -dumpbase sqlite3.c ... -O0 -std=c11 -fportcosmo ...
         -o D:\a\_temp\msys64\tmp\ccfs5cmy.s
pid=7140 ppid=5396 aarch64-linux-cosmo-gcc  cpu=0.09s  dcpu=0s
pid=8512 ppid=500  make.exe                 cpu=0.66s  dcpu=0.04s
```

`dcpu=0.01s` over a 5-second window is the key measurement: **`cc1` is blocked,
not spinning.** It has accumulated only 3-7s of CPU and then stops, consistently
at roughly the same point.

## Negative control: it does NOT reproduce on a Windows 11 desktop

Everything above was measured on GitHub Actions. Running the same reproducer -
same cosmocc 4.0.2, same `-O0`, same MSYS2 shell, the SQLite amalgamation as
the TU - on a **Windows 11 Pro 26200 desktop** gave **0 wedges in 40
consecutive compiles**, every one finishing in 4-5s.

That is not just "did not reproduce". If p were 0.12, the chance of 40 clean
compiles is `0.88^40 = 0.6%`; zero events in 40 puts a 95% upper bound of
`3/40 = 7.5%` on the rate for that host. The two hosts are statistically
incompatible.

So the trigger is not "cosmocc compiling a large TU on Windows". Something
about the Actions image differs - its ephemeral `D:\` work volume, real-time
scanning on the temp path `cc1` writes its `.s` into, or that image's MSYS2
installation. Note the temp-directory arm below moved `TMPDIR`/`TMP`/`TEMP`
*within* the image, which does not rule out the volume or the scanner.

This also means the retry mitigation is aimed at CI specifically, and a
developer building on a Windows desktop may never see the wedge at all.

## What has been ruled out

Each of these was tested rather than reasoned about, on the same runner image,
with both arms in a single dispatch:

| Hypothesis | Test | Result |
|---|---|---|
| Optimizer or codegen blow-up | CPU delta while wedged | **No.** `dcpu ~ 0`; a blow-up would burn CPU. |
| MSYS2 temp directory | 10 runs with `TMPDIR`/`TMP`/`TEMP` relocated off the MSYS2 tree, vs 10 without | **No.** 2/10 vs 4/10, Fisher exact two-tailed p=0.63. Relocating did not eliminate it. |
| Parallel-make contention | 10 full builds at `-j1` | **No.** Still 2/10. |
| Needs a big build around it | 250 compiles of `sqlite3.c` alone, nothing else running | **No.** 10/10 jobs wedged. |
| One bad source file | which TU each wedge was on | **No.** `sqlite3.c` and `quickjs.c` both. |
| Architecture-specific | arch of the stuck `cc1` | **No.** Both `x86_64-linux-cosmo` and `aarch64-linux-cosmo`. |

## Rate measurement

250 solo compiles across 10 parallel jobs, each looping until it wedged:

```
first wedge at iteration: 1, 1, 2, 2, 3, 8, 11, 13, 19, 23
mean 8.3 iterations
```

A geometric distribution with p = 0.12 has mean 1/p = 8.3, so the observed
mean matches a constant ~12% per-compile probability closely.

At the whole-build level (one `sqlite3.c` plus one `quickjs.c` compile, `-j2`)
this shows up as roughly 30% of builds hanging: 6/20 observed.

## What would help

Anything that narrows where `cc1` is blocking would be more useful than more
sampling from outside. Specifically:

- whether the temporary `.s` output path is implicated (the block is observed
  while `cc1` holds it open);
- whether this is known behaviour of the Windows port of the GCC used by
  cosmocc 4.0.2, or specific to the cosmo build of it;
- whether a newer cosmocc changes the rate.

Happy to run further arms on request. The sampling method is recorded in the
appendix below, so any additional arm can be stood up and answered within about
half an hour.

## Workaround in use

Retrying the build on a stall. `make` resumes from the objects already
completed, so a retry costs a fraction of a build, and three attempts leave
roughly 3% residual failure. This is a mitigation, not a fix.

## Appendix: how the numbers were produced

The probe workflow that generated these figures has been retired from the tree
(it was diagnostic scaffolding, and leaving a dispatch-only workflow lying
around invites it being run by accident). The method is recorded here instead,
because it is short and because the report offers further arms: anything below
can be re-run from this description alone.

**Shape.** A `workflow_dispatch` matrix of `arm x n`, `fail-fast: false`, one
job per sample, on `windows-latest` with `msys2/setup-msys2` (`msystem: MSYS`,
installing `make binutils vim diffutils git`) and the SHA-pinned cosmocc above.
Both arms run in the SAME dispatch so they share a runner image.

**Stall detection.** Run the compile in the background writing to a log, poll
its size, and treat N seconds of no growth as a wedge. 120s for a single TU,
300s for a whole build; a healthy build never goes quiet for more than a
fraction of a second.

```sh
run_watched() {                       # $1 = stall seconds, rest = command
  _limit=$1; shift
  _log=$(mktemp); "$@" > "$_log" 2>&1 &
  _pid=$!; _last=0; _stalled=0
  while kill -0 "$_pid" 2>/dev/null; do
    sleep 5
    _size=$(wc -c < "$_log" 2>/dev/null || echo 0)
    if [ "$_size" -eq "$_last" ]; then _stalled=$((_stalled+5)); else _stalled=0; _last=$_size; fi
    if [ "$_stalled" -ge "$_limit" ]; then
      kill "$_pid" 2>/dev/null || true
      return 2                        # wedged
    fi
  done
  _rc=0; wait "$_pid" || _rc=$?
  [ "$_rc" -eq 0 ] || return 1        # genuine build failure, NOT a wedge
  return 0
}
```

Distinguishing the two non-zero returns matters: a compile error and a wedge
are different events, and conflating them would have made the rate meaningless.

**Rate measurement (the `solo` arm).** Each job loops up to 25 times, removing
the object and recompiling only `sqlite3.o`, and stops at its first wedge:

```sh
for i in $(seq 1 25); do
  rm -f build/sqlite3.o
  run_watched 120 make CC=cosmocc HL_OPT=-O0 HL_ENABLE_WASM=0 build/sqlite3.o
  [ $? -eq 2 ] && { echo "wedged at iteration $i"; break; }
done
```

Ten such jobs gave first-wedge iterations 1, 1, 2, 2, 3, 8, 11, 13, 19, 23.
Mean 8.3; a geometric distribution has mean 1/p, giving p = 0.12.

**Process snapshot.** On a stall, sample every build process twice five seconds
apart and report cumulative CPU plus the delta, matching whole base names only
(an unanchored pattern matches unrelated Windows processes and buries the one
that matters). The delta is the entire point: `dcpu ~ 0` means blocked,
`dcpu ~ interval` would mean spinning. `Get-CimInstance Win32_Process` is the
only thing on Windows that exposes a full command line, and `Get-Process`
supplies `.CPU`.

**Arms run so far.** `baseline` (unchanged), `tmpdir` (`TMPDIR`/`TMP`/`TEMP`
relocated off the MSYS2 tree), `solo` (one TU, nothing else running),
`full-j1` (whole build serialised).
