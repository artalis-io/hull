<!--
SPDX-License-Identifier: AGPL-3.0-or-later
-->
# Lua source-frontend: pinned, parser-scoped official Lua 5.4.7 test corpus

Status: design (parser-scoped leg implemented; corpus vendoring pending review).
The Lua analogue of the Test262 slice (docs/js_test262_design.md): a second
pinned, upstream, language-corpus confidence layer for `hull.source.lua`, this
time the official Lua test suite.

## 1. Purpose and scope

Add a standards-derived conformance gate: run Hull's pure-Lua parser
(`hull.source.lua`) against the **official Lua 5.4.7 test suite**
(https://www.lua.org/tests/) and gate accept/reject against the **exact vendored
Lua 5.4.7** `load(src, name, "t")` oracle. This complements the existing Lua
conformance legs (repository corpus, curated syntax negatives, pinned semantic
divergences, seeded mutation fuzz) with a large, real, adversarial body of Lua
5.4 source written by the language authors.

### 1.1 Version pinning is mandatory

Hull vendors Lua **5.4.7** (`vendor/lua/lua.h`: `LUA_VERSION_RELEASE "7"`), so
the corpus is the release-matched `lua-5.4.7-tests.tar.gz`, NOT the newer 5.4.8
suite. Lua explicitly warns that test suites are release-specific and may not
work across versions. Pinning the corpus to the vendored VM keeps the oracle
(`load`) and the target identical.

- Archive: `https://www.lua.org/tests/lua-5.4.7-tests.tar.gz`
- SHA-256: `8a4898ffe4c7613c8009327a0722db7a41ef861d526c77c5b46114e59ebf811e`
- License: MIT (the Lua license, https://www.lua.org/license.html).

### 1.2 Two distinct uses (only the first is in scope here)

**(1) Parser-scoped conformance gate — THIS slice.** The suite is NOT executed.
Each official `.lua` file's exact bytes are fed to two oracles:

```
official Lua .lua file bytes
        |-- vendored Lua 5.4.7 load(bytes, "@name", "t")   (compile-only, no run)
        `-- hull.source.lua parser
```

**(2) Optional upstream runtime suite — SEPARATE, future.** The suite is
*designed to execute* and exercises the stdlib, the C API, the filesystem,
`load`/`dofile`/`os`/`io`, dynamic C libraries, and debug hooks. It must NOT run
through Hull's restricted application/tool sandbox (wrong contract; needs
authorities Hull intentionally removes). A future job could run the official
"basic" suite (`lua -e "_U=true" all.lua`) against Hull's exact vendored vanilla
Lua build to validate the **VM** (not `hull.source.lua`), on vendored-Lua/core
changes, on `main`, and nightly. The complete/internal suites are heavier and
platform-sensitive; keep those nightly/manual if adopted. Not designed here.

## 2. Directional model (reused verbatim from the existing harness)

Hull's parser is a SYNTACTIC recognizer; `load()` is syntactic + a thin
static-semantic layer. So (docs/lua_source_conformance_design.md,
`test_conformance.lua`):

- **Oracle accepts, Hull syntax-rejects → false reject.** Gate 0 (the key bug).
- **Oracle rejects, Hull clean-accepts → false accept**, UNLESS an exact
  reviewed static-semantic divergence (the pinned `SEMANTIC_REJECTS`
  categories: break-outside-loop, goto-no-label, goto-into-scope,
  vararg-outside, assign-const, too-many-locals).
- **Hull `lua.unsupported` → gate 0.** Hull claims FULL Lua 5.4 support, so an
  official file that is valid Lua must never be declined.
- **`lua.internal` / `lua.limit.*` → indeterminate.** Gate 0.
- **Range / source round-trip violations → gate 0.** Every AST node's
  half-open range is in bounds, `unit:text(node)` round-trips the exact slice,
  and children nest within parents.

Application source remains **unexecuted**. `load()` is used only by the C test
oracle, exactly as the current contract permits.

### 2.1 Shebang / first-`#`-line normalization

`load(string, ...)` does not skip a leading `#` line; Lua's *file* loader does.
The harness already strips a leading `#!` line from the oracle input (Hull's
lexer treats `#!` as a shebang comment). The official `main.lua` begins with a
bare `# testing special comment on first line` (a single `#`, not `#!`). Hull's
lexer intentionally only skips `#!` (so `#t` length-operator at chunk start
lexes as an operator), so Hull lexes the bare `#` as the length operator and
REJECTS `main.lua` — and `load(string)` rejects it identically. It is therefore
a clean **both-reject agree**, no expectation needed. (This slice does NOT
change the shebang policy; it reuses the existing `#!`-only normalization.)

## 3. Delivery: pinned, committed, generated subset (offline)

Mirrors Test262. Layout:

```
tests/fixtures/lua54-tests/
  LICENSE           # the Lua MIT license, verbatim (the archive bundles none)
  UPSTREAM.md       # pinned URL + archive SHA-256, fetched date (informational), selected summary
  manifest.json     # GENERATED; upstream facts only (path, lua_version, source_hash); canonical
  MANIFEST.sha256   # sha256 of the exact manifest.json bytes
  cases/...         # the selected .lua files, verbatim
```

`scripts/fetch_lua_tests.sh` (maintainer-run, NEVER in CI):

- downloads the exact 5.4.7 archive and verifies the official SHA-256;
- extracts into a temporary directory;
- rejects symlinks and any path escape;
- selects regular `.lua` files deterministically (`LC_ALL=C` sort);
- preserves exact bytes; computes a per-case `source_hash`;
- emits a canonical `manifest.json` (`lua_version`, `archive_sha256`, `count`,
  sorted `cases`) + `MANIFEST.sha256` (sha256 of the exact file bytes);
- writes the whole tree through a temp sibling and swaps into place on success;
- never runs in CI.

The C source libs (`libs/*.c`, `ltests/*.c`) and the `libs/` dir are for the
execution suite (use 2) and are NOT selected — only regular `.lua` files.

## 4. C leg (in the existing Lua conformance harness)

A new leg in `tests/hull/source/test_lua_source.c` (alongside repo corpus,
curated negatives, semantic divergences, mutation fuzz). It:

- verifies the manifest schema version, the pinned Lua version (`5.4.7`), the
  archive SHA-256, and each case's `source_hash`;
- enforces a manifest ↔ `cases/` bijection (no missing / extra `.lua`, fail
  closed on enumeration, no symlinks);
- full-reads every case (exact byte count, no short read);
- parses the exact committed bytes with BOTH oracles (`load(...,"t")` +
  `hull.source.lua`), reusing the existing `oracle_src` shebang normalization,
  `hull_state` three-state verdict, `classify` semantic allowlist, and
  `check_ranges` round-trip;
- reports enumerated / read / hashed / analyzed counts and oracle accept/reject;
- uses **exact-path expectations** for any genuine top-level static-semantic
  divergence (there are currently NONE — the pinned `SEMANTIC_*` categories are
  not tripped at file top level by this corpus);
- gates false-reject, indeterminate, unexpected false-accept, and range
  violations at zero, and fails on a stale expectation.

Because the analysis brain is pure Lua in a vanilla `lua_State`, the leg follows
the existing hybrid: C enumerates + reads + injects `HULL_LUA54_CORPUS`, the Lua
side runs the two oracles. CI stays completely offline (committed subset only).

## 5. Pre-vendor report (this slice)

Measured locally over the pinned archive (33 `.lua` files, 444,773 bytes):

- oracle: **accept 32, reject 1** (`main.lua`, bare-`#` first line — both reject);
- Hull buckets: **agree 33, false-reject 0, false-accept 0, unsupported 0,
  indeterminate 0**; semantic divergences: **none**;
- one genuine Hull lexer bug surfaced and fixed before vendoring: the
  hex-literal exponent radix (`0xE+1`), see the fix commit + `test_lexer.lua`
  regression.

So the entire official Lua 5.4.7 suite parses in FULL agreement with the
vendored Lua 5.4.7 `load` oracle — zero expectations required.

## 6. Non-scope

No execution of the suite, no C-API / stdlib / filesystem / dynamic-library
testing, no VM validation (that is the separate optional runtime job, §1.2),
no shebang-policy change, and no CI network access (fetch is refresh tooling).
