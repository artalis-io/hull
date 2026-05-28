# Hull — Platform Assessment

**As of 2026-05-16. Approaching v0.1.0.**

An honest evaluation of where Hull stands, what's strong, what's still
weak, and what the scaling path looks like.

The previous version of this document (2026-03-04) treated WASM compute,
the agent platform, the deploy command, signed self-update, and most of
the stdlib expansion as future work. All of those are now shipped. This
revision reflects the current state.

## What Hull gets right

### Capability-secure runtime is fully realized

The capability layer (`src/hull/cap/`) is the structural boundary between
sandboxed scripting and system resources. Every filesystem read, database
query, HTTP request, environment-variable lookup, SMTP send, WASM call,
and GPU dispatch passes through a `hl_cap_*` C function that enforces the
manifest's allowlist. Apps cannot bypass this — the runtimes don't have
direct access to `open`, `connect`, `sqlite3_*`, or any other ambient
primitive. SQL injection is structurally impossible (every query uses
`sqlite3_bind_*`). Path traversal is blocked by `hl_cap_fs_validate` plus
kernel `unveil`. Internal `_hull_*` tables are protected by call-stack
inspection. None of this is bolted-on — it's the architecture.

Kernel enforcement is real:

- **Linux / Cosmopolitan:** SECCOMP-BPF + Landlock LSM via pledge/unveil
  polyfill. Violation = SIGKILL.
- **OpenBSD:** native pledge/unveil. Violation = SIGABRT.
- **macOS:** Seatbelt (`sandbox_init_with_parameters`) with a deny-default
  SBPL profile built dynamically from the manifest. Violation = EPERM.

Three independent Ed25519 signature layers (platform, app, release) plus
file-content hashing means the chain from `hull update` through `hull
build` through runtime startup is verifiable end-to-end.

### The single-binary thesis is delivered

The build pipeline — source files collected into a sorted registry,
embedded into a generated C source, compiled, linked against a signed
platform library — actually delivers on the promise. The current default
binary is ~5 MB on aarch64 (Lua + JS + WAMR + DB + TLS + CA bundle +
embedded TinyCC); compute-only with `HL_ENABLE_DB=0` is ~3.66 MB. With
Cosmopolitan, one APE binary runs on Linux, macOS, Windows, FreeBSD,
OpenBSD, and NetBSD. `hull build` works on a fresh machine with no
system compiler thanks to embedded TinyCC and the embedded Mozilla CA
bundle.

### The C foundation is production-grade

Allocator discipline (Keel's `KlAllocator` vtable, no raw `malloc`/`free`
in Hull's own code), overflow guards before arithmetic, three sanitizer
configurations (ASan+UBSan, MSan+UBSan, debug), static analysis
(scan-build + cppcheck on every commit), fuzz targets for the HTTP
parser and multipart reader. Keel sustains 100K+ req/s on a single core.
The VFS provides O(log n) lookups over embedded file arrays.

Three independent security audits in the last month — main audit (49
findings), Phase 6 audit (21 findings), Phase 6 re-audit (3 follow-ups) —
all closed. 27 unit-test suites, ~610 test cases, plus 12 E2E scripts.

### The runtime stack is no longer thin

The stdlib has expanded substantially since the March assessment. Lua
and JS now ship parity coverage of: cors, ratelimit, csrf, auth (session
+ JWT), session, cookie, jwt, template, csv, validate, form, i18n,
logger, transaction, idempotency, outbox, inbox, rbac, health, etag,
search (SQLite FTS5), email (SMTP + 3 API providers). Plus WebSockets,
SSE, background timers (`app.every`, `app.daily`), and `db.udf` for
user-defined SQL functions in Lua, JS, or WASM. The "missing modules
are the leak in the funnel" critique from March still applies for niche
needs, but the baseline for a typical backend is covered.

### WASM and GPU compute are shipped

What the March assessment treated as designed-but-not-built is now
production. WASM compute runs through WAMR with:

- Fast interpreter + AOT (auto-compiled during `hull build` when
  `wamrc` is present).
- Gas metering, configurable heap/stack/I-O caps.
- Instance pooling (per-module pools cut per-call overhead to ~µs).
- Persistent instances (linear memory retained across calls — for
  stateful models / pre-built indexes).
- Shared data segments (multi-GB read-only datasets in shared heaps).
- Streaming I/O (file → file, buffer → buffer, callback).
- SIMD128 + Memory64.
- The unified buffer protocol (`HlBufferView`) — fs.mmap + WasmBuffer
  + ArrayBuffer + Lua string all flow into compute and GPU without
  copying.

GPU compute (optional, `HL_ENABLE_GPU=1`) via wgpu-native:

- WGSL shader dispatch + multi-stage pipeline with shared named buffers.
- Persistent buffers / textures.
- Fire-and-forget mode (in-place updates, no readback).
- GPU-side buffer copy (no CPU roundtrip).
- Async dispatch (yields to event loop).
- 5-second per-dispatch timeout (`HL_GPU_TIMEOUT_MS`).

Measured: on Apple M1 Max, GPU beats WASM AOT past ~16K-vector cosine
similarity workloads.

### Agent-native development is a first-class workflow

The `hull agent` family ships 26 subcommands covering routes, schema,
read-only SQL, request preview, status, errors, tests, deploy readiness,
manifest analysis, capabilities (declared vs used), validate, vfs,
compute modules + AOT, GPU shaders + devices, perf snapshot, log tail,
one-shot eval, template render, compute-call, schema-diff, sql-named.
All emit JSON to stdout. All are also exposed via MCP (`hull mcp`) so
Cursor and Claude Code can hit the same surface natively. Together with
`hull dev --agent` (sidecar `.hull/dev.json` + `.hull/last_error.json`),
agents have a complete feedback loop. This is genuinely differentiated
— most "AI-friendly" tools mean "we wrote good docs."

### Distribution lifecycle is closed

`install.sh` (POSIX, ~250 lines), shell completions for bash/zsh/fish,
`hull doctor` for environment checks, `hull update` for signed
self-update (Ed25519 release signature + SHA-256 manifest verification),
`hull deploy` for Dockerfile / systemd / fly.toml generation,
reproducible build chain (`make self-build` produces hull → hull2 →
hull3 identical). The release signing key infrastructure is in place
(`HL_RELEASE_PUBKEY_HEX`, GitHub Actions sign step in workflow). The
gap to v0.1.0 is release engineering, not feature engineering.

## What still needs honest acknowledgment

### Multi-user server use cases are bounded

Lua/JS handlers run one at a time on the event loop. Async DB and HTTP
work hits the thread pool. SQLite serializes writes. Hull benchmarks
70-100K req/s on I/O-bound routes and ~19K req/s on SQLite writes on a
single core — excellent for local tools and small-team servers, not for
thousands of concurrent users on one box. This is by design but it's
the ceiling that matters if Hull targets multi-user servers.

The path forward is in `roadmap.md` (PostgreSQL backend, worker-pool
expansion, SQLite-per-tenant sharding via the DB vtable). None of these
require architectural changes — the DB capability layer is already a
vtable, the thread pool already exists, the allocator is already
pluggable.

### AGPL still creates adoption friction

The developers most likely to benefit from Hull — small teams shipping
internal tools, indie hackers, vibecoders — are also most likely to be
uncertain about AGPL implications. The commercial license option helps,
but it adds a decision point at exactly the moment you want zero
friction. The LICENSE file's dead-man's-switch (§9: 24 months without a
release → automatic MIT conversion) is a real risk hedge for adopters,
but it isn't surfaced prominently. This is a positioning + business-model
problem, not a technical one, and worth revisiting as adoption data comes
in.

### QuickJS is still ~10-20× slower than V8 for compute

For I/O-bound CRUD (the primary use case), this barely matters — 52K
req/s on no-DB JS routes is plenty. But it sets a real ceiling for
compute-heavy JS workloads. The escape hatches (`compute.async.call`
for WASM, `gpu.async.dispatch` for GPU) cover this for any workload
significant enough to feel the gap.

### Encrypted-at-rest is not implemented

The MANIFESTO previously claimed "AES-256 database encryption,
license-key-derived." This is corrected. The infrastructure exists
(TweetNaCl's Curve25519 + XSalsa20+Poly1305 gives apps `crypto.secretbox_*`
for value-level encryption today), but transparent SQLite-level
encryption (SEE-compatible or SQLCipher-style) is roadmap, not shipped.
Apps that need it now wrap sensitive values with `secretbox` before
writing.

### Hosted platform services don't exist yet

Hull Build, Hull Verify, and Hull Sync — described in INVESTORS.md
with pricing — are planned post-v0.1.0. None are live. Every Hull
application works without them today; the value proposition holds.
But the revenue projections in the investor brief depend on services
that haven't been built.

## The scaling path

Three engineering tracks, all incremental on the existing architecture:

### PostgreSQL backend (HlDbBackend)

The DB capability layer is already a vtable (`HlDbBackend` in
`include/hull/cap/db.h`). The compute-only build (`HL_ENABLE_DB=0`)
already proves that the rest of the platform doesn't depend on SQLite.
A PostgreSQL backend (libpq + statement caching) is the natural next
step. Per-handler config or `--db postgres://…` selects the backend at
startup. App code is unchanged.

This validates the abstraction and unlocks two new deployment shapes
(shared-DB multi-instance, multi-tenant hosted) without breaking the
local-first single-binary story.

### SQLite-per-tenant sharding

For multi-tenant servers, per-tenant SQLite files (or per-workspace,
or per-partition-key) give each shard its own WAL and its own write
lock. The DB vtable is the natural routing point — app code doesn't
need to change if the underlying handle resolves to a different SQLite
file per request.

Proven pattern: Cloudflare D1, Turso/LibSQL, Litestream. Hull's clean
DB capability boundary means shard routing fits below the application
layer without touching app code.

### Worker-pool concurrency expansion

The current shape is correct (event loop accepts + parses, thread pool
handles async DB/HTTP, Lua/JS runs synchronously per request). The
scaling step is one Lua state / QuickJS runtime per worker thread with
explicit dispatch from the event loop. The same model Nginx uses (event
loop + worker processes) and that Node.js worker_threads provide. The
allocator discipline and capability isolation that already exist make
this cleaner here than in most C codebases.

Not needed for the primary local-first thesis (100K req/s on a single
thread is far more than one person clicking buttons requires). Build
this when multi-user server demand is a real signal, not before.

## Strategic positioning (still valid)

### Lead with ownership, not with Lua

`hull build` produces a file the creator owns. No hosting costs. No
vendor lock-in. No runtime dependency. The binary is the product. This
is the message that resonates with the AI-coding wave: millions of
people can now describe software, but they can't distribute it without
renting infrastructure. Hull eliminates the rental.

### The capability-secure boundary is the underused differentiator

In a world where AI agents write and run code, the manifest-declared,
kernel-enforced sandbox is exactly what's needed. The app declares what
it can touch. The kernel enforces it. The signing chain verifies the
app hasn't been tampered with. This is the opposite of "give the AI
agent access to everything and hope for the best." As AI-generated
software becomes common, the question "how do I know this app is safe
to run?" becomes critical. Hull has a real answer that competitors
literally cannot match without rewriting their stack. Lead with it.

### Agent-native is a real moat

The 26-subcommand agent surface + MCP + sidecar files means an AI coding
assistant has a closed feedback loop on a Hull project that doesn't exist
on any competing stack. "Did the reload succeed?", "what's the schema?",
"what's the declared manifest vs what the code actually uses?", "render
this template with this data" — all single CLI calls returning JSON. The
combination of `hull dev --agent` + `hull agent eval` + `hull agent
validate` + MCP integration is a workflow nobody else has shipped.

## What to build next (priority order)

This matches `docs/roadmap_next.md`:

1. **v0.1.0 release engineering.** Tag, sign, publish, announce.
   Everything technical is in place; this is product work.

2. **PostgreSQL backend (HlDbBackend).** Validates the DB-vtable
   abstraction. Unlocks shared-DB deployments without breaking
   local-first.

3. **Module ecosystem / package format.** Currently apps are flat.
   A versioned, signed module format (Hull's answer to luarocks/npm,
   except with manifest-aware capabilities) would let community modules
   compose without re-introducing supply-chain risk.

4. **Worker-pool concurrency expansion** for multi-user servers.

5. **Hosted services (Hull Build / Verify / Sync).** Post-v0.1.0.

6. **Encrypted-at-rest** (SQLCipher-style transparent SQLite encryption,
   license-key-derived). Roadmap item that closes the MANIFESTO claim.

## The bottom line

Hull's technical foundation — the C quality, the capability-secure
architecture, the dual-signature build pipeline, the cross-platform
story, the WASM/GPU compute layers, the agent platform — is
production-grade and the security audits prove it. The constraints
that remain are ecosystem richness and developer adoption, not runtime
architecture. The single-binary thesis, the ownership model, and the
capability-secure story are genuine, defensible differentiators.

The question for the next year is execution speed: can the v0.1.0
release ship, can the module ecosystem mature, and can the AI-coding
workflow surface (`hull agent` + MCP) capture mindshare fast enough to
make Hull the default exit ramp for AI-generated code before the window
closes?
