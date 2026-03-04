# Hull — Platform Assessment

An honest evaluation of where Hull stands, what's strong, what's weak, and what the scaling path looks like.

## What Hull gets right

### The single-binary thesis is real

The build pipeline — source files collected into a sorted registry, compiled, linked against a signed platform library, Ed25519 signed — actually delivers on the promise. Sub-2MB binaries that run on six operating systems via Cosmopolitan APE. Most "single binary" claims involve Docker images or bundled runtimes. Hull means a literal file you can email to someone.

### The security architecture is unusually mature

Capability-based manifests with kernel enforcement isn't something you see at this stage. Three independent verification points (pre-download browser tool, pre-install CLI verification, runtime startup check). Pledge/unveil via seccomp-bpf + landlock on Linux. Ed25519 signing chains for both platform and app layers. The C enforcement boundary between application scripting and system resources is structural, not bolted-on. The attack model in `docs/security.md` addresses real adversaries, not theoretical ones.

### The C foundation is production-grade

Allocator discipline (no raw malloc/free anywhere), overflow guards before arithmetic, three sanitizer configurations in CI (ASan+UBSan, MSan+UBSan, debug), static analysis (scan-build + cppcheck), fuzz targets for the HTTP parser and multipart reader. Keel sustains 100K+ req/s on a single core. The VFS provides O(log n) lookups over embedded file arrays. This is not prototype-quality C.

### The architecture is extensible

Each layer talks only to the one below it. The allocator interface means memory management can be swapped. The event loop is abstracted behind epoll/kqueue/io_uring/poll backends. The parser is a vtable. The capability system is a clean boundary. These abstractions aren't academic — they're what makes the scaling path (below) tractable without rewrites.

## What needs honest acknowledgment

### The runtime ecosystem is thin

Lua and QuickJS are defensible technical choices for embedding and sandboxing. But the combined stdlib (cors, ratelimit, csrf, auth, jwt, session, template, json, cookie, i18n) is sparse compared to Express, Django, or Rails. Developers who hit the edge of what the stdlib provides have no npm or luarocks to fall back on. Every missing library is a feature request or a hand-rolled solution. This is the primary adoption friction point.

### QuickJS performance is a known ceiling

QuickJS is roughly 10-20x slower than V8 for compute-heavy workloads. For I/O-bound CRUD (the primary use case), this matters less than it sounds — the 52K req/s on no-DB routes and 4.5K req/s on DB-write routes are adequate for local-first single-user apps. But it sets a ceiling that matters if Hull targets multi-user servers.

### AGPL creates adoption friction

The developers most likely to benefit from Hull — small teams shipping internal tools, indie hackers building products, vibecoders turning ideas into distributable software — are the ones most likely to be uncertain about AGPL implications. The commercial license option helps, but it adds a decision point at exactly the moment you want zero friction. This is a business model choice, not a technical one, and it may need revisiting as adoption data comes in.

### The "AI writes Lua" thesis is necessary but not sufficient

The README correctly identifies that AI solved code generation but not deployment. But AI writes Go, Rust, and Python just as easily as Lua, and those ecosystems have their own deployment stories. The differentiator isn't that AI can write Lua — it's that Hull's deployment model produces an artifact the creator owns. The positioning should lead with ownership and distribution, not with the language.

## The scaling path

Two common objections — SQLite scalability and single-threaded throughput — are engineering tasks, not architectural dead ends. The foundation already supports both.

### SQLite sharding

SQLite's single-writer constraint disappears with sharding. Per-tenant, per-workspace, or per-partition-key database files give each shard its own WAL and its own write lock. The capability layer (`db.query`, `db.exec`) is the natural routing point — application code doesn't need to change if the underlying handle points to a different SQLite file.

This is a proven pattern. Cloudflare D1, Turso/LibSQL, and Litestream all bet on SQLite-per-tenant. The architecture is right: Hull's clean DB capability boundary means shard routing can be added below the application layer without touching app code.

For the primary use case (local-first, single-user), sharding isn't needed. For multi-tenant servers, it's a straightforward extension of what's already there.

### Worker pool concurrency

The current model is single-threaded: the event loop accepts connections, reads requests, runs the handler, writes the response. CPU-bound work in Lua/QuickJS blocks everything.

The path to concurrency is a producer/consumer worker pool:

1. **Event loop (producer):** Accepts connections, reads requests, parses headers — stays single-threaded on the event loop, which is where it's most efficient.
2. **Worker threads (consumers):** A pool of N threads, each with its own Lua state or QuickJS runtime, handles request processing. The event loop dispatches parsed requests to the pool and gets responses back.
3. **No shared mutable state:** Lua states are per-thread, QuickJS runtimes are per-thread, arena allocators are per-request. The allocator discipline and capability isolation that already exist make this cleaner than it would be in most C codebases.

This is the same model Nginx uses (event loop + worker processes) and what Node.js worker_threads provide. The Keel event loop abstraction, the allocator interface, and the per-request arena allocation are exactly the foundation this requires. It's an incremental addition, not a rewrite.

## Strategic positioning

### Lead with ownership, not with Lua

The strongest thing about Hull isn't the language choice — it's that `hull build` produces a file the creator owns. No hosting costs. No vendor lock-in. No runtime dependency. No cloud account. The binary is the product. This is the message that resonates with the AI-coding wave: millions of people can now describe software, but they can't distribute it without renting infrastructure. Hull eliminates the rental.

### The AI output target is the unlock

If Hull reaches a point where an AI agent goes from natural language to a working, distributable binary in one shot — with the security model providing the guardrails that make that safe — that's a genuinely new capability. Not "AI writes code you then have to deploy" but "AI produces a product." This requires the AI integration to be a first-class workflow, not a positioning statement. The `hull new` + `hull dev` + `hull build` pipeline is close, but the gap between "scaffold" and "production app" needs to shrink.

### The security model is an underused differentiator

In a world where AI agents are writing and running code, Hull's capability-based sandbox is exactly what's needed. The manifest declares what the app can touch. The kernel enforces it. The signing chain verifies the app hasn't been tampered with. This is the opposite of "give the AI agent access to everything and hope for the best." As AI-generated software becomes common, the question "how do I know this app is safe to run?" becomes critical. Hull has a real answer. Lead with it.

## What to build next (priority order)

1. **Stdlib expansion** — Email, CSV, FTS5, RBAC. Every missing module is a reason to reach for Express instead. When a vibecoder's AI writes `require("hull.email")` and it doesn't exist, they leave. The stdlib gap is the leak in the adoption funnel. This directly serves the people Hull is built for today. (Input validation is already shipped: `hull.validate` in both Lua and JS.)
2. **AI workflow integration** — Make the path from description to distributable binary as short as possible. The thesis lives or dies on how few steps it takes to go from "I want an invoicing app" to a file you can distribute. This is product work, not infrastructure work.
3. **Worker pool concurrency** — Removes the single-threaded ceiling for multi-user server use cases. Not needed for the primary local-first, single-user thesis — 100K req/s on a single thread is absurdly more than enough for one person clicking buttons. Build this when multi-user servers become a real demand signal.
4. **SQLite sharding** — Per-tenant database files, routed through the capability layer. Same as above: needed when multi-tenant servers are a real use case, not before.
5. **WASM compute plugins** — Architecture is designed. Opens Hull to compiled-language performance for compute-heavy tasks without breaking the sandbox model. Build when the stdlib and workflow are mature enough that compute performance is the bottleneck, not missing features.

## The bottom line

Hull's technical foundation — the C quality, the security model, the build pipeline, the cross-platform story, the clean architecture — is strong enough to support the scaling path. The constraints are ecosystem richness and developer adoption, not runtime architecture. The single-binary thesis, the ownership model, and the security story are genuine differentiators. The question is execution speed: can the stdlib and developer experience mature fast enough to capture the AI-coding wave before the window closes?
