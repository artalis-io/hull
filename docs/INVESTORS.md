# Hull. Investor Brief

> **Code became disposable. Trust is not.** AI generates code endlessly. Hull constrains what that code can actually do, at a kernel-enforced boundary the script cannot cross.
>
> Canonical descriptor: hardened, capability-secure runtime infrastructure for AI-native systems. See [POSITIONING.md](POSITIONING.md) for the operational style reference; this brief is the investor-facing rationale.

## One sentence

Hull is a capability-secure runtime that turns AI-generated code into
self-contained, distributable, local-first products. Sandboxed by default, no
cloud, no hosting, no runtime dependencies.

## The sharper thesis

Hull is the hardened, capability-secure runtime and build system for AI-generated local software.

The first wave of AI coding tools made agents productive. The next wave has to
make them governable. Hull's bet is that the valuable market is not simply
"generate more code"; it is letting organizations use AI agents to build real
software while constraining what the agent and the generated app can touch,
proving what happened, and shipping an artifact the customer owns.

That matters most where cloud AI, unrestricted developer machines, npm-style
dependency graphs, and ambient filesystem/network authority are unacceptable:
regulated enterprises, industrial/OT teams, defense, healthcare, financial
institutions, embedded product teams, and air-gapped environments.

## The problem

Enterprises want AI coding, but many cannot use it the way consumer developers
do. They cannot send source code to cloud models. They cannot let agents run
arbitrary shell commands on developer machines. They cannot accept undeclared
network access, dependency sprawl, or unverifiable build artifacts. The
commercial wedge for Hull is this: make AI-generated software governable enough
for regulated and on-prem environments.

AI coding assistants (Claude Code, Cursor, Copilot, Codex) solved code
generation for the broad market. Millions of people can now describe software
and have it written. But the default output is usually the same: a React
frontend, a Node.js backend, a Postgres database, and a cloud deployment
problem.

The vibecoder (the person who builds software by describing it to an AI) swapped one dependency (coding skill) for another (cloud infrastructure). They don't own anything more than before. They just rent different things.

There are ways to package software today. Electron, Docker, Tauri,
PyInstaller, installers, cloud templates. But they remain operationally heavy,
cloud-oriented, dependency-heavy, or unsafe for the environments Hull targets.
The AI-to-product pipeline has a missing piece: governed local distribution.

Hull is that piece.

## The product

Hull is a capability-secure runtime for local-first applications. It is not a do-everything framework. It is a sandboxed environment where AI-generated code runs inside declared capability boundaries. The app states what it can access (files, hosts, environment variables, GPU devices), and the kernel enforces it. The agent writes the code; Hull constrains what that code can do.

The same architecture extends naturally to the agent itself. Hull's shipped
agent surface gives models structured tools (`hull agent routes`, schema,
tests, requests, manifest analysis, logs, eval, deployment checks) rather than
raw shell access. The roadmap extends this into a local agent runtime that can
use open-weight models through `bitnet.c`, `llama.cpp`, Ollama, or compatible
local servers. In that planned workflow, the model inspects, patches, tests,
and builds through Hull's bounded tool surface. The app it produces runs inside
Hull's bounded runtime.

The developer writes backend logic in Lua or JavaScript, frontend in HTML5/JS, data in SQLite. `hull build` compiles everything into a single portable executable (around 5 MB on aarch64, ~3.66 MB for compute-only deployments) that runs on Linux, macOS, Windows, FreeBSD, OpenBSD, and NetBSD.

The whole stack is vendored: Keel (HTTP server), Lua 5.4 + QuickJS (dual scripting), SQLite (database), mbedTLS (TLS client), TweetNaCl (Ed25519 signatures), pledge/unveil (kernel sandbox), miniz (gzip), the Mozilla CA bundle (HTTPS without a system store), and embedded TinyCC (so `hull build` does not require a separately-installed C compiler; the system linker is still used for the link step). Optional: WAMR for WASM compute, wgpu-native for GPU. No external package managers, no transitive dependency graphs, no runtime installation. The user double-clicks a file and has a working application. Their data never leaves their machine.

In a world where AI writes the code, the runtime must be the trust boundary. Unrestricted frameworks assume trusted code; Hull assumes untrusted code and proves containment. This is the architectural difference: Hull apps cannot access undeclared files, connect to undeclared hosts, spawn processes, or open GPU devices not in the manifest. A Hull app can only communicate through declared capabilities. Not by convention but by kernel enforcement where available (pledge/unveil on Linux/Cosmo/OpenBSD, Seatbelt on macOS) plus Hull's C capability layer.

Hull ships batteries included: routing, authentication, RBAC, sessions, JWT, CSRF, transactional outbox/inbox, idempotency keys, rate limiting, CORS, health checks, ETags, HTML templates, input validation, form parsing, full-text search (SQLite FTS5), i18n, CSV, image codecs, SMTP, WebSockets, SSE, background timers, audit logging. WASM compute plugins for CPU-bound work; GPU compute shaders for parallel data. Plus a complete AI-agent surface. Over two dozen `hull agent` subcommands exposing routes, schema, tests, request previews, manifest analysis, and structured eval as JSON, all also reachable over MCP. A vibecoder describes an invoicing app to an AI, the AI writes Lua, `hull build` produces a file. That file is the product.

For secure and embedded environments, the planned local-agent workflow is
stronger: an engineer or operator runs a local model, gives it a bounded goal,
Hull records the session, applies patches through policy, runs tests/builds,
signs the output, and produces provenance. The organization gets AI
productivity without granting the agent or the generated code uncontrolled
authority.

## Why now

Five forces are converging:

**1. AI coding is mainstream.** Claude Code, Cursor, and Copilot have millions of active users. The number of people who can describe software and have it built is growing exponentially. Every one of them hits the same wall: deployment. This wall didn't exist two years ago because these people weren't building software two years ago.

**2. Local-first is being driven by regulation.** GDPR, CCPA, the EU Digital Markets Act, and data sovereignty laws are pushing data back to the edge. Organizations want software that keeps data local, not software that sends it to a server they don't control. This is not ideology. It's compliance.

**3. Supply chain security is a board-level concern.** SolarWinds, Log4j, xz-utils. "How many dependencies does this have?" is a question executives ask now. Hull's answer (all vendored, no package managers, no build-time downloads, and a small C capability surface that has already gone through multiple full-source audits) is the strongest possible position.

**4. AI-generated code is untrusted code.** When an AI writes an application, nobody has read every line. The vibecoder described what they wanted. They didn't audit what they got. Traditional frameworks assume the developer trusts the code they wrote. Hull assumes the opposite: the code is untrusted, and the runtime must prove containment. This is a new requirement that did not exist at this scale before AI coding, and few mainstream app frameworks address it as their core design constraint.

**5. Enterprises want AI coding, but not unconstrained AI coding.** Regulated
teams are under pressure to adopt AI development tools, but many cannot send
source or data to cloud models, cannot allow agents arbitrary shell/network
access, and cannot accept unaudited dependency sprawl. Hull's shipped bounded
runtime and agent introspection surface, plus the planned local model path,
signed sessions, policy gates, SBOMs, and reproducible build attestations, turn
AI development into something a security team can review.

The timing window is narrow. Whoever builds the vibecoder-to-product pipeline first accumulates the trust, community, and ecosystem that defines the category. This is a land grab.

## The economics

Hull's cost structure is unusually favorable for a software business.

**Revenue model:** Three streams. Commercial licenses, optional platform
services, and enterprise local-agent deployments.

**Stream 1: Commercial licenses.** One-time purchase.

| Tier | Price | Target |
|------|-------|--------|
| Standard | $99 | Solo developers |
| Team | $299 | Small teams (up to 5) |
| Perpetual | $499 | Lifetime updates |

Update renewals ($49-149/year) provide optional recurring revenue.

**Why one-time works:** The license is a legal instrument (AGPL exemption for closed-source distribution), not a feature gate. AGPL and commercial builds are functionally identical. Customers pay to distribute without source. Once. This removes churn from the business model entirely. No retention marketing. No cancellation anxiety. Revenue arrives at the point of sale.

**COGS: ~$0 per license.** A license is an Ed25519-signed key file delivered as a download. No infrastructure per customer. No activation server. No hosting obligation. Customers run Hull on their own machines.

**Stream 2: Hosted platform services *(planned, not yet shipped).*** Recurring, optional, complementary to local-first.

| Service | What it does | Why it can't be local | Status |
|---------|-------------|----------------------|--------|
| **Hull Build** | Hosted `hull build`. Upload source, download a signed binary | Cross-compilation requires cosmocc toolchain; convenience for vibecoders who don't want to install a C compiler | Planned post-v0.1.0 |
| **Hull Verify** | Browser-side binary integrity verification (verify.gethull.dev) | Trust anchor must be independent of the binary being verified | Planned post-v0.1.0 |
| **Hull Sync** | Encrypted mailbox relay for multi-user Hull apps | Local-first apps need a rendezvous point for multi-device/multi-user sync; zero-knowledge. The server relays encrypted payloads it cannot read | Planned post-v1.0 |

These services are by design optional. Every Hull app works without them today. `hull eject` already gives you the build toolchain locally. Sync can be replaced by any file-sharing mechanism. The services are convenience and infrastructure. Not a dependency. This is the critical distinction: customers can stop paying and their apps keep working. That's not SaaS. That's a service.

**Consumer pricing sketch (planned. None of these are live yet):**

| Service | Pricing direction | Model |
|---------|-------------------|-------|
| Hull Build | Free tier + low-cost paid tier | Usage-based with cap |
| Hull Verify | Free | Trust infrastructure, not a revenue line |
| Hull Sync | Low-cost per-user plan | Per-user; covers encrypted storage and relay |

**Why this matters:** License revenue is front-loaded (a spike at each release, then a trough. Once shipped, platform services would provide steady monthly recurring revenue that smooths the curve. Together, these streams would give Hull both the high-margin economics of one-time sales and the predictability of recurring revenue) without the ethical contradiction of locking customers into infrastructure they can't leave.

**Stream 3: Enterprise local agent workbench *(planned, highest-ACV wedge).***
Recurring enterprise licensing for regulated teams that want on-prem/offline AI
development with constrained tools and signed provenance.

| Package | Target | Model |
|---------|--------|-------|
| Hull Local Agent Workbench | Regulated engineering teams | $25k-100k/year per team/site |
| Hull Secure Build & Attestation | Security/compliance teams | $50k-150k/year platform license |
| Hull Embedded/OT Profile | Industrial and embedded teams | $50k-200k/year + support |
| Air-gapped Enterprise Bundle | Defense/government/classified networks | $150k-500k/year high-touch contract |

This is the more financially attractive wedge than solo vibecoders. Individual
licenses create adoption and community; enterprise/offline agent development
captures the willingness to pay. These buyers pay for trust, auditability,
offline operation, and support. Not just convenience.

**Gross margin.** Self-serve software licenses and hosted-light services can
approach ~95% gross margin because the marginal cost is a signed key, a build,
or a relay account. Enterprise air-gapped deployments and certification support
are lower-margin because they include services, but they are high-ACV and can
fund the platform without requiring consumer-scale adoption.

**Break-even is low.** A 2-3 person team with near-zero self-serve COGS needs
hundreds of licenses or a small number of enterprise pilots. Not thousands of
customers. To cover costs. At $99-499 per self-serve license plus planned
enterprise contracts, the math works early.

**Expansion:** Enterprise contracts for compliance documentation, priority
support, on-prem local-agent deployments, certification bundles, and embedded
profiles. Hull Marketplace (curated app directory, 15% commission). Training
and certification. Migration consulting (Excel/SaaS to Hull).

## The market

We don't have credible TAM/SAM/SOM numbers because this exact category doesn't
exist yet. Market sizing for a category that doesn't exist is fiction, and we
won't insult you with fabricated numbers.

What we know:

- **AI coding assistants have millions of users.** Some fraction want to distribute what they build. Today that is harder, more cloud-bound, and less governable than it should be. Hull removes that wall for local-first products.
- **SMBs are massively underserved.** Millions of small businesses run on spreadsheets because custom software was too expensive. A vibecoder building a Hull app for €500 is cheaper than any SaaS subscription over 2 years.
- **AI coding agents are becoming a major software budget.** Independent analyst
  estimates vary widely, but the direction is clear: code-generation agents are
  one of the fastest-growing AI software categories. The crowded market is
  horizontal cloud coding assistants. Hull's wedge is the part those tools
  handle poorly: local, governed, auditable agentic development.
- **High-value verticals align naturally.** Defense and government (air-gapped,
  zero supply chain, kernel sandbox). Medical and healthcare (HIPAA, offline
  operation, auditability). Legal and financial (data residency, client
  confidentiality). Industrial/OT (offline factory-floor tools, local
  diagnostics, no cloud dependency). Embedded product teams (single-file local
  controllers, telemetry tools, update/config services). These sectors have high
  willingness to pay and compliance requirements that Hull satisfies by
  architecture.

The most attractive initial market is regulated/on-prem agentic software
development:

| Segment | Why Hull fits | Sales motion |
|---------|---------------|--------------|
| Regulated enterprises | Want AI coding without cloud code exposure or unconstrained agents | Enterprise platform/security sale |
| Industrial/OT teams | Need local tools, offline operation, audit logs, no dependency sprawl | Departmental pilot -> site license |
| Defense/government | Air-gapped/open-weight model use, provenance, SBOM, reproducible builds | Slow, high-ACV enterprise sale |
| Healthcare/lab automation | Local data, validated workflows, support bundles, auditability | Compliance-led pilot |
| Embedded product teams | Control-adjacent apps, telemetry, config/update, local inference | Engineering platform sale |
| Solo builders/SMBs | Great adoption channel, lower ACV | Bottom-up community/licensing |

The honest answer: we'll know the market size when we see adoption. Hull's cost
structure means we don't need a large market to be profitable. The first
commercial target should be regulated enterprise and secure industrial teams
that want agentic software development but need on-prem/offline execution,
explicit capabilities, audit trails, and reproducible signed artifacts.

Bottom-up beachhead:

| Milestone | Assumption | Annualized revenue |
|-----------|------------|--------------------|
| 5 secure pilots | $25k average pilot | $125k |
| 10 team/site licenses | $75k average contract | $750k |
| 20 enterprise/OT customers | $125k average contract | $2.5M |
| 50 enterprise/OT customers | $150k average contract | $7.5M |

This is not a TAM claim. It is the practical reason Hull can be a good business
before it becomes a broad developer platform.

## The moat

The technical stack is not the moat. It could be replicated with sufficient effort.

The moat is the ecosystem:

**First mover in a new category.** The vibecoder-to-product pipeline doesn't exist. Hull is building it. Whoever gets there first accumulates trust, community, documentation, tutorials, and real-world applications that compound. Catching up means replicating the ecosystem, not just the technology.

**Trust accumulation.** Every signed Hull binary, every verify.gethull.dev check, every AGPL application with visible source builds a trust network. Ed25519 signatures create a verifiable chain from source to binary. Trust compounds over time and doesn't transfer to competitors.

**Agent provenance.** As agents write more software, the defensible asset is not
only the runtime; it is the provenance layer around agent-generated changes:
model identity, prompts, context packs, tool calls, patches, tests, policy
results, and build hashes. A signed agent session attached to a signed binary is
a compliance primitive that generic coding assistants do not provide.

**Community gravity.** AGPL means every free Hull application is a showcase. Users see the source, see the platform, see the commercial license as the natural upgrade. The more apps that exist, the more discoverable Hull becomes.

**Ejection as trust signal.** `hull eject` copies the build tool into the project. Developers can leave anytime. This paradoxically increases loyalty. People trust platforms that let them leave. No lock-in means the platform must earn retention through value, which is exactly the kind of retention that lasts.

**Simplicity as structural advantage.** Vendored dependency set with zero package-manager graphs, a small well-bounded capability boundary in C, and a build system that doesn't pull anything from the internet. A 2-3 person team maintains the entire platform. Competitors building on Electron/Node/npm stacks need much larger teams for parity. Hull's simplicity is a cost advantage that compounds.

**Offline model optionality.** Hull can integrate with local open-weight
inference backends (`bitnet.c`, `llama.cpp`, Ollama) without making any model a
platform dependency. `bitnet.c` is Artalis-owned code, so bundling it as an
optional CPU-first provider is a licensing-clean path to air-gapped inference;
keeping model weights outside ordinary app bundles preserves small artifacts and
clean model licensing.

**Platform services as non-coercive lock-in.** Hull Build, Hull Verify, and Hull Sync create value without creating dependency. Every service is optional and replaceable. But once a developer is building with hosted `hull build`, verifying with verify.gethull.dev, and syncing with Hull Sync, switching costs are real. Not because we lock them in, but because the integrated experience is better than the alternatives. This is the same dynamic that makes GitHub sticky despite git being decentralized.

## Comparables

| Company | Model | Revenue | Relevance |
|---------|-------|---------|-----------|
| Sublime Text | One developer, one-time purchase, text editor | ~$30M+ lifetime | Proves solo/small-team dev tools with one-time pricing work |
| JetBrains | Developer tooling, perpetual fallback licenses | $600M+/year | Proves developer platforms can build large businesses |
| Panic (Transmit, Nova) | Small team, premium dev tools, one-time purchase | Profitable, private | Proves small teams can build premium, profitable dev tool businesses |
| Laravel (Spark, Forge, Vapor) | Open-source framework + commercial tools | $25M+/year | Proves AGPL/open-source + commercial dual license works |
| GitLab | Self-managed DevSecOps platform | Public company | Shows enterprise willingness to pay for on-prem developer infrastructure |
| HashiCorp | Infrastructure automation, open-core enterprise | Acquired for $6.4B | Shows policy/workflow infrastructure can become strategic enterprise software |
| Snyk | Developer security platform | Private, venture-backed | Shows security review can be sold inside developer workflows |
| Chainguard | Software supply-chain security | Private, venture-backed | Comparable trust/SBOM/provenance buyer |
| Tailscale | Zero-trust networking with strong developer adoption | Private, venture-backed | Shows security infrastructure can win through simplicity |

Hull's cost structure is leaner than the enterprise comparables and closer to
the indie tools for self-serve distribution. No servers are required for
customers to run what they build. Enterprise services are optional, not the
runtime dependency.

## The risks (honest)

**New category.** The vibecoder-to-product market doesn't exist yet. It might not materialize as expected. Mitigation: Hull's cost structure means profitability at small scale. We don't need the market to be large. We need it to exist.

**Lua adoption.** Lua is less popular than Python, JavaScript, or Rust. Developers may resist learning a new language. Mitigation: vibecoders don't learn languages. They describe what they want and the AI writes it. Lua's simplicity makes LLM generation more reliable, not less. The developer never needs to read Lua if they don't want to.

**Single-threaded scripting + bounded worker pool.** Lua/JS handlers run one at a time on the event loop; async DB and HTTP work runs on a configurable thread pool. SQLite serializes writes. This is excellent for local tools and small-team servers (Hull benchmarks 70-100K req/s for I/O-bound routes on a single core, ~19K req/s for SQLite writes). It's deliberately not designed for thousands of concurrent users on one box. Mitigation: this is the design (Hull is for local tools and edge/embedded workloads. Optional WASM compute (via WAMR, AOT-compiled) and GPU compute (wgpu-native) handle CPU-bound and parallel-data workloads at near-native speed when needed. For multi-tenant scaling, SQLite-per-tenant sharding is on the post-v0.1.0 roadmap) the capability layer is the natural routing point.

**Sandbox gaps.** Kernel-enforced sandboxing (pledge/unveil) only works on Linux and OpenBSD. macOS and Windows get libc-level pledge/unveil emulation via Cosmopolitan libc, plus application-level safety via the Lua sandbox. The C attack surface is minimal (~1,500 lines of binding code). Mitigation: Windows App Container and macOS App Sandbox are on the roadmap.

**Platform risk.** Hull depends on Cosmopolitan C for cross-platform binaries. If Cosmopolitan development stalls, Hull's "runs anywhere" promise weakens. Mitigation: Cosmopolitan is open source and actively maintained by Justine Tunney. Hull could fall back to per-platform builds. Less elegant, still functional.

**Competitive response.** A well-funded competitor (or a big tech company) could build something similar. Mitigation: first-mover ecosystem advantage. The combination of Cosmopolitan APE + Lua + SQLite + kernel sandbox + Ed25519 licensing + AGPL dual license is specific enough that a generic "local-first framework" from a big company would lack the coherence. And big companies don't ship AGPL.

**Mission-critical positioning risk.** Hull is suitable for semantic and
control-adjacent layers. Mission planning, policy, command validation,
telemetry, local UI, config/update, audit, and bounded local inference. It is
not a hard realtime flight-control or actuator runtime today. Mitigation:
position correctly, integrate through narrow protocol boundaries with certified
controllers/autopilots, and build embedded profiles, watchdogs, simulation/HIL
tests, and certification artifacts over time.

## Survivability

**Dead man's switch.** If Hull publishes no release for 24 consecutive months,
the Hull-owned source code. C runtime, build tool, and standard library.
automatically converts from AGPL-3.0 to MIT. This is a legally binding clause in
the license. Third-party vendored dependencies remain under their own licenses.
It means:

- The community can fork under MIT if the project is abandoned
- Commercial license holders keep their rights regardless
- The trigger is objective and verifiable (GitHub release tags)

This protects investors and customers. It also signals confidence. We wouldn't include a dead man's switch if we planned to abandon the project.

**Zero runtime dependency.** Every Hull application ever built continues to work forever, regardless of what happens to the company. The binaries are standalone. No phone-home. No activation server. No infrastructure to maintain. If Hull disappears tomorrow, the products built on it keep running.

## The team

Artalis Consulting Kft. Budapest, Hungary.

Small team, low burn, high leverage. The platform's simplicity is intentional. A vendored, in-tree dependency set with no package-manager graphs means a 2-3 person team can build and maintain the entire platform. This is not a company that needs 50 engineers. It's a company that needs 3 good ones.

## What we're looking for

**Strategic capital, not just money.** Hull benefits most from investors who:

- Have reach into the vibecoder/AI developer community
- Understand developer tooling go-to-market (bottom-up adoption, community-driven growth)
- Can open doors in high-value verticals (defense, medical, financial)
- Have patience for category creation. This is a 3-5 year build, not a 12-month flip

**Where Hull is today (as of 2026-05):** Hull v0.1.4 shipped on 2026-05-28 with signed binaries for Linux x86_64/aarch64, macOS arm64, and the universal Cosmopolitan APE, all downloadable from GitHub Releases. Shipped: dual runtime (Lua 5.4 + QuickJS), capability layer with manifest-declared fs/env/host/process gates, kernel sandbox (pledge/unveil on Linux/Cosmo/OpenBSD, Seatbelt on macOS), three-layer Ed25519 signature chain (release + per-platform + per-app, with the embedded gethull release key live as of v0.1.3), `hull build` with embedded TinyCC, WASM compute (interpreter + AOT + Memory64 + SIMD128), GPU compute (wgpu-native, Vulkan/Metal/DX12), the full stdlib (sessions, CSRF, RBAC, idempotency, outbox/inbox, FTS5, i18n, ETags, health, SMTP, WebSockets, SSE, background timers, audit logging), agent platform (~27 `hull agent` subcommands + MCP server), `hull update` with SHA-256 and release-signature enforcement, `hull tools install wamrc` for side-loading the AOT compiler, browser-side verifier at verify.gethull.dev, the gethull.dev landing site, and three completed security audits with all findings closed. The product exists, it ships, and it's downloadable today. What remains for v1.0 is the multi-user server hardening, PostgreSQL backend, local-agent workbench, and ecosystem growth.

**What the capital would fund:**

- v1.0 release engineering and post-release stabilization
- PostgreSQL backend (validates the DB-vtable abstraction)
- Worker-pool concurrency expansion for multi-user server use cases
- Local Agent Runtime: model provider layer (`bitnet.c`, `llama.cpp`, Ollama), `hull develop`, bounded tool policies, signed agent sessions
- Mission-critical/embedded features: policy profiles, capability diffs, reproducible build attestation, certification bundles, local diagnostics
- Optional `bitnet.c` developer-tool bundle for single-binary offline inference
- gethull.dev documentation expansion and example gallery
- Hosted platform services (Hull Build, Hull Verify, Hull Sync). Planned post-v1.0
- Initial community building and developer relations
- First enterprise pilot engagements (defense/medical verticals)

## Contact

Artalis Consulting Kft.
Email: info@artalis.io
GitHub: [github.com/artalis-io/hull](https://github.com/artalis-io/hull)
Web: [gethull.dev](https://gethull.dev)
