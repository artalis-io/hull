# JS frontend Slice 3 - JSDoc / Hull annotation scan + structural attachment

Design record for Slice 3 of the JavaScript source frontend
(docs/javascript_source_frontend_design.md slice 3, sections 17-18). Mirrors the Lua
counterpart stdlib/cli/lua/hull/source/annotations.lua
(docs/lua_source_analysis_design.md sections 6/8) so a future codegen consumer sees
one annotation model across both languages. This is the DESIGN; implementation
follows on approval. (ASCII-only, no em-dashes, per repo convention.)

## 1. Goal + scope

Turn the @tags inside JSDoc block comments into structured annotation records, and
attach contiguous leading runs of them to the declaration they document. Two hard
constraints, both from the Lua layer:

- Generic + whitelist-free. `name` is whatever follows @. An app's own @query /
  @compute / @route / @derive is captured with the same fidelity as a standard @param
  / @returns. This layer only RECORDS names with exact ranges; consumers (a future
  lowering step) give them meaning. No tag is special-cased.
- Exact byte ranges + raw provenance. Every annotation record carries the half-open,
  1-based byte range of the @tag it came from and the exact raw source bytes of that
  tag. Nothing is inferred or normalized away at this layer.

In scope: the annotation scanner, the record shape, and structural attachment to
declaration AST nodes. Out of scope (later slices): interpreting any tag, the scope
resolver (Slice 4), and the frontend adapter that turns attached annotations into the
ProjectDiscovery (Slice 5). Never executes application JS.

## 2. Annotation source: JSDoc block comments only

The lexer already classifies each comment as line / block / jsdoc (`kind:"jsdoc"` iff
`/** ... */` and not the empty `/**/`; see stdlib/cli/js/hull/source/lexer.js:245).
Annotations are scanned ONLY from jsdoc comments. Rationale:

- The JS convention for machine-readable annotations is JSDoc (`/** @tag */`), exactly
  as `---@` line comments are the Lua/LuaCATS convention. The committed corpus confirms
  it: 55 files carry `/** ... */` blocks with @file / @param / @returns / @module, and
  the dominant shape is a block on its own lines immediately above a declaration
  (cache.js:37-43, jwt.js, validate.js).
- A line comment beginning with @ is NOT an annotation source. In JS `// @x` is not a
  convention, and treating it as one would invite false matches. The mirror is "each
  language's native annotation-comment form", not "line comments". A plain block
  comment (`/* ... */`, single star) is likewise not scanned.

This choice gives the "no string / expression / prose false-match" guarantee
structurally: the lexer already separated comments from strings, templates, and regex
literals, so an @ inside a string or a regex is never a comment and is never scanned
(see section 8).

## 3. Tag grammar inside a JSDoc block (generic)

A jsdoc comment's interior is the bytes between `/**` and `*/`. Within it, a tag begins
at an @ that is the first non-margin character of a content line: after optional
leading whitespace and an optional single `*` star margin and optional following
whitespace. Requiring line-start-after-margin is what prevents matching an @ inside
prose (`see user@example.com`, `@ mid-sentence`): only a @ that opens a JSDoc tag line
is a tag.

Each tag is parsed exactly like the Lua layer's parse_comment, so the two record
shapes are identical:

```
@name                     -> { name }
@name(args)               -> { name, args }          # args = raw text inside the balanced (...)
@name  trailing text      -> { name, text }          # text = trailing free text, trimmed
@name(args) trailing text -> { name, args, text }
```

- name = @ followed by `[A-Za-z_][A-Za-z0-9_]*` (mirror Lua exactly). A bare @ with no
  identifier is not a tag and is ignored.
- args = the text inside a BALANCED (...) group that immediately follows the name, or
  absent (see section 3.2 for the exact scan and every malformed case). JSDoc's own
  `{Type}` braces are NOT the args convention: a `{...}` is left in text. This keeps the
  layer generic; we do not model JSDoc's type grammar.
- text = the remaining free text after the name (and after the (...) group if present),
  with per-line star margins stripped and surrounding whitespace trimmed; multi-line
  tag continuation (a description that wraps onto the next margin line, until the next
  tag or the block end) is joined into text. Empty text is normalized to absent.

### 3.1 Exact range + raw provenance (hard constraint)

Each annotation record's range is the half-open, 1-based byte range OF THE TAG ITSELF:
from the @ to the end of the tag's logical extent (the byte before the next tag's @, or
the block's interior end before `*/`), with trailing whitespace / margin trimmed back
off the stop. raw is the exact source substring over that range (margins and all: raw
is verbatim, text is the cleaned form). One JSDoc block with three @tags yields three
records with three DISTINCT exact ranges, not one range for the whole block.

Ranges are PURE BYTE OFFSETS into the source, computed from the comment's known byte
extent; they never depend on UTF-8 validity or on decoding. So a block containing
invalid UTF-8 (already a lexer js.syntax, section 3.2) cannot shift or corrupt a tag
range: the scanner walks bytes, and a non-ASCII / invalid byte is simply a byte that is
neither @ nor a margin/newline.

### 3.2 Malformed tag behavior (deterministic, metadata-preserving)

The args group is scanned as a RAW BALANCED-PAREN span, quote-agnostic and
escape-agnostic (the layer does not parse JS string grammar inside a comment). Depth
starts at 0 on the opening `(` and each `(` / `)` increments / decrements it; the group
ends at the `)` that returns depth to 0. Precise, documented outcomes:

- Nested parentheses: handled by the depth counter. `@foo((a), b)` -> args = "(a), b".
- Unmatched `(` (no closing `)` before the tag's end): the group does NOT close. Fall
  back deterministically: NO args; the entire remainder from the `(` onward becomes
  text. `@foo(bar` -> { name:"foo", text:"(bar" }. Metadata is preserved (raw is the
  whole tag), never dropped.
- Quoted or escaped `)` inside the group: the scan is quote-agnostic, so the FIRST `)`
  that balances depth closes the group. `@foo("a)b")` -> args = `"a`, and `b")` becomes
  text. This is deterministic and documented; because raw always holds the verbatim
  tag, no information is lost even when the split is not what a JS-string-aware parser
  would pick. (Rationale: modelling JS string/escape grammar inside a comment would
  make the layer non-generic and is unnecessary given raw provenance.)
- A continuation line whose content contains a prose @ (not line-leading after the
  margin): the @ is NOT a new tag; it is folded into the CURRENT tag's text. Only a
  line-leading-after-margin @ opens a tag (section 3). So `@param x\n *  the @ sign`
  is one tag with text "x the @ sign".
- Invalid UTF-8 inside the block: the lexer already emitted js.syntax
  ("invalid UTF-8 in comment", lexer.js:239) for the block; the annotation scanner
  still runs over the block's bytes and produces valid byte ranges (section 3.1). It
  does not emit a second diagnostic and does not fabricate a range.

None of these malformed-tag cases is an INTERNAL failure: each is a documented,
deterministic fallback, so none emits js.internal (contrast section 9.1). The record is
always retained with verbatim raw.

### 3.3 Record shape (parser layer)

Mirrors the Lua parse-layer record verbatim (annotations.lua:11-16):

```
{ name: "param",             // identifier after @
  args: "a, b" | undefined,  // balanced (...) group text, or absent
  text: "x f64" | undefined, // trailing free text, cleaned + trimmed, or absent
  raw:  "@param x f64",      // exact source bytes of the tag
  range: { start, stop } }   // half-open 1-based byte range of the tag
```

(The Slice-5 frontend adapter renames text -> value when normalizing, exactly as
frontend_lua.lua:norm_annotations does; the parser layer keeps text for parity with the
Lua parse layer.)

## 4. Attachment targets - which AST nodes carry annotations

Mirror the Lua DECLARATION_KINDS set: annotations attach to DECLARATIONS only. The JS
declaration node kinds the parser produces:

| AST node | example | target |
|---|---|---|
| VariableDeclaration | `const x = 1;` / `let a, b;` | the declaration statement (section 7) |
| FunctionDeclaration | `function f() {}` / `async function g() {}` | the declaration |
| ClassDeclaration | `class C {}` | the declaration |
| ExportNamedDeclaration wrapping one of the above | `export const x = 1;` | the INNER declaration, via the export line (section 6) |
| ExportDefaultDeclaration wrapping a function/class | `export default function f() {}` | the inner declaration, via the export line (section 6) |

Every other statement (an expression statement, a call, an assignment
`obj.m = function(){}`, a control-flow statement, a bare `export { a, b }` specifier
list, `export default <expr>`) is NOT a declaration node and receives no attachment
(section 7).

On a target node, exactly as Lua (annotations.lua:22-24):

- node.annotationList - ordered array of every annotation record in the leading run,
  top-to-bottom (source order).
- node.annotations - name -> the FIRST annotation of that name (repeat tags such as
  @param keep every copy in annotationList; annotations["param"] is the first).

JS camelCase annotationList mirrors Lua's snake_case annotation_list; the field is
internal to the AST and consumed only by the Slice-5 adapter. It is present (an array,
possibly empty is NOT produced - see below) only when the run yields at least one
annotation; absent otherwise, matching Lua.

## 5. Attachment rule - the leading run (comment-only lines)

A declaration's annotations are the JSDoc tags found in the unbroken run of leading
COMMENT-ONLY lines directly above it: no blank line and no code between the run and the
declaration.

Strengthened "leading" predicate (a correction over the Lua layer, which checked only
the bytes BEFORE the comment). A comment PARTICIPATES in a leading run iff every
physical line it spans is COMMENT-ONLY: every byte on that line is either whitespace or
inside some comment's [start, stop) extent. This is computed from the comment set +
source bytes (a line is comment-only iff its non-whitespace bytes are all covered by
comments), independent of the token stream. Consequences:

- `/** @foo */ doThing();` on ONE line: line 1 contains the code bytes `doThing();`
  (not whitespace, not inside a comment), so line 1 is NOT comment-only, so the JSDoc
  does NOT participate in any run. A declaration on the next line gets no attachment.
  (This is the specific case the whitespace-before-only check would wrongly attach.)
- `/** @a */ /** @b */` on one line: both comments cover the line, only whitespace
  between them, no code -> line comment-only -> both participate.
- `x = 1; /** @n */` : the line has the code `x = 1;`, so the JSDoc does not participate
  (a trailing comment never attaches).

Attachment walk, mirroring annotations.lua:119-143 with the strengthened predicate:

1. Index every PARTICIPATING comment (all its lines comment-only) by the line its end
   (`*/`) falls on.
2. For each target declaration, let L be the line of its effective start (section 6).
   Walk upward: while a participating comment ENDS on line L-1, include it and continue
   from that comment's START line minus one. Stop at the first line with no
   participating comment ending on it.
3. The run's jsdoc comments contribute their @tags (source order, top comment first,
   top tag first) to annotationList; annotations indexes first-by-name.

A blank line breaks the run (step 2 stops), and a code line breaks the run (its line is
not comment-only, so no participating comment ends there). Both follow from "the
immediately-preceding line must carry a participating comment".

## 6. Exports - wrapper attachment

An exported declaration is wrapped: ExportNamedDeclaration.declaration /
ExportDefaultDeclaration.declaration points at the inner VariableDeclaration /
FunctionDeclaration / ClassDeclaration. The JSDoc sits above the `export` keyword (the
export node's start, column 1) NOT above the inner declaration, whose start is mid-line.
`export const x = 1;` parses to ExportNamedDeclaration{start:1} with declaration:
VariableDeclaration{start:8}.

Rule: attach to the inner declaration node, but compute the leading run from the
OUTERMOST enclosing export statement's start line. So a JSDoc above `export const x=1;`
attaches to the VariableDeclaration (which carries the name / group the adapter reads),
using the export statement's line for the run walk. `export default function f(){}` and
`export default class C {}` attach the same way. `export default <expression>`
(e.g. `export default 42;`) has NO inner declaration node and receives no attachment.
A re-export (`export { a } from "m"`, `export * from "m"`) has no inner declaration
either.

## 7. The explicit cases

Multi-declarator declarations. `/** @foo */ const a = 1, b = 2;` attaches the run to the
single VariableDeclaration statement, NOT to the individual VariableDeclarators. Mirrors
Lua's `local a, b`. In the ProjectDiscovery group model (project/model.lua:26,86-91) the
declaration node is the GROUP: a and b become per-name facts sharing one group_id, and
each attached annotation carries target_group_id = that group's id. The annotation is
group-scoped; the Slice-5 adapter distributes group membership. Slice 3's structural job
is purely: attach to the VariableDeclaration node.

Exports. Covered in section 6.

Intervening comments. A non-annotation comment between a JSDoc block and the declaration
does NOT break the run, PROVIDED its line is comment-only:
```
/** @foo */
// an ordinary note
const x = 1;      // @foo attaches to x
```
Both leading lines are comment-only, so both participate; the `//` line contributes no
tags. But if the intervening comment shares its line with code:
```
/** @foo */
doThing(); // note
const x = 1;      // @foo does NOT attach (the doThing() line is not comment-only)
```
the run is broken.

Blank lines. A blank line between the JSDoc and the declaration breaks the run -> NO
attachment. A blank line within the leading stack splits it; only the contiguous block
touching the declaration attaches.

Unsupported declaration forms. A JSDoc whose nearest following statement is not a
declaration node attaches to nothing:
- an expression / call statement (`/** @foo */ doThing();`),
- a property-assignment function expression (`/** @foo */ retry.run = function(){};` -
  an assignment, not a declaration; several corpus APIs are written this way, and
  promoting them to discoverable declarations is a Slice-5 adapter question, NOT a
  Slice-3 structural one),
- a bare `export { a, b };` / `export * from "m";` / `export default <expr>;`,
- any statement the parser declined as js.unsupported (its node is an Error placeholder,
  not a declaration kind).
In every such case the tags are still RECORDED on comment.annotationList (section 4a)
but are attached to no declaration. Deterministic, never a guess at a non-declaration
target.

Same-line inline JSDoc. `/** @type {number} */ const x = 1;` on ONE physical line does
NOT attach in v1: the block's line is comment-only ONLY if nothing else is on it, but
here `const x = 1;` shares the line, so the JSDoc does not participate (section 5), and
even were it isolated the leading-run walk starts at L-1. Matches Lua and the corpus
(own-line blocks). Documented limitation; addable later without changing the record
shape.

## 4a. Unattached-tag exposure on comments

Every jsdoc comment ALSO carries its own scanned tags, under an explicit, consistently
shaped field:

- comment.annotationList - on a jsdoc comment, ALWAYS an array (possibly empty `[]`
  when the block has no tags) of the records scanned from THAT comment (its own tags,
  in source order, each with the comment-local exact range + raw). On a non-jsdoc
  comment (line / block), the field is ABSENT.

So the field is present-and-array iff the comment is jsdoc, and absent otherwise -
consistent, never sometimes-nil-sometimes-array on the same comment kind. This is what
lets a consumer inspect tags that attached to no declaration (an @file / @module header
block, a JSDoc above a non-declaration), and it is the stable surface the idempotency
test reads. (Distinguish from node.annotationList in section 4, which is the FLATTENED
run across possibly several comments; comment.annotationList is one comment's own tags.)

## 8. No false matches (guarantee)

- Strings / templates / regex: an @ inside a string, template, or regex literal is lexed
  as part of that token, never as a comment, so it is never scanned. No
  string/expression position can produce an annotation.
- Prose inside a block: a @ that is not the first non-margin character of a content line
  (`contact me@host`, `x @ y`, a continuation line's mid-text @) is not a tag.
- Two-hyphen / single-star comments: only jsdoc (`/** */`) comments are scanned; `//`
  and single-star `/* */` are ignored, mirroring Lua's three-or-more-hyphens gate.

## 9. Module boundary + pipeline integration

New module stdlib/cli/js/hull/source/annotations.js (hull:source:annotations), mirroring
hull.source.annotations. Exports:

- parseTag(rawTag, range) -> an annotation record, or null when it is not a tag.
- scanBlock(comment, bytes) -> the array of records inside one jsdoc comment (each with
  a comment-local exact range); sets comment.annotationList (section 4a).
- attach(ast, comments, bytes, linemap, emitInternal) -> mutates comment.annotationList
  on every jsdoc comment and node.annotationList / node.annotations on the run targets;
  returns nothing.

The parser calls attach(...) at the end of parseInternal, before assembling the returned
SourceUnit, exactly as lua.parse calls annotations.attach, so a hull:source:parse result
already carries annotations on its AST nodes and comments. attach needs the source bytes
(to test comment-only lines) and the linemap (already computed by the tokenizer); both
are in scope in parseInternal.

### 9.1 Hardened failure contract (attachment defects surface as js.internal)

The invariant, matching the Lua project layer's hardened boundary: if
unit.diagnostics is empty, attachment did NOT internally fail. So attachment defects
must NOT degrade silently.

Two distinct failure classes:

- Malformed source / tag CONTENT (an unmatched `(`, a quoted `)`, invalid UTF-8 inside
  the block, a prose @): NOT an internal failure. Handled by the deterministic,
  documented fallbacks in section 3.2 (retain the whole raw tag; unmatched group -> text;
  invalid UTF-8 already js.syntax from the lexer). No js.internal.
- An UNEXPECTED INTERNAL defect: an AST node of an unexpected shape, an invalid range
  (start > stop, or outside [1, n+1]), or any exception thrown inside attach. This is a
  frontend bug, not malformed input. attach runs inside a guard that, on ANY such
  defect, emits a single js.internal diagnostic through the shared budget (the same
  budget the tokenizer + parser use) and stops attaching further (already-attached nodes
  are kept, so the SourceUnit still carries its AST plus the js.internal). The parse's
  valid becomes false and diagnostics is non-empty.

Because attach emits js.internal on an internal defect rather than swallowing it,
`unit.diagnostics.length == 0` guarantees attachment ran to completion without an
internal fault (the tested invariant, mirroring the Lua layer). The pre-existing
protectedParse boundary remains as the outer backstop; the attach guard is the inner,
finer one that keeps the AST while still flagging the fault.

### 9.2 Idempotent

Attachment is a pure function of (ast, comments, bytes). Running it twice reproduces the
same comment.annotationList / node.annotationList / node.annotations. scanBlock reads
the comment's bytes (not a mutated flag), so re-scanning yields identical records.

## 10. Testing plan (Slice-3 additions)

A new test_js_annotations C suite driving hull:source:parse and asserting on the attached
nodes + comments (JSON), covering the section 7 cases explicitly:

- generic/whitelist-free: @query, @derive, @route, @custom recorded like @param/@returns;
- record shapes: @name, @name(args), @name rest, @name(args) rest;
- exact range + raw provenance: three tags in one block -> three distinct ranges; each
  raw is the verbatim tag; ranges half-open 1-based;
- malformed tags (section 3.2): unmatched `(` -> text fallback; nested parens balanced;
  quoted `)` closes early (deterministic); prose @ on a continuation line folded into
  text; invalid-UTF-8 block still yields valid ranges + the lexer's js.syntax and no
  js.internal;
- unknown-tag survival; repeat @param all in annotationList, first-by-name in
  annotations;
- multi-declarator: attaches to VariableDeclaration, not the declarators;
- exports: export const / export function / export default function attach to the inner
  declaration via the export line; export { a } / export default 42 attach to nothing;
- comment-only-line rule (the section-2 correction): CODE AFTER JSDoc
  (`/** @foo */ doThing();\nconst x=1;`) does NOT attach; CODE AFTER AN INTERVENING
  ORDINARY COMMENT (`/** @foo */\ndoThing(); // n\nconst x=1;`) does NOT attach; a
  clean intervening comment-only line DOES continue the run; blank line breaks it;
- unsupported forms (expression stmt, obj.m = function(){}, js.unsupported stmt) ->
  recorded on comment.annotationList, attached to no declaration;
- no false matches: @ inside a string / template / regex / prose is never a tag;
- unattached-tag exposure: an @file header block's tags on comment.annotationList with a
  bare declaration-less file; the field is absent on line/block comments and an array
  (possibly empty) on every jsdoc comment;
- hardened contract (section 9.1): a forced internal defect (test-only injection, like
  the parser's __parseWithInjection) emits js.internal and keeps the AST; and a
  positive assertion that on the whole corpus diagnostics-empty implies attachment
  completed;
- idempotency: a second attach reproduces identical output;
- the Slice-2 conformance corpus stays clean=165 (annotations do not change parse
  validity) - a regression guard.

All under ASan + UBSan; ASCII-only; no em-dashes.

## 11. Non-goals

No tag interpretation (no @param type parsing, no @query semantics). No scope resolution
(Slice 4). No ProjectDiscovery construction or target_group_id assignment (Slice 5 - this
layer only puts annotationList on the declaration node). No same-line-inline attachment
(v1). No execution of application JS.
