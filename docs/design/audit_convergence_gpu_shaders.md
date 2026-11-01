# Amendment: audit convergence §7.3 - GPU shader source

- **Status:** Implemented (phase 3, GPU half); the helper process is implemented too (see
  `gpu_compile_helper.md`)
- **Amends:** the audit-convergence design (`audit_convergence` in this directory, landing with phase 1) §7.3 ("GPU shader source"), §10, §11
- **Code:** `src/hull/cap/gpu_shader_policy.c`, the policy fields in `HlGpuCtx`'s sealed span
  (`include/hull/cap/gpu.h`), `serve.c` (sets the policy), `tool.gpu_shaders_check`
  (`runtime/lua/mod_tool.c`) + `stdlib/cli/lua/hull/build.lua`

## Why

Rounds 36-41 each found a WGSL shape the compile-cost estimators (`hl_gpu_wgsl_precheck`:
expression depth, block depth, type depth, declaration chains, inferred expansion, function
size) missed, so naga or the platform compiler held the event loop or overflowed a stack. The
compile thread and the round-40 deadline bound the damage, but the estimators kept being the
thing an attacker had to beat. §7.3's goal: compile time of a black-box compiler stops being a
security boundary. The app's shaders are known at build time, so a deployed app compiles only
those.

## Decisions

### The rule

The policy is a property of the GPU context, set by `serve.c` after the manifest is applied and
sealed with the device allowlist (so app code cannot widen it). One enforcement point:
`hl_cap_gpu_compile_ex` runs `hl_gpu_shader_admit` before the pre-checks and the backend, so
both runtimes and any future caller get it.

| Mode | When | `gpu.load(name)` | `gpu.compile(name, src)` |
|---|---|---|---|
| `HL_GPU_SHADERS_EMBEDDED` | the app VFS holds embedded files (`hl_vfs_embedded`): every built binary, served or `app.main` | the embedded `shaders/<name>.wgsl` | accepted only when `src` is byte-identical to an embedded `shaders/*.wgsl`; else `HL_GPU_ERR_POLICY` / `dynamic_shader_refused` |
| same, `gpu.dynamic_shaders = true` | manifest opt-in (JS `gpu.dynamicShaders`; only the boolean `true`) | as above | any source, compiled first in the helper process (`gpu_compile_helper.md`), then in-process under the compile deadline |
| `HL_GPU_SHADERS_DEV` | no embedded files: `hull app.lua`, `hull dev` | `shaders/<name>.wgsl` on disk (the existing dev fallback) | any source; one WARN per process when the opt-in is absent and the source did not come from `shaders/` |
| `HL_GPU_SHADERS_ANY` | no policy set: unit tests, embedders | any | any |

"Byte-identical" is checked by comparing against the `shaders/` entries of the same length
(`hl_gpu_shader_is_embedded`). A hash table would add nothing: an app embeds a handful of
shaders, and only equal-length entries are compared, so the cost is bounded by the source size
(already charged to the run before the compile). Allowing an identical inline copy keeps apps
that hold their WGSL in a constant working, as long as the same text is in `shaders/`.

`gpu.load` passes `HL_GPU_COMPILE_FROM_SHADERS`. The flag only silences the development
warning; in a built binary the source is looked up like any other, so the flag cannot widen
the policy.

### Development mode

Development keeps compiling inline source: shaders are edited in place, `hull dev` reloads, and
breaking that would push developers to the opt-in for no security gain (the developer is A3,
and a dev server is not a deployment). To stop "works in dev, fails in the build" surprises,
the first inline `gpu.compile` without the opt-in logs a WARN naming the rule. `hull test` and
`hull agent` create no GPU context today, so they are unaffected.

A stricter alternative - dev also requires `shaders/` - was rejected for this phase: it breaks
existing inline examples without improving the deployed binary's boundary.

### Build-time validation

`hull build` runs `tool.gpu_shaders_check` over every `shaders/*.wgsl` it embeds
(`hl_gpu_shaders_build_check`): the 1 MiB size cap and the full WGSL pre-check always, and a
test compile on a real device when the building `hull` has a GPU backend (an `HL_ENABLE_GPU`
toolchain) and the host an adapter. A failure fails the build, naming the file. The distributed
hull is GPU-less (GPU is a `--with` feature), so in practice release builds run the pre-check
only; "pre-compiled where the backend allows" from §7.3 is therefore not a runtime cache - the
built binary still compiles its embedded shaders at `gpu.load`, but only shaders the author
shipped and the build checked.

### Dynamic source: in-process, helper deferred

> **Superseded:** the helper process is implemented; see `gpu_compile_helper.md`. It uses a
> supervisor spawned before the sandbox and one worker per compile rather than a fork, and the
> server still compiles an accepted source in-process, as reasoned below. The text below is the
> reasoning at the time of #978.

With `dynamic_shaders = true`, runtime source goes through the existing in-process path: the
pre-checks, the large-stack compile thread, and the round-40 deadline with abandoned-compile
handling (the device refuses work until the abandoned thread ends). **The helper process with a
hard kill at the deadline is not implemented in this phase**; it is a follow-up. Reasons:

- A helper must initialise its own wgpu instance and adapter (~0.1-0.3 s on Metal per start),
  so it is either a long-lived child or a per-compile cost.
- On macOS the Seatbelt profile is irreversible and applied in phase 2, so a long-lived helper
  must be forked before the sandbox and given its own (narrower: no network, no fs) profile;
  on Linux it inherits pledge / Landlock and needs `/dev/dri` only. The fork must happen before
  the app's top level runs, which means before the manifest says whether the app opted in.
- The helper's verdict alone does not bound the parent's own compile: the parent still runs
  naga + the driver compiler for the real pipeline. Measured on Apple M1 Max with a shader near
  the round-40 function-size bound: the first compile takes ~0.64 s, a recompile of the same
  source on a fresh device in the same process ~0.01 s
  (`hull_cap_gpu_audit_convergence.recompile_on_a_fresh_device_measured`), and a recompile in a
  NEW process ~6 ms (Metal's system shader cache, which the sandboxed process could use). So on
  Metal a helper's compile warms the cache and the parent's compile of an approved source is
  cheap. Vulkan drivers keep pipeline caches too, but not uniformly, so the parent compile stays
  under the in-process deadline either way: the helper is a validation and cost gate, not a
  replacement for it.

Design sketch for the follow-up: `hull gpu-compile-helper` (an internal mode of the same binary),
forked by serve.c before phase 1 when a GPU context exists, connected over a socketpair; the
parent sends `(len, wgsl)` and waits up to the compile deadline; on expiry it `SIGKILL`s the
helper and starts a new one lazily; a source the helper accepted is compiled in-process. Its
sandbox: Seatbelt with GPU IOKit access only / pledge `stdio` plus the DRM device. Until then,
the opt-in is documented as "never compile request-supplied WGSL".

## Testing

- `test_gpu` (`hull_cap_gpu_audit_convergence`): embedded accepted (load and identical copy),
  dynamic refused without the opt-in (before the backend; the origin flag does not widen it; a
  binary with no shaders refuses all), dynamic accepted with the opt-in (pre-checks still run),
  dev mode compiles inline source, the opt-in compile is abandoned at the deadline (slow-compile
  test hook), the build check's pre-check and size cap, a real-device test compile, and the
  fresh-device recompile measurement.
- `test_lua` / `test_js` (`*_gpu_audit_convergence`): the same rules through the bindings over
  the mock backend, and the manifest field (`dynamic_shaders` / `dynamicShaders`, only `true`).
- `tests/e2e_feature_gpu.sh`: a built `--with=gpu` app loads its embedded shader, has inline
  source refused and an identical copy accepted, dispatches; an opt-in app compiles runtime
  source; a shader the pre-check refuses fails `hull build`.

## Migration

Move each inline shader to `shaders/<name>.wgsl` and replace `gpu.compile(name, SRC)` with
`gpu.load(name)`. The examples (`gpu_search`, `gpu_pipeline`, `compute_gpu_chain`) were moved.
