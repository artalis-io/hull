# Reporting IR - portable document model with pluggable render backends

**Status:** PROPOSED / not scheduled. Captured design of record.
**Provenance:** grew out of the NEXOGEN (`nexogen-asset-tracker`) PDF gap
(`PLATFORM_GAPS.md` §8.3 - printable movement reports for wet signature) and the
follow-on discussion that reframed "a PDF feature" into "a portable report/
document IR whose PDF, SVG, HTML, and Markdown outputs are just render targets."
**Related tracking issue:** #242 (Typst rendering engine epic - superseded/
expanded by this doc).

> **Recommendation (do not skip):** this full IR is the right *eventual*
> architecture but is **over-scoped as the immediate next feature**. The real,
> dated need (NEXOGEN wet-signature PDFs) is satisfiable by a much smaller slice
> (a Typst renderer + one template + the jobs/blob pipeline). Build the IR
> **incrementally**, letting it emerge from 2-3 real document types, rather than
> designing the whole vocabulary top-down for hypothetical invoices/POs/
> contracts. Phase plan is at the end of this doc. **First de-risk the long
> pole:** the Typst->WASM spike (§ Backends / Typst).

---

## 1. Core principle: stdlib-heavy, minimal C

The **bulk** of the Reporting IR lives in Hull's standard-library layer
(`stdlib/lua/hull/reporting/*` + `stdlib/js/hull/reporting/*`), embedded like the
rest of the stdlib. Only the **smallest necessary native layer** is C:

- renderer **registration / discovery / capability query / invocation** (a
  vtable + registry, modelled on the existing `HlImageCodec` precedent - see §
  "Native renderer interface"),
- **streaming** output and **cancellation** glue (reuse `HlAsyncCtx`),
- the **Typst** integration (an optional composable feature).

Everything else - builders, typed values, validation, normalization, canonical
JSON, tree walking, references/resources, the Markdown and HTML renderers - is
pure Lua/JS with full Lua<->JS parity. This matches Hull's taxonomy: pure
orchestration over existing caps = stdlib; a large optional native subsystem
(Typst) = a composable `--with=` feature.

## 2. Architecture

```
App code (Lua / JS)
    |  builds an IR document via hull.reporting builders
    v
hull.reporting            (stdlib: build / value / validate / normalize / walk /
    |                      resource / reference / render-prep)  -- pure Lua/JS
    |  normalized, canonical IR (a plain table/object; canonical JSON on the wire)
    v
render backends
    |-- markdown           (stdlib, pure Lua/JS)      -> text/markdown
    |-- html               (stdlib, pure Lua/JS)      -> text/html (semantic; HTMX-embeddable)
    |-- typst              (composable feature)        -> application/pdf, image/svg+xml, image/png
    v
C renderer registry + vtable  (hl_report_* : register / discover / capabilities /
                               invoke / stream / cancel)  -- minimal native seam
```

- **IR is runtime-neutral data.** A document is a plain Lua table / JS object; on
  the C boundary and for hashing it is **canonical JSON** (deterministic key
  order, normalized numbers) produced via `hull.json` + the C `sh_json` writer.
- **Renderers are `(normalized_doc, options) -> bytes` (+ diagnostics).** Some
  are pure stdlib (markdown, html); some are native (typst). Both plug into the
  same registry uniformly.
- **Async + jobs are first-class** (see § "Async + hull.jobs").

## 3. The IR: what it owns vs. what backends own

The IR owns **semantics and structure**: what a thing *is* (a section, a table,
a monetary value, a signature block, a clause), not how it looks in a specific
output. Backends own **rendering**: fonts, page geometry, exact HTML/CSS, PDF
layout. Portable **presentation hints** (§9) let the IR *suggest* emphasis
without dictating pixels.

## 4. Versioned envelope

Every document is a versioned envelope. Required top-level fields:

```jsonc
{
  "kind": "hull.report",          // discriminator (required)
  "ir_version": "1",              // IR schema major (required)
  "body": { "type": "document", ...}  // the root node (required)
  // optional:
  "meta":      { "title": ..., "authors": [...], "created": <date>, "locale": "en-GB", ... },
  "document_type": "invoice",     // business-doc discriminator (§8)
  "resources": [ ... ],           // §6 assets/datasets/bibliography/attachments
  "extensions": { ... }           // §8.x namespaced extension payloads
}
```

Unknown top-level keys are rejected in **strict** mode, preserved-and-ignored in
**permissive** mode (§10).

## 5. Node vocabulary

### 5.1 Block nodes (`body` is a `document`; nodes nest via `children`)

`document`, `section` (heading + level + children), `paragraph`, `list`
(ordered/unordered/definition), `list_item`, `table` (§7), `figure` (caption +
resource ref or chart), `chart` (§7.3 portable chart grammar), `metric`
(label + typed value + optional delta/trend), `code` (language + text, never
executed), `quote`, `callout` (severity: info/warning/danger/success),
`clause` (numbered legal/contract clause, §8), `signature_block` (§8),
`page_break`, `custom` (§8.x extension node with a fallback), `native`
(§8.x backend-specific escape hatch with a fallback).

### 5.2 Inline nodes (inside `paragraph`/`table` cells/captions)

`text` (plain, **always escaped** by every renderer), `value` (a typed value,
§6, rendered per locale), `span` (styled run: emphasis vocab below),
`link` (href + inline children), `reference` (cross-ref to a labelled node,
§8.x), `citation` (bibliography ref, §6), `inline_code`, `math` (TeX-subset
string; renderers that can't typeset fall back to code), `line_break`.

**Emphasis vocab (portable, not CSS):** `strong`, `emphasis`, `underline`,
`strike`, `subscript`, `superscript`, `small_caps`, `code`. Backends map these
to their own styling.

## 6. Typed values

Values carry **type + canonical data**, so every backend formats them
consistently per locale (currency symbol, decimal/grouping, date format):

`string`, `integer`, `decimal` (arbitrary precision as a string; never a float),
`money` (`{ amount: "<decimal-string>", currency: "GBP" }`), `quantity`
(`{ value, unit }`), `percentage`, `boolean`, `date`, `datetime`, `duration`,
`bytes`, `enum` (`{ value, labels? }`), `null`.

- **Decimals/money are strings**, never IEEE floats - no rounding drift in a
  financial document. Formatting is a render concern (locale from `meta.locale`
  or an override).
- A bare Lua/JS number in a builder is coerced to `integer` or `decimal`
  deterministically (integral -> integer; else decimal-from-string).

## 7. Structured blocks

### 7.1 Tables

`{ type: "table", columns: [ { key, header (inline), align?, width? (hint) } ],
   rows: [ { cells: { <key>: <inline|value> } } ], caption?, summary? (a11y),
   header_rows?, footer_rows? }`. Cells are inline content or typed values.

### 7.2 Metrics

`{ type: "metric", label, value: <typed value>, delta?: <typed value>,
   trend?: "up"|"down"|"flat", importance? }` - a KPI tile.

### 7.3 Portable chart grammar

A **backend-neutral** chart spec (not a Vega/ECharts passthrough):
`{ type: "chart", chart: "bar"|"line"|"area"|"pie"|"scatter",
   series: [ { name, points: [ { x: <value>, y: <value> } ] } ],
   axes?: { x: {label,type}, y: {label,type} }, legend?, stacked? }`.
Backends that can't draw charts render a **data table fallback** from `series`.

## 8. Business-document support

### 8.1 `document_type` + typed fields

`document_type` (`invoice`, `quotation`, `purchase_order`, `contract`,
`report`, `delivery_note`, `statement`, ...) selects a **field schema** validated
in strict mode (e.g. an `invoice` expects `parties`, `line_items`, `totals`).
Fields live under `meta.fields` as typed values.

### 8.2 Parties + roles

`parties: [ { id, name, role: "seller"|"buyer"|"carrier"|"signatory"|...,
   address?, tax_id?, contact? } ]`. Referenced by id from clauses/signatures.

### 8.3 Clauses + signature blocks

- `clause` node: `{ number, title?, children, binding? }` - numbered, nestable.
- `signature_block`: `{ party: <party-id>, method: "wet"|"electronic",
   label, signed?: { name, date, evidence? } }`. The NEXOGEN wet-signature
   requirement renders as an unsigned `signature_block` with a ruled line.

### 8.4 Workflow + integrity

- `meta.workflow`: `{ state, history: [ { state, at, by } ] }` - provenance of an
  approval/routing flow (composes naturally with `hull.jobs` state).
- `meta.integrity`: `{ hash: "sha256:...", algo, canonical: true }` - a content
  hash over the **canonical JSON** (§11), so a rendered doc is tamper-evident and
  reproducible.

### 8.x Namespaced extensions with fallbacks

- `custom` node: `{ type: "custom", ns: "acme.badge", data: {...},
   fallback: <node> }` - an unknown-ns renderer draws the `fallback` (a plain
   node) instead of failing. Extensions **never** break a portable render.
- `native` node: `{ type: "native", backend: "typst", source: "<typst-markup>",
   fallback: <node> }` - a backend-specific escape hatch. In **normal** mode raw
   backend source is **not executed** unless the doc is explicitly trusted
   (`render` option `allow_native = true`); otherwise the `fallback` renders.
- `reference` / labels: any node may carry `label`; a `reference` inline resolves
  to it (figure/table/section numbering is a render concern).

## 9. Portable presentation hints

Optional, advisory, never pixels: `importance` (`low`|`normal`|`high`),
`width` (`auto`|`full`|`narrow`), `density` (`comfortable`|`compact`),
`break_before`/`break_after` (page/section), `align`, `hidden` (per-medium:
`{ hidden: ["screen"] }` to print-only, which is exactly the NEXOGEN
print-vs-screen split). Backends honor what they can, ignore the rest.

## 10. Validation (strict + permissive)

`reporting.validate(doc, { mode = "strict"|"permissive" })` returns
`ok, errors[]` where each error is structured: `{ path: "body.children[3].
cells.total", code: "type_mismatch", message, expected?, got? }` (a JSON-pointer-
ish path, never a bare string). Strict rejects unknown keys / unknown node types
/ missing required business-doc fields; permissive preserves-and-warns. Builders
validate incrementally; `render` validates once up front and refuses an invalid
doc with the structured errors.

## 11. Normalization + canonical JSON

`reporting.normalize(doc)` produces a **deterministic** canonical form: sorted
object keys, normalized number/decimal representation, defaulted optional fields,
resolved short-hands (a bare string inline -> a `text` node; a bare number ->
typed value). `reporting.canonical_json(doc)` serializes the normalized doc with
stable key order (via `sh_json`) - the input to `meta.integrity.hash` and the
**Lua==JS conformance** oracle (§ Testing).

## 12. Stdlib API (Lua; JS is the camelCase mirror)

```lua
local report = require("hull.reporting")

-- Builders (fluent; each returns a node table)
local doc = report.document({ title = "Movement Report", locale = "en-GB" })
  :section("Summary", 1)
    :paragraph(report.text("Assets moved on "), report.value(report.date("2026-08-05")))
    :metric("Total moved", report.quantity(42, "units"))
  :table({
      columns = { {key="asset", header="Asset"}, {key="qty", header="Qty", align="right"} },
      rows    = { { cells = { asset = "Forklift-7", qty = report.integer(3) } } },
  })
  :signature_block({ party = "carrier", method = "wet", label = "Received by" })

-- Values
report.money("1234.50", "GBP"); report.decimal("3.14159"); report.percentage("12.5")

-- Validation / normalization / hashing
local ok, errs = report.validate(doc, { mode = "strict" })
local norm     = report.normalize(doc)
local cj       = report.canonical_json(doc)

-- Discovery
report.renderers()                 -- { "markdown", "html", "typst"? }  (typst only if the feature is present)
report.renderer_capabilities("typst")  -- { outputs = {"pdf","svg","png"}, streaming = true, ... }

-- Render (sync) and async
local bytes, err = report.render(doc, { to = "markdown" })
local bytes, err = report.render(doc, { to = "typst", output = "pdf", locale = "en-GB" })
report.render_async(doc, { to = "typst", output = "pdf" })   -- yields via HlAsyncCtx; returns bytes
```

- `render` options: `to` (renderer), `output` (media within the renderer:
  pdf/svg/png/html/markdown), `locale`, `allow_native` (default false),
  `mode` (validation), `stream` (a sink callback for large outputs),
  `signal` (cancellation).
- A missing renderer -> `renderer_unavailable` structured error (e.g. `typst`
  when the feature isn't composed), never a crash. `report.renderers()` omits
  absent ones, so apps can branch/fallback.

## 13. Native renderer interface (the minimal C)

Modelled **directly** on the existing `HlImageCodec` vtable +
`hl_image_register_codec()` precedent (`include/hull/cap/image.h`). New:
`include/hull/cap/report.h`.

```c
typedef struct HlReportRenderer {
    const char *name;                    // "markdown" | "html" | "typst"
    // capability query
    const char *const *outputs;          // NULL-terminated: "pdf","svg",...
    unsigned    supports_streaming : 1;
    unsigned    supports_cancel    : 1;
    // invoke: normalized canonical-JSON doc in; bytes out via the sink.
    // returns 0 ok, <0 with a structured diagnostic written to *diag.
    int (*render)(const HlReportRenderInput *in,   // { canonical_json, len, output, locale, options }
                  HlReportSink *sink,              // write(bytes,len) + a media_type setter
                  HlAsyncCtx *async,               // non-NULL for the async path (worker pool)
                  HlReportDiag *diag);             // structured error out-param
} HlReportRenderer;

int  hl_report_register_renderer(const HlReportRenderer *r);   // like hl_image_register_codec
int  hl_report_renderers(const char ***names_out);             // discovery
const HlReportRenderer *hl_report_find(const char *name);
```

- The struct is `static const` (rodata / vtable-hardening rule - a
  function-pointer table must be `const`; see the c-audit §5b conventions).
- **Runtime-neutral**: no Lua/JS pointers cross the seam - only canonical JSON in
  and a byte sink out. The Lua/JS bridges (`mod_reporting.c` per runtime) marshal
  the normalized doc to canonical JSON, call `hl_report_find(name)->render`, and
  surface diagnostics as structured tables.
- **Weak-hook composition** for the optional Typst backend: the base carries a
  weak `hl_report_renderer_typst` no-op; the composed `--with=typst` feature
  provides the strong override that registers the real renderer (mirrors the
  gpu/db feature hooks). Markdown + HTML are pure stdlib renderers, registered
  from Lua/JS (a stdlib renderer registers a Lua/JS `render` fn into the same
  registry via a thin bridge, so native and stdlib renderers are uniform).

## 14. Backends

### 14.1 Markdown (stdlib, pure Lua/JS)

IR -> GitHub-flavored Markdown. Escapes all text. Charts -> data-table fallback.
Zero native code. Golden-file tested.

### 14.2 HTML (stdlib, pure Lua/JS)

IR -> **semantic** HTML (`<section>`/`<table>`/`<figure>`/`<dl>`/...), escaped,
**no raw HTML from text nodes**, class hooks for app CSS. This is the
**HTMX-embeddable** output (the user's "Typst SVG within HTMX" is one option; the
HTML renderer is the other, and is pure-stdlib + needs no feature). Presentation
hints map to classes / `hidden` maps to a print/screen media split. Golden-file +
XSS-escaping tested.

### 14.3 Typst (composable feature, `--with=typst`)

IR -> **PDF / SVG / PNG** via Typst (Apache-2.0; permissive, AGPL-compatible).
A default Typst **template package** (`report.typ`) receives the canonical IR
JSON via `sys.inputs` and walks it to typeset. Fonts embedded as Hull
`compute.segment` shared data segments (portable, no system fonts).

**Hull integration decision (the key finding):** Hull's kernel sandbox
(pledge/seatbelt) **blocks runtime `exec`/`proc`** for app code -
`hl_tool_spawn` (fork/execvp, `cap/tool.c`) is **build-mode only**. So
Typst-**as-a-spawned-CLI conflicts with the sandbox** and is the *wrong* path
here, despite being the "obvious" one on a normal OS. Ranking for Hull:

1. **Typst-core -> WASM compute module** (runs in vendored WAMR: no exec, every
   platform incl. cosmo, **async-for-free** via the existing `compute.async`
   worker pool, canonical-IR JSON in + fonts-as-segments -> PDF/SVG bytes out).
   Smallest C of all - Typst becomes "just another compute module." **Chosen,
   pending a spike.** ⭐
2. Native Rust **static-lib** feature (in-process C ABI, no exec; but Rust in the
   build + no cosmo).
3. Process-spawn (needs a brand-new runtime-exec capability; breaks the
   self-contained/sandbox story). **Rejected.**

**Long-pole spike (do first):** does Typst-core compile to `wasm32-unknown-
unknown` and render a hello-report to PDF+SVG under WAMR? Go/no-go for the whole
Typst direction.

## 15. Render pipeline + async + hull.jobs

`render` is `(doc, opts) -> bytes | err`. Heavy renders (PDF) are async- and
jobs-native:

- **`render_async`** dispatches the backend to the worker pool and yields via
  `HlAsyncCtx` - the same path `compute.async` / `db.async` use. The WASM-Typst
  backend inherits this for free (it *is* a compute call).
- **Render-as-a-job** (the at-scale path, no new machinery): a `render` job ->
  output bytes to **`hull.blob`** (durable content-addressed store: `blob.put`
  -> id) -> handler returns `{ blob = id, media_type }` (stored in
  `_hull_job_results`) -> an optional `deliver` **workflow dependent**
  (`job.deps[1].blob`) emails/uploads it. Reuses jobs' retries/backoff,
  **heartbeat** (long renders), **`get(id)`** ("is the PDF ready?"),
  concurrency (K worker processes for CPU-bound Typst), and rate-limited
  delivery. The IR renderers stay pure `(doc -> bytes)`; jobs+blob is the async
  wrapper and it costs nothing new.

## 16. Errors + diagnostics

Structured everywhere (never bare strings): `{ code, message, path?, renderer?,
detail? }`. Codes: `validation_failed` (carries the §10 error list),
`renderer_unavailable`, `unsupported_output`, `render_failed`, `cancelled`,
`resource_missing`. The C seam's `HlReportDiag` marshals 1:1 to the Lua/JS
structured error.

## 17. Templates

A document can be built imperatively (§12 builders) **or** from a **data-driven
template**: `reporting.from_template(template_doc, data)` where `template_doc` is
an IR document containing `bind` placeholders (`{ type: "value", bind:
"invoice.total" }`) resolved against `data`. This keeps report *layout* as data
(shippable, embeddable) while the *content* comes from the app - the same
compile-once/render-many spirit as `hull.template`, but producing IR (hence any
output format), not HTML strings.

## 18. Optional-feature behavior

- Markdown + HTML are **always present** (pure stdlib).
- Typst is present only when `--with=typst` composed the feature. `hull/reporting`
  itself is a normal declared module (`"hull/reporting@1"`); a doc that requests
  `to = "typst"` on a base without the feature gets `renderer_unavailable` with a
  `hull feature install typst` hint. Mark a Typst dependency optional in app code
  by branching on `report.renderers()`.

## 19. Module layout

```
include/hull/cap/report.h                  # HlReportRenderer vtable + registry (minimal C)
src/hull/cap/report.c                       # registry + weak typst hook + sink/diag glue
src/hull/runtime/lua/mod_reporting.c        # Lua bridge (marshal canonical JSON <-> registry)
src/hull/runtime/js/mod_reporting.c         # JS bridge
stdlib/lua/hull/reporting/                  # builders, value, validate, normalize, walk,
  init.lua  build.lua  value.lua  validate.lua  normalize.lua  walk.lua
  render.lua  render_markdown.lua  render_html.lua  reference.lua  resource.lua
stdlib/js/hull/reporting/                   # camelCase mirror (same files, .js)
features/typst/                             # the --with=typst feature (WASM-Typst crate + report.typ)
docs/reporting.md                           # user guide
examples/reporting_invoice/                 # invoice -> PDF/HTML/Markdown
examples/reporting_asset_handover/          # NEXOGEN-style wet-signature movement report
```

## 20. Testing

- **Lua==JS conformance**: build the same doc in both runtimes, assert identical
  `canonical_json` (the oracle). Covers builders + normalize + values.
- **Golden files**: fixed IR docs -> fixed Markdown / HTML (byte-exact).
- **Security**: HTML/Markdown escaping of hostile text/`native`/`custom`;
  `allow_native=false` renders the fallback, never the raw source.
- **Validation**: strict rejects malformed docs with the right structured paths.
- **Typst** (feature): a PDF-magic + SVG smoke test; disabled-feature ->
  `renderer_unavailable`.
- **Jobs/blob**: render-as-a-job -> blob -> deliver dependent, under concurrency.

## 21. Scope discipline

**Do:** the IR vocabulary, validation, normalization, the two pure-stdlib
renderers, the minimal C registry/vtable, the Typst feature, jobs/blob
composition. **Don't:** a WYSIWYG editor, a CSS engine, arbitrary Vega/ECharts
passthrough, executing arbitrary Typst/JS/Lua/HTML in normal mode, a
general-purpose typesetting language. Keep raw-backend-source gated behind
`allow_native` + trust.

## 22. Acceptance criteria

1. Build an IR document in Lua and in JS; identical canonical JSON.
2. Typed values format per locale (money/date/percentage).
3. Strict + permissive validation with structured error paths.
4. Markdown renderer (pure stdlib) - golden.
5. HTML renderer (pure stdlib, semantic, escaped, HTMX-embeddable) - golden.
6. Tables / metrics / charts (with data-table fallback).
7. Business-doc fields (invoice/asset-handover), parties, clauses,
   `signature_block` (wet), `document_type` schema validation.
8. Namespaced `custom`/`native` extensions with rendered fallbacks; `native`
   source not executed unless `allow_native`.
9. `meta.integrity` content hash over canonical JSON; reproducible.
10. Presentation hints incl. per-medium `hidden` (print-vs-screen).
11. Renderer discovery + capability query; graceful `renderer_unavailable`.
12. `render` + `render_async` (yields; jobs-composable).
13. Render-as-a-job -> `hull.blob` output -> `deliver` workflow dependent.
14. Structured diagnostics across the whole stack.
15. Data-driven templates (`from_template`).
16. Typst feature: IR -> PDF + SVG; fonts embedded; disabled-feature graceful.
17. Streaming output + cancellation for large renders.
18. Docs + the two examples.
19. All native code obeys the c-audit conventions (`const` vtable, no runtime
    exec, structured errors, bounded buffers).

## 23. Phasing (incremental - the recommended build order)

- **Phase 0 - spike (do first):** Typst -> `wasm32` under WAMR. Go/no-go.
- **Phase 1 - pure-stdlib IR core, zero native:** builders + values + validate +
  normalize + walk + canonical_json + **Markdown & HTML renderers**, both
  runtimes, + conformance/golden/escaping tests. Satisfies AC 1-6, 8-11, 14-15,
  17(partial), 18. **The bulk of the value, no risk.**
- **Phase 2 - minimal C registry:** `HlReportRenderer` vtable + registry
  (image-codec-modelled) + Lua/JS bridges + streaming/cancel; re-register the
  stdlib renderers through it uniformly. AC 11-12, 14, 17, 19.
- **Phase 3 - Typst feature + jobs/blob pipeline:** the WASM-Typst renderer
  behind the weak hook + `report.typ` + the render-job/blob/deliver example +
  the two `examples/`. AC 7, 9, 13, 16, 18, 20.

---

## Appendix A - Hull-grounding notes (why this fits)

- **Renderer registry = the `HlImageCodec` pattern**, already in-tree
  (`hl_image_register_codec`) - a proven minimal-C, runtime-neutral, `const`
  vtable seam. Nothing new architecturally.
- **Typst = WASM compute, not process-spawn** - forced by the sandbox
  (`hl_tool_spawn` is build-only). This makes the C *smaller* (Typst is a compute
  module) and async *free* (compute.async worker pool).
- **Async = `HlAsyncCtx`**, the existing compute.async/db.async yield model - no
  new async plumbing.
- **Scale = hull.jobs + hull.blob**, both already shipped/landing - render jobs,
  durable blob output, workflow delivery, heartbeat for long renders.
- **Stdlib-heavy** matches the taxonomy: only Typst (a large optional vendored
  engine) is a feature; everything else is stdlib or the tiny registry seam.
