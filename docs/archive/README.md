# Hull — Documentation Archive

Historical material preserved for reproducibility. All findings + roadmap
items below are closed; this content is not the current state of record.

For the current state see [`../README.md`](../README.md).

## Historical audits (`audits/`)

| Audit | Date | Scope | Outcome |
|---|---|---|---|
| [`audits/audit_report_20260317.md`](audits/audit_report_20260317.md) | 2026-03-17 | Full codebase | 13 findings (2H, 5M, 6L) + 4 informational. All addressed. |
| [`audits/audit_wasm_buffer.md`](audits/audit_wasm_buffer.md) | early 2026 | New `HlWasmBuffer` code | 4 findings (1C, 1H, 1M, 1L). All fixed. |
| [`audits/c_audit_2026_05.md`](audits/c_audit_2026_05.md) | 2026-05-11 | C core, 108 files | 10 findings (2C, 3H, 3M, 2L). All fixed. |
| [`audits/hull_audit.md`](audits/hull_audit.md) | 2026-03 | Full codebase, 179 files | 14 findings (2H, 6M, 6L). Superseded by `c_audit_2026_05.md`. |
| [`audits/hull_runtime_audit.md`](audits/hull_runtime_audit.md) | 2026-03 | Runtime layer + caps | Per-category breakdown. Superseded. |
| [`audits/hull_redteam.md`](audits/hull_redteam.md) | 2026-03-03 | Red-team pass on `main.c` | 6 exploitable issues. All fixed. |
| [`audits/audit_2026_05_15.md`](audits/audit_2026_05_15.md) | 2026-05-15 | Phase 5 surface | 49 findings. All addressed. |
| [`audits/audit_2026_05_15_phase6.md`](audits/audit_2026_05_15_phase6.md) | 2026-05-15 | Phase 6 (`hull agent` extended + MCP wiring) | 21 findings. All addressed. |
| [`audits/audit_2026_05_15_phase6_reaudit.md`](audits/audit_2026_05_15_phase6_reaudit.md) | 2026-05-15 | Phase 6 fix verification | 3 follow-ups. All closed. |

## Historical assessments & reviews

| Document | Date | Scope | Why archived |
|---|---|---|---|
| [`ASSESSMENT.md`](ASSESSMENT.md) | 2026-05-16 | Platform self-assessment ("approaching v0.1.0") | Snapshot before v0.1.0 shipped. Hull is at v0.2.0 now; current investor-facing state lives in [`../INVESTORS.md`](../INVESTORS.md) and the v0.2.0 release notes. |
| [`api_review.md`](api_review.md) | Pre-v0.1.0 | Public-surface review, companion to `stability.md` | One-shot pre-release review; findings either landed or were intentionally rejected. The current API contract lives in [`../stability.md`](../stability.md). |

## Completed roadmaps (`roadmaps/`)

| Roadmap | Original scope | Status |
|---|---|---|
| [`roadmaps/architecture_roadmap.md`](roadmaps/architecture_roadmap.md) | 12 architectural improvements (items A–L: cohesion / coupling refactor) | **All shipped** — see commits `92742db..d5250a3`. |
| [`roadmaps/roadmap_db_vtable.md`](roadmaps/roadmap_db_vtable.md) | Decouple query engine from SQLite via `HlDbBackend` vtable | **Shipped** — enables `HL_ENABLE_DB=0` compute-only builds. |
| [`roadmaps/wasm_improvement_roadmap.md`](roadmaps/wasm_improvement_roadmap.md) | 10 WASM hardening items | **All ✅** — shared-data MappedBuffer, pool mutex per module, etc. |
| [`roadmaps/roadmap_v0_to_v1.md`](roadmaps/roadmap_v0_to_v1.md) | Pre-v0.1.0 release-readiness checklist | Mostly shipped. Remaining items folded into [`../roadmap.md`](../roadmap.md). |

The **current** roadmap is [`../roadmap.md`](../roadmap.md) + [`../roadmap_next.md`](../roadmap_next.md). WASM/GPU compute work is merged into the main roadmap (the former `roadmap_wasm_compute.md` was retired in May 2026 along with `plan_memory64.md` and `keel_audit.md`).
