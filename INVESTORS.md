# Hull — Investor Brief

## One sentence

Hull turns AI-generated code into self-contained, distributable, local-first products — no cloud, no hosting, no dependencies.

## The problem

AI coding assistants (Claude Code, Cursor, Copilot, Codex) solved code generation. Millions of people can now describe software and have it written. But the output is always the same: a React frontend, a Node.js backend, a Postgres database, and a cloud deployment problem.

The vibecoder — the person who builds software by describing it to an AI — swapped one dependency (coding skill) for another (cloud infrastructure). They don't own anything more than before. They just rent different things.

There is no tool that takes AI-generated code and produces a product the creator owns. A file they can distribute, sell, or put in Dropbox. The AI-to-product pipeline has a missing piece: distribution.

Hull is that piece.

## The product

Hull is a local-first application platform. The developer writes backend logic in Lua, frontend in HTML5/JS, data in SQLite. `hull build` compiles everything into a single portable executable — under 2 MB — that runs on Linux, macOS, Windows, FreeBSD, OpenBSD, and NetBSD.

Six vendored C libraries. Zero external dependencies. No package manager. No runtime installation. No cloud. The user double-clicks a file and has a working application. Their data never leaves their machine.

Hull ships batteries included: routing, authentication, RBAC, email, CSV import/export, internationalization, full-text search, PDF generation, HTML templates, input validation, rate limiting, WebSockets, sessions, CSRF protection. A vibecoder describes an invoicing app to an AI, the AI writes Lua, `hull build` produces a file. That file is the product.

## Why now

Three forces are converging:

**1. AI coding is mainstream.** Claude Code, Cursor, and Copilot have millions of active users. The number of people who can describe software and have it built is growing exponentially. Every one of them hits the same wall: deployment. This wall didn't exist two years ago because these people weren't building software two years ago.

**2. Local-first is being driven by regulation.** GDPR, CCPA, the EU Digital Markets Act, and data sovereignty laws are pushing data back to the edge. Organizations want software that keeps data local, not software that sends it to a server they don't control. This is not ideology — it's compliance.

**3. Supply chain security is a board-level concern.** SolarWinds, Log4j, xz-utils. "How many dependencies does this have?" is a question executives ask now. Hull's answer — six, all vendored, all auditable in a day — is the strongest possible position.

The timing window is narrow. Whoever builds the vibecoder-to-product pipeline first accumulates the trust, community, and ecosystem that defines the category. This is a land grab.

## The economics

Hull's cost structure is unusually favorable for a software business.

**Revenue model:** Commercial licenses. One-time purchase, not subscription.

| Tier | Price | Target |
|------|-------|--------|
| Standard | $99 | Solo developers |
| Team | $299 | Small teams (up to 5) |
| Perpetual | $499 | Lifetime updates |

Update renewals ($49-149/year) provide optional recurring revenue.

**Why one-time works:** The license is a legal instrument (AGPL exemption for closed-source distribution), not a feature gate. AGPL and commercial builds are functionally identical. Customers pay to distribute without source — once. This removes churn from the business model entirely. No retention marketing. No cancellation anxiety. Revenue arrives at the point of sale.

**COGS: ~$0 per license.** A license is an Ed25519-signed key file delivered as a download. No infrastructure per customer. No activation server. No hosting obligation — customers run Hull on their own machines.

**Gross margin: ~95%.** Costs are salaries, CI, and CDN for downloads/docs. These are fixed. Adding the 10,000th customer costs the same as adding the 10th.

**Break-even is low.** A 2-3 person team with near-zero COGS needs hundreds of licenses — not thousands — to cover costs. At $99-499 per license, the math works early.

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

**Simplicity as structural advantage.** Six vendored C libraries. A 2-3 person team maintains the entire platform. Competitors building on larger stacks need larger teams for parity. Hull's simplicity is a cost advantage that compounds.

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

**Single-threaded architecture.** SQLite serializes writes. Hull handles 1-5 concurrent users well. It won't handle 1,000. Mitigation: this is the design — Hull is for local tools, not cloud services. The limitation matches the use case. WASM workers are on the roadmap for CPU-intensive computation.

**Sandbox gaps.** Kernel-enforced sandboxing (pledge/unveil) only works on Linux and OpenBSD. macOS and Windows rely on application-level safety. Mitigation: Windows App Container and macOS App Sandbox are on the roadmap. The Lua sandbox provides meaningful protection on all platforms.

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

Small team, low burn, high leverage. The platform's simplicity is intentional — six C libraries means a 2-3 person team can build and maintain the entire platform. This is not a company that needs 50 engineers. It's a company that needs 3 good ones.

## What we're looking for

**Strategic capital, not just money.** Hull benefits most from investors who:

- Have reach into the vibecoder/AI developer community
- Understand developer tooling go-to-market (bottom-up adoption, community-driven growth)
- Can open doors in high-value verticals (defense, medical, financial)
- Have patience for category creation — this is a 3-5 year build, not a 12-month flip

**What the capital would fund:**

- Hull v1.0 development (C runtime, Lua standard library, build tool)
- gethull.dev website and documentation
- verify.gethull.dev (web-based binary verification tool)
- Initial community building and developer relations
- First enterprise pilot engagements (defense/medical verticals)

## Contact

Artalis Consulting Kft.
Email: info@artalis.io
GitHub: [github.com/artalis-io/hull](https://github.com/artalis-io/hull)
Web: [gethull.dev](https://gethull.dev)
