# Native Sidecar Services - Design

> **Status: design (Phase 0 complete).** This document is the
> committed design; implementation lives on the roadmap as §1.6 in
> [`roadmap_next.md`](roadmap_next.md). For the broader execution
> tiering (WASM at Tier 2, native sidecars at Tier 3, native in-
> process plugins forbidden) see [`security.md`](security.md) §3.A.
> For the existing WASM compute path see
> [`wamr_architecture.md`](wamr_architecture.md).

## A) Executive Summary

- **Goal.** Let Hull apps consume large native accelerators (`bitnet.c`,
  `llama.cpp`, GLPK, HiGHS, …) inside the same capability model the
  rest of an app runs under, **without growing Hull's trusted core**.
- **Mechanism.** A small generic sidecar host inside Hull (~2,000-
  2,500 LOC, ~30-50 KB binary growth) supervises out-of-process
  native services. All accelerator code lives in independent
  repositories. Hull's release pipeline does not bloat per
  supported accelerator.
- **Trust posture.** Native sidecars are Tier 3 (lower trust than
  WASM). They run in their own processes, under the strongest OS
  sandbox available on the platform (Linux seccomp + Landlock,
  OpenBSD pledge/unveil, macOS Seatbelt, Windows Job Object), with
  zero ambient authority. Hull-resolved file/directory resources
  are passed as inherited FDs; no path string crosses the
  boundary.
- **Wire protocol.** JSON-RPC 2.0 with LSP-style Content-Length
  framing for control. Opt-in length-prefixed binary frames on the
  same FD for hot streams (LLM tokens, audio, video, large
  matrices).
- **Trust chain.** Each sidecar declares either a vendor Ed25519
  pubkey OR a Sigstore attestation source (GitHub `org/repo` +
  Fulcio + Rekor). App manifests pin one or both. Verified at
  install time AND at every launch.
- **Concurrency model.** One supervisor process per declared
  service. Each sidecar manages its own internal model load/unload
  and inflight concurrency. Matches how `llama-server` and
  bitnet.c's reference implementation already work.
- **First production target.** `bitnet-sidecar` - CPU-only,
  statically linked, demonstrates end-to-end against a Hull app
  summarizing asset history on a Trimble HU dev VM. GPU,
  `llama-cpp-sidecar`, and an ergonomic `hull/llm@1` wrapper are
  explicitly deferred to follow-up phases.

## B) The "no bloat" constraint

This is the design's load-bearing principle, so it gets its own
section.

The execution tiering already documented in
[`security.md`](security.md) §3.A keeps four trust layers, with
in-process native plugins explicitly forbidden:

| Tier | Runtime | Trust posture | Mechanism |
|---|---|---|---|
| 0 | Hull core | Small trusted runtime | Audited C, signed releases, reproducible builds |
| 1 | Lua / JS app logic | Hull capability model + kernel sandbox | `hl_cap_*` boundary + pledge/unveil/Seatbelt |
| 2 | WASM plugins | Portable in-process sandbox, no I/O | WAMR with no WASI imports |
| 3 | **Native sidecars** | **Isolated native accelerators, out-of-process** | **This document** |
| 4 | Native in-process plugins | **Forbidden by default** | No `dlopen`, no `dlsym`; trusted/unsafe only via off-roadmap escape hatch |

Native sidecars give Tier-3 ergonomics - call a binary, get a
result - without collapsing Tier 2 or punching a hole into Tier 0.
Three concrete consequences:

1. **No accelerator source code in Hull's repo.** Not vendored,
   not built, not shipped. `bitnet.c`, `llama.cpp`, GLPK, HiGHS
   each live in their own repo with their own release pipeline and
   their own signing key.
2. **No accelerator-specific stdlib in Hull.** No `hull/llm@1` in
   the first cut. Apps call `services.call("bitnet", "generate",
   ...)` directly. A higher-level stdlib only ships after two
   sidecars exist so the abstraction is informed by real usage.
3. **No model-format knowledge in Hull.** Hull passes a directory
   FD; the sidecar finds, loads, and dispatches models inside it.
   No `.gguf` / `.safetensors` / quantization parsing in Hull's
   binary.

The numbers fall out of this: ~2,000-2,500 LOC of generic
supervision + RPC + sandbox in Hull's repo, ~30-50 KB binary growth
stripped. Adding `llama-cpp-sidecar` later adds zero bytes to
Hull's binary.

## C) Two-layer split

### Layer A - Hull repo (generic host)

| Component | File | Size | Purpose |
|---|---|---|---|
| Static `services` pre-extraction | `src/hull/manifest.c` | ~200 LOC | Parse the literal-only subset of `app.manifest({services = {...}})` BEFORE app code runs and BEFORE phase-1 pledge. Rejects function calls, env reads, conditionals, computed keys, anything non-literal. |
| Sidecar supervisor | `src/hull/services/supervisor.c` | ~600 LOC | One supervisor per declared service. Spawns process, sets up RPC socketpair, passes pre-opened resource FDs, applies sandbox + rlimits, monitors lifecycle, enforces restart policy. Event-driven on Keel's loop, no new thread. |
| JSON-RPC framing | `src/hull/services/rpc.c` | ~150 LOC | LSP-style Content-Length framing. Parse via existing `sh_json`. |
| Binary stream fast-path | `src/hull/services/rpc.c` | ~80 LOC | 4-byte length-prefix binary frames on the same FD when the sidecar's `rpc.discover` advertises `streaming.binary = true`. |
| Tool resolver extensions | `src/hull/tools_install.c` | ~200 LOC | New optional fields in the tool registry: `vendor_pubkey` (Ed25519 hex), `attestation_repo` (`org/repo` for Sigstore). Verifies at install time; either path sufficient. |
| Sandbox backends | `src/hull/services/sandbox_*.c` | ~500 LOC × 4 platforms | Linux seccomp + Landlock + rlimits; OpenBSD/Cosmo pledge/unveil; macOS Seatbelt + Hardened Runtime; Windows Job Object. Reuses the existing patterns from `sandbox.c`. |
| `hull/services@1` stdlib | `stdlib/{lua,js}/hull/services.{lua,js}` | ~200 LOC each | Thin wrapper: `services.call(name, method, params, opts)`, `services.stream(name, method, params, cb)`, `services.cancel(req_id)`, `services.stats(name)`. Token iterators (Lua coroutine, JS async generator). |
| `hull agent services` | `src/hull/agent/services.c` | ~150 LOC | JSON-out: declared services, resolved tool name+hash, actual sandbox level (`strong | good | partial`), pubkey/attestation source, PID, uptime, restart count, health. |
| **Total** | | **~2,000-2,500 LOC** | Binary growth ~30-50 KB stripped |

What is **not** in Hull's repo: any LLM-specific code, any model-
format knowledge, any tokenizer, any prompt-template logic, any
GPU dispatch code.

### Layer B - Independent repos

| Repo | What | Trust | Status |
|---|---|---|---|
| `artalis-io/hull-sidecar-sdk` | Small C library (~500 LOC + headers) any sidecar author links to get the RPC protocol for free. Handles FD setup, framing, resource-FD lookup, signal handling. Apache or MIT so third parties can use it. | Code only; consumers vendor it. | Phase 6. |
| `artalis-io/bitnet-sidecar` | bitnet.c + the SDK. Implements `generate`, `embed`, `tokenize`, `load_model`, `unload_model`, `health`, `stats`, `rpc.discover`. **Statically linked.** Ships `hull-bitnet-linux-x86_64`, `hull-bitnet-linux-aarch64`, `hull-bitnet-darwin-arm64`. | Vendor Ed25519 pubkey + Sigstore attestation from public CI. Pinned in app manifest. | Phase 7 (first production target). |
| `artalis-io/llama-cpp-sidecar` | Same SDK, llama.cpp inside. Adds GPU sandbox-degradation negotiation (separate design pass). | Same trust model. | Phase 9, after GPU design. |

## D) Manifest format

```lua
app.manifest({
    modules = { "hull/services@1" },
    services = {
        bitnet = {
            type      = "native-sidecar",
            tool      = "bitnet-worker@1",
            vendor    = {
                pubkey = "a4f2…hex",                       -- pinned per-app
                attestation_repo = "artalis-io/bitnet-sidecar",  -- OR Sigstore
            },
            transport = "stdio-fd",
            protocol  = "json-rpc",
            streaming = "binary",                          -- opts into fast-path
            restart   = "on-crash",
            trust     = "confined-native",
            models = {
                dir = { path = "data/models/bitnet", access = "dir-read" },
            },
            fs_write = { "data/cache/bitnet" },
            net      = { connect = {}, listen = {} },
            limits   = {
                memory_mb   = 8192,
                cpu_percent = 400,
                processes   = 1,
                open_files  = 64,
                wall_ms     = 600000,
            },
        },
    },
})
```

**Trust rule.** Either `vendor.pubkey` OR `vendor.attestation_repo`
is sufficient. Both together is belt-and-suspenders. Hull verifies
whichever is present at install time and at every launch; refuses
the bind otherwise.

**Static-extraction rule.** Every value in the `services` block
must be a literal table, array, string, number, or boolean. The
parser rejects function calls, `require`/`import`, env reads,
string concatenation, conditionals, loops, computed keys, or
runtime values. This is enforced before app code runs so sidecars
can be launched **before** phase-1 pledge seals `exec`/`proc`/
`fork`. Normal app capability extraction (`fs`, `hosts`, `env`)
still runs at the usual time on the same `app.manifest()` call;
only the `services` subset is privileged.

**Local-dev escape hatch.** During development a sidecar may be
declared as `dev_exec = "./bin/bitnet-worker"` instead of `tool =
"bitnet-worker@1"`. This emits a loud diagnostic, disables Sigstore
verification, and refuses to attach a production-signing claim to
any signature output. There is no way to ship a release with
`dev_exec` set; `hull build` rejects it.

## E) Resource passing model

Sidecars receive Hull-resolved resources, not ambient path
authority. The contract is "the child sees only the FDs Hull
intentionally gave it."

### On Unix

For each declared resource Hull opens the path with the requested
access mode (read-only for `dir-read`, read/write for `dir-rw`,
etc.) and inherits the FD across `execve`. The FD number is
exposed to the child via an env var:

```
HULL_RESOURCE_models=10
HULL_RESOURCE_cache=11
```

The sidecar SDK turns these into usable handles via `fdopendir(10)`
or `openat(10, name, …)`. No path strings cross the boundary; the
sidecar literally cannot open files outside what Hull granted.

### On Windows

Inherited handles via `STARTUPINFOEX` + `PROC_THREAD_ATTRIBUTE_
HANDLE_LIST`. Handle numbers exposed via the same env-var pattern;
SDK abstracts the platform difference.

### What's closed before exec

Everything except: stdio (with `stdin` pointed at `/dev/null` by
default, `stdout`/`stderr` at the supervisor's log pipe), the RPC
FD, and the declared resource FDs. Listing-based close is used on
Linux (`/proc/self/fd`) and BSDs (`closefrom()`); on Windows the
default `bInheritHandles = FALSE` is overridden only by the explicit
inherit list.

## F) Wire protocol

### Control plane: JSON-RPC 2.0 + LSP framing

```
Content-Length: 87\r\n
\r\n
{"jsonrpc":"2.0","id":1,"method":"generate","params":{"model":"bitnet-3b","prompt":"…"}}
```

- `Content-Length` is the byte count of the body.
- Body MUST be valid JSON-RPC 2.0.
- One frame at a time on a single bidirectional FD.
- The RPC FD is its own socketpair; **never** stdin/stdout. stderr
  is logs only, captured by the supervisor.

### Discovery handshake

First message after launch is always `rpc.discover`:

```json
{"jsonrpc":"2.0","id":1,"method":"rpc.discover"}
```

Sidecar responds with capabilities:

```json
{
  "jsonrpc":"2.0","id":1,
  "result":{
    "sdk_version":"1.0.0",
    "methods":["health","generate","embed","tokenize","load_model","unload_model","stats","cancel"],
    "streaming":{"binary":true,"max_frame_bytes":65536},
    "limits":{"max_inflight":4,"max_prompt_tokens":8192}
  }
}
```

Hull caches the discovery result; subsequent `services.call`
invocations validate the method exists before sending.

### Streaming hot path: binary frames

When `streaming.binary = true` was advertised AND the app called
`services.stream(...)`, the sidecar may reply with binary frames
instead of JSON-RPC notifications:

```
[4-byte big-endian length N][N bytes of opaque payload]
```

Framing is unambiguous because every JSON-RPC frame begins with
`Content-Length:` (an ASCII header), and every binary stream frame
begins with a raw 4-byte length. The supervisor peeks the first
byte: if it's `C` (0x43), parse as JSON-RPC; otherwise parse as
binary. The binary fast-path is always associated with an
in-flight request id, transmitted as the first 4 bytes of the
payload before any opaque tokens. The final reply to the request
is always JSON-RPC (with `result` or `error`), signalling
completion.

Apps in Lua / JS see:

```lua
for token in services.tokens("bitnet", "generate", {prompt = "..."}) do
    print(token)
end
```

```javascript
for await (const token of services.tokens("bitnet", "generate", {prompt: "..."})) {
    console.log(token);
}
```

### Cancellation

Standard LSP-style:

```json
{"jsonrpc":"2.0","method":"$/cancelRequest","params":{"id":42}}
```

Plus Hull-enforced wall-clock deadlines per call. On timeout Hull
sends `$/cancelRequest`, waits a grace period (default 250ms),
then terminates the sidecar per restart policy.

## G) End-to-end flow

```
1. PRE-EXTRACTION
   Hull reads `services = {...}` literally from app source
   BEFORE phase-1 pledge.

2. TOOL RESOLVE
   Hull verifies `bitnet-worker@1` exists in ~/.hull/tools/,
   signed by pinned pubkey OR has a valid Sigstore attestation
   matching attestation_repo. Hash verified.

3. RESOURCE OPEN
   Hull opens data/models/bitnet/  read-only   -> FD 10.
   Hull opens data/cache/bitnet/   rw          -> FD 11.
   Hull creates RPC socketpair                 -> FD 3.
   Hull creates stderr pipe                    -> FD 2.

4. SANDBOX SETUP (in child, after fork before exec)
   Empty env + HULL_RESOURCE_models=10 + HULL_RESOURCE_cache=11.
   Empty cwd.
   Close all FDs except {2, 3, 10, 11}.
   Apply rlimits (memory_mb, cpu_percent, processes, open_files,
   wall_ms).
   Apply Linux seccomp + Landlock (or platform equivalent).

5. EXEC
   execve(bitnet-worker, argv, envp).
   Phase-2 sandbox applied to Hull (the parent) AFTER the
   fork+exec dance is done - Hull's own sandbox is unchanged.

6. HANDSHAKE
   Hull sends {"jsonrpc":"2.0","method":"rpc.discover","id":1}
   on FD 3. Sidecar responds with capabilities including
   streaming.binary = true.

7. APP CALL (streaming)
   services.stream("bitnet", "generate",
                   {model = "bitnet-3b", prompt = "..."},
                   function(token) ... end)

   Hull frames the request as JSON-RPC, sends on FD 3.
   Sidecar processes; for each token, writes a binary frame
   to FD 3 (request id + 4-byte length + payload).
   Hull dispatches each frame to the app callback.
   Final JSON-RPC response signals completion.

8. SHUTDOWN
   SIGTERM -> grace period (default 5s) -> SIGKILL.
   Stale socket cleanup. State written to ~/.hull/services/
   <name>.json for `hull agent services`.
```

## H) Threat model

Native sidecars sit at Tier 3 - lower trust than WASM. They run in
their own address space, under the strongest OS sandbox available,
with the smallest possible ambient authority. The execution tiering
in [`security.md`](security.md) §3.A stays exactly as documented.
This design adds the following enforced mitigations:

| Risk | Mitigation |
|---|---|
| Sidecar reads arbitrary files via path string | Resources passed as FDs only; sidecar gets `HULL_RESOURCE_models=10` and uses `fdopendir(10)`. No path string crosses the boundary. |
| Sidecar spawns child processes | `processes = 1` rlimit + seccomp denial of `fork`/`clone`/`execve`. macOS uses Seatbelt `(deny process-fork)`. |
| Sidecar opens network | `net.connect = {}` + seccomp denial of `connect`/`bind`. Landlock network rule on supported kernels. |
| Sidecar leaks Hull FDs back in | Hull closes everything except declared RPC + stdio + resource FDs before `execve`. SDK refuses to `dup` arbitrary FDs. |
| Sidecar binary swapped post-install | Hash + signature re-verified at every launch, not just install. |
| Restart loop DoS | Restart-rate limit (default max 3 in 60s); supervisor escalates to `failed` state and exposes via `hull agent services`. |
| Stream backpressure / app slow consumer | Bounded write queues per service; bounded inflight requests; binary frames have explicit per-request flow control. |
| Logs leak into RPC stream | RPC and stderr are separate FDs from setup. SDK refuses to write to FD 3 outside the frame writer. |
| TCP listen as ambient channel | TCP listen disabled by default; requires explicit `net.listen`; defaults to localhost-only when granted. |
| Dynamic-linker discovery | Sidecars MUST be statically linked or ship their loader path explicitly. `hull tools install` rejects sidecars with unresolved `DT_NEEDED` entries on Linux unless they declare an allowlist. |
| Sandbox unsupported on platform | Fail-closed unless operator passes `--allow-degraded-sandbox` (development only). `hull agent services` reports actual `sandbox.level`. |

### Error model (mirrors §1.6 of roadmap_next.md)

| Class | Meaning |
|---|---|
| `rpc_error` | Valid JSON-RPC error returned by a method |
| `protocol_error` | Malformed JSON, bad frame, invalid id, unsupported method |
| `transport_error` | EOF, EPIPE, timeout, reset on the RPC FD |
| `service_exit` | Sidecar process exited (status/signal) |
| `sandbox_violation` | OS denied or killed the sidecar |
| `supervisor_error` | Launch failed, bad tool, denied path/resource/capability |
| `app_error` | Lua/JS misuse of the service API |

Each class is surfaced separately to the app and audited
separately by `hull agent services`.

## I) Trust chain

Sidecars use the same Ed25519 + Sigstore three-tier-trust posture
the Hull binary itself uses ([`security.md`](security.md) §7), with
one structural difference: **per-vendor pubkeys are pinned by the
app manifest**, not by Hull's embedded constants. This keeps Hull
out of the business of curating which sidecars are "blessed."

### Verification paths (either is sufficient)

1. **Vendor pubkey path.** The tool registry entry for `bitnet-
   worker@1` carries `vendor_pubkey = "a4f2…"`. The app's manifest
   either pins the same hex or accepts the registry's value (one
   line; explicit by convention). `hull tools install` downloads
   the sidecar binary + its Ed25519 signature, verifies against
   the pubkey, refuses on mismatch. The same verification reruns
   on every launch.
2. **Sigstore path.** The tool registry entry carries
   `attestation_repo = "artalis-io/bitnet-sidecar"`. `hull tools
   install` fetches the Sigstore bundle (`.sigstore` file from the
   release), validates the Fulcio cert chain, validates the Rekor
   log entry, validates the cert's subject matches the declared
   GitHub repo. No long-lived signing keys involved on the vendor
   side.

Both paths together is the recommended default for production
sidecars - the vendor pubkey is the fast / offline-friendly check,
Sigstore is the auditor's independent confirmation.

### What this means operationally

- **Hull's release pipeline.** Adding `bitnet-sidecar` does NOT
  require a Hull release. The sidecar repo cuts releases on its
  own schedule.
- **Auditor's job.** "Show me every sidecar this app trusts" is
  `hull agent services` plus the manifest. No hidden trust.
- **Forking.** A fork that signs its own sidecars and pins its own
  pubkeys works out of the box - Hull doesn't enforce any
  specific signing identity.

## J) Sandbox backends

The actual enforcement level is reported at runtime and in
`hull agent services`. Operators must not confuse "native
sidecar" with WASM-grade containment.

| Level | Platforms | Backend |
|---|---|---|
| `strong` | Linux | `no_new_privs` + seccomp + Landlock + rlimits + FD discipline |
| `strong` | OpenBSD / Cosmopolitan where supported | `pledge` + `unveil` + rlimits + FD discipline |
| `good` | macOS | Seatbelt SBPL + Hardened Runtime + rlimits + inherited-handle discipline |
| `partial` | FreeBSD | Capsicum where available, otherwise documented degradation |
| `partial` | Windows | Job Object + restricted token / AppContainer later |

`hull agent services` surfaces the actual level:

```json
{
  "name": "bitnet",
  "pid": 1234,
  "uptime_s": 4521,
  "restarts": 0,
  "tool": "bitnet-worker@1",
  "tool_hash": "sha256:...",
  "trust": {"pubkey": "a4f2…", "attestation": "rekor:..."},
  "sandbox": {"level": "strong", "backend": "linux-seccomp-landlock", "degraded": false},
  "health": "ok"
}
```

If a platform cannot enforce a requested capability boundary,
default behavior is fail-closed unless the operator passes
`--allow-degraded-sandbox`.

## K) Keel vs Hull ownership

Keel owns reusable event-loop transport primitives only: FD
watchers, timers, write queues, framing parser/serializer, and
optional JSON-RPC client/server helpers if they stay dependency-
light.

Hull owns process boundaries: manifest parsing, tool resolution,
service supervision, process launch, FD/handle/resource passing,
sandbox/resource limits, capability checks, audit logs, app
bindings, and lifecycle policy.

Start with a thin `hull/rpc` layer above Keel. Promote
transport-neutral pieces to Keel later only if they prove useful
outside Hull.

## L) Phased plan

- [x] **Phase 0** - design doc (this document) + threat-model
      section + ABI lock for the SDK + accepted design decisions
      on trust model, model lifecycle, RPC protocol, and
      concurrency.
- [ ] **Phase 1** - Hull-core: static `services` pre-extraction +
      supervisor + stdio-fd JSON-RPC + `rpc.discover` + `health` +
      sandbox stubs. Local-dev escape hatch `dev_exec` for
      iteration. **1.5 weeks.**
- [ ] **Phase 2** - Hull-core: binary stream fast-path; resource-
      FD passing (`HULL_RESOURCE_*` env + open FDs); `hull/services
      @1` stdlib with `services.stream` iterator. **1 week.**
- [ ] **Phase 3** - Hull-core: full lifecycle supervision
      (readiness, restart policy, log capture, graceful shutdown);
      `hull agent services`. **1 week.**
- [ ] **Phase 4** - Hull-core: Linux seccomp + Landlock backend;
      macOS Seatbelt backend; OpenBSD/Cosmo pledge/unveil backend;
      Windows Job Object backend. **2 weeks.**
- [ ] **Phase 5** - Hull-core: tool resolver extensions for
      `vendor_pubkey` + `attestation_repo`; `hull tools install`
      verifies both paths. **1 week.**
- [ ] **Phase 6** - Sidecar side: `hull-sidecar-sdk` repo
      (independent). Documented ABI. Apache/MIT. **1 week.**
- [ ] **Phase 7** - Sidecar side: `bitnet-sidecar` repo
      (independent). First end-to-end demo: a Hull app summarizing
      asset history via bitnet on a Trimble dev VM. **1.5 weeks.**
- [ ] **Phase 8** - Hull-core hardening based on Phase 7 findings.
      Audit pass (parallel-reviewer cadence per `/auth-audit`).
      Document Sigstore attestation flow. **1 week.**
- [ ] **Phase 9** (deferred) - GPU sandbox design;
      `llama-cpp-sidecar`; optional `hull/llm@1` ergonomic
      wrapper. **Unscheduled.**

**Total Phase 0 → bitnet working end-to-end: ~10 weeks.**

## M) Explicitly deferred

- **GPU access.** First sidecar (bitnet) is CPU-only. GPU device-
  FD passing, the sandbox holes it punches (Vulkan/Metal/CUDA
  device-file access), and per-platform GPU-driver concerns get
  their own design pass after bitnet ships. `llama-cpp-sidecar`
  cannot ship before this.
- **`hull/llm@1` high-level stdlib.** Pure orchestration - token
  iterators, conversation state, prompt templates, chat history.
  Lives entirely above `hull/services@1`. Defer until two
  sidecars exist so the abstraction is informed by real usage,
  not anticipated usage.
- **Model registry (`hull models pull`).** Sidecar owns model
  lifecycle; Hull just passes a directory FD. Revisit only if
  model discovery/install becomes a UX bottleneck.
- **TCP transport for sidecars.** Stays gated behind explicit
  manifest capability and localhost-only default. Stays deferred
  until at least one sidecar needs it (bitnet doesn't).
- **Sidecars spawning sub-sidecars.** Out of scope; sandbox denies
  subprocess creation.
- **`hull sandbox run/service` CLI surface.** This is §1.7 in the
  roadmap, designed as a follow-on that reuses this document's
  supervision primitives without RPC. Separate user-facing
  product; ships after this document's Phase 1-3 are in.

## N) Hard parts / security traps (reminder list)

- **Manifest timing.** Sidecars cannot be discovered by executing
  app code after phase-1 pledge; service declarations need static
  pre-extraction.
- **Dynamic linker behavior.** Native tools may need shared
  libraries unless shipped static. Prefer static sidecar artifacts;
  reject unresolved `DT_NEEDED` at install time.
- **Inherited FDs/handles are ambient authority.** Close everything
  except explicit RPC/log/resource handles.
- **Environment variables leak authority.** Start from an empty env
  and add only declared values plus `HULL_RESOURCE_*`.
- **`cwd` leaks filesystem reachability.** Set to a controlled
  service workdir.
- **TCP can accidentally become an undeclared local service.** Keep
  it off until capability gates and tests are in place.
- **Subprocess spawn must be blocked** by sandbox/resource policy,
  not just by Hull convention.
- **Logs must never share the RPC stream.** Separate FDs from
  setup; SDK enforces.
- **Restart loops can DoS the host.** Cap restart rate; expose
  status.
- **Native sidecars are lower trust than WASM.** Documentation,
  `hull inspect`, and `hull agent services` output must make this
  explicit.

## O) Out of scope

Arbitrary user-controlled process execution, in-process native
plugins, unrestricted `dlopen`, plugin package managers, remote
sidecar orchestration, and any claim that sidecars are equivalent
to WASM sandboxing.
