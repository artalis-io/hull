# Hull — Next Features Roadmap

Status: **Active** | Last reviewed: 2026-05-27

Companion to [`roadmap.md`](roadmap.md). That doc records what's built;
this one tracks the **next** feature batches in priority order.

For completed historical roadmaps see [`archive/roadmaps/`](archive/roadmaps/).

---

## Shipped since this file was last revised

- ✅ **v0.1.0 release** — Ed25519-signed `hull.sha256` manifest, four native+APE binaries (linux-x86_64, linux-aarch64, darwin-arm64, cosmo), one-line install script, `hull update` with signature verification. See [`../CHANGELOG.md`](../CHANGELOG.md).
- ✅ **gethull.dev landing site** — S3 + CloudFront, deploy-site workflow, browser verifier at `/verify.html` covering all three signature tiers, trust-chain section + honest scorecard.
- ✅ **WebSocket support** — `app.ws(path, handlers)` server + `ws.connect(url, …)` client, broadcast, per-connection state, host allowlist on client. Documented in [`../CLAUDE.md`](../CLAUDE.md) "WebSocket Endpoints".
- ✅ **SSE support** — `app.sse(path, handler)` with `stream:event(name, data, [id])` and `stream:close()`.
- ✅ **`hull deploy`** — Dockerfile, systemd, fly.toml targets; `hull agent deploy` JSON readiness analysis.
- ✅ **Extended `hull agent`** — 16 new subcommands (Phase 6) covering manifest preview, request preview, single-file validate, eval, schema-diff, sql-named, vfs/compute/gpu/perf/logs/template/compute-call. Wired into MCP. See [`agent_guide.md`](agent_guide.md) §5.
- ✅ **v0.1.2 batch** — `hull tools install/list/uninstall` (first tool: wamrc), shared `release_io.{c,h}` extracted from `commands/update.c`, top-level `hull help`, audit fixes (OOB defense, JSON escape, fsync/close checks, constant-time SHA-256 compare), agent surface expansion (`hull agent tools/overview` + `agent context --list` + wamrc state in agent compute), six new opinionated context docs (orientation, quickstart × 3, gpu, tools), discoverability breadcrumbs in `hull --help` + bare-hull + install.sh, `build-wamrc` CI matrix. See [`../CHANGELOG.md#012`](../CHANGELOG.md).

---

## 1. PostgreSQL backend (HlDbBackend)

**Priority:** Medium-High — first non-SQLite backend; validates the DB-vtable
abstraction that already powers `HL_ENABLE_DB=0` compute-only builds.

**Approach:**

- `HlDbBackend` implementation using libpq.
- Connection string via `--db postgres://…` or per-handler config.
- Statement caching via `PQprepare`.
- Async queries via libpq's async protocol (not worker threads — avoids the
  extra hop and gives us pipelining).
- Hull internals (`_hull_*` tables) **stay on embedded SQLite** so apps can
  be ported incrementally; only application tables move to Postgres.

**Tasks:**

- [ ] Vendor or dynamic-link libpq (choose: more deps but real prod use vs.
      simpler distribution)
- [ ] `src/hull/cap/db_postgres.c` — backend vtable impl
- [ ] Connection pooling (single connection vs. pool — start with pool)
- [ ] Parameter binding (Hull's `?` placeholder → Postgres `$1, $2…` rewrite)
- [ ] Type mapping (HlValue ↔ Postgres OIDs)
- [ ] Migration runner compatibility (Postgres dialect for `_hull_migrations`)

**Out of scope:** transactions across SQLite + Postgres (no XA / two-phase
commit). Apps that mix both must accept eventual consistency.

---

## 1.5 Hypermedia web application profile (HTMX + Pico)

**Priority:** High — fills the gap between full-page SSR apps and
React-style SPAs for business workflow software. The target use case is
internal tools such as IT asset trackers, admin consoles, inventory systems,
approval workflows, CRM-like dashboards, and other CRUD-heavy applications
where the server should remain the source of truth.

**Thesis:**

Hull already has the right backend primitives for secure SSR: templates,
forms, sessions, CSRF, RBAC, SQLite, migrations, static files, search, CSV,
email, audit logging, and single-binary deployment. HTMX adds partial page
updates while preserving HTML as the application protocol. Pico.css provides a
classless baseline that rewards semantic templates and avoids a frontend build
pipeline.

The desired application profile is:

```
templates/
  base.html
  pages/
  fragments/
static/
  vendor/htmx.min.js
  vendor/pico.classless.min.css
  app.css
```

No CDN by default. Assets are vendored into `static/`, embedded by
`hull build`, served from `/static/*`, and covered by a self-hosted CSP.

**Tasks:**

- [ ] Add a small `hull.htmx` helper module:
      `is(req)`, `boosted(req)`, `redirect(req, res, path)`,
      `retarget(res, selector)`, `reswap(res, mode)`,
      `trigger(res, event, payload)`.
- [ ] Add `hull new --profile htmx` / scaffold support with vendored HTMX,
      vendored Pico classless CSS, base layout, fragment layout, CSRF forms,
      and no JavaScript build step.
- [ ] Add a named CSP profile for self-hosted hypermedia apps, equivalent to
      `script-src 'self'`, `style-src 'self' 'unsafe-inline'`, `img-src 'self'`,
      `form-action 'self'`, and `frame-ancestors 'none'`.
- [ ] Add `examples/hypermedia_todo` or `examples/asset_tracker` showing
      progressive enhancement: normal links/forms work without JavaScript,
      HTMX requests receive fragments, and full browser navigation receives
      complete pages.
- [ ] Extend the test helpers and examples to send `HX-Request: true` and
      assert fragment responses, redirects, retargeting, and triggered events.
- [ ] Document the pattern: "return full pages for ordinary navigation,
      fragments for `HX-Request`", including CSRF, validation errors, flash
      messages, and empty states.

**Enterprise/internal-app gaps this profile should unblock or make explicit:**

- [ ] Streaming multipart uploads. Current request bodies are capped at 1 MB;
      real asset trackers need photos, invoices, receipts, PDFs, and bulk
      import files.
- [ ] Attachment/document storage helpers: declared `fs_write` location,
      checksum, MIME type, size limits, metadata table, and secure download
      routes.
- [ ] OIDC middleware for enterprise SSO. SAML/LDAP/SCIM can follow, but OIDC
      should be the first supported path for company identity providers.
- [ ] First-party app audit-log helper distinct from capability audit logs:
      record who changed which business object, before/after values, source
      request, timestamp, and optional reason.
- [ ] Reusable admin UI conventions for paginated tables, filter forms,
      inline validation, confirm-delete flows, flash messages, and optimistic
      row replacement.
- [ ] Import/export workflow helpers: CSV preview, per-row validation errors,
      dry-run mode, commit step, and background processing hooks for large
      imports.

**Out of scope:** adopting React/Vue/Svelte, client-side hydration, npm as a
required app build step, or making HTMX a runtime dependency of Hull itself.
HTMX and Pico should remain vendored static assets at the application layer;
Hull provides the server-side conventions and helpers.

---

## 1.6 Native sidecar services (`hull.services`)

**Priority:** High — enables large native accelerators such as bitnet.c /
llama.cpp-style inference engines without embedding them into Hull's trusted
core and without pretending they are as safe as WASM plugins. Native sidecars
are lower-trust, out-of-process services with explicit capabilities, narrow
RPC, supervised lifecycle, and OS sandboxing where available.

**Security stance:**

Preserve the execution tiering:

| Tier | Runtime | Trust posture |
|---|---|---|
| 0 | Hull core | Small trusted runtime |
| 1 | Lua/JS app logic | Hull capability model + kernel sandbox |
| 2 | WASM plugins | Portable in-process sandbox, no I/O |
| 3 | Native sidecars | Isolated native accelerators, out-of-process |
| 4 | Native in-process plugins | Forbidden by default; trusted/unsafe only |

Native sidecars must never become a general `exec` escape hatch. They are
declared services resolved by Hull, launched by Hull, sandboxed by Hull, and
communicated with through Hull-owned transports. Sidecar tool metadata may
declare what the tool can support, but the app manifest remains the final
authority; tool metadata must not expand app capabilities.

**Key architecture constraint:**

Current server startup applies phase-1 pledge before loading app code, and
that phase intentionally blocks `exec`/`proc`/`fork`. Do not weaken this. To
keep services in `app.manifest()` while still spawning before the sandbox is
sealed, Hull needs a manifest pre-extraction pass for privileged launch-time
declarations.

The `services` block must be statically extractable from a literal
`app.manifest({...})` table/object before executing the app:

- Accept: literal strings, numbers, booleans, arrays, and objects/tables.
- Reject for service declarations: function calls, imports/requires, env
  reads, string concatenation, conditionals, loops, computed keys, or runtime
  values.
- Normal app capability extraction can still run after app load; native
  sidecar launch decisions must come from the static subset.

**Manifest sketch:**

```lua
app.manifest({
    modules = { "hull/services@1" },
    services = {
        bitnet = {
            type = "native-sidecar",
            tool = "bitnet-worker@1",
            transport = "stdio-fd", -- stdio-fd | unix | tcp
            protocol = "json-rpc",
            restart = "on-crash",
            trust = "confined-native",
            models = {
                main = {
                    path = "data/models/bitnet/model.gguf",
                    access = "fd-read",
                },
            },
            fs_write = { "data/cache/bitnet" },
            net = { connect = {}, listen = {} },
            limits = {
                memory_mb = 8192,
                cpu_percent = 400,
                processes = 1,
                open_files = 64,
                wall_ms = 600000,
            },
        },
    },
})
```

Prefer `tool = "name@major"` over `exec = "./path"`. Hull resolves tools via
the signed `hull tools install` registry, platform-specific artifacts, hashes,
install paths, and optional sidecar metadata. Local executable paths should be
development-only escape hatches, e.g. `dev_exec = "./bin/bitnet-worker"`, with
loud diagnostics and no production-signing claim.

**Resource passing model:**

Sidecars should receive Hull-resolved resources, not ambient path authority.
For model files, prefer pre-opened read-only resources:

```json
{
  "models": {
    "main": {
      "resource": 10,
      "kind": "file-read",
      "size": 4213379012,
      "label": "main"
    }
  },
  "cache": {
    "resource": 11,
    "kind": "dir-rw"
  }
}
```

On Unix, resources are inherited FDs or passed via `SCM_RIGHTS`. On Windows,
they are inherited handles. The protocol should call them "resources", not
POSIX-only FDs. The sidecar should not discover arbitrary model/cache paths on
its own.

**Transport and protocol model:**

- Default transport: dedicated inherited RPC FD (`stdio-fd`), not child
  stdin/stdout. Keep stdin/stdout closed or pointed at `/dev/null`; stderr is
  logs only. This avoids log/protocol mixing.
- Phase 2 transport: Unix domain sockets for long-running local daemons,
  including socketpair and filesystem socket paths.
- TCP: design now, but keep disabled by default until listen/connect
  capabilities are explicit and audited. Default TCP binding must be
  localhost-only unless manifest says otherwise.
- Initial protocol: JSON-RPC 2.0 with Content-Length framing
  (LSP-style), not newline-delimited JSON. This supports pretty JSON, robust
  framing, future binary headers, and clean parser error handling.
- Streaming responses: JSON-RPC notifications keyed by request id, e.g.
  `$/stream` events for tokens/progress/done.
- Cancellation: LSP-style `$/cancelRequest` plus Hull-enforced local
  deadlines. On timeout, send cancel, wait a grace period, then terminate or
  restart by policy.
- Backpressure: bounded write queues, bounded in-flight requests, bounded
  stream buffering, and explicit failure when the app does not consume fast
  enough.

**Example app API:**

```lua
local services = require("hull.services")

local out = services.call("bitnet", "generate", {
    model = "main",
    prompt = "Summarize this asset history",
    max_tokens = 256,
}, { timeout_ms = 60000 })
```

Sidecar method baseline:

- `rpc.discover`
- `health`
- `load_model`
- `unload_model`
- `generate`
- `embed`
- `tokenize`
- `cancel`
- `stats`

**Keel vs Hull ownership:**

Keel should own reusable event-loop transport primitives only: FD watchers,
timers, write queues, framing parser/serializer, and optional JSON-RPC
client/server helpers if they stay dependency-light.

Hull owns process boundaries: manifest parsing, tool resolution, service
supervision, process launch, FD/handle/resource passing, sandbox/resource
limits, capability checks, audit logs, app bindings, and lifecycle policy.

Start with a thin `hull/rpc` layer above Keel. Promote transport-neutral pieces
to Keel later only if they prove useful outside Hull.

**Platform sandbox tiers:**

Expose the actual enforcement level at runtime and in `hull agent services` so
operators do not confuse "native sidecar" with WASM-grade containment.

| Level | Platforms | Backend |
|---|---|---|
| strong | Linux | `no_new_privs` + seccomp + Landlock + rlimits + FD discipline |
| strong | OpenBSD / Cosmopolitan where supported | pledge/unveil + rlimits + FD discipline |
| good | macOS | Seatbelt + Hardened Runtime + rlimits + inherited-handle discipline |
| partial | FreeBSD | Capsicum where available, otherwise documented degradation |
| partial | Windows | Job Object + restricted token / AppContainer later |

**Error model:**

Keep failure classes distinct:

- `rpc_error` — valid JSON-RPC error returned by a method.
- `protocol_error` — malformed JSON, bad frame, invalid id, unsupported method.
- `transport_error` — EOF, EPIPE, timeout, reset.
- `service_exit` — process exited with status/signal.
- `sandbox_violation` — OS denied/killed the sidecar.
- `supervisor_error` — launch failed, bad tool, denied path/resource/capability.
- `app_error` — Lua/JS misuse of the service API.

**Phased plan:**

- [ ] Phase 0: design doc + threat model + repo survey. Decide the exact
      static manifest pre-extraction subset and signing implications.
- [ ] Phase 1: minimal `stdio-fd` Content-Length JSON-RPC client/server,
      dedicated RPC FD, stderr log separation, `rpc.discover`, `health`, and
      one request/response method.
- [ ] Phase 2: Unix socket transport, including socketpair and filesystem
      socket paths; consider FD passing for resources.
- [ ] Phase 3: lifecycle supervision: launch, readiness, health checks,
      crash detection, restart policy, log capture, graceful shutdown, and
      `hull agent services`.
- [ ] Phase 4: sandbox/resource limits: close inherited FDs, scrub env, set
      cwd, rlimits, Linux seccomp/Landlock, OpenBSD pledge/unveil, macOS
      Seatbelt profile, Windows/FreeBSD degraded backends.
- [ ] Phase 5: TCP transport gated by explicit manifest capabilities.
      Localhost-only default; no production remote bind without a declared
      listen policy.
- [ ] Phase 6: bitnet.c proof-of-concept sidecar installed through
      `hull tools install`, using pre-opened model resources and no arbitrary
      filesystem discovery.
- [ ] Phase 7: signed sidecar packaging: tool metadata declares supported
      needs, app manifest grants concrete resources, `hull verify` includes
      sidecar tool identity/hash in the app's signed deployment surface.

**Hard parts / security traps:**

- Manifest timing: sidecars cannot be discovered by executing app code after
  phase-1 pledge; service declarations need static pre-extraction.
- Dynamic linker behavior: native tools may need shared libraries unless
  shipped static. Prefer static sidecar artifacts where practical.
- Inherited FDs/handles are ambient authority; close everything except
  explicit RPC/log/resource handles.
- Environment variables leak authority; start from an empty env and add only
  declared values.
- `cwd` leaks filesystem reachability; set to a controlled service workdir.
- TCP can accidentally become an undeclared local network service; keep it
  off until capability gates and tests are in place.
- Sidecars spawning subprocesses must be blocked by sandbox/resource policy.
- Logs must never share the RPC stream.
- Restart loops can DoS the host; cap restart rate and expose status.
- Native sidecars are lower trust than WASM. Documentation and `inspect` /
  `agent services` output should make that explicit.

**Out of scope:** arbitrary user-controlled process execution, in-process
native plugins, unrestricted `dlopen`, plugin package managers, remote sidecar
orchestration, and claiming sidecars are equivalent to WASM sandboxing.

---

## 1.7 Native sandbox runner and services (`hull sandbox`)

**Priority:** Medium-High — follow-on to native sidecar services. Reuses the
same process-supervision, resource-passing, sandbox, and limit machinery to
run native executables under declared capability manifests, either as
foreground one-shot processes or supervised daemon processes.

This is a different product surface from app-owned sidecars:

- `hull sandbox run` runs a foreground / one-shot native process.
- `hull sandbox service` supervises a long-running native daemon.
- `hull.services` launches app-owned RPC sidecars on behalf of Lua/JS app
  logic.

The goal is not to make arbitrary native binaries "safe" in the same sense as
WASM or Hull app code. The goal is to provide an honest, capability-oriented
launcher for native programs that can operate inside a narrow declared surface.

**Proposed CLI:**

```bash
hull sandbox run ./blabla --manifest manifest.json
hull sandbox run ./blabla --manifest -
hull sandbox run glpsol --manifest - < glpk.solve.json
hull sandbox service start --manifest daemon.json
hull sandbox service status local-solver
hull sandbox service stop local-solver
hull sandbox service logs local-solver
hull sandbox inspect manifest.json
hull sandbox explain ./blabla --manifest manifest.json
```

Avoid overloading `hull run`; Hull app execution and native process sandboxing
should remain visibly distinct.

`--manifest -` reads the manifest JSON from stdin. This is important for
agent workflows and one-shot generated runs where writing a temporary manifest
file is unnecessary or undesirable. When stdin is used for the manifest, the
child's stdin must be explicitly configured separately (`"stdin": "inherit"`,
`"stdin": "null"`, `"stdin": {"file": "..."}`, or a future dedicated input
FD), so manifest input cannot be confused with child process input.

**Best-fit workloads:**

| Fit | Workload |
|---|---|
| Best | Native tools designed for Hull sandboxing |
| Good | Static or mostly self-contained CLI tools |
| Good | Solver/optimizer tools such as GLPK and HiGHS, run unmodified against declared input/output files |
| Good | Local daemons with narrow Unix-socket interfaces and explicit readiness checks |
| Mixed | Ordinary Unix programs with predictable file/network needs |
| Poor | GUI apps, browsers, package managers, shells, build systems |

**Process manifest sketch:**

```json
{
  "type": "native-process",
  "exec": "./blabla",
  "args": ["--model-fd", "$HULL_RESOURCE_model"],
  "env": {},
  "cwd": "work/blabla",
  "resources": {
    "model": {
      "path": "models/model.bin",
      "access": "fd-read"
    },
    "cache": {
      "path": "cache/blabla",
      "access": "dir-rw"
    }
  },
  "fs": {
    "read": [],
    "write": []
  },
  "net": {
    "connect": [],
    "listen": []
  },
  "stdio": {
    "stdin": "inherit",
    "stdout": "inherit",
    "stderr": "inherit"
  },
  "limits": {
    "memory_mb": 4096,
    "open_files": 32,
    "processes": 1,
    "wall_ms": 600000
  }
}
```

Prefer pre-opened resources (`fd-read`, `dir-rw`, inherited handles on
Windows) over broad path visibility. Path allowlists are still useful for
legacy tools, but the highest-integrity mode is "the child sees only handles
Hull intentionally gives it."

**Unmodified solver tools:**

This runner should make it practical to use established native command-line
tools such as GLPK (`glpsol`) and HiGHS without linking them into Hull and
without rewriting them as WASM modules. Many solvers already have a narrow
file/stdio shape:

```bash
hull sandbox run glpsol --manifest glpk.solve.json
hull sandbox run highs --manifest highs.solve.json
```

Example GLPK-style manifest:

```json
{
  "type": "native-process",
  "tool": "glpk@5",
  "args": [
    "--math", "model/problem.mod",
    "--data", "model/data.dat",
    "--output", "out/solution.txt"
  ],
  "cwd": ".",
  "fs": {
    "read": ["model/problem.mod", "model/data.dat"],
    "write": ["out"]
  },
  "net": { "connect": [], "listen": [] },
  "env": {},
  "limits": {
    "memory_mb": 2048,
    "open_files": 32,
    "processes": 1,
    "wall_ms": 300000
  }
}
```

For unmodified tools, path allowlists are necessary because the program does
not know how to consume Hull resource FDs. For Hull-aware tools, prefer
resource placeholders. The runner should support both modes and make the
security tradeoff visible in `hull sandbox inspect`.

**Daemon/service mode:**

Sandboxed daemons need distinct lifecycle semantics. Do not stretch
`hull sandbox run` to mean "background this and hope"; make service mode
explicit.

Example service manifest:

```json
{
  "type": "native-service",
  "name": "local-solver",
  "tool": "highs-server@1",
  "args": ["--socket", "$HULL_SOCKET"],
  "cwd": "work/local-solver",
  "transport": {
    "type": "unix",
    "path": "run/highs.sock"
  },
  "readiness": {
    "type": "unix-connect",
    "timeout_ms": 5000
  },
  "restart": {
    "policy": "on-crash",
    "max_restarts": 3,
    "window_ms": 60000
  },
  "fs": {
    "read": ["models"],
    "write": ["run", "cache"]
  },
  "net": { "connect": [], "listen": [] },
  "env": {},
  "limits": {
    "memory_mb": 2048,
    "processes": 1,
    "open_files": 64
  },
  "logs": {
    "mode": "capture",
    "max_bytes": 10485760
  }
}
```

Daemon-specific requirements:

- Readiness detection: process-started is not ready. Support Unix-connect,
  TCP-connect, HTTP health endpoint, JSON-RPC `health`, and process-only as a
  last resort.
- Stable identity: services have names, state directories, PID/state files,
  socket paths, and status JSON.
- Lifecycle commands: `start`, `stop`, `restart`, `status`, `logs`.
- Graceful shutdown: send configured signal/request, wait grace period, then
  terminate.
- Restart policy: disabled by default; `on-crash` must include restart-rate
  limits.
- Stale socket cleanup: only remove sockets owned by the service state.
- Double-fork/background escape: treat daemonization that detaches from the
  supervisor as a policy violation unless a future platform service-manager
  integration explicitly supports it.
- Network daemons: prefer Unix sockets. TCP listen requires explicit
  `net.listen`, defaults to `127.0.0.1`, and must appear in `inspect/status`.

**Unified execution model:**

1. Parse and validate the native process manifest.
2. If the manifest came from stdin, require an explicit `base_dir` or use the
   current working directory with a diagnostic in `inspect`.
3. Resolve all paths relative to the manifest location, explicit `base_dir`,
   or the stdin fallback base.
4. Open declared resources before sandboxing.
5. Build the child argv from literal args plus resource placeholders.
6. Start from an empty environment; add only declared env vars.
7. Set a controlled cwd.
8. Close every inherited FD/handle except stdio and declared resources.
9. Apply resource limits.
10. Apply the best available OS sandbox backend.
11. Exec/spawn the child.
12. For `native-process`, monitor exit status, signals, wall timeout, logs, and
    sandbox diagnostics until the foreground process exits.
13. For `native-service`, wait for readiness, persist service state, monitor
    lifecycle, enforce restart policy, and keep logs/status queryable.
14. Emit structured JSON when requested (`--json`) for agents and tests.

**Relationship to sidecars:**

This should be implemented after the sidecar supervisor proves the core
primitives:

- process launch without shell invocation
- environment scrubbing
- close-on-exec / inherited FD discipline
- pre-opened resource passing
- rlimits / Job Objects / platform limits
- Linux seccomp/Landlock, OpenBSD pledge/unveil, macOS Seatbelt, Windows and
  FreeBSD degraded backends
- structured error and audit model

The runner should reuse the same internal API where possible, but without RPC
or restart policy by default.

**Platform semantics:**

The runner must report actual enforcement strength, not a generic "sandboxed"
claim:

```json
{
  "sandbox": {
    "level": "strong",
    "backend": "linux-seccomp-landlock",
    "degraded": false
  }
}
```

If a platform cannot enforce a requested capability boundary, default behavior
should be fail-closed unless the operator passes an explicit development flag
such as `--allow-degraded-sandbox`.

**Tasks:**

- [ ] Define `manifest.json` schema for `type = "native-process"` and resource
      placeholders.
- [ ] Define `type = "native-service"` schema for daemon lifecycle:
      name, transport, readiness, restart, logs, state dir, graceful shutdown.
- [ ] Support `--manifest -` to read manifest JSON from stdin, with explicit
      child-stdin configuration and clear `base_dir` path-resolution rules.
- [ ] Add `hull sandbox inspect` to normalize and display the resolved
      capability surface without running the process.
- [ ] Add `hull sandbox explain` to show which OS backend rules would be
      applied on the current platform.
- [ ] Implement `hull sandbox run` using the sidecar process/sandbox
      supervisor primitives.
- [ ] Implement `hull sandbox service start|stop|restart|status|logs` with
      service state files, stale-socket handling, readiness checks, graceful
      shutdown, and restart-rate limits.
- [ ] Support inherited stdio plus optional capture mode (`--json`,
      `--capture-output`, max output size).
- [ ] Support pre-opened file/dir resources and placeholder expansion in argv.
- [ ] Support path-allowlisted legacy tools that cannot consume resource FDs,
      with `inspect` clearly marking the broader path-based authority.
- [ ] Add network capability gates: no connect/listen by default; localhost
      listen/connect only when declared; remote connect only when declared.
- [ ] Add solver examples for GLPK and HiGHS, including read-only model/data
      inputs, write-only solution directories, no environment, no network, and
      bounded memory/wall-time.
- [ ] Add daemon examples using Unix sockets first; TCP daemon examples only
      after `net.listen` policy is implemented and tested.
- [ ] Add clear error classes mirroring sidecars: manifest error, launch
      error, sandbox unsupported, sandbox violation, timeout, exit status,
      signal, readiness failure, restart exhaustion, escaped daemon,
      resource denied.
- [ ] Add tests with tiny static helper binaries that attempt allowed and
      denied file reads/writes, env reads, network access, subprocess spawn,
      and FD leakage.
- [ ] Document honest workload fit and platform degradation behavior.

**Security traps:**

- Arbitrary native programs often depend on dynamic libraries, locale files,
  `/proc`, temp dirs, config files, DNS files, and subprocesses. Tight
  manifests will break many programs; broad manifests weaken the guarantee.
- Shells and build systems are poor targets because they are designed to
  discover and execute more programs.
- GUI applications are out of scope for v1; window-system access is a broad
  ambient channel.
- Dynamic linker behavior can undermine "no file access" claims unless the
  child is static or the loader/library paths are explicitly accounted for.
- Passing path strings instead of resources invites confused-deputy bugs.
- Leaking an inherited directory FD can bypass path allowlists.
- Network controls must be OS-enforced where possible, not only checked by
  Hull before launch.
- Daemons that double-fork, write arbitrary PID files, or reopen logs outside
  declared paths can escape supervision assumptions.
- Stale Unix sockets and PID files can cause confused ownership unless state
  files include Hull-generated service identity.
- On platforms with partial sandbox support, the CLI must be explicit about
  degraded enforcement.

**Out of scope:** remote orchestration, containers, VM isolation, package
manager sandboxes, GUI app sandboxing, system-wide init/service-manager
integration, and claiming arbitrary native binaries are equivalent to Hull
apps or WASM plugins.

---

## 2. `hull tools install` — side-loaded optional tools  ✅ Shipped (v0.1.2)

**Design:** [`tools_install.md`](tools_install.md). What landed:

- `hull tools install <name>` / `list [--json]` / `uninstall` subcommands.
- Tools land in `$HOME/.hull/tools/` (mode 0755), isolated from PATH.
- Trust chain reuses the same Ed25519-signed `hull.sha256` manifest as
  `hull update` — no new keys, no new ceremonies.
- Version-coupled: pulls from the SAME release as the running hull
  binary (not "latest"), so e.g. wamrc stays at the WAMR commit hull
  was compiled against.
- First concrete tool: `wamrc` (WAMR AOT compiler), published for
  linux-x86_64 / linux-aarch64 / darwin-arm64. Cosmo unsupported
  (LLVM doesn't fit a fat APE binary) — cosmo users `make wamrc`.
- Shared `release_io.{c,h}` extracted from `commands/update.c` so both
  self-update and tool-install paths share the same HTTPS / SHA-256 /
  signature-verification / atomic-rename plumbing.
- `tool.find_tool()` Lua binding so `build.lua`'s wamrc resolver
  consults the canonical install location without reimplementing the
  4-step lookup in script.
- `hull doctor` reports installed / managed / unmanaged state.
- Audit fixes shipped together: OOB-read defense in
  `hl_release_io_find_checksum`, JSON-string escaper for
  `tools list --json` descriptions, fsync/close error checks in
  atomic-write, constant-time SHA-256 hex compares (both `tools` and
  `update` paths).

**Out of scope (deferred):** wgpu-native (needs runtime dlopen
architecture change), system-wide install path (stay user-scoped),
`hull update --with-tools` auto-refresh.

---

## 3. Platform-sig completion — make `HL_PLATFORM_PUBKEY_HEX` meaningful

**Status: SHIPPED in v0.1.3 (six commits, landed on `main`).** The
release pipeline now signs the per-arch `libhull_platform.a`
manifest with `HULL_PLATFORM_KEY` at release time, `hull build`
cross-checks the embedded `.a` against the inherited signed
manifest and writes it into `package.sig.platform.gethull`, and
both `hull verify` and runtime `--verify-sig` enforce the
gethull-layer signature against the real
`HL_PLATFORM_PUBKEY_HEX`. Browser verifier (`site/verify.html`)
matches. Escape valve `--no-verify-platform` exists on every
consumer for dev hulls and forks. Honest-scorecard bullet moved
from "Not yet" → "Ships"; new explicit out-of-scope note for
post-install binary integrity (an OS-layer concern).

The original plan and execution order are preserved below as
historical context for future readers tracing similar trust-chain
work.

**Priority:** High for v0.1.3 — this was the loudest remaining gap on
the v0.1.x "honest scorecard" and the symmetric companion to the
release-sig trust chain that shipped fully in v0.1.0 (release-side)
and matured in v0.1.2 (audit-hardened constant-time compare, OOB
defense, `release_io.{c,h}` shared between `hull update` and
`hull tools install`).

**Target:** v0.1.3 (shipped).

**Current state:** the cryptographic primitives, the embedded
pubkeys (`HL_PLATFORM_PUBKEY_HEX` in `signature.h`,
`GETHULL_DEV_PLATFORM_KEY` in `site/verify.html`), and the verifier
code paths (`hull verify`, browser verifier) all exist. But:

- `HL_PLATFORM_PUBKEY_HEX` is the all-zeros placeholder. v0.1.1
  reverted it from the real key after a test-only override Makefile
  rule got removed — the real key is in the GH secrets, just not
  embedded.
- No signed platform artefact is produced at release time, so
  `package.sig`'s `platform` field is empty in every built app and
  the verifier has nothing to check against.
- The browser verifier at `gethull.dev/verify.html` enforces the
  platform layer; the CLI and runtime do not.

### Why v0.1.2 unblocks this

v0.1.2 established the patterns this work needs to copy:

| v0.1.2 shipped | Reused here |
|---|---|
| `release_io.{c,h}` — HTTPS GET, signed-manifest verify, SHA-256, atomic write | Same module verifies the embedded platform-sig blob (no new code paths) |
| Audit-hardened trust chain (constant-time compare, OOB defense, fsync checks) | Platform-sig path is implemented against the same hardened helpers |
| `release.yml` matrix: build artifact → sha256 → Ed25519 sign → publish | Same shape applies to per-arch platform archives |
| Hex pubkey override via `-DHL_*_PUBKEY_HEX=…` in `release.h` | Already in `signature.h` too; tests can flip back to placeholder |

The architecture is right. What's missing is the wire format and
the release-time signing step.

### Locked design decisions

After scoping discussion 2026-05-27, three design questions were
resolved:

- **Verification strength: strong measurement.** Hull-binary build
  (in CI) emits per-arch `.a` SHA-256 as embedded constants in the
  hull binary text, alongside the signed manifest blob. `hull build`
  computes the SHA-256 of the `libhull_platform.a` it's actually
  embedding and cross-checks against the manifest entry before
  writing `package.sig.platform`. Runtime verify validates the
  signed-blob signature against `HL_PLATFORM_PUBKEY_HEX`. Stronger
  claim than transitive-trust; doesn't prove the linked code matches
  at runtime (that's reproducible-builds territory, Phase 9), but
  makes the trust path explicit in artifacts.
- **No canary.** Skipped entirely. The signed manifest + per-arch
  SHA-256 do all the integrity work the canary was hypothesized
  for. Avoids Makefile post-link gymnastics across cosmocc + Mach-O
  + ELF. Smaller diff, simpler reasoning.
- **Hard reject on empty/invalid `package.sig.platform`** in the
  explicit verify paths (`hull verify <app>` and `--verify-sig` at
  startup). Default `hull <app>` startup (no verify flag) keeps
  running unsigned apps — same opt-in behavior as today, so legacy
  v0.1.0–v0.1.2 apps continue working at runtime.

### Manifest format

Mirror `hull.sha256`'s shape — line-based text, not JSON:

```
0000000000000000000000000000000000000000000000000000000000000001  linux-x86_64
0000000000000000000000000000000000000000000000000000000000000002  linux-aarch64
0000000000000000000000000000000000000000000000000000000000000003  darwin-arm64
0000000000000000000000000000000000000000000000000000000000000004  cosmo-x86_64
0000000000000000000000000000000000000000000000000000000000000005  cosmo-aarch64
```

Why: avoids JSON canonicalization headaches entirely (deterministic
key order, whitespace, escaping). Signed against the file bytes.
Reuses every helper from `release_io.{c,h}` —
`hl_release_io_find_checksum`, `hl_release_io_sha256_hex`,
`hl_release_verify_manifest_sig`. Zero new format code.

### Three pieces, in execution order

**(A) Restore the real `HL_PLATFORM_PUBKEY_HEX`.** Replace the
all-zeros placeholder with the actual pubkey embedded for v0.1.0
(the secret half is already in the `HULL_PLATFORM_KEY` GH secret
and the pubkey is already in `site/verify.html`). Keep the
`#ifndef HL_PLATFORM_PUBKEY_HEX` override guard so tests can flip
back to placeholder.

**(B) Signed platform manifest produced at release time.** Extend
`release.yml` per Option A: reorganize so `build-platform` runs as
a matrix uploading per-arch `.a` artifacts → new
`sign-platform-manifest` single-Linux job downloads them, computes
SHA-256s, emits `platform-manifest.txt`, signs with
`HULL_PLATFORM_KEY`, `xxd`-embeds both into
`build/embedded_platform_sig.h` (signed manifest) and
`build/embedded_platform_hashes.h` (per-arch SHA-256 C constants)
→ `build-native` + `build-cosmo` (matrix) depend on the headers
artifact and include both during hull build. Bootstrap check:
fail loudly if `HULL_PLATFORM_KEY` is empty in CI.

**(C) `hull build` cross-check + `hull verify`/`--verify-sig`
enforce.** `hull build` computes SHA-256 of the
`libhull_platform.a` it's embedding, calls
`hl_platform_sig_extract_for_arch(this_arch)` (new C helper) to
get the expected hash + the signed blob, hard-rejects on mismatch
unless `--no-verify-platform` is passed, writes
`(manifest + sig + arch_hash)` into `package.sig.platform`. App
runtime verify against `HL_PLATFORM_PUBKEY_HEX` short-circuits to
hard-reject on empty (legacy apps) or invalid signature.

### Behavior matrix

| Scenario | Behavior |
|---|---|
| Release-built hull, embedded `.a` matches manifest | `hull build` writes signed platform-sig; runtime `--verify-sig` ✅ |
| Release-built hull, `.a` SHA-256 mismatch (user re-ran `make platform` locally on a release hull) | `hull build` **hard rejects** with: `"libhull_platform.a hash does not match the embedded signed manifest"` (use `--no-verify-platform` to override) |
| Self-built hull (no embedded manifest, dev workflow) | `hull build` **hard rejects** with: `"this hull was built locally and has no embedded platform manifest"` (use `--no-verify-platform` to build anyway; runtime verify will fail) |
| App with empty `package.sig.platform` at runtime + `--verify-sig` | **Hard reject**: `"app was built without platform-sig (rebuild against a release-built hull >=0.1.3)"` |
| App with empty `package.sig.platform` at runtime WITHOUT `--verify-sig` | No change from today — runs as-is. Default `hull <app>` doesn't verify signatures unless asked. |
| `--no-verify-platform` passed at any step | Skip the check, log once at info level. |

The `--no-verify-platform` flag exists on both `hull build` and the
runtime serve path. It's the documented escape valve for
dev-built hulls and for forensic-mode operation; expected to be
rare in production. Without it, self-built dev hulls can't build
production-ready apps — an acceptable strict-default tradeoff
matching the v0.1.2 audit-hardening posture.

### Six commits, ordered

The original draft of this plan put "restore the real pubkey" first
as a small spike commit. That sequencing was wrong: restoring the
pubkey activates `hl_verify_startup`'s platform-key pinning, which
hard-rejects any app whose `package.sig.platform.public_key_hex`
doesn't match the embedded key. Today's
`hull sign-platform` + `hull build --sign` developer flow (exercised
by `e2e_build.sh` Step 14) signs platforms with the developer's own
key — so the moment the real gethull pubkey is embedded, every
existing dev-signed app fails verify. The same failure mode triggered
the `ff0a39b` reversion during v0.1.1. C1 is therefore a *dependent*
commit, not an independent one.

The corrected order lands the chain bottom-up: helpers → CI →
build-side cross-check + opt-out flag → runtime enforcement →
THEN flip the pubkey + update the dev-flow e2e to use the
`--no-verify-platform` opt-out. Each step compiles and passes its
own tests; restoring the pubkey becomes safe only after every
consumer of the pin can opt out of it.

| # | Prefix | Summary | Effort |
|---|---|---|---|
| 1 | `sig:` | `src/hull/platform_sig.{c,h}` — manifest builder, signer, verifier, `extract_for_arch` helper. Pure data; reuses `release_io` helpers (`find_checksum`, `verify_manifest_sig`, `sha256_hex`). Unit tests with synthetic hashes including mismatch + tamper cases. Standalone — no runtime behavior change. | 1d |
| 2 | `ci:` | `release.yml` reorg per Option A: `build-platform` (matrix, uploads `.a`) → `sign-platform-manifest` (single Linux job, signs with `HULL_PLATFORM_KEY`, emits `build/embedded_platform_sig.h` + `build/embedded_platform_hashes.h`, uploads as artifact) → `build-native` + `build-cosmo` (matrix, depend on the headers artifact, include them in hull build). Bootstrap check fails the workflow if `HULL_PLATFORM_KEY` is empty in CI. Hull binaries now embed signed platform metadata but nothing reads it yet. | 1.5d |
| 3 | `build:` | `hull build` integration: new `--no-verify-platform` flag (works on both `hull build` and runtime), `tool.platform_sig_get()` Lua binding wrapping `hl_platform_sig_extract_for_arch()`, `build.lua` computes SHA-256 of embedded `.a`, cross-checks against manifest entry, writes verified `(manifest + sig + arch_hash)` into `package.sig.platform`. Hard-reject paths with the messages in the behavior matrix above. | 1d |
| 4 | `sig:` | `hull verify` + `--verify-sig` enforce platform layer at startup. Hard reject on empty/invalid with clear messages. `--no-verify-platform` opt-out at runtime mirrors the build-time flag. E2E test: build an app via the full release pipeline, mutate embedded `.a` bytes via hex editor, expect verify fails non-zero with the specific message. | 0.5d |
| 5 | `sig:` | Restore real `HL_PLATFORM_PUBKEY_HEX` (revert the v0.1.1 placeholder's hex value; keep the `#ifndef` override guard). Update `test_signature.c`'s `create_test_package_sig` to support a `platform_kind` parameter so `verify_startup_good` can emit `platform: null` (the unit test doesn't intend to exercise pinning). Update `tests/e2e_build.sh` Step 14 to add `--no-verify-platform` when running developer-signed apps under `--verify-sig` (the test is acting as a fork developer, not a gethull-signed app). This is the commit that *activates* the pin. | 0.5d |
| 6 | `docs:` | `docs/security.md §6` "shipped", `site/index.html` scorecard updates (move platform-sig bullet from "Not yet" to "Ships"), `site/verify.html` fixture with a real v0.1.3 example, roadmap section 3 → Shipped (v0.1.3), CHANGELOG entry. | 0.5d |

**Total: ~4.5 days.** Slightly less than the prior 5-day estimate
because the pubkey-restoration commit (C5) is bundled with the
narrow e2e_build.sh update it depends on, rather than being a
standalone 0.5d spike that turned out to need 0.5d of dependency
work anyway. CI reorg in C2 remains the highest-risk piece.

### Release-time validation

Same pattern as v0.1.2: tag `v0.1.3-rc1` first, watch the workflow,
run `tests/release_smoke.sh` (extended to also `hull verify` the
published binaries and confirm the platform layer reports valid).
Only tag clean `v0.1.3` after rc1 is green.

### Tasks (in dependency order — mirrors the commit table)

- [ ] `src/hull/platform_sig.{c,h}` — manifest builder + signer +
      verifier + `extract_for_arch` helper. Unit tests including
      mismatch + tamper cases. **[C1]**
- [ ] `release.yml` reorg: `build-platform` matrix +
      `sign-platform-manifest` job + dependency on
      `build-native`/`build-cosmo` jobs + bootstrap check on
      `HULL_PLATFORM_KEY` presence. Generates
      `embedded_platform_sig.h` + `embedded_platform_hashes.h`. **[C2]**
- [ ] `--no-verify-platform` flag on `hull build` + runtime.
      `tool.platform_sig_get()` Lua binding + `build.lua`
      integration writing `package.sig.platform` with the verified
      `(manifest + sig + arch_hash)`. **[C3]**
- [ ] `hull verify` + `--verify-sig` runtime enforcement;
      hard-reject paths with the documented error messages.
      E2E test: build app, mutate embedded `.a` bytes, expect
      verify fails non-zero with specific message. **[C4]**
- [ ] Restore real `HL_PLATFORM_PUBKEY_HEX` (revert the v0.1.1
      placeholder's hex value; keep the `#ifndef` guard). Update
      `test_signature.c` to support `platform_kind` param so the
      verify_startup unit test can emit `platform: null`. Update
      `e2e_build.sh` Step 14 to add `--no-verify-platform` for the
      developer-signed app path. **[C5]**
- [ ] Audit pass (mirror v0.1.2): OOB defense on
      `hl_platform_sig_extract_for_arch`, constant-time SHA-256
      compare in the cross-check path, fsync/close on any new
      atomic writes.
- [ ] Update `docs/security.md §6` to flip "platform layer
      inactive" → "shipped".
- [ ] Update `site/index.html` honest-scorecard: move
      platform-sig bullet from "Not yet" to "Ships".
- [ ] Update `site/verify.html` fixture with a real v0.1.3
      example.
- [ ] Post-release smoke: extend `tests/release_smoke.sh` to run
      `hull verify` against the published artifacts and confirm
      the platform layer reports valid.

### Out of scope

- **Reproducible builds** (Phase 9). Platform-sig proves bytes were
  endorsed by whoever holds the platform key; reproducible builds
  prove WHAT got endorsed. Separate concern.
- **Key rotation tooling.** The current model assumes the platform
  key doesn't rotate within a major version. If rotation is needed
  before v0.2, that's a separate commit batch (extending
  `HL_PLATFORM_PUBKEY_HEX` to a small array of accepted keys).
- **Backwards compatibility with v0.1.0/v0.1.1/v0.1.2 apps.** Those
  apps were built with hull versions that emit an empty
  `package.sig.platform`. They continue to run on v0.1.3+ hull
  normally (default `hull <app>` doesn't signature-check). They
  only fail under `hull verify` or `--verify-sig`, which is the
  documented intended behavior — opt-in stricter verification.
  Rebuild against v0.1.3+ hull to make those apps pass.

### Effort

Realistic estimate: **5 engineering days** (split roughly 4
engineering + 1 release-engineering, matching the v0.1.2 shape).
The CI reorg in step 3 is the highest-risk piece; the rc1 → smoke →
clean tag dance from v0.1.2 applies here too.

---

## 3.1 Cosmo APE: tool-mode compiler invocation on Linux

**Priority:** Medium. Discovered while gating v0.1.3 — the release
workflow's platform-sig E2E smoke test passes on all three native
arches (darwin-arm64, linux-x86_64, linux-aarch64) but had to be
skipped for the Cosmopolitan APE build because `hull build` can't
spawn a system compiler when run as a cosmo binary on Linux.

**Symptom:**

```
/usr/bin/cc: error while loading shared libraries: libc.so.6:
              failed to map segment from shared object
/usr/bin/gcc: error while loading shared libraries: ...
/usr/bin/clang: error while loading shared libraries: ...
hull: compiler 'cosmocc' not found in PATH
hull build: no C compiler available
```

Even with `--compiler cosmocc` and `/opt/cosmo/bin` on `$PATH`,
hull's tool-mode sandbox (pledge/unveil polyfill provided by
jart/cosmopolitan) restricts file-system access to a small allowlist
that doesn't include the system loader paths needed by `cc`/`gcc`/
`clang`, nor the cosmocc install location. The fork+exec succeeds
but the child's dynamic loader can't map its dependencies.

**Impact:**

End-users running `hull-cosmo` on Linux can't run `hull build`
(unless they bypass the sandbox, which we don't expose for tool
mode). The cosmo binary is fine for `hull dev`, `hull test`,
`hull <app>`, `hull update`, `hull tools install` — those don't
spawn a system compiler. Only `hull build` is affected, and only
when `hull-cosmo` is the binary doing the building.

End-users have two workable paths today:

1. Install a native hull (`hull-linux-x86_64` / `hull-linux-aarch64`)
   for the platform doing the building, keep `hull-cosmo` for
   distributing to mixed-OS targets.
2. Build outside the sandbox manually (extract platform library +
   invoke the compiler by hand).

Neither is a great story for the "one binary for all OSes" cosmo
promise.

**Fix candidates** (one or more — design once we pick up the task):

1. **Widen the tool-mode unveil set.** Add `/lib`, `/lib64`,
   `/usr/lib`, `/usr/lib64`, and the `cosmocc` install dir
   (`$HOME/.cosmocc/` and `/opt/cosmo/`) to the tool-mode allowlist.
   The risk: a wider sandbox during `hull build` weakens the
   capability story for build-time tooling. Possibly OK because the
   build step is supposed to invoke a compiler.
2. **Auto-detect `cosmocc` location.** `hl_compiler_select`'s system
   candidates today are `cc, gcc, clang`. Add `cosmocc` and try a
   few well-known install paths (`$HOME/.cosmocc/bin`,
   `/opt/cosmo/bin`) before falling back to `$PATH`. Doesn't fix the
   shared-library mmap issue for native compilers, but at least
   lets `cosmocc` work.
3. **Use embedded TinyCC on the cosmo binary too.** If TCC's
   embedded codegen can target the cosmo runtime, `hull build` could
   skip the system compiler entirely on cosmo. Requires investigating
   whether the embedded TCC builds cleanly under cosmocc and produces
   loadable APE/native output.
4. **Run the cosmo-side E2E smoke test inside a chroot** that
   doesn't apply the polyfill. Closes the CI gap without affecting
   end-users.

**Definition of done:**

- `hull build --sign` works end-to-end from a `hull-cosmo` binary
  on Linux, in the default sandbox, with the same `--verify-sig`
  pass as the native builds.
- Re-enable the cosmo case in `release.yml`'s "Platform-sig E2E
  smoke test" step.

---

## 3.2 Auto-extract embedded `libhull_platform.a` for tool-mode commands

**Priority:** Medium-High. The companion to §3.1 — both fall out of
the same architectural gap.

End-users installing a release binary via
`curl -fsSL https://gethull.dev/install.sh | sh` get a hull binary
with `libhull_platform.a` *embedded* (the `EMBED_PLATFORM=1` build
flow). `hull build` knows how to extract the embedded archive to a
tmpdir and feed it into the link step. But two other tool-mode
commands hit a missing-file error because they don't:

```
$ hull eject
hull eject: cannot find libhull_platform.a
hint: run `make platform` first

$ hull sign-platform --dir /some/dir/ key
hull sign-platform: no platform libraries found in /some/dir/
```

The "run `make platform`" hint is only actionable for someone with
the Hull source tree. End-users who installed via the release
binary have only the `hull` executable — there's no make,
no `vendor/`, no way to materialize the .a without rebuilding hull
from scratch.

**Impact:**

- `hull eject` is unusable on installed release binaries. Eject's
  whole purpose is "give me a self-contained scaffold I can build
  from without hull" — exactly the audience that has only the
  binary.
- `hull sign-platform` (the v0.1.2 per-app developer-signed
  platform layer) is unusable for the same reason. End-users who
  want to ship signed apps with `hull build --sign` need to either
  build hull from source or transplant a .a from somewhere.

**Fix:**

Both commands already have a clear extraction sink: the same
embedded blob the `hl_embedded_platform_*` accessors expose to
`hull build`. The fix is to call those accessors from
`commands/eject.c` and `commands/sign_platform.c` (or their Lua
stdlib equivalents), write the bytes to a tmpdir, and pass that
path through to the existing logic. `build.lua` already does this
pattern — it's just not factored into a shared helper that the
other tool commands can reuse.

Suggested factoring:

- `hl_platform_lib_extract(tmpdir, &out_path)` in
  `src/hull/build_assets.c` (where the embedded blob lives) —
  writes `libhull_platform.a` (single-arch) or both cosmo arches to
  `tmpdir/` and returns the path. Returns -1 if no platform is
  embedded.
- `stdlib/cli/lua/hull/eject.lua` calls this before scanning for
  `libhull_platform.a`. Same for `stdlib/cli/lua/hull/sign_platform.lua`.

Same blob, same trust chain (the embedded bytes are what
sign-platform-manifest signed at release time), no new code paths
through the sandbox.

**Definition of done:**

- `hull eject` works on an installed release binary, with no source
  tree present.
- `hull sign-platform` works the same way — produces a `platform.sig`
  that `hull build --sign` accepts.
- The "run `make platform` first" hint is replaced with the
  extraction logic for binaries with embedded platforms; the hint
  stays only for hulls built without `EMBED_PLATFORM=1` (where
  there's genuinely no .a to extract).

---

## 4. Background job queue (`hull.jobs`)

**Priority:** Low — the existing transactional outbox + inbox patterns cover
most reliable side-effect use cases. Add this when an explicit user need
appears (background image processing, scheduled cleanup, async chains).

**API sketch:**

```lua
local jobs = require("hull.jobs")
jobs.init()  -- creates _hull_jobs table

-- Enqueue
jobs.enqueue("send_email", { to = "user@example.com", subject = "Hello" })

-- Process (called from app.every() timer)
jobs.process(function(job)
    if job.type == "send_email" then return require("hull.email").send(job.data) end
end, { batch = 10, retry = 3 })
```

**Tasks:**

- [ ] `_hull_jobs` table schema (type, data, status, attempts, scheduled_at, last_error)
- [ ] `jobs.enqueue()` — insert with optional delay
- [ ] `jobs.process()` — atomic claim + execute + update status
- [ ] Retry with exponential backoff (reuse `outbox.backoffDelay` math)
- [ ] Dead-letter queue for permanently-failed jobs
- [ ] JS parity

---

## 5. Email retry/backoff (Phase 7 candidate from Phase 6 audit)

**Priority:** Low — Phase 6 wrapped `email.js` / `email.lua` providers in
try/catch; now that errors surface cleanly as `{ok:false, error}`, retry on
transient failures is a small follow-up.

**Approach:** wrap `email.send` itself (not each provider) in an
`opts.retry = { max_attempts, base_delay_ms }` envelope. Use the same
exponential backoff math as `outbox.backoffDelay`.

---

## 6. Test coverage gaps surfaced by audits

Three items the audits flagged as deserving unit tests (currently e2e-only):

- [ ] **`hl_migrate_*`** — `src/hull/migrate.c` has no unit-test suite. Edge cases (checksum mismatch, missing migrations table, concurrent attempts) deserve in-process tests.
- [ ] **Sandbox profile builder** (`sandbox.c::build_seatbelt_profile` / unveil-path builder) — only e2e-covered today, and only on the platforms CI runs. A unit test calling the profile-build helper and asserting on the generated SBPL/unveil list would catch regressions on platforms CI doesn't run.
- [ ] **`hl_snprintf_append` helper** — Phase 5 audit recommendation. Replaces the brittle `req_len += snprintf(...)` idiom that recurs in `agent/request.c`, `cap/smtp.c`, and template codegen. Land the helper + tests + sweep the call sites.

---

## ✅ Done: pre-v0.1.0 release gate

Five operational steps that gated the v0.1.0 tag. All executed
2026-05-25:

1. ✅ Release keypair generated via `hull keygen release` and backed
   up offline.
2. ✅ Pubkey embedded into `HL_RELEASE_PUBKEY_HEX` at
   `include/hull/release.h` (commit `869f18a`).
3. ✅ `HULL_RELEASE_KEY` GitHub secret installed.
4. ✅ Platform keypair generated symmetrically and embedded as
   `HL_PLATFORM_PUBKEY_HEX` in `include/hull/signature.h` (commit
   `bad31b9`). `HULL_PLATFORM_KEY` secret installed too, even though
   the platform-sig wire format isn't active yet — see section 2.
5. ✅ `v0.1.0` tagged 2026-05-25, release workflow signed
   `hull.sha256` and published all six artefacts.

See [`release_signing.md`](release_signing.md) for the full flow and
the "Release Process" section of [`../CLAUDE.md`](../CLAUDE.md) for the
operational checklist.
