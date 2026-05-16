# Hull — Investor Brief

## One sentence

Hull is a capability-secure runtime that turns AI-generated code into self-contained, distributable, local-first products — sandboxed by default, no cloud, no hosting, no dependencies.

## The problem

AI coding assistants (Claude Code, Cursor, Copilot, Codex) solved code generation. Millions of people can now describe software and have it written. But the output is always the same: a React frontend, a Node.js backend, a Postgres database, and a cloud deployment problem.

The vibecoder — the person who builds software by describing it to an AI — swapped one dependency (coding skill) for another (cloud infrastructure). They don't own anything more than before. They just rent different things.

There is no tool that takes AI-generated code and produces a product the creator owns. A file they can distribute, sell, or put in Dropbox. The AI-to-product pipeline has a missing piece: distribution.

Hull is that piece.

## The product

Hull is a capability-secure runtime for local-first applications. It is not a do-everything framework — it is a sandboxed environment where AI-generated code runs inside declared capability boundaries. The app states what it can access (files, hosts, environment variables, GPU devices), and the kernel enforces it. The agent writes the code; Hull constrains what that code can do.

The developer writes backend logic in Lua or JavaScript, frontend in HTML5/JS, data in SQLite. `hull build` compiles everything into a single portable executable — around 5 MB on aarch64, ~3.66 MB for compute-only deployments — that runs on Linux, macOS, Windows, FreeBSD, OpenBSD, and NetBSD.

The whole stack is vendored: Keel (HTTP server), Lua 5.4 + QuickJS (dual scripting), SQLite (database), mbedTLS (TLS client), TweetNaCl (Ed25519 signatures), pledge/unveil (kernel sandbox), miniz (gzip), the Mozilla CA bundle (HTTPS without a system store), and embedded TinyCC (so `hull build` works with zero system dependencies). Optional: WAMR for WASM compute, wgpu-native for GPU. No external package managers, no transitive dependency graphs, no runtime installation. The user double-clicks a file and has a working application. Their data never leaves their machine.

In a world where AI writes the code, the runtime must be the trust boundary. Unrestricted frameworks assume trusted code — Hull assumes untrusted code and proves containment. This is the architectural difference: Hull apps can't exfiltrate data, can't access undeclared files, can't connect to undeclared hosts, can't spawn processes, can't open GPU devices not in the manifest. Not by policy — by kernel enforcement (pledge/unveil on Linux/Cosmo/OpenBSD, Seatbelt on macOS).

Hull ships batteries included: routing, authentication, RBAC, sessions, JWT, CSRF, transactional outbox/inbox, idempotency keys, rate limiting, CORS, health checks, ETags, HTML templates, input validation, form parsing, full-text search (SQLite FTS5), i18n, CSV, image codecs, SMTP, WebSockets, SSE, background timers, audit logging. WASM compute plugins for CPU-bound work; GPU compute shaders for parallel data. Plus a complete AI-agent surface — 26 `hull agent` subcommands exposing routes, schema, tests, request previews, manifest analysis, and structured eval as JSON, all also reachable over MCP. A vibecoder describes an invoicing app to an AI, the AI writes Lua, `hull build` produces a file. That file is the product.

## Why now

Three forces are converging:

**1. AI coding is mainstream.** Claude Code, Cursor, and Copilot have millions of active users. The number of people who can describe software and have it built is growing exponentially. Every one of them hits the same wall: deployment. This wall didn't exist two years ago because these people weren't building software two years ago.

**2. Local-first is being driven by regulation.** GDPR, CCPA, the EU Digital Markets Act, and data sovereignty laws are pushing data back to the edge. Organizations want software that keeps data local, not software that sends it to a server they don't control. This is not ideology — it's compliance.

**3. Supply chain security is a board-level concern.** SolarWinds, Log4j, xz-utils. "How many dependencies does this have?" is a question executives ask now. Hull's answer — all vendored, no package managers, no build-time downloads, the entire C surface auditable in a few days — is the strongest possible position.

**4. AI-generated code is untrusted code.** When an AI writes an application, nobody has read every line. The vibecoder described what they wanted — they didn't audit what they got. Traditional frameworks assume the developer trusts the code they wrote. Hull assumes the opposite: the code is untrusted, and the runtime must prove containment. This is a new requirement that didn't exist before AI coding — and no existing framework addresses it.

The timing window is narrow. Whoever builds the vibecoder-to-product pipeline first accumulates the trust, community, and ecosystem that defines the category. This is a land grab.

## The economics

Hull's cost structure is unusually favorable for a software business.

**Revenue model:** Two streams — one-time licenses and recurring platform services.

**Stream 1: Commercial licenses.** One-time purchase.

| Tier | Price | Target |
|------|-------|--------|
| Standard | $99 | Solo developers |
| Team | $299 | Small teams (up to 5) |
| Perpetual | $499 | Lifetime updates |

Update renewals ($49-149/year) provide optional recurring revenue.

**Why one-time works:** The license is a legal instrument (AGPL exemption for closed-source distribution), not a feature gate. AGPL and commercial builds are functionally identical. Customers pay to distribute without source — once. This removes churn from the business model entirely. No retention marketing. No cancellation anxiety. Revenue arrives at the point of sale.

**COGS: ~$0 per license.** A license is an Ed25519-signed key file delivered as a download. No infrastructure per customer. No activation server. No hosting obligation — customers run Hull on their own machines.

**Stream 2: Hosted platform services *(planned, not yet shipped).*** Recurring, optional, complementary to local-first.

| Service | What it does | Why it can't be local | Status |
|---------|-------------|----------------------|--------|
| **Hull Build** | Hosted `hull build` — upload source, download a signed binary | Cross-compilation requires cosmocc toolchain; convenience for vibecoders who don't want to install a C compiler | Planned post-v0.1.0 |
| **Hull Verify** | Browser-side binary integrity verification (verify.gethull.dev) | Trust anchor must be independent of the binary being verified | Planned post-v0.1.0 |
| **Hull Sync** | Encrypted mailbox relay for multi-user Hull apps | Local-first apps need a rendezvous point for multi-device/multi-user sync; zero-knowledge — the server relays encrypted payloads it cannot read | Planned post-v1.0 |

These services are by design optional. Every Hull app works without them today. `hull eject` already gives you the build toolchain locally. Sync can be replaced by any file-sharing mechanism. The services are convenience and infrastructure — not a dependency. This is the critical distinction: customers can stop paying and their apps keep working. That's not SaaS. That's a service.

**Pricing (planned — none of these are live yet):**

| Service | Price | Model |
|---------|-------|-------|
| Hull Build | Free tier (5 builds/month) + $9/month unlimited | Usage-based with cap |
| Hull Verify | Free | Trust infrastructure, not a revenue line |
| Hull Sync | $5/month per user | Per-user; covers encrypted storage and relay |

**Why this matters:** License revenue is front-loaded — a spike at each release, then a trough. Once shipped, platform services would provide steady monthly recurring revenue that smooths the curve. Together, the two streams would give Hull both the high-margin economics of one-time sales and the predictability of recurring revenue — without the ethical contradiction of locking customers into infrastructure they can't leave.

**Gross margin: ~95%.** Costs are salaries, CI (GitHub Actions, runners), CDN for downloads/docs, and — once shipped — modest infrastructure for hosted services. These are largely fixed. Adding the 10,000th customer costs the same as adding the 10th.

**Break-even is low.** A 2-3 person team with near-zero COGS needs hundreds of licenses — not thousands — to cover costs. At $99-499 per license plus platform services, the math works early.

**Expansion:** Enterprise contracts ($2,000-10,000/year) for compliance documentation and priority support. Hull Marketplace (curated app directory, 15% commission). Training and certification. Migration consulting (Excel/SaaS to Hull).

## The market

We don't have credible TAM/SAM/SOM numbers because this category doesn't exist yet. Market sizing for a category that doesn't exist is fiction, and we won't insult you with fabricated numbers.

What we know:

- **AI coding assistants have millions of users.** Some fraction want to distribute what they build. Today they can't — Hull removes that wall.
- **SMBs are massively underserved.** Millions of small businesses run on spreadsheets because custom software was too expensive. A vibecoder building a Hull app for €500 is cheaper than any SaaS subscription over 2 years.
- **High-value verticals align naturally.** Defense and government (air-gapped, zero supply chain, kernel sandbox). Medical and healthcare (HIPAA, encrypted database, offline operation). Legal and financial (data residency, client confidentiality). These sectors have high willingness to pay and compliance requirements that Hull satisfies by architecture.

The honest answer: we'll know the market size when we see adoption. Hull's cost structure means we don't need a large market to be profitable.

## The moat

The technical stack is not the moat. It could be replicated with sufficient effort.

The moat is the ecosystem:

**First mover in a new category.** The vibecoder-to-product pipeline doesn't exist. Hull is building it. Whoever gets there first accumulates trust, community, documentation, tutorials, and real-world applications that compound. Catching up means replicating the ecosystem, not just the technology.

**Trust accumulation.** Every signed Hull binary, every verify.gethull.dev check, every AGPL application with visible source builds a trust network. Ed25519 signatures create a verifiable chain from source to binary. Trust compounds over time and doesn't transfer to competitors.

**Community gravity.** AGPL means every free Hull application is a showcase. Users see the source, see the platform, see the commercial license as the natural upgrade. The more apps that exist, the more discoverable Hull becomes.

**Ejection as trust signal.** `hull eject` copies the build tool into the project. Developers can leave anytime. This paradoxically increases loyalty — people trust platforms that let them leave. No lock-in means the platform must earn retention through value, which is exactly the kind of retention that lasts.

**Simplicity as structural advantage.** Vendored dependency set with zero package-manager graphs, a small well-bounded capability boundary in C, and a build system that doesn't pull anything from the internet. A 2-3 person team maintains the entire platform. Competitors building on Electron/Node/npm stacks need much larger teams for parity. Hull's simplicity is a cost advantage that compounds.

**Platform services as non-coercive lock-in.** Hull Build, Hull Verify, and Hull Sync create value without creating dependency. Every service is optional and replaceable. But once a developer is building with hosted `hull build`, verifying with verify.gethull.dev, and syncing with Hull Sync, switching costs are real — not because we lock them in, but because the integrated experience is better than the alternatives. This is the same dynamic that makes GitHub sticky despite git being decentralized.

## Comparables

| Company | Model | Revenue | Relevance |
|---------|-------|---------|-----------|
| Sublime Text | One developer, one-time purchase, text editor | ~$30M+ lifetime | Proves solo/small-team dev tools with one-time pricing work |
| JetBrains | Developer tooling, perpetual fallback licenses | $600M+/year | Proves developer platforms can build large businesses |
| Panic (Transmit, Nova) | Small team, premium dev tools, one-time purchase | Profitable, private | Proves small teams can build premium, profitable dev tool businesses |
| Laravel (Spark, Forge, Vapor) | Open-source framework + commercial tools | $25M+/year | Proves AGPL/open-source + commercial dual license works |

Hull's cost structure is leaner than all of these. No servers to operate for customers. No per-customer infrastructure costs. No SaaS operational burden.

## The risks (honest)

**New category.** The vibecoder-to-product market doesn't exist yet. It might not materialize as expected. Mitigation: Hull's cost structure means profitability at small scale. We don't need the market to be large — we need it to exist.

**Lua adoption.** Lua is less popular than Python, JavaScript, or Rust. Developers may resist learning a new language. Mitigation: vibecoders don't learn languages — they describe what they want and the AI writes it. Lua's simplicity makes LLM generation more reliable, not less. The developer never needs to read Lua if they don't want to.

**Single-threaded scripting + bounded worker pool.** Lua/JS handlers run one at a time on the event loop; async DB and HTTP work runs on a configurable thread pool. SQLite serializes writes. This is excellent for local tools and small-team servers (Hull benchmarks 70-100K req/s for I/O-bound routes on a single core, ~19K req/s for SQLite writes). It's deliberately not designed for thousands of concurrent users on one box. Mitigation: this is the design — Hull is for local tools and edge/embedded workloads. Optional WASM compute (via WAMR, AOT-compiled) and GPU compute (wgpu-native) handle CPU-bound and parallel-data workloads at near-native speed when needed. For multi-tenant scaling, SQLite-per-tenant sharding is on the post-v0.1.0 roadmap — the capability layer is the natural routing point.

**Sandbox gaps.** Kernel-enforced sandboxing (pledge/unveil) only works on Linux and OpenBSD. macOS and Windows get libc-level pledge/unveil emulation via Cosmopolitan libc, plus application-level safety via the Lua sandbox. The C attack surface is minimal (~1,500 lines of binding code). Mitigation: Windows App Container and macOS App Sandbox are on the roadmap.

**Platform risk.** Hull depends on Cosmopolitan C for cross-platform binaries. If Cosmopolitan development stalls, Hull's "runs anywhere" promise weakens. Mitigation: Cosmopolitan is open source and actively maintained by Justine Tunney. Hull could fall back to per-platform builds — less elegant, still functional.

**Competitive response.** A well-funded competitor (or a big tech company) could build something similar. Mitigation: first-mover ecosystem advantage. The combination of Cosmopolitan APE + Lua + SQLite + kernel sandbox + Ed25519 licensing + AGPL dual license is specific enough that a generic "local-first framework" from a big company would lack the coherence. And big companies don't ship AGPL.

## Survivability

**Dead man's switch.** If Hull publishes no release for 24 consecutive months, the entire codebase automatically converts from AGPL-3.0 to MIT. This is a legally binding clause in the license. It means:

- The community can fork under MIT if the project is abandoned
- Commercial license holders keep their rights regardless
- The trigger is objective and verifiable (GitHub release tags)

This protects investors and customers. It also signals confidence — we wouldn't include a dead man's switch if we planned to abandon the project.

**Zero runtime dependency.** Every Hull application ever built continues to work forever, regardless of what happens to the company. The binaries are standalone. No phone-home. No activation server. No infrastructure to maintain. If Hull disappears tomorrow, the products built on it keep running.

## The team

Artalis Consulting Kft. Budapest, Hungary.

Small team, low burn, high leverage. The platform's simplicity is intentional — a vendored, in-tree dependency set with no package-manager graphs means a 2-3 person team can build and maintain the entire platform. This is not a company that needs 50 engineers. It's a company that needs 3 good ones.

## What we're looking for

**Strategic capital, not just money.** Hull benefits most from investors who:

- Have reach into the vibecoder/AI developer community
- Understand developer tooling go-to-market (bottom-up adoption, community-driven growth)
- Can open doors in high-value verticals (defense, medical, financial)
- Have patience for category creation — this is a 3-5 year build, not a 12-month flip

**Where Hull is today (as of 2026-05):** Hull v0.1.0 is feature-complete and approaching release. Core platform, dual runtime, capability layer, kernel sandbox, signature chain (three Ed25519 layers), `hull build` with embedded TinyCC, WASM compute (interpreter + AOT + Memory64), GPU compute (wgpu-native), the full stdlib (sessions, CSRF, RBAC, idempotency, outbox/inbox, FTS5, i18n, ETags, health, etc.), agent platform (26 `hull agent` subcommands + MCP), `hull update` (signed self-update), three completed security audits with all 73 findings closed. The product exists; what's left for v1.0 is mostly polish, docs, and ecosystem.

**What the capital would fund:**

- v0.1.0 release engineering and post-release stabilization
- PostgreSQL backend (validates the DB-vtable abstraction)
- Worker-pool concurrency expansion for multi-user server use cases
- gethull.dev website, docs site, and example gallery
- Hosted platform services (Hull Build, Hull Verify, Hull Sync) — planned post-v0.1.0
- Initial community building and developer relations
- First enterprise pilot engagements (defense/medical verticals)

## Contact

Artalis Consulting Kft.
Email: info@artalis.io
GitHub: [github.com/artalis-io/hull](https://github.com/artalis-io/hull)
Web: [gethull.dev](https://gethull.dev)
