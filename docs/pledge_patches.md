# pledge polyfill patches

Hull vendors Justine Tunney's Linux `pledge()` / `unveil()` polyfill
(jart/pledge, carved out of Cosmopolitan libc) in `vendor/pledge/`, edited in
tree. It is linked into native Linux builds only (`mk/platform/linux.mk`);
a cosmo APE uses the `pledge()` / `unveil()` built into Cosmopolitan's own
libc, which Hull does not patch. Every local change is marked in the source
with a `HULL PATCH NNNN` comment and listed here.

**On an upstream update:** grep the new tree for `HULL PATCH`, confirm each
entry below is still needed, and re-apply the ones that are. Then run
`make e2e-sandbox` on Linux: Test 6 (`tests/sandbox_violation.c`, built against
the polyfill objects, and again with `-DHULL_UNVEIL_TEST_NO_TSYNC=1`) and
Test 8 (every thread of a running server) are the regression gates.

## Patch 0001 - rseq is allowed, not ENOSYS-stubbed

`libc/calls/pledge-linux.c`: `kPledgeStart` no longer answers `rseq` with
ENOSYS, and `kPledgeDefault` allows it. glibc >= 2.35 registers a restartable
sequences area per thread and treats a registration that fails after an
earlier thread's succeeded as fatal ("rseq registration failed"). Hull's main
thread registers before the sandbox applies, so a thread created afterwards
(DuckDB's worker pool opened from a request handler, and since audit 42 Hull's
own pool workers) would abort the process. `rseq` grants no filesystem,
network or exec reach. See also docs/duckdb_backend_design.md.

## Patch 0002 - TRUNCATE is a file right

`libc/calls/unveil.c`: `FILE_BITS` includes `LANDLOCK_ACCESS_FS_TRUNCATE`
(Landlock ABI >= 3; masked out below that by `State.fs_mask`). Without it a
write grant on an existing FILE could not `O_TRUNC` it.

## Patch 0003 - seccomp filters go to every thread (TSYNC) - audit 42

`libc/calls/pledge-linux.c`, `libc/calls/unveil.c`,
`libc/calls/pledge.internal.h`.

Upstream installs each filter with `prctl(PR_SET_SECCOMP,
SECCOMP_MODE_FILTER)`, which attaches it to the CALLING thread only. Hull's
runners created their worker pool (Keel's `kl_thread_pool_create` starts the
threads at once) before either sandbox phase, so the threads that run
`worker.dispatch` VMs, parse `db.async` replies and run `compute.async` WASM
had no filter at all - and threads share one address space, so a memory-safety
bug in any of them voided the main thread's filter too.

- `hull_seccomp_install(prog)` installs a filter with
  `seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, prog)`: every
  thread of the process gets it atomically, and `no_new_privs` is set on each
  where the caller has it. A positive return (a thread whose filter tree is
  not an ancestor of the caller's, so it cannot be synchronised) is a failure
  (EPERM). Only when the `seccomp(2)` system call is missing (Linux < 3.17)
  does it fall back to `prctl`, which covers the calling thread only.
- `sys_pledge_linux` and `unveil_final` (its small truncate / setxattr
  blacklist filter) both go through it.
- The `stdio` promise allows `seccomp(2)` - but only op
  `SECCOMP_SET_MODE_FILTER` with flags exactly `SECCOMP_FILTER_FLAG_TSYNC`
  (`AllowSeccompTsync`, `__NR_seccomp | STDIO`) - so a later `pledge()` /
  `unveil()` can install its filter under the earlier one. A new filter can
  only narrow what the older ones allow (the most restrictive action of all
  attached filters wins), so this grants no authority; `prctl(PR_SET_SECCOMP)`
  was already allowed for the same reason.

## Patch 0004 - the Landlock domain reaches every thread - audit 42

`libc/calls/unveil.c` (`unveil_final`, `unveil_sync_by_signal`).

`landlock_restrict_self(fd, 0)` restricts the calling thread and the threads
it creates later, not the ones that already exist (the run watchdog started
while the app loads, a GPU driver's or DuckDB's threads). So:

1. Where the kernel has `LANDLOCK_RESTRICT_SELF_TSYNC` (`1U << 3`, Landlock
   ABI >= 8), `unveil(NULL, NULL)` applies the domain to every thread
   atomically. An `EINVAL` (a kernel that reports ABI 8 without the flag)
   falls through to 2.
2. Otherwise every other thread is asked to restrict ITSELF - the libpsx /
   glibc setxid pattern. Before restricting the caller (once restricted it can
   no longer list `/proc/self/task`), each thread in `/proc/self/task` is sent
   `SIGRTMAX - 3` with `tgkill`; the handler sets `no_new_privs` and calls
   `landlock_restrict_self` on the same ruleset, then records its thread id
   and the result. The list is re-read until a round finds no thread it has
   not signalled (up to 16 rounds), so a thread an unrestricted one started
   meanwhile is reached too. A thread that does not answer within 2 s (its
   signal blocked), or whose call failed, and that still exists, is counted in
   `hull_unveil_unsynced_threads` (-1 when `/proc/self/task` could not be
   read). Afterwards the signal is set to `SIG_IGN`, so a late delivery to a
   thread that unblocks it cannot take the default action (which ends the
   process).

The calling thread and every thread that answered are restricted either way,
and the seccomp filters then go to every thread (patch 0003).
`hull_unveil_sync_mode` reports what was used: 0 = no other thread,
1 = Landlock TSYNC, 2 = per-thread signal. The caller decides what an
unsynced thread means: Hull's `hl_sandbox_apply` (`src/hull/sandbox.c`) treats
it as an incomplete backend and refuses to start unless
`--allow-degraded-sandbox`, and logs the mode as `threads: single|tsync|signal`
on the `[sandbox] applied` line.

The handler runs under the phase-1 filter, whose promises (`stdio` +
`unveil`) allow `gettid`, `prctl(PR_SET_NO_NEW_PRIVS)` and
`landlock_restrict_self`; the caller's `tgkill` is `tgkill | SELF`.

`-DHULL_UNVEIL_TEST_NO_TSYNC=1` (test builds only) skips step 1, so
`tests/e2e_sandbox.sh` exercises step 2 on a kernel that has TSYNC too.

**Hull's side of audit 42.** Independently of these patches, the worker
pool's threads now start on the first submit rather than at pool creation
(`src/hull/async/keel.c`, `src/hull/async/poll.c`), which in every runner is
after the phase-2 sandbox, so they are born inside it. That is the only line
of defence for a cosmo APE, whose libc pledge is per-thread and unpatched; the
run and WASM watchdog threads, which can start earlier, are reached on native
Linux by these patches only.
