# H1 / S4 - whole-tree milestone-narration inventory (closing record)

Status: **S4 COMPLETE.** This is the authoritative whole-tree record after the
comment-archaeology sweep. It certifies **removable narration = 0** and
enumerates every intentional survivor by category and exact location. It is the
**precise exception set** for S5's milestone gate: that gate must match narration
patterns narrowly or whitelist these exact reviewed locations - it must NEVER
broadly allow all `Phase`/`Slice`/`checkpoint` references.

Reproducible census (first-party source + build files; excludes `docs/*.md`
archives, which legitimately narrate history):

```
grep -rnE 'Phase [0-9]|Phase [A-Z]\b|Slice [0-9]|Slice [A-Z]\b|checkpoint [0-9]' \
  src include stdlib Makefile mk/*.mk templates | grep -vE '\.md:'
```

## 0. Removable narration: ZERO

After the six C-source area commits + the stdlib/Makefile area commit, no
removable development-milestone narration remains. Verified by the exclusion
census (every hit resolves to a category below):

```
# same census, minus the three keep-categories -> EMPTY:
grep -rnE 'Phase [0-9]|Phase [A-Z]\b|Slice [0-9]|checkpoint [0-9]' \
    src include stdlib Makefile mk/*.mk templates \
 | grep -vE '\.md:' \
 | grep -vE 'serve\.c:|serve_cli\.c:|sandbox\.c:|sandbox\.h:' \
 | grep -viE 'audit|Vendor TU exclusions' \
 | grep -vE 'jobs\.(js|lua):(18|14):' \
 | grep -vE 'sbom\.c:.*Since Phase 4\.3' \
 | grep -viE 'Phase 6 (fail-fast|patch)'
# -> no output
```

## 1. Category B - intentional ARCHITECTURAL phase labels (39)

These name enduring runtime structure, not a development milestone. **Keep.**

| Location | What | Why it stays |
|----------|------|--------------|
| `src/hull/serve.c` (31) | The `Phase 1..11` boot-pipeline section headers + the mode-runner cross-references (`hull run` / `app.main`) | These label the actual sequential boot steps the code executes (parse args -> resolve entry -> VFS -> TLS -> logging -> Keel server -> thread pool -> app-context -> signature verify -> manifest/caps/sandbox -> event loop -> teardown). Removing them loses real structure. |
| `src/hull/sandbox.c` (5), `include/hull/sandbox.h` (2) | The documented **two-phase sandbox**: `Phase 1` (pre-load pledge, blocks exec/proc/fork) and `Phase 2` (full profile after manifest extraction) | This is the security architecture (see docs/security.md), not sequencing. |
| `src/hull/serve_cli.c` (1) | `Phase 1 sandbox: block exec/proc/fork` | The same two-phase sandbox, CLI path. |

The S5 gate should treat `serve.c` / `serve_cli.c` / `sandbox.c` / `sandbox.h`
as reviewed exceptions, OR match only the *development* narration shapes (e.g.
`Phase [A-Z]\b`, `Phase \d+\.\d`, `Phase 3d-`, `Slice \d`, `checkpoint \d`,
`Phase \d+ of`, `(?:in|since|lands|deferred|moves|relocates|adds) .*Phase`) which
these architectural labels do not match.

## 2. Category C - durable AUDIT / SECURITY provenance (13)

Each references a specific audit finding or dated hardening pass. The boundaries
direct that these be **preserved**.

| Location | Provenance |
|----------|-----------|
| `stdlib/js/hull/template.js:482` | Phase 6 audit L-5 |
| `stdlib/js/hull/web/middleware/idempotency.js:37` | Phase 6 audit M-1 (+ M-7 Phase 5) |
| `stdlib/js/hull/web/middleware/idempotency.js:361` | Phase 6 audit M-3 |
| `stdlib/js/hull/web/middleware/outbox.js:152` | Phase 6 audit L-4 |
| `stdlib/js/hull/web/middleware/csrf.js:199` | Phase 6 audit L-2 |
| `stdlib/cli/lua/hull/deploy.lua:144` | Phase 6 audit L-1 |
| `stdlib/lua/hull/web/middleware/idempotency.lua:56` | Phase 6 audit M-2 |
| `stdlib/lua/hull/web/middleware/idempotency.lua:369` | Phase 6 audit M-3 |
| `stdlib/lua/hull/web/middleware/health.lua:85` | Phase 6 audit M-1 (the original Phase 6 patch) |
| `stdlib/lua/hull/web/middleware/csrf.lua:229` | Phase 6 audit M-4 |
| `stdlib/lua/hull/web/middleware/cors.lua:36` | Phase 6 fail-fast (audit-hardening) |
| `Makefile:444` | Phase 1 audit (2026-06-19, Apple clang 17 arm64) - dated hardening pass |
| `Makefile:527` | Vendor TU exclusions (Phase 3, 2026-06-20) - dated hardening pass |

## 3. Category D - PUBLIC user-facing text (3)

Changing these would alter public-documentation semantics or CLI output. **Keep.**

| Location | What |
|----------|------|
| `stdlib/js/hull/jobs.js:18` | The `hull/jobs` module-doc changelog (`v1.1`/`v1.5`/`Phase 1` durable-workflows), public API documentation |
| `stdlib/lua/hull/jobs.lua:14` | The same module-doc changelog (Lua side) |
| `src/hull/commands/sbom.c:38` | A user-facing `hull sbom` help string mentioning `Since Phase 4.3` |

## 4. Also preserved (not milestone narration)

Durable provenance kept verbatim throughout the sweep: `issue #114` seam markers
and `docs/*_feature.md` / `docs/*_design.md` design-record references. These are
not counted above because they are not bare `Phase`/`Slice` tokens.

## 5. Guidance for the S5 milestone gate

- Match the **development-narration shapes**, not the bare words. A gate that
  greps `Phase|Slice|checkpoint` and allows everything is wrong (it would let new
  narration through by co-locating it with an existing label); a gate that
  forbids them entirely is wrong (it would fail on the Category B/C/D survivors).
- The precise exception set is sections 1-3 above (exact files/lines). Prefer an
  explicit reviewed allowlist of those locations over a broad pattern exemption.
- New code should carry present-tense architectural rationale, audit-finding IDs,
  or doc references - never "Phase N of <refactor>" sequencing.
