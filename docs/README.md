# Hull — Documentation Index

This directory holds Hull's reference documentation. Top-level docs live at
the repo root: [`README.md`](../README.md), [`CLAUDE.md`](../CLAUDE.md),
[`AGENTS.md`](../AGENTS.md).

## Start here

| If you're… | Read this |
|---|---|
| A new developer writing a Hull app | [`../README.md`](../README.md) → [`agent_guide.md`](agent_guide.md) §1–§5 |
| Looking up a specific function | **[`api/`](api/) — per-function reference, Javadoc-style.** `api/c.md` / `api/lua.md` / `api/js.md`. |
| An AI agent introspecting a Hull project | [`../AGENTS.md`](../AGENTS.md) + [`agent_guide.md`](agent_guide.md) §17 (Agent workflow) |
| A contributor to the Hull core | [`../CLAUDE.md`](../CLAUDE.md) → [`architecture.md`](architecture.md) → [`api/c.md`](api/c.md) |
| Auditing Hull's security posture | [`security.md`](security.md) → [`audit_2026_05_15.md`](audit_2026_05_15.md) → [`audit_2026_05_15_phase6.md`](audit_2026_05_15_phase6.md) → [`audit_2026_05_15_phase6_reaudit.md`](audit_2026_05_15_phase6_reaudit.md) |
| Packaging a Hull release | [`release_signing.md`](release_signing.md) |
| Deciding whether to depend on a Hull API | [`stability.md`](stability.md) + [`api_review.md`](api_review.md) + [`api/`](api/) |

## API reference

Per-function Javadoc-style reference. Use these when you need to look up a
specific signature, parameter list, or return value.

| Doc | Surface |
|---|---|
| [`api/c.md`](api/c.md) | Hull's public C headers (`include/hull/*.h`). ~250 functions. For embedders, contributors, runtime authors. |
| [`api/lua.md`](api/lua.md) | Lua 5.4 stdlib (`stdlib/lua/hull/*.lua`). ~200 functions. For app developers. |
| [`api/js.md`](api/js.md) | JS stdlib (`stdlib/js/hull/*.js`). ~200 functions. Same surface as Lua, camelCase. |
| [`api/README.md`](api/README.md) | Format conventions, naming, how the API doc relates to the prose docs. |

## Core reference

| Doc | What it covers |
|---|---|
| [`agent_guide.md`](agent_guide.md) | **Full SDLC reference.** Install → dev → test → build → sign → deploy → release. Every CLI command, every module's API, security model, performance, common patterns/anti-patterns. ~1700 lines, deeply hyperlinked. The deep dive for both humans and agents. |
| [`architecture.md`](architecture.md) | System layers, dual-runtime polymorphism, request flow, capability layer symbol map, VFS, orchestration vs compute split. |
| [`security.md`](security.md) | Threat model, three-layer signature system, sandbox phases (pledge/unveil/seatbelt), capability enforcement invariants, audit logging. |
| [`stability.md`](stability.md) | API stability tiers, semver mapping, surface conventions. |
| [`known_limitations.md`](known_limitations.md) | Compile-time limit constants, when each applies, how to override. |
| [`benchmark.md`](benchmark.md) | Performance methodology, measured numbers for HTTP / DB / WASM AOT / GPU. |

## Subsystem deep-dives

| Doc | What it covers |
|---|---|
| [`wamr_architecture.md`](wamr_architecture.md) | WASM compute design — WAMR integration, ABI, gas metering, pooling, segments, streaming, AOT, Memory64. |
| [`release_signing.md`](release_signing.md) | Three-layer signature system + the release-key flow (`HL_RELEASE_PUBKEY_HEX` embedding, GitHub Actions sign step, `hull update` verification). |
| [`api_review.md`](api_review.md) | Pre-v0.1.0 public-surface review findings + pending decisions. |

## Audits (current)

The three docs below are the current security posture (commit `fb730f0`).
Findings are closed; the reports stay as evidence + reproducibility records.

| Doc | What it covers |
|---|---|
| [`audit_2026_05_15.md`](audit_2026_05_15.md) | Main audit, Phase 5 surface. 49 findings, all resolved. C / JS / Lua. |
| [`audit_2026_05_15_phase6.md`](audit_2026_05_15_phase6.md) | Phase 6 (extended `hull agent` + MCP wiring) audit. 21 findings, all resolved. |
| [`audit_2026_05_15_phase6_reaudit.md`](audit_2026_05_15_phase6_reaudit.md) | Re-audit of the Phase 6 fixes. 3 follow-up issues found, 2 fixed, 1 informational. |

## Roadmap

| Doc | What it covers |
|---|---|
| [`roadmap.md`](roadmap.md) | What's built + what's next. Living document. WASM/GPU compute work merged in. |
| [`roadmap_next.md`](roadmap_next.md) | Targeted next-feature priorities. |

## Strategic / positioning

These are written for non-developer audiences (investors, contributors
evaluating fit, design discussion).

| Doc | Audience |
|---|---|
| [`ASSESSMENT.md`](ASSESSMENT.md) | Platform assessment, scaling path, strategic positioning. |
| [`INVESTORS.md`](INVESTORS.md) | Investor pitch material. |
| [`MANIFESTO.md`](MANIFESTO.md) | Long-form design philosophy. |
| [`PERSONAS.md`](PERSONAS.md) | Target-user personas. |

## Archive

Historical material preserved for reproducibility but no longer the
current state of record:

- [`archive/audits/`](archive/) — six pre-Phase-5 audit reports (all
  findings closed). Useful for tracing the project's security history.
- [`archive/roadmaps/`](archive/) — four fully-completed roadmaps
  (architecture A–L, db-vtable, wasm-improvement, v0-to-v1 prep).

See [`archive/README.md`](archive/README.md) for the inventory.
