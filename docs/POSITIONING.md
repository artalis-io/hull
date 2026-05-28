# Hull. Positioning & Messaging Reference

This is the operational style guide for anyone writing about Hull. Landing-page copy, READMEs, release notes, conference talks, blog posts, RFPs, AI agents echoing the project back to users. It encodes the canonical thesis, vocabulary, tone, and audience priorities so every surface a visitor encounters carries the same load-bearing message.

The long-form philosophical *why* lives in [MANIFESTO.md](MANIFESTO.md). This file is the *how to write about it* counterpart.

Read this before editing:

- `README.md` (top of repo)
- `site/index.html` (gethull.dev landing)
- `site/verify.html` (signature verifier)
- `AGENTS.md` (agent dev guide opening)
- GitHub repo About
- Release notes (`gh release create` body)
- Any external slide deck, blog post, or talk

CLAUDE.md and reference docs under `docs/` are technical references and don't need to apply the positioning guide. They should be accurate, not on-message.

## Core thesis

One sentence. Never deviate.

> **Code became disposable. Trust is not.**

When one sentence isn't enough:

> AI generates code endlessly. Hull constrains what that code can actually do, at a kernel-enforced boundary the script cannot cross.

## Canonical descriptor

> **Hardened, capability-secure runtime infrastructure for AI-native systems.**

This is what Hull *is* in one line. Use it verbatim in `<title>` tags, GitHub About, JSON-LD, conference bios. Don't paraphrase.

## Target audiences

Listed in priority order. Frame copy for the highest-ranked audience that fits the surface; everyone else still understands but the load-bearing pitch goes to the top of the list.

1. **Infrastructure & security engineers.** Judge on: technical accuracy, defense depth, no hand-waving. Care about pledge/unveil, kernel enforcement, W^X, no runtime codegen, audit logging, threat model, attack surface.
2. **Agent / AI platform builders.** Care about: capability boundaries for LLM-callable tools, single-binary distribution of agent runtimes, MCP integration, deny-default semantics.
3. **Sovereign / regulated / air-gap buyers.** Care about: deploy-anywhere, no cloud dependency, no telemetry, signed releases, European origin as a risk-reduction factor, reproducible builds.
4. **Local-first / indie systems developers.** Care about: SQLite, single binary, low ops burden, AGPL, reproducibility, no package-manager dependency tree.

## Emotional tone

Calibrate every sentence to all four:

- **Calm, not urgent.** No "act now" energy. Infrastructure-grade.
- **Technically confident, not boastful.** Numbers beat adjectives.
- **Honest about scope.** "Hull is not X" sections are a feature.
- **Restrained vocabulary.** No "revolutionary", "next-gen", "AI-powered", "future of", "10x", "paradigm shift".

If a sentence could appear in a Y Combinator pitch deck, rewrite it. If a sentence could appear in an OpenBSD release announcement, ship it.

## Vocabulary

| Always say | Never say | Why |
|---|---|---|
| capability-secure | sandboxed (alone) | "Sandboxed" is generic; "capability-secure" names the model |
| AI-native systems | agent-native software | "AI-native" is broader and the new external standard |
| hardened runtime | secure runtime (vague) | "Hardened" carries weight; "secure" is unfalsifiable marketing |
| local-first | self-hosted (narrower) | Self-hosted implies a server; local-first includes single-user apps |
| single static binary | portable executable (vague) | "Static binary" is the technical claim |
| declared capabilities | granted permissions | "Declared" puts the manifest at the center |
| trust boundary | security perimeter | "Perimeter" implies network/topology; "boundary" is the right scope |
| constrains what code can do | restricts what users can do | The constraint is on the code, not the user |
| kernel-enforced boundary the script cannot cross | safe sandboxed APIs | Names the actual enforcement layer |
| sovereign infrastructure | private cloud / on-prem | "Sovereign" is the modern frame |
| AI generates code endlessly | AI writes code at scale | "Endlessly" carries the abundance claim |
| AI-generated code | AI-written code | "Generated" emphasizes the volume and disposability |

Architectural-detail sections in technical docs can still say "C boundary" or "C cap layer" where that's the precise implementation. These terms are correct in architecture diagrams and per-cap technical notes. The kernel framing is for marketing/positioning copy where the value claim matters more than the layer specificity.

## Canonical key phrases

Rotate these; don't repeat the same one twice on one page.

- "Code became disposable. Trust is not."
- "AI generates code endlessly. Hull constrains what it can actually do."
- "The runtime is the trust boundary."
- "Every external capability is declared."
- "Fail at load, not at first use."
- "No ambient authority."
- "W^X. No runtime codegen."
- "The binary is the product."

## The asymmetry to always restate

This is what differentiates Hull from "yet another sandbox". Restate it whenever someone asks "why now?":

> Sandboxing is decades old. What changed: code is no longer scarce. The trust layer is.

Variants:
- "Sandboxes are old. The reason to want one is new."
- "We didn't invent capability security. We made it the runtime AI-generated code lands in."

## Geographic positioning

Single line. Always restrained.

- **Site footer:** "Built in Europe for sovereign infrastructure."
- **README license area:** "Engineered in Europe for sovereign, deploy-anywhere AI infrastructure."
- **Conference bio / press:** "Engineered in Europe."

**Never:** flags, EU policy references, anti-US framing, GDPR boilerplate, nationalism, sovereignty language that implies political alignment beyond infrastructure independence.

## Typography & character rules

- **No em-dashes (`—`).** Replace with `. ` (period + capitalize next word) for between-clause breaks, parentheses for parentheticals, or colons for elaborations. See git history of commit `6b6f394` for the precedent. Applies to: README, site/, AGENTS.md, CLAUDE.md, release notes. UI placeholders that need a visual "no value" marker use middle dot `·` instead.
- **Hyphens are fine.** `kernel-enforced`, `capability-secure`, `single-binary`, etc.
- **En-dashes (`–`) are acceptable for numeric ranges** (`Phase 1–5`, `Lua 5.4`) but not for prose.
- **Title case for headings; sentence case for subheads.** Match existing site/README style.
- **Code in `monospace`** via backticks. Don't quote command names.

## Where this lives

| Surface | What changes when thesis evolves | Cadence |
|---|---|---|
| gethull.dev | Hero H1, subhead, OG card, meta tags, JSON-LD | Every positioning evolution |
| README.md | Tagline, Why, Status section | Every minor release if positioning evolves |
| AGENTS.md | Opening line only (descriptor) | When canonical descriptor changes |
| CLAUDE.md | Nothing. Instructions only | Stable |
| MANIFESTO.md | Title + opening callout | Only on major repositioning |
| GitHub repo About | One line mirror of canonical descriptor | When positioning evolves |
| GitHub release notes | Lead each release with what it advances about the thesis | Per release |
| Conference talks / decks | Open with thesis, close with the asymmetry | Per talk |
| External press / blog posts | Use canonical descriptor verbatim; lift one canonical key phrase | Per piece |
| `docs/POSITIONING.md` (this file) | Updated first; everything else cascades from here | When positioning evolves |

## Authoring checklist

Before publishing copy that mentions Hull externally:

- [ ] Lead with the canonical thesis or the canonical descriptor (don't paraphrase the descriptor)
- [ ] Use vocabulary from the "Always say" column
- [ ] No words from the "Never say" column
- [ ] No em-dashes
- [ ] No AI-startup phrases (revolutionary, 10x, etc.)
- [ ] If claiming a number, link to the source or use language like "measured"
- [ ] If listing differentiators, name the asymmetry (AI made code abundant, trust is scarce)
- [ ] Geographic line (if used) matches one of the three approved variants
- [ ] Honest about scope (if there's a tradeoff or limitation, name it)

## Updating this document

When positioning evolves:

1. Update this file first.
2. Cascade to the surfaces in the table above, in the order listed.
3. Bump the title/H1 last, after the supporting copy is consistent.
4. Commit the cascade together (one PR/commit), not in pieces. The diff explains itself.

Do not update individual surfaces without updating this file. If you're tempted to write something here that isn't already in your cascade, do the cascade first.
