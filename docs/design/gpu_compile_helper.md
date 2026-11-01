# Design: the GPU shader compile helper process

- **Status:** Implemented (audit convergence phase 3, the second half of §7.3)
- **Amends:** `audit_convergence.md` §7.3 and `audit_convergence_gpu_shaders.md` ("Dynamic
  source: in-process, helper deferred")
- **Code:** `src/hull/cap/gpu_helper.c`, `include/hull/cap/gpu_helper.h`, the helper limits in
  `include/hull/limits/gpu.h`, `hl_cap_gpu_compile_ex` (`src/hull/cap/gpu.c`),
  `hl_gpu_shader_admit` (`src/hull/cap/gpu_shader_policy.c`), `serve.c` (spawn, stop, the
  hidden mode), the `gpu-shaders` cache kind (`src/hull/runtime/cache_common.c`,
  `src/hull/shared/cache_registry.c`)

## 1. Problem

PR #978 made a built binary compile only the shaders it was built with. An app can still opt in
to runtime WGSL with `gpu.dynamic_shaders = true` (JS `gpu.dynamicShaders`). Until now that
source compiled in the server process. It went through the WGSL pre-checks, the large-stack
compile thread and the round-40 compile deadline with abandoned-compile handling. Those bound
the damage, but they all run inside the process that serves requests:

- a compile that recurses past the compile thread's stack kills the server;
- a compile that exhausts memory takes the server with it;
- a compile past its deadline is abandoned, not stopped. Its thread keeps running, and the
  device refuses GPU work (`gpu_busy`) until the thread finishes.

Rounds 36-41 each found a WGSL shape that the estimators missed. §7.3 asks for runtime source
to compile in a helper process that is killed at the deadline, so that the estimators become a
fast first line instead of the security boundary.

## 2. Decision summary

| Question | Decision |
|---|---|
| What the helper returns | A **verdict** (OK / refused), not compiled code. The server compiles a source the helper accepted with its existing compile path (option (b) in the task). |
| Process | Hull's own binary in a hidden mode (`argv[1] == "__hull_gpu_compile_helper"`, taken at the top of `hull_serve`), started with `posix_spawn` and no shell. |
| Topology | server → **supervisor** (spawned once, before the sandbox) → one fresh **worker** per compile |
| Bounds | Start-up and compile deadlines with SIGKILL, a memory cap (macOS: the supervisor polls the footprint and kills; Linux: `RLIMIT_DATA`), rlimits (core 0, CPU, Linux `RLIMIT_NPROC`), a compile stack below the server's, a 512-byte reply cap, and an OS sandbox in the worker |
| Event loop | A run-aware wait: the server polls every 10 ms, asks the run's deadline and budget, and sends a cancel when the run stops. Not a promise; see §6. |
| Charging | `HL_GPU_HELPER_UNITS` (4M) before a round trip on a cache miss. The wait itself is excluded. The existing compile charge is unchanged. |
| Caching | All verdicts in memory (64 entries). OK verdicts are also kept on disk in the sealed runtime cache (`gpu-shaders`). |
| `hull build` | Stays in-process (tool mode); see §8 |
| Failure | Fail closed. A dead or missing supervisor refuses all runtime source (`shader_helper_failed`); the sandboxed server cannot spawn a new one. |

## 3. Why the helper returns a verdict, not code

The server needs a `WGPUComputePipeline` on its own device. A device object cannot cross a
process boundary, so the only question is what form of the shader the helper could hand back.

- **Translated code (option (a))**: wgpu-native takes WGSL, GLSL and SPIR-V (`WGPUShaderSourceSPIRV`).
  Each of these goes back through naga: the SPIR-V front end, **validation**, and the platform
  back end (MSL on Metal, SPIR-V on Vulkan). Those are the stages that recurse and that the
  audits attacked, so handing back SPIR-V saves nothing. The one input that skips naga is
  `WGPUNativeFeature_SpirvShaderPassthrough`, which is Vulkan-only and has no Metal or MSL
  equivalent. Passthrough also skips naga's validation, which is what keeps a shader's memory
  accesses in bounds on the GPU. If the helper process were compromised (its whole point is to
  run the risky code), it could then hand the server unvalidated GPU code. Option (a) is
  unavailable on Metal and weakens the boundary on Vulkan.
- **A verdict (option (b), chosen)**: the worker runs the **same** compile the server would:
  `backend->compile`, which is naga parse, validation, back end and the driver's pipeline
  compile. It returns OK or refused. The server compiles an accepted source in-process as
  before, keeping its own compile thread, deadline and abandoned-compile handling.

Why the verdict carries over to the server:

- naga is deterministic, the same code in the same binary.
- A verdict carries over for stack, time and memory **only because each helper limit is
  strictly below what the server can afford**:
  - **Stack.** The worker compiles on `min(HL_GPU_HELPER_STACK_BYTES (768 MiB), 7/8 of the
    largest stack a compile thread can get in the server now)`. The server measures the latter
    before each helper round trip (`hl_gpu_compile_stack_available`: 2 GiB, else 1 GiB / 256 /
    64 MiB on a host short of address space), sends the result in the request (it is part of
    the verdict's cache key), and refuses to compile a helper-admitted source on a thread
    smaller than what the worker had (`HlGpuCompileJob.min_stack`). Before the §7.3 confirming
    audit the worker always had 768 MiB, while the server's thread could fall back to 256 MiB.
  - **Time.** The worker's compile deadline is the server's (`HL_GPU_HELPER_COMPILE_TIMEOUT_MS`
    = `HL_GPU_COMPILE_TIMEOUT_MS`).
  - **Memory.** The worker is capped at `HL_GPU_HELPER_MEM_BYTES` (§5); the server has no
    memory limit of its own, so a source that compiled within the cap fits it. Before the
    confirming audit only Linux had a bound (`RLIMIT_DATA` 8 GiB), and macOS none.

So a source that crashes, recurses or runs long does so in the worker, which is killed or dies,
and the server never compiles it. The server's in-process deadline stays as a backstop for
nondeterminism the helper cannot see, such as a driver whose cache behaves differently or a
loaded host.

### Measured residual cost in the server (Apple M1 Max, Metal, macOS 26)

From `test_gpu` (`hull_cap_gpu_helper.server_residual_measured` / `real_compile_on_device`).
Every source was made unique per run, so no shader cache had seen it.

| Shader | Cold in-process compile | Helper round trip | Server compile after the helper |
|---|---|---|---|
| Large (2 × 403 stores, near the audit-40 function-size bound) | 0.310 s | 0.324 s | **0.006 s** |
| Domain-warp (3.4 KB, audit 42) | 0.001-0.07 s (first compile in a process: ~0.07 s) | 0.04-0.05 s | 0.001-0.002 s |
| Small (one store) | - | 0.041 s (0.14 s first run after build) | 0.0002 s with a cached verdict |

On Metal, the worker's compile fills the system Metal shader cache. The server's compile of the
same source then reuses the driver's work and pays only naga plus a cache hit: about 2% of the
cold compile for the large shader. The worker writes that cache under its sandbox, because the
profile allows writes below `/private/var/folders`. On Vulkan the driver may or may not share a
pipeline cache across processes. Where it does not, the server pays the compile a second time,
still under its in-process deadline. That doubles the latency of a first compile but is never
unbounded.

A worker's fixed start-up cost (spawn, dyld, wgpu instance, adapter and device, sandbox, plus a
mock reply) is about 50-55 ms warm. The first worker after a fresh build or boot takes about
0.3 s.

## 4. Process model

```
server (sandboxed after phase 1)
  │  socketpair, fd 3 in the child; spawned in serve.c after GPU init, BEFORE
  │  hl_sandbox_apply_pledge (phase 1 forbids exec / fork on Linux; phase 2's
  │  Seatbelt profile on macOS denies process-exec)
  ▼
supervisor  (`<self> __hull_gpu_compile_helper supervisor`)
  │  single-threaded; parses only fixed frames, never WGSL; no GPU state
  │  socketpair per compile, fd 3 in the child
  ▼
worker      (`<self> __hull_gpu_compile_helper worker`)  one per compile, then exits
```

- **Why a supervisor.** On Linux, the server's phase-1 pledge drops `exec` and `proc`. On macOS,
  phase 2's Seatbelt profile denies process-exec. Once sandboxed, the server can spawn nothing.
  Yet a worker that was SIGKILLed has to be replaced, and a fresh worker per compile is wanted
  anyway (below). The supervisor is spawned before the sandbox and does the spawning. The server
  cannot `kill()` another process under pledge either: pledge polyfill `kill` is `SELF` only
  without `proc`. So the supervisor also enforces the deadlines.
- **Why spawn before knowing the manifest.** The phase-1 sandbox applies before the app's top
  level runs, and the top level is where `app.manifest` lives. So serve.c starts the supervisor
  whenever the binary has a GPU device **and** embedded app files: a built binary, the only mode
  that uses the helper. It closes the supervisor's socket right after the manifest is applied if
  the app did not opt in. Cost: one short-lived idle process at startup for GPU binaries. The
  supervisor initialises no GPU state; a worker does that.
- **Why a fresh worker per compile.** A worker that has compiled hostile source may be in any
  state. Reusing it would let one exploited compile vouch for the next source. A fresh process
  per compile also makes the deadline a plain SIGKILL, and memory a worker grew dies with it.
  The cost is about 50 ms of start-up per cache miss, which the verdict cache amortises.
- **Spawning.** `posix_spawn` of the binary itself, with no shell: `/proc/self/exe` on Linux
  (immune to the file being replaced) and `_NSGetExecutablePath` + `realpath` on macOS. stdin,
  stdout and stderr go to `/dev/null`, so a naga error message quoting the source never reaches
  the server's terminal or log. fd 3 is the socket. Apple's `POSIX_SPAWN_CLOEXEC_DEFAULT` closes
  every other descriptor. On Linux the helper closes descriptors above 3 at startup. Signal mask
  and dispositions are reset. The environment is an **allowlist** (`VK_*`, `MESA_*`, `WGPU_*`,
  `MTL_*`, `__GLX_*`, `HOME`, `TMPDIR`, `XDG_CACHE_HOME`, `XDG_RUNTIME_DIR`,
  `LD_LIBRARY_PATH`, and `HULL_TEST_GPU_HELPER` in a test build only), so `manifest.secrets`
  and DSN passwords never reach it.
- **Lifetime.** The supervisor exits on its socket's EOF: server teardown (`hl_gpu_helper_stop`
  in serve.c), a server crash, or the "no opt-in" stop. On the way out it kills a running worker.
  On Linux both helper roles also set `PR_SET_PDEATHSIG`. The server reaps the supervisor with
  `waitpid(WNOHANG)`, which pledge's `stdio` allows. It never signals it.

## 5. Bounds on the worker

| Bound | Value | Where |
|---|---|---|
| Start-up deadline (spawn, backend init, sandbox, until READY) | `HL_GPU_HELPER_INIT_TIMEOUT_MS` 3000 ms | supervisor, SIGKILL |
| Compile deadline (after the source is sent) | `HL_GPU_HELPER_COMPILE_TIMEOUT_MS` = `HL_GPU_COMPILE_TIMEOUT_MS` (2000 ms) | supervisor, SIGKILL |
| Server's own bound on a round trip | the two above + `HL_GPU_HELPER_MARGIN_MS` (1000 ms) | server: a supervisor past it is dropped (fail closed) |
| CPU | `RLIMIT_CPU` = the two deadlines in seconds + 2 | worker |
| Core dumps | `RLIMIT_CORE` 0 | supervisor, worker |
| Memory | `HL_GPU_HELPER_MEM_BYTES` 1 GiB (the request may ask for less; the supervisor clamps). macOS: the supervisor reads the worker's `ri_lifetime_max_phys_footprint` (`proc_pid_rusage`) every `HL_GPU_HELPER_MEM_POLL_MS` (5 ms) and SIGKILLs it above the cap (`HL_GPU_HELPER_MEMORY`). Linux: `RLIMIT_DATA` = the cap + the compile stack (a thread stack is a private writable mapping, counted as data); an allocation past it fails and the worker aborts (`CRASHED`). Both are refused as `shader_helper_failed`. Sanitizer builds skip the rlimit (it refuses the shadow reservation). | supervisor (macOS), worker (Linux) |
| Compile stack | the request's, at most `HL_GPU_HELPER_STACK_BYTES` 768 MiB (§3; a smaller fallback only makes the verdict stricter) | worker |
| New processes | `RLIMIT_NPROC` 0 on Linux; Seatbelt `deny process-fork` / `process-exec*` on macOS | worker |
| Request size | header + 8 + `HL_GPU_WGSL_MAX_BYTES` | supervisor and worker frame readers |
| Reply size | `HL_GPU_HELPER_REPLY_MAX` 512 bytes; longer kills the worker (BADREPLY) | supervisor |

**Sizing the memory cap.** `gpu_convergence_policy.worker_peak_measured` compiles the largest
real shaders through the helper and prints the worker's peak as the supervisor saw it (the
reply to the server carries it; `HlGpuHelperStats.peak_bytes`). Apple M1 Max, Metal, macOS 26:

| Shader | Worker peak |
|---|---|
| audit-40 large (2 x stores, 17 KB, near the function-size bound) | 22.8 MiB |
| audit-42 domain warp (3.4 KB) | 21.1 MiB |
| each of the examples' `shaders/*.wgsl` | 21.0-21.2 MiB |

So the worker's own compile adds about 2 MiB to a ~21 MiB baseline (backend init, Metal
framework mappings). The Metal back end's driver compile runs in Apple's `MTLCompilerService`
XPC process, which is not the worker's child and not counted; naga, the part the audits
attacked, runs in the worker. The cap of 1 GiB is ~45x the measured peak: room for a large
legitimate shader near the 1 MiB source cap, and far below what the server could absorb.

**The worker's OS sandbox** is applied after backend init, so the device and its driver
connection are already open, and before the worker reads the source:

- **macOS**: Seatbelt via `sandbox_init_with_parameters`. The profile is allow-default with
  these denied: `network*`, `process-fork`, `process-exec*`, every `file-write*` except the
  per-user temp tree (`/private/var/folders`, where Metal keeps its shader cache) and
  `/dev/null`, and every `file-read*` under `$HOME`. The test probe confirms the network and
  home denials. A deny-default profile would have to list what Metal and its XPC compiler service
  read, which changes between macOS releases, so it was rejected.
- **Linux**: `PR_SET_NO_NEW_PRIVS`, then a Landlock ruleset that handles every filesystem right
  the kernel's ABI knows. Its only grants are read access to the system trees a driver may still
  read during a compile (`/usr`, `/lib`, `/lib64`, `/etc`, `/opt`, `/proc`, `/sys`) and read,
  write and ioctl on existing nodes under `/dev`. Nothing can be created or removed, and the
  user's home and the app's files cannot be read. On Landlock ABI 4+ the ruleset also handles
  TCP bind and connect (`handled_access_net`) with no rule granting either; an older kernel
  gets the filesystem rules only. Then comes a seccomp filter that returns EPERM / EACCES for
  `execve`, `execveat`, `io_uring_setup` / `io_uring_enter` / `io_uring_register`
  (`IORING_OP_SOCKET` makes a socket without the `socket()` call the filter checks), `socket()`
  of any family but `AF_UNIX`, x32 and other ABIs, and finally `RLIMIT_NPROC` 0. Landlock and seccomp cover the calling thread and the compile thread it
  starts. Threads the GPU backend started during init are not covered, since there is no TSYNC.
  On a Linux arm64 VM (kernel 7.0, no GPU) the sandbox applied and the probe test passed: no
  `$HOME` read, no network connect. A compile on a real Linux device (Mesa / NVIDIA) under this
  sandbox has not been run yet, because CI's GPU job is build-only.
- **Elsewhere**: the helper is native-only, because GPU is a native-only feature. On cosmo and
  Windows the functions are stubs, and the opt-in's runtime source fails closed.

**What the worker sandbox is, and is not.** It is defence in depth. It narrows what a worker
can reach if a hostile source made the compiler misbehave, but it is not a boundary against a
naga memory-corruption exploit: the server re-compiles every source the helper accepted, in its
own process. The helper's guarantee is about the bounded failures (a crash, a deep recursion, a
long compile, a large allocation) of a deterministic compiler; it moves those out of the server.
An exploit that makes naga misbehave only on some runs, or that corrupts memory without failing
in the worker, would reach the server's compile too.

The worker reports in its reply whether its sandbox applied (flag bit 0). If it did not apply, or
applied only in part (for example, a kernel without Landlock), the server logs **one WARN**. The
process boundary, the deadlines and the rlimits still hold, so this is not fatal.

## 6. The wire protocol and the wait

Frames have a 16-byte header: magic `HGC1`, type, request id and payload length, each a
little-endian u32. Payloads:

- REQ: compile deadline (ms), device, the worker's stack and memory cap (MiB; the supervisor
  clamps both to `HL_GPU_HELPER_STACK_BYTES` / `HL_GPU_HELPER_MEM_BYTES` and rewrites them in
  the request the worker reads), then the WGSL. Magic `HGC2` since the stack and memory fields.
- CANCEL: empty.
- READY: empty.
- RESP: status, flags, and an optional detail of at most 512 bytes. The supervisor's RESP to
  the server carries the worker's peak memory (KiB) after the flags.

Every reader has a payload cap and treats a bad magic, an unknown type or an over-cap length as
a protocol error. A worker that does this is killed. A supervisor that does this to the server is
dropped.

**The server's wait** (`round_trip`) polls the socket in `HL_GPU_COMPILE_POLL_MS` (10 ms) slices.
Between slices it asks the run with `hl_db_budget_charge(0)`, as every GPU wait does. When the
run's deadline passes or its budget trips, the server sends `CANCEL(id)`. The supervisor kills
the worker and answers `CANCELLED`, so the next request does not queue behind a dying worker.
Replies whose id does not match the current request (a cancelled one's) are dropped. The call
then returns `compile_timeout`.

**Not async.** `gpu.compile` / `gpu.load` stay synchronous calls in both runtimes. The event loop
waits, but the wait is bounded by:

- the helper's deadlines: about 5 s worst case on a cache miss, typically 40-300 ms;
- the run's deadline and budget, through the cancel;
- the verdict cache: a source is sent at most once per process (and an accepted one once per
  cache lifetime);
- the floor charge (§7).

Making `gpu.compile` yield, a promise in JS or a coroutine park in Lua, would add a new
async-op binding in both runtimes. That means transaction checks before arming, run-tag wiring,
cancel paths and teardown entries. It is a follow-up and needs no change to the helper: the wait
already sits on a socket that an async backend watcher could take over.

## 7. Charging (§6.1)

- The existing compile charge (`hl_gpu_wgsl_compile_units`, per byte, word, statement and
  expansion) is paid by the bindings before any compile, as before.
- On a verdict-cache miss, `hl_gpu_helper_check` charges `HL_GPU_HELPER_UNITS` = 2^22 (about
  4.2M) to the calling thread's budget before it sends the request. If that trips the budget, the
  call returns `compile_timeout` and nothing is sent. At the standard rate of 64 units per µs,
  2^22 units is about 65 ms, roughly one warm round trip, and it allows about 24 helper compiles
  per default 100M budget.
- The wait is **excluded**. It is the helper's CPU, not the server's, and the server sleeps in
  `poll`. Its length is bounded by the deadlines and the run deadline, not by the budget.
- A cache hit pays only the compile charge. The in-process compile after an OK verdict pays
  nothing extra.

## 8. Caching

- **Key**: SHA-256 of `hull-gpu-verdict-v2 | HL_VERSION | backend | arch | device index |
  adapter name | worker stack MiB | worker memory cap MiB | SHA-256(source)`. A new Hull build, and so a new wgpu and naga, never reuses an
  old verdict.
- **In memory**: 64 entries, round robin, every status (OK, SHADER, TIMEOUT, CRASHED,
  BADREPLY, MEMORY). A source that timed out or crashed is refused at once on the next attempt, without
  another round trip. CANCELLED and UNAVAILABLE are not cached.
- **On disk**: OK verdicts only, through the sealed runtime cache
  (`hl_runtime_cache_get_sealed` / `_put_sealed`, HMAC under `~/.hull/cache.key`), cache kind
  `gpu-shaders` (`hull cache list | prune | clear`), opt-out `HULL_NO_GPU_SHADER_CACHE`. Like the
  bytecode caches, it is off under `--verify-sig` (`hl_runtime_cache_code_off`). Timeouts and
  crashes stay in memory, because they may reflect the host's load rather than the source. The
  disk functions live in `runtime/cache_common.c`. Weak no-op defaults in `cap/gpu_helper.c`
  keep `libhull.a` embedders and the cap unit tests, which do not link the cache, working with
  memory-only verdicts.
- **Trust**: every app process of the user holds `cache.key`, the same trust as the bytecode
  caches. A sibling app compromised at native level could seal an OK verdict for a hostile
  source. Such a source is then compiled in-process without the helper, but still behind the
  pre-checks, the compile thread and the in-process deadline. That is the pre-helper state, not
  an open door.

## 9. `hull build` stays in-process

`hull build`'s shader check (`tool.gpu_shaders_check` → `hl_gpu_shaders_build_check`) compiles
embedded shaders in the tool process. Keeping that in-process is deliberate:

- The input is the developer's own `shaders/` tree, threat actor A3 rather than a remote client.
  A build that crashes on its own shader is a failed build, not a downed server.
- The distributed `hull` has no GPU backend (GPU is a `--with` feature), so in practice the build
  runs only the pre-check. The test compile happens only on a source-built GPU toolchain.
- The tool sandbox applies before the check, and on OpenBSD it pledges without `proc`, so the
  helper could not be spawned there anyway.

## 10. Errors the app sees

| Situation | Lua (`nil, err`) / JS (throws `gpu.compile failed: <err>`) |
|---|---|
| Helper refused the source (naga / driver error) | `shader_error` |
| Helper past its start-up or compile deadline, or the run stopped (cancel), or the floor tripped the budget | `compile_timeout` |
| Worker crashed or exited without a reply, went past its memory cap, sent an oversized or malformed reply, or there is no supervisor (not started, lost, stopped) | `shader_helper_failed` |
| Accepted by the helper, then the in-process compile timed out / device busy | `compile_timeout` / `gpu_busy`, as before |

## 11. Testing

- `test_gpu` (`hull_cap_gpu_helper.*`): the test binary is its own helper (its `main` routes
  `__hull_gpu_compile_helper`). The cases are:
  - an accepted source compiles and its verdict is cached;
  - a crash is an error, not a crash, and the supervisor survives it;
  - a worker that exits without a reply;
  - a worker killed at a 200 ms deadline (returned after 0.25 s), with the timeout verdict cached;
  - an oversized reply is refused;
  - a lost supervisor fails closed;
  - a stopped run cancels the wait;
  - the floor is charged before the round trip;
  - the worker sandbox denies `$HOME` reads and network connects;
  - a real compile on the device (OK, and an invalid shader refused by the helper);
  - the residual-cost measurement in §3.
- The hooks run in the worker after its sandbox and before any compile, so they also run on
  hosts without a GPU: `HULL_TEST_GPU_HELPER=crash|slow|oversize|silent|accept|probe|alloc`.
  They are compiled only with `-DHL_GPU_HELPER_TEST_HOOKS`: the unit tests link their own copy
  of `cap/gpu_helper.c` built with it (`mk/tests.mk`), and the GPU e2e builds with
  `make HL_GPU_HELPER_TEST_HOOKS=1`. A release binary neither has them nor passes the variable
  to the helper; before the §7.3 confirming audit it honoured `accept`, which made the worker
  vouch for any source and sealed the OK into the disk cache. `alloc` allocates past the memory
  cap with no shader involved; `probe` also tries `io_uring_setup` on Linux.
- `test_gpu` (`gpu_convergence_policy.*`): a worker past a (lowered) memory cap is stopped and
  the source refused while the supervisor carries on; the supervisor reports the worker's peak;
  a verdict is per stack size and the server asks for no more than its compile thread can get;
  the peak measurement in §5.
- `test_lua` / `test_js`: without a supervisor, the opt-in's runtime source fails with
  `shader_helper_failed` through both bindings.
- `tests/e2e_feature_gpu.sh`: a served `--with=gpu` binary with the opt-in. For each hook, a
  compile route answers the hook's error (`compile_timeout`, `shader_helper_failed`) while
  `/ping` keeps answering (the `alloc` hook too: past the memory cap). A top-level compile of
  runtime source is refused even with the opt-in. Without a hook, runtime source compiles
  through the helper, and an OK
  verdict lands in the sealed cache. After a restart the cached verdict is used, so the crash
  hook is never reached. The existing build-time cases are unchanged: embedded shaders compile
  in-process, dynamic source is refused without the opt-in, and a refused shader fails the
  build.

## 12. Follow-ups

- An async `gpu.compile` (§6).
- A pre-warmed worker, spawned and sandboxed ahead of the next request, to hide the about 50 ms
  start-up. It holds a GPU device while idle, so it was left out for now.
- Run the Linux worker sandbox with a real device (Mesa / NVIDIA) in CI. Revisit Landlock TSYNC
  when the kernel offers it.
