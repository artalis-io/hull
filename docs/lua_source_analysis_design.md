# `hull.source.lua` — Lua source-analysis layer (design / decision record)

**Status:** DESIGN, locked for implementation. Establishes the reusable,
Hull-owned Lua source-analysis foundation (AST + comments + annotations + exact
source ranges + structured diagnostics) that later powers `hull analyze`, generic
build plugins, `hull.query`, `hull.compute`, derive/codegen, schema generation,
and editor/agent tooling. **None of those consumers are built here.** This layer
only parses/analyzes source structure (no type-checking, no bytecode/WASM, no IR,
no source mutation, no module loading — §14).

## 0. Decision: a Hull-owned pure-Lua parser, not vendored LuaLS

The preferred starting point was LuaLS. Investigation (against LuaLS's actual
architecture) trips the spec's stop conditions:

- LuaLS's tokenizer depends on **LPegLabel**, a **native C library** — violates
  "no native dependencies" and "no C parser" (keep parsing out of the trusted
  core). The tool VM ships no extra native libs.
- The syntactic parse is **coupled** to `parser/guide` + `parser/compile` (the
  semantic phase this layer explicitly excludes); a syntax-only extraction is a
  large, surgical subset.
- LuaLS node positions are an internal encoding, not raw byte offsets; and its
  `luadoc` uses a fixed tag grammar (poor fit for arbitrary Hull annotations).

**Chosen:** a compact **pure-Lua recursive-descent Lua 5.4 parser** (~0.8–1.4k
LOC), same pattern Hull already uses (`stdlib/lua/hull/template.lua`). Zero native
deps, exact byte ranges by construction, arbitrary annotations trivial, no
vendoring/provenance burden. **LuaLS is used only as a grammar/AST reference.**
The parser is the single replaceable implementation detail behind the
`hull.source.lua` contract — so it can be swapped later without touching any
consumer.

```
        pure-Lua RD parser (implementation detail, replaceable)
                              │
   ┌──────────────────────────┴──────────────────────────┐
   │ stdlib/cli/lua/hull/source/                          │
   │   range.lua        SourceRange + line map (neutral)  │
   │   diagnostic.lua   Diagnostic            (neutral)   │
   │   annotations.lua  ---@ scanner + attach (neutral)   │
   │   lexer.lua        Lua 5.4 lexer (byte ranges)       │
   │   parser.lua       Lua 5.4 RD parser → Hull AST      │
   │   ast.lua          node kinds + walk/is helpers      │
   │   lua.lua          parse() → SourceUnit  ◄─ CONTRACT │
   └──────┬───────────────┬───────────────┬──────────────┘
       analyze          query           compute   (future — NOT here)
```

Placement is the **CLI/tool tree** (`stdlib/cli/lua/hull/source/`): this is
build-time metaprogramming, never the runtime request path. Reached via
`require("hull.source.lua")` in the tool VM.

## 1. Public surface (§39 — keep it this small)

```lua
local lua = require("hull.source.lua")

local unit, err = lua.parse(source, { path = "src/example.lua" })

lua.walk(unit.ast, function(node) ... end)   -- deterministic depth-first
if lua.is(node, "call") then ... end
local a = lua.annotation(node, "query")       -- annotation table or nil
local text = unit:text(node_or_range)         -- original bytes for a node/range
local line, col = unit:position(byte_offset)  -- line/col on demand
```

Everything else is plain-data tables (nodes, ranges, comments, diagnostics,
annotations). No OO hierarchy. Fewer functions if fewer suffice.

## 2. `parse()` contract (PRECISE — tightening #2)

`lua.parse(source, opts) -> unit, err`

- **`source` must be a string.** `opts` optional: `{ path = string?, limits =
  {...}? }` (§7 limits).
- **Ordinary syntax failure → `unit` is returned with `err == nil`**; the
  problems appear in `unit.diagnostics` (severity `"error"`). A caller that wants
  "did it parse cleanly?" checks `#unit.diagnostics == 0`, not `err`.
- **`err ~= nil` (and `unit == nil`) is reserved for:** invalid API arguments
  (e.g. `source` not a string) and internal parser failure / limit-guard trips
  that make continuing unsafe. `err` is a `Diagnostic`-shaped table (or a string
  for pure API misuse) — never a raw Lua `error()` across the boundary; the
  parser catches its own internal faults and reports them as `err`.
- The layer **never prints** (§36). Diagnostics are data.

## 3. `SourceRange` (language-neutral, §6 — tightening #5)

- `range = { start = <int>, stop = <int> }`, **half-open `[start, stop)`**,
  **1-based byte offsets** (Lua string convention). `stop` is one-past-end.
- Original text of a range: `source:sub(range.start, range.stop - 1)` — provided
  by `unit:text(x)`; consumers never do byte math themselves (§20).
- **Nodes carry byte offsets only.** Line/column is **resolved on demand** via a
  per-unit **line-start index** (built once, binary-searched): `unit:position(off)
  -> line, col` and `unit:line_col(range) -> sl, sc, el, ec`. Nodes are NOT
  eagerly annotated with line/col (avoids duplicating expensive data on every
  node, §6). The line map is generic (reused verbatim for JS later, §21/§33).
- Empty/zero-width ranges use `start == stop`.

## 4. `SourceUnit` (§5)

```lua
{ path = "src/example.lua", language = "lua", source = "<original bytes>",
  ast = <chunk node>, comments = { <comment>, ... }, diagnostics = { <diag>, ... } }
```
Methods: `unit:text(node_or_range)`, `unit:position(off)`, `unit:line_col(range)`,
`unit:annotations_for(node)`. Annotations attach **onto AST nodes**
(`node.annotations`, a name→annotation map; ordered list in
`node.annotation_list`) — the KISS choice; consumers use `lua.annotation(node,
name)`.

## 5. Hull AST vocabulary (§7/§8 — stable, small, source-faithful)

`{ kind = <string>, range = {start,stop}, ... }` tables. Source order preserved;
no IR, no semantic scopes. Node kinds (final set fixed in slice 3):

- **Statements:** `chunk`, `local_declaration` (`names[]` each `{name, attrib?}`
  where attrib ∈ `const|close`, `values[]`), `assignment` (`targets[]`,
  `values[]`), `call_statement`, `do`, `while`, `repeat`, `if`
  (`clauses[]`=`{cond?, body}`), `numeric_for`, `generic_for`,
  `function_declaration` (`name` path, `is_local`, `is_method`, `params[]`,
  `is_vararg`, `body`), `return` (`values[]`), `break`, `goto` (`label`),
  `label`.
- **Expressions:** `name`, `index` (`obj`, `key`), `field` (`obj`, `name`),
  `call` (`callee`, `args[]`), `method_call` (`obj`, `method`, `args[]`),
  `function_expr` (`params[]`, `is_vararg`, `body`), `table` (`fields[]` =
  positional / `name=` / `[expr]=`), `binary` (`op`, `lhs`, `rhs`), `unary`
  (`op`, `operand`), `vararg`, `paren`, `literal` (`subtype` ∈
  `string|number|boolean|nil`, `value`, plus `string_kind` ∈ `quoted|long`,
  `number_kind` ∈ `int|float|hex|hexfloat`).

Preserves local-vs-global, function-vs-local-function, method syntax, params,
literal sub-forms. Consumers recognize e.g. `local q = query{...}`
(`local_declaration` whose `values[1]` is a `call` with a single `table` arg) and
`function score(x,y) ... end` structurally.

## 6. Full Lua 5.4 syntax is REQUIRED (tightening #1)

The parser must accept **all** valid Lua 5.4: long strings/comments
(`[==[ ... ]==]`, level-matched), every numeral form (decimal int/float, hex
`0xff`, **hex float** `0x1.8p3`), all string escapes (`\n \t \\ \" \' \a \b \f \r
\v \xFF \ddd \u{XXXX} \z` and `\<newline>`), labels/`goto`, attributes
(`<const>` / `<close>`), method calls (`:`), varargs (`...`), and the **complete**
operator precedence + associativity table (incl. right-assoc `^` and `..`, unary
binding, `and`/`or` short-circuit precedence).

**Hard rule:** any **valid** Lua 5.4 syntax the parser cannot yet represent must
emit an **explicit diagnostic** (`code = "lua.unsupported"`) and stop that
construct — **never** silently produce a malformed/partial AST that lies. A
missing feature is a loud diagnostic, not a wrong tree.

## 7. Resource limits (tightening #3 — adversarial build inputs)

`opts.limits` with safe defaults (documented, overridable): `max_bytes`
(source length), `max_tokens`, `max_depth` (nesting), `max_diagnostics`.
Exceeding any cap emits a terminal diagnostic (`code = "lua.limit.<which>"`,
severity error), stops parsing, and returns the partial `unit` with `err == nil`
(it's an input problem, not API misuse). Guarantees bounded work + memory so a
hostile source in a build plugin cannot exhaust the tool VM. Recursion-depth is
tracked explicitly (no unbounded native Lua recursion).

## 8. Comments (§11) + annotations (§9/§10/§25 — Hull-owned, arbitrary)

- **Comments** preserved as `{ text, range, kind }`, `kind ∈ line | long |
  annotation`, in `unit.comments` (source order).
- **Annotations are Hull's own scanner over comment text — no known-tag
  whitelist.** Any `---@name`, optional `(args)`, optional trailing text becomes
  `{ name, args = <string?>, text = <string?>, raw = <full comment>, range }`.
  Arbitrary `---@query`, `---@compute`, `---@derive(json)`,
  `---@some_future_plugin(a,b)` all survive generically. Known LuaDoc tags
  (`@param/@return/@field/@class`) MAY additionally expose parsed fields, but the
  generic form is always present.
- **Attachment rule (deterministic, documented):** a **contiguous run** of
  comment lines (`--` line comments and `--[[ ]]` blocks — plain comments
  participate in the run, contributing no annotation) immediately preceding a
  **declaration**, with **no blank line** between the run and the declaration,
  attaches to that declaration's node (`node.annotations` /
  `node.annotation_list`). Attachment is **declaration-scoped** (the smaller, safer
  surface): only `local_declaration` and `function_declaration` (both `local` and
  global `function name() ... end`) carry annotations. A `---@` run above any other
  statement (assignment, call, loop, conditional, return, break/goto/label)
  attaches to the **nearest following declaration** instead, or to nothing. A blank
  line, intervening code, or a different declaration breaks the run.
  Trailing/inline comments do not attach. `unit:annotations_for(node)` and
  `lua.annotation(node, name)` read them. Every `---@` comment is retagged
  `kind = "annotation"` in `unit.comments` whether or not it attaches; comments
  that attach to nothing remain in `unit.comments` only.

## 9. Diagnostics (§12/§13) + recovery

`{ severity = "error"|"warning", code = "lua.syntax"|"lua.unsupported"|"lua.limit.*",
   message, path, range, related? }`. Returned in `unit.diagnostics`, never
printed. This is THE shared diagnostic shape (JS reuses it later, §33).

**Recovery:** best-effort, statement-level only. On a syntax error the parser
emits the diagnostic, resynchronizes to the next statement boundary, and
continues — yielding a **partial** AST for the well-formed statements. Documented
as "partial, not editor-grade recovery." Correctness over speculative recovery.

## 10. Isolation boundary (§16/§27)

No consumer touches parser/lexer internals — only the Hull AST + `SourceUnit`.
Enforced by tests asserting solely on the public AST/API (no lexer-private
fields). Because the parser is Hull-owned there is no third-party field leakage to
guard, but the adapter boundary (`lua.lua`) remains the sole contract so the
parser stays swappable.

## 11. Tests + Lua 5.4 differential conformance (§22–27 — tightening #4)

- **Unit tests** for every node kind (§22), exact byte ranges (§23: incl.
  UTF-8-before-node, multiline, long strings/comments, CRLF), annotation
  name/raw/range/attachment (§24), arbitrary-annotation survival (§25), malformed
  → diagnostics (§26), and adapter-stability (§27).
- **Differential conformance corpus** against Lua 5.4's own parser (Hull already
  embeds Lua 5.4): for each valid sample, `load()` succeeds **and** our parser
  emits no diagnostics; for each invalid sample, both reject. **Excluded** are the
  cases where `load()` is stricter than pure syntax (compile-time *semantic*
  checks our syntactic layer intentionally does not enforce): `break`/`goto`
  outside a valid target, `...` in a non-vararg function, assignment to a
  `<const>`, jump-into-scope, and the various "too many locals/upvalues/constants"
  limits. Those are listed explicitly and asserted as "parser accepts, `load`
  rejects (semantic)" so the boundary is documented, not accidental.
- Tests assert on Hull's public representation only.

## 12. Debug (§28) + performance/memory (§31/§32)

`unit`→JSON dump via `hull.json` for tests/dev (deterministic; no new CLI).
Build-time code, not the request path — correctness first, no premature
optimization; a single-pass lexer + RD parser with the line map built once. Do
not retain duplicate source/AST copies beyond the `SourceUnit`.

## 13. Implementation slices (locked order)

1. **Lexer + ranges** — full Lua 5.4 tokens with exact half-open byte ranges;
   the line-start index; comment capture. Tests: token/range correctness incl.
   long strings/comments, numerals, escapes, CRLF, UTF-8.
2. **Expression grammar** — complete precedence/associativity; literals,
   tables, calls/index/method, function expr, varargs, parens, unary/binary.
3. **Statements/declarations** — all statement kinds incl. attributes,
   labels/goto, for-forms, function declarations; the final node vocabulary.
4. **Comments/annotations** — the generic `---@` scanner + the attachment rule.
5. **Recovery + conformance** — statement-level resync, resource limits, the
   differential corpus.

**Consumers wait until the adapter-level (slice 3–4) AST tests are stable.**

## 14. Out of scope (§37 — do not build here)

Type system, symbol/scope solver beyond parsing needs, bytecode/WASM, Query IR,
Compute IR, source transformation engine, macros, LSP/editor server, incremental
parser, JS parser, build plugins, package manager. One parser, one annotation
model, one range representation, one diagnostic representation (§38).

## 15. Deliverables / report template (§40)

On completion, report: vendored LOC/files (**0** — none vendored); Hull adapter
LOC (parser + 5 modules); dependencies introduced (**none**); Lua versions tested
(**5.4**); known parser limitations; annotation limitations (generic-only
semantics; attachment is contiguous-leading-run only); error-recovery behavior
(statement-level partial). License/provenance: N/A (Hull-owned; standard
`AGPL-3.0-or-later` SPDX headers).
