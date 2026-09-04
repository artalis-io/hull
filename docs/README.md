# Hull. Documentation Index

This directory holds Hull's reference documentation. Top-level docs live at
the repo root: [`README.md`](../README.md), [`CLAUDE.md`](../CLAUDE.md),
[`AGENTS.md`](../AGENTS.md).

Docs are organized into three buckets:

1. **[Active architecture](#1-active-architecture)** - the maintained references
   that describe how Hull works *today*, plus in-progress designs.
2. **[Invariants & contracts](#2-invariants--contracts)** - the rules that must
   hold: API stability, limits, house style, fork procedure.
3. **[Historical archive & design records](#3-historical-archive--design-records)**
   - completed efforts: shipped-subsystem design records (kept in-tree because
   code comments link to them as rationale) and the physically-archived audits,
   roadmaps, and concluded spikes.

## Start here

| If you're… | Read this |
|---|---|
| A new developer writing a Hull app | [`../README.md`](../README.md) → [`agent_guide.md`](agent_guide.md) §1–§5 |
| Looking up a specific function | **[`api/`](api/). Per-function reference, Javadoc-style.** `api/c.md` / `api/lua.md` / `api/js.md`. |
| An AI agent introspecting a Hull project | [`../AGENTS.md`](../AGENTS.md) + [`agent_guide.md`](agent_guide.md) §17 (Agent workflow) |
| A contributor to the Hull core | [`../CLAUDE.md`](../CLAUDE.md) → [`architecture.md`](architecture.md) → [`api/c.md`](api/c.md) |
| Auditing Hull's security posture | [`security.md`](security.md) → [`archive/audits/`](archive/audits/) |
| Packaging a Hull release | [`release_signing.md`](release_signing.md) |
| Deciding whether to depend on a Hull API | [`stability.md`](stability.md) + [`archive/api_review.md`](archive/api_review.md) + [`api/`](api/) |

## API reference

Per-function Javadoc-style reference. Use these when you need to look up a
specific signature, parameter list, or return value.

| Doc | Surface |
|---|---|
| [`api/c.md`](api/c.md) | Hull's public C headers (`include/hull/*.h`). ~250 functions. For embedders, contributors, runtime authors. |
| [`api/lua.md`](api/lua.md) | Lua 5.4 stdlib (`stdlib/lua/hull/*.lua`). ~200 functions. For app developers. |
| [`api/js.md`](api/js.md) | JS stdlib (`stdlib/js/hull/*.js`). ~200 functions. Same surface as Lua, camelCase. |
| [`api/README.md`](api/README.md) | Format conventions, naming, how the API doc relates to the prose docs. |

---

## 1. Active architecture

### Core

| Doc | What it covers |
|---|---|
| [`agent_guide.md`](agent_guide.md) | **Full SDLC reference.** Install → dev → test → build → sign → deploy → release. Every CLI command, every module's API, security model, performance, patterns/anti-patterns. The deep dive for humans and agents. |
| [`architecture.md`](architecture.md) | System layers, dual-runtime polymorphism, request flow, capability layer symbol map, VFS, orchestration vs compute split. |
| [`security.md`](security.md) | Threat model, three-layer signature system, sandbox phases (pledge/unveil/seatbelt), capability enforcement invariants, sealed runtime tables, audit logging. |
| [`wamr_architecture.md`](wamr_architecture.md) | WASM compute design. WAMR integration, ABI, gas metering, pooling, segments, streaming, AOT, Memory64. |
| [`wamr_patches.md`](wamr_patches.md) | The vendored WAMR patch set (numbered, out-of-tree, against the pinned base commit). |
| [`backend_vtables.md`](backend_vtables.md) | The `HlAsyncBackend` / `HlNetBackend` vtables that make the event loop / networking composable. |
| [`benchmark.md`](benchmark.md) | Performance methodology + measured reference numbers (HTTP / DB / WASM AOT / GPU). |
| [`ci_architecture_design.md`](ci_architecture_design.md) | The change-aware CI architecture: fail-closed path classifier + applicability-aware required gate + focused per-subsystem jobs. **Slices 1-6 implemented**; the branch-protection cutover remains a separate policy decision. |

### Distribution, build & packaging

| Doc | What it covers |
|---|---|
| [`features_and_flavors.md`](features_and_flavors.md) | The distribution model: **features** (additive `--with=<name>`) vs **flavors** (subtractive base builds) vs **tools** (companion programs). The signed-archive trust chain, M+N vs M×N scaling. |
| [`build_flavors.md`](build_flavors.md) | The subtractive flavor set (`full` → `pure-compute`), `--flavor` / `--flavor=auto`, `hull flavor install`, the two signature layers. |
| [`libhull_flavor.md`](libhull_flavor.md) | The no-runtime `libhull.a` embedding flavor for native embedders. |
| [`tools_install.md`](tools_install.md) | Side-loaded companion tools (`hull tools install`), the version-coupled trust chain, single-binary vs bundle assets. |
| [`composed_feature_signing.md`](composed_feature_signing.md) | `package.sig.gethull.composed` - attesting every composed archive across the platform + release trust domains. |
| [`release_signing.md`](release_signing.md) | Three-layer signature system + the release-key flow (embedding, sign step, `hull update` verification). |
| [`release_acceptance.md`](release_acceptance.md) | The pre-promotion acceptance run: the read-only fail-closed verification workflow, the maintainer-prepopulated staging-repo model for testing `hull update`, and the Windows acceptance stage. |
| [`release_acceptance_v0.14.0.md`](release_acceptance_v0.14.0.md) | Durable v0.14.0 acceptance record: release tag/commit, published binary SHA-256s, environment assertions, and each smoke result (self-contained after the Actions artifacts expire). |
| [`windows_install.md`](windows_install.md) | User-facing Windows install guide: `install.ps1` quick start, options, verification, uninstall, and the one-time v0.13.0 -> v0.14.0 upgrade. |
| [`windows_install_design.md`](windows_install_design.md) | Windows-native install/upgrade/verify/remove UX design: the explicit trust model, the user-local install dir + PATH convention, the `install.ps1` CLI contract, Winget/Scoop shape, and the Authenticode-on-APE experiment plan. |
| [`sbom.md`](sbom.md) | The SBOM subsystem (human / JSON / CycloneDX / SPDX formats). |
| [`compiler_free_build.md`](compiler_free_build.md) | The object-emitter path: `hull build` needs only a linker (no C compiler). |
| [`toolchain_free_build.md`](toolchain_free_build.md) | Linker-as-tool + cross-compilation (`--linker=zig`), the musl static-link floors. |
| [`build_modularization.md`](build_modularization.md) | The `mk/` Makefile-fragment structure + the `build.lua` decomposition. |
| [`cosmocc_install.md`](cosmocc_install.md) | Self-contained Windows `hull build` (cosmo hull + the trimmed cosmocc bundle). |
| [`musl_build.md`](musl_build.md) | Building Hull on musl (Alpine) with zero source changes. |

### Live runtime subsystems

| Doc | What it covers |
|---|---|
| [`jobs.md`](jobs.md) | `hull/jobs@1` - the durable DB-backed job queue (workers, steps/timers/signals/saga, observability). |
| [`multipart.md`](multipart.md) | Streaming `multipart/form-data` uploads (`req:multipart()` / `req.multipart()`). |
| [`htmx.md`](htmx.md) | The HTMX hypermedia profile + CSP preset. |
| [`htmx_widgets.md`](htmx_widgets.md) | The shipped HTMX widget set in the stdlib. |
| [`attachments.md`](attachments.md) | The `hull/attachment@1` module. |
| [`blob.md`](blob.md) | The content-addressed blob store + per-app cache isolation. |
| [`cache.md`](cache.md) | The on-disk runtime cache pool (`hull cache`), registered kinds, eviction. |
| [`kv_cache.md`](kv_cache.md) | `hull/kv` (durable KV store) + `hull.cache` (byte cache) - backends, capability model. |
| [`cli_mode.md`](cli_mode.md) | `app.main` CLI apps + the `HL_ENABLE_HTTP_SERVER=0` flavor. |
| [`tui_mode.md`](tui_mode.md) | The `hull.tui` terminal-UI module (`tui.run`, cell-diff rendering). |

### In-progress designs (not yet fully shipped)

| Doc | Status |
|---|---|
| [`hull_fs_design.md`](hull_fs_design.md) | The application `hull.fs` surface. Resolver + compiled path-authorization policy + `stat`/`list` **shipped** (checkpoints 1-3); **BuildContext is checkpoint 4 (next)**. |
| [`hull_fs_resolver_parity.md`](hull_fs_resolver_parity.md) | Checkpoint 2 ratification record: `openat2` vs the manual walk parity + the ratified depth divergence. |
| [`hull_fs_buildcontext_audit.md`](hull_fs_buildcontext_audit.md) | The BuildContext / app-vs-plugin authority-split audit that feeds checkpoint 4. |
| [`buildcontext_design.md`](buildcontext_design.md) | **Checkpoint 4 implementation design:** action transactions, declared inputs, immutable artifact inputs, private outputs, constrained tools, and one plugin ABI for bundled/application plugins over Lua + JS source adapters. |
| [`h1_cleanup_inventory.md`](h1_cleanup_inventory.md) | Design-only inventory + freeze for the H1 code-housekeeping cleanup (dead code, redundancy, comment archaeology, em-dashes) that precedes BuildContext. |
| [`h1_s1_deadcode_audit.md`](h1_s1_deadcode_audit.md) | H1 slice S1: recorded build-system + fixture reference-coverage evidence and the `cap.h` disposition. Result: retain as-is, no deletion. |
| [`h1_s2b_hex_ownership.md`](h1_s2b_hex_ownership.md) | H1 slice S2b (design-only): exhaustive hex-caller inventory, link-closure/dependency table, and ownership recommendation for the byte->hex encoders. No code change. |
| [`h1_s3_comparison_contracts.md`](h1_s3_comparison_contracts.md) | H1 slice S3 (design-only): exhaustive comparison/constant-time contract matrix (14 security-relevant sites), threat context, equivalence classes, and per-class recommendation. Result: retain, no consolidation. |
| [`h1_s4_milestone_inventory.md`](h1_s4_milestone_inventory.md) | H1 slice S4 closing record: whole-tree milestone-narration inventory certifying removable narration = 0 and enumerating the intentional survivors (architectural labels / audit provenance / public text) as the precise exception set for S5's gate. |
| [`h1_s5_emdash_narration_gates.md`](h1_s5_emdash_narration_gates.md) | H1 slice S5 record: em-dash sweep across living first-party prose + the two self-tested gates (no-em-dash, no-milestone-narration) and their exact scope/survivor decisions. |
| [`reporting_ir_design.md`](reporting_ir_design.md) | Proposed reporting IR - **deferred** (over-scoped as the immediate next feature). |
| [`sidecar_design.md`](sidecar_design.md) | Native sidecars - design only (Phase 0), not yet scheduled. |
| [`smtp_keel_client_design.md`](smtp_keel_client_design.md) | Design-only transition of Hull SMTP from direct POSIX/blocking transport to an SMTP-owned composition of public Keel v3 primitives, preserving Hull policy and protocol ownership. |
| [`smtp_keel_slice2c_plan.md`](smtp_keel_slice2c_plan.md) | Plan-only architectural gate for SMTP Slice 2c: moving the transport onto a bounded worker (model 2), with frozen decisions for worker ownership, the linearizable op state machine, the post-resolution deadline, the SMTP pool cap, verified backend-shutdown ownership, TLS trust-material ownership, and the zero-Keel link seam. |
| [`pg_keel_transport_slice3.md`](pg_keel_transport_slice3.md) | Decisions record for routing the PostgreSQL client transport onto Keel v3 (Slice 3): `KlConnectOp` racing with raw `KlSocketProvider` blocking I/O (no `KlStream`), TLS kept on `hl_tls_client_*`, a TCP-only connect deadline, no async DNS (IP literals via `kl_sockaddr_parse`, hostnames via a blocking `getaddrinfo` adapter), whole-connection transport ownership, an adopt path preserving `hl_pg_conn_start`, and the zero-Keel-in-a-non-Postgres-app link seam. |
| [`mysql_keel_transport_slice4.md`](mysql_keel_transport_slice4.md) | Decisions record for routing the MySQL/MariaDB client transport onto Keel v3 (Slice 4): an independent `mysql_transport.{c,h}` copy of the proven PG transport (no shared abstraction yet, deferred to a later extraction), identical TLS ownership / TCP-only timeout, and the MySQL-specific interleaved-TLS boundary (no separate SSLRequest probe, so checkpoint 3 wires `attach_tls` and checkpoint 4 is the focused sslmode/fail-closed verification). |

### Roadmap

| Doc | What it covers |
|---|---|
| [`roadmap.md`](roadmap.md) | What's built + what's next. Living document. |
| [`roadmap_next.md`](roadmap_next.md) | Targeted next-feature priorities. |
| [`roadmaps.md`](roadmaps.md) | Meta-index of the forward-looking design roadmaps. |

### Strategic / positioning

Written for non-developer audiences (investors, contributors evaluating fit,
design discussion).

| Doc | Audience |
|---|---|
| [`INVESTORS.md`](INVESTORS.md) | Investor pitch material. |
| [`MANIFESTO.md`](MANIFESTO.md) | Long-form design philosophy. |
| [`PERSONAS.md`](PERSONAS.md) | Target-user personas. |
| [`POSITIONING.md`](POSITIONING.md) | Operational messaging guide across Hull surfaces. |

---

## 2. Invariants & contracts

Rules and contracts that must hold. Change these deliberately.

| Doc | What it constrains |
|---|---|
| [`stability.md`](stability.md) | API stability tiers (Tier 1–4), semver mapping, surface conventions. |
| [`known_limitations.md`](known_limitations.md) | Compile-time limit constants, when each applies, how to override. |
| [`stdlib_style.md`](stdlib_style.md) | House style for the dual-runtime stdlib: error handling, naming (snake_case / camelCase), Lua/JS parity rules. |
| [`fork_playbook.md`](fork_playbook.md) | The authoritative procedure for cutting forks / follow-up branches (fork from `main`, not a sibling PR branch). |
| [`benchmark.md`](benchmark.md) | The performance-measurement methodology the reference numbers must be reproduced with. |
| [`security.md`](security.md) | The capability-enforcement invariants (§ "Capability Enforcement Invariants") + the sealed-table rules are contracts, not just description. |

---

## 3. Historical archive & design records

### Design records (shipped efforts, kept in-tree)

These are the design-of-record for subsystems that **already shipped**. They are
historical - the *current* behavior is the code plus the canonical doc above -
but they remain in `docs/` (not `archive/`) because `CLAUDE.md`, `AGENTS.md`, and
in-source `// Full design: docs/X` comments link to them as rationale. Read them
for *why*, not *what-is-now*.

**Composable-feature & distribution build-out** (current state: [`features_and_flavors.md`](features_and_flavors.md))

| Doc | Effort |
|---|---|
| [`http_feature_phase1.md`](http_feature_phase1.md) | HTTP as a composable feature (the base seam + per-runtime web-binding split). |
| [`sqlite_feature.md`](sqlite_feature.md) | SQLite as a composable feature. |
| [`tls_feature.md`](tls_feature.md) | TLS / mbedTLS as a composable feature. |
| [`keel_feature.md`](keel_feature.md) | The Keel event loop as a composable feature (flavors → presets). |
| [`image_feature.md`](image_feature.md) | The image codec subsystem as an auto-composed feature. |
| [`wasm_feature.md`](wasm_feature.md) | WASM/WAMR as a composable feature (the `needs_wasm` gate). |
| [`runtime_feature_phase1.md`](runtime_feature_phase1.md) · [`runtime_feature_phase2.md`](runtime_feature_phase2.md) · [`runtime_feature_phase3.md`](runtime_feature_phase3.md) | Making the Lua/JS runtimes composable feature archives. |
| [`runtime_feature_publishing.md`](runtime_feature_publishing.md) | Wiring the runtime archives into the release + feature-install pipeline. |

**Database backends** (current state: `CLAUDE.md` DB section + [`features_and_flavors.md`](features_and_flavors.md))

| Doc | Effort |
|---|---|
| [`postgres_backend_design.md`](postgres_backend_design.md) | The pure-C PostgreSQL wire backend. |
| [`duckdb_backend_design.md`](duckdb_backend_design.md) | The embedded DuckDB OLAP backend (first composable feature). |
| [`db_api_review.md`](db_api_review.md) | The multi-backend DB API review. |

**Compute (WASM / mapped spans / Memory64)** (current state: [`wamr_architecture.md`](wamr_architecture.md))

| Doc | Effort |
|---|---|
| [`wasm_mapped_spans_design.md`](wasm_mapped_spans_design.md) · [`wasm_mapped_spans_checkpoint3.md`](wasm_mapped_spans_checkpoint3.md) | Zero-copy host-backed mapped spans (design + the shipped cut). |
| [`mapped_span_benchmark_design.md`](mapped_span_benchmark_design.md) | The four-workload × four-impl mapped-span benchmark. |
| [`memory64_dispatch_design.md`](memory64_dispatch_design.md) · [`memory64_spans_design.md`](memory64_spans_design.md) · [`memory64_build_design.md`](memory64_build_design.md) | Memory64 cap-layer dispatch, spans, and `hull build`. |
| [`wamr_shared_heap_destroy_design.md`](wamr_shared_heap_destroy_design.md) · [`wamr_shared_heap_guarded_subrange_design.md`](wamr_shared_heap_guarded_subrange_design.md) | Shared-heap destruction + the guarded-subrange RO shared heap. |

**Durable jobs epic** (current state: [`jobs.md`](jobs.md))

| Doc | Effort |
|---|---|
| [`jobs_design.md`](jobs_design.md) | The core durable job-queue design. |
| [`jobs_durable_execution_design.md`](jobs_durable_execution_design.md) | Phase-memoized workflow orchestration (steps/timers/signals/saga). |
| [`jobs_observability_design.md`](jobs_observability_design.md) | Metrics gauges + trace propagation. |
| [`jobs_events_design.md`](jobs_events_design.md) · [`jobs_events_phase4_design.md`](jobs_events_phase4_design.md) | Fleet-wide durable job events + the LISTEN/NOTIFY low-latency wakeup. |

**Source analysis & agent tooling** (current state: the `hull.source.*` / `hull.project.*` code + `hull analyze` / `hull agent inspect`)

| Doc | Effort |
|---|---|
| [`lua_source_analysis_design.md`](lua_source_analysis_design.md) | The pure-Lua Lua-5.4 source-analysis layer (`hull.source.lua`). |
| [`hull_source_scope_design.md`](hull_source_scope_design.md) | The lexical scope/binding resolver. |
| [`hull_analyze_design.md`](hull_analyze_design.md) · [`hull_analyze_lint_design.md`](hull_analyze_lint_design.md) | `hull analyze` (syntax) + the lint engine. |
| [`project_discovery_design.md`](project_discovery_design.md) | The frontend-neutral `hull.project.*` discovery model. |
| [`lua_source_conformance_design.md`](lua_source_conformance_design.md) · [`lua_official_tests_design.md`](lua_official_tests_design.md) · [`lua_source_fuzz_design.md`](lua_source_fuzz_design.md) | Lua parser conformance corpus + official-tests + libFuzzer harness. |
| [`javascript_source_frontend_design.md`](javascript_source_frontend_design.md) · [`js_frontend_slice3_annotations.md`](js_frontend_slice3_annotations.md) · [`js_frontend_slice4_scope.md`](js_frontend_slice4_scope.md) · [`js_frontend_slice5_adapter.md`](js_frontend_slice5_adapter.md) · [`js_frontend_slice6_dispatcher.md`](js_frontend_slice6_dispatcher.md) · [`js_frontend_slice7_design.md`](js_frontend_slice7_design.md) | The JS source-analysis frontend (parse → annotations → scope → adapter → dispatcher → mixed-language discovery). |
| [`js_source_fuzz_design.md`](js_source_fuzz_design.md) · [`js_test262_design.md`](js_test262_design.md) | JS parser libFuzzer + Test262 conformance gate. |

**Build effort audits** (current state: [`compiler_free_build.md`](compiler_free_build.md) / [`build_modularization.md`](build_modularization.md))

| Doc | Effort |
|---|---|
| [`build_arc_audit.md`](build_arc_audit.md) | The build-arc correctness audit tracked during the compiler-free / modularization work. |

### Physically archived (`archive/`)

Material with **no** inbound references from `CLAUDE.md` / `AGENTS.md` / source -
relocated out of `docs/` proper. See [`archive/README.md`](archive/README.md) for
the full inventory.

| Group | What |
|---|---|
| [`archive/audits/`](archive/audits/) | Nine historical security audits (all findings closed) - the project's security history. |
| [`archive/roadmaps/`](archive/roadmaps/) | Four fully-completed roadmaps (architecture A–L, db-vtable, wasm-improvement, v0→v1). |
| [`archive/design_records/`](archive/design_records/) | Concluded spikes & negative results: `cachelib_spike`, `kvmem_design`, `kvmem_negative_result`, `memstore_lru_plan`, `jobs_wasm_replay_spike` (verdict: do-not-build / did-not-ship). |
| [`archive/ASSESSMENT.md`](archive/ASSESSMENT.md) | Pre-v0.1.0 platform self-assessment snapshot. |
| [`archive/api_review.md`](archive/api_review.md) | Pre-v0.1.0 public-surface review (findings landed or intentionally rejected; current contract → [`stability.md`](stability.md)). |
