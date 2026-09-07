# cosmocc on Windows: cc1 blocks indefinitely on large translation units

Draft of an upstream report for [jart/cosmopolitan](https://github.com/jart/cosmopolitan),
kept in-tree so the measurements have a home and the numbers do not have to be
re-derived. Tracked on the Hull side as
[hull#462](https://github.com/artalis-io/hull/issues/462).

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

Happy to run further arms on request: the probe harness takes an arm name and
returns 10 to 250 samples within about half an hour.

## Workaround in use

Retrying the build on a stall. `make` resumes from the objects already
completed, so a retry costs a fraction of a build, and three attempts leave
roughly 3% residual failure. This is a mitigation, not a fix.
