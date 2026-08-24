# Spike: durable-execution "strict mode" - WASM replay vs. Lua/JS-strict

**Status:** SPIKE FINDINGS. De-risks Phase 2 of
[docs/jobs_durable_execution_design.md](jobs_durable_execution_design.md).
**Question:** the RFC proposed a Phase 2 "deterministic **WASM-replay** strict
mode" - run the workflow body as a WASM module so the sandbox structurally
enforces determinism. Is that the right build, and what does it actually take?
**Verdict (short):** **No - do not build WASM-replay for workflow orchestration.**
A **Lua/JS-strict mode** (memoized deterministic primitives + traps on ambient
non-determinism) delivers the practical determinism guarantee for a fraction of
the cost, keeps the ergonomic authoring model, and reuses the Phase-1 step-memo
engine. **Validated with a working proof (below).** WASM determinism stays the
right tool for *pure compute inside a step*, which Hull already ships
(`compute.call`).

---

## 1. What "deterministic replay" needs

Phase 1 (step-memoization) already gives crash-safe resume: the workflow body
re-runs from the top and completed `ctx.step`s return memoized results. Its one
contract is that the **step-call sequence is stable across replays**. That breaks
only if the body branches on **ambient non-determinism** read *outside* a step -
wall clock, RNG, an un-stepped external read - so a replay reaches a different
step and the memo keys no longer line up.

So "strict mode" = **remove ambient non-determinism from the body**. There are
two ways to do that.

## 2. Path A - WASM replay (the RFC's proposal). Grounded findings

Run the workflow body as a `wasm32` module: the sandbox has no clock/RNG/IO, so
every effect must be a host call, and re-execution is bit-for-bit deterministic.
Investigating Hull's actual WASM layer (`src/hull/cap/wasm.c`,
`include/hull/cap/wasm.h`) surfaced four hard problems:

1. **Steps can't be WASM.** A workflow *step* is the effectful part (a DB write,
   an HTTP call). WASM has no I/O, so steps must run in the host. That splits a
   workflow across a **WASM orchestration body + host-side step bodies** - the
   author writes the control flow in C/Rust and the steps somewhere else. A poor,
   bifurcated authoring model versus "a workflow is a normal Lua/JS function."
2. **The CALLBACK ABI returns only an int.** `host_call(CALLBACK, ...)` hands the
   guest back a single `int32` status - the code comment in `cap/wasm.c` is
   explicit: `out_buf` is "not currently copied back to WASM linear memory - only
   the integer return value is visible to the guest." A memoized step **result**
   (JSON, a struct) can't flow back into the WASM body without an **ABI
   extension** (host to guest data return).
3. **No mid-execution yield.** `wasm_runtime_call_wasm` is run-to-completion;
   there is no suspend/resume. `ctx.sleep`/`ctx.wait_signal` (which must yield and
   reschedule) would need a cooperative "return-early on a host-signalled yield"
   protocol layered on top - workable, but more new ABI.
4. **A whole build path.** workflow-source → `.wasm`, plus the host-call ABI for
   step/sleep/signal/memo, plus replay tooling. This is a multi-week subsystem.

**Marginal benefit for orchestration.** The determinism WASM adds over Path B is
"the *interpreter itself* can't drift." For *effectful orchestration* whose
side-effects already live in host-side steps, that extra guarantee is largely
theoretical - the body is glue, not compute. WASM's real determinism win is for
**pure compute**, which is already reachable *inside* a step via `compute.call`
(a jobs handler / workflow step can already run a gas-metered, deterministic WASM
module - see the "Compute-heavy jobs" note in docs/jobs.md).

## 3. Path B - Lua/JS-strict (recommended). Validated

Keep the workflow a normal Lua/JS function; make replay deterministic by
**memoizing the ambient non-determinism through the existing step store**:

- Provide `ctx.now()`, `ctx.random()` (and `ctx.uuid()`, ...) that record their
  value on first run and **return the stored value on every replay** - built
  on the Phase-1 `run_step` memo, ~15 lines total.
- (Enforcement, optional) During the workflow body, install a **strict
  environment** that traps direct `time.now` / `math.random` / `Math.random`
  calls, so accidental ambient non-determinism raises instead of silently
  breaking replay. The supported way to be non-deterministic becomes "go through
  `ctx.*`", which is memoized.

This is the DBOS/Restate model (deterministic ctx-provided primitives + a step
contract), and it needs **no new ABI, no build path, no authoring change**.

### Proof (run on `main`, Phase-1 engine)

A `ctx.now()` + `ctx.random()` memoized via `run_step`, in a workflow that fails
its first attempt (forcing a replay):

```lua
jobs.workflow("det", function(w)
  local n = w.now()      -- deterministic across replays
  local r = w.random()
  runs[#runs+1] = n .. ":" .. string.format("%.6f", r)
  if #runs == 1 then error("force one retry") end   -- fail once -> replay
  return { n = n, r = r }
end)
```

Result - the first attempt and the replay are **byte-identical**:

```
DET run1=1785970208:0.896827 run2=1785970208:0.896827 identical=YES
```

The replay saw the same clock and the same random draw the first attempt did.
Determinism achieved with the Phase-1 memo engine, zero WASM.

## 4. Recommendation

- **Build Path B as Phase 2.** Ship memoized deterministic primitives
  (`ctx.now`, `ctx.random`, `ctx.uuid`) + a `strict` opt-in on `jobs.workflow`
  that installs the ambient-non-determinism traps. Both runtimes, pure stdlib,
  no C. Small (~a day), high value, and it makes the Phase-1 "keep
  non-determinism in steps" contract *enforced* instead of *advised*.
- **Do NOT build Path A (WASM replay) for orchestration.** The authoring split
  (WASM body + host steps), the ABI extensions (data-return + yield protocol),
  and the build path are a large cost for a benefit that is marginal over Path B
  for effectful glue.
- **Keep WASM determinism where it belongs:** *pure compute within a step*, which
  already works via `compute.call`. A workflow that needs a deterministic heavy
  computation runs it as a compute module inside a `ctx.step` - the best of both
  (deterministic compute, ergonomic orchestration) with nothing new to build.

## 5. Proposed Phase 2 (Path B) surface

```lua
jobs.workflow("wf", function(ctx)
  local ts  = ctx.now()      -- memoized: same on every replay
  local r   = ctx.random()   -- memoized
  local id  = ctx.uuid()     -- memoized
  ...
end, { strict = true })       -- opt-in: trap direct time.now / math.random in the body
```

- `strict` defaults **off** (Phase 1 stays a superset - existing workflows
  unchanged). On, the body runs with `time.now`/`math.random` (Lua) and
  `Date.now`/`Math.random` (JS) shadowed by trapping stubs that point the author
  at `ctx.*`.
- The memoized primitives are ordinary `__now:N` / `__rand:N` step rows (hidden
  from `workflow_status.steps_done`, like `__sleep:N`), swept by `jobs.cleanup`.
- Interop is unchanged: steps, timers, signals, saga all work as-is; strict mode
  only tightens the body's determinism.

## 6. Residual risks (Path B)

- **Un-trappable non-determinism** (e.g. Lua table iteration order via `pairs`,
  or a step whose *own* output is non-deterministic) is still the author's
  responsibility - Path B enforces the *known, common* sources, not all of them.
  This is the same practical guarantee DBOS/Restate give; full structural
  determinism (Path A) is the only way to close 100%, at the cost above. For a
  job runner this trade is clearly correct.
- Trapping the ambient globals per-run needs care in both sandboxes (swap on
  entry, restore on exit, including across a `ctx.step` that legitimately does
  I/O). Feasible; to be designed in the Phase-2 PR.
