# H1 / S1 - dead-code evidence audit (result: no deletion)

Status: **AUDIT COMPLETE. ZERO CODE CHANGE.** S1 of the ratified
[H1 cleanup freeze](h1_cleanup_inventory.md). It records reproducible evidence for
the freeze's 1.1 survey observations and settles the `include/hull/cap.h`
disposition. **Outcome: retain as-is - there is nothing safe to delete in the
dead-code dimension.** ("Finding no safe cleanup is a valid result.")

**What this evidence does and does not prove (per review).** The two commands below
establish REFERENCE COVERAGE - that the build system and the test/build harness
mention every source and every fixture - NOT the prerequisite CLOSURE of a specific
linked target. That is sufficient for the audit's purpose (there is no orphan file
and nothing to delete); a per-composition prerequisite-closure proof is only needed
if a deletion were ever proposed, and none is.

## 1. Build-system reference coverage - COMPLETE (every `.c` is referenced)

Claim (freeze 1.1): no orphan `.c`. Evidence: every `src/hull/**/*.c` is referenced
by the build system across the main build (all feature flags) AND the feature-
archive targets (`make feature-<name>`). Fail-closed (the loop aborts if any
`make -pn` errors, so a partial database is never silently accepted):

```sh
set -eu
cov=$(mktemp)
for goal in "" "HL_ENABLE_GPU=1 HL_ENABLE_DUCKDB=1 HL_ENABLE_POSTGRES=1 HL_ENABLE_MYSQL=1 HL_ENABLE_TUI=1 HL_ENABLE_VALKEY=1"; do
  make -pn $goal >/dev/null   # fail closed: non-zero aborts under `set -e`
  make -pn $goal 2>/dev/null | grep -oE 'src/hull/[A-Za-z0-9_/.-]+\.c'
done > "$cov"
for t in feature-duckdb feature-mysql feature-postgres feature-gpu feature-valkey; do
  make -pn "$t" >/dev/null   # fail closed: same preflight as the runs above
  make -pn "$t" 2>/dev/null | grep -oE 'src/hull/[A-Za-z0-9_/.-]+\.c' >> "$cov"
done
comm -23 <(find src/hull -name '*.c' | sort -u) <(sort -u "$cov")   # -> empty
```

Result: **275 / 275 referenced, 0 orphans.** 270 are referenced by the main build
(default + feature flags); the remaining **5 are feature-BACKEND sources built by
the feature-archive targets, not the main hull link** - `cap/db_duckdb.c`,
`cap/db_mysql.c`, `cap/db_postgres.c` (`make feature-<db>`, 8 refs each),
`cap/gpu_wgpu.c` (`feature-gpu`, 8 refs), `cap/valkey_register.c`
(`feature-valkey`). This is exactly the "optional composition" case the freeze
flagged: a main-build-only oracle mis-reports them, which is why the feature targets
are included. `make -pn` expands the `$(wildcard …)` globs and feature `ifeq`
blocks, so the oracle is the build's own resolved reference set - not a basename grep.

**Corroboration (actual compilation, not just reference).** The generated
`build/*.d` prerequisite files record what was ACTUALLY compiled in the
currently-built configuration: `grep -hoE 'src/hull/[^ ]+\.c' build/*.d | sort -u`
lists 253 `.c` for the default+test build present at audit time. This is stronger
than reference coverage but composition-specific; extending it to a full
per-composition compile+link closure is the strengthening step S1 would run IF a
deletion were proposed.

## 2. Fixture-name reference coverage - COMPLETE (every fixture has a validated consumer)

Claim (freeze 1.1): no stale `tests/fixtures/*`. Evidence: a path-exact
fixture->consumer table over **executable test/build wiring only** (Make / mk /
shell / test `.c`), with DOCUMENTATION references excluded (a `CLAUDE.md` mention is
not test use - the earlier draft's inclusion of it, and its bare-basename substring
match, overstated the claim):

```sh
for fx in tests/fixtures/*; do b=$(basename "$fx")
  grep -rn "tests/fixtures/$b\b" Makefile mk scripts tests \
    --include='*.mk' --include='*.sh' --include='*.c' --include='Makefile' \
    | grep -v "^tests/fixtures/$b/" || echo "NO PATH-EXACT CONSUMER $b"
done
```

Result: **every fixture has an executable consumer**, each manually confirmed to be
real test/build wiring (not a doc mention). The path-exact table maps e.g.
`auth_flows_*` -> `e2e_auth_flows*.sh`, `oauth_*` -> `e2e_oauth.sh`, `mime` ->
`tests/hull/cap/test_mime.c`, `null_app` / `selfbuild_app` -> `Makefile`
(compiler-free / self-build e2e), `test262` / `lua54-tests` -> `scripts/fetch_*.sh`.
The only two the path-exact pass flagged - `valkey_kv_lua` / `valkey_kv_js` - are
consumed by `tests/e2e_valkey.sh:104-105` via a variable-prefixed path
(`"$FIX/valkey_kv_lua/app.lua"`), which a literal-path grep cannot see;
manually validated as executable wiring.

Caveat: this is fixture-NAME reference coverage over the enumerated wiring, plus a
manual check that each hit is executable - not a mechanical proof that every
referenced fixture is exercised on every CI run.

## 3. `include/hull/cap.h` - disposition: RETAIN

Evidence:
- **Content:** a pure umbrella - 9 `#include "hull/cap/*.h"` lines plus the
  `HL_CAP_H` guard. It defines no symbols of its own.
- **In-repo use:** zero (every Hull TU includes the individual `cap/*.h`).
- **Public surface:** `include/hull/**` IS Hull's public C-header tree, shipped to
  native embedders (see [`libhull_flavor.md`](libhull_flavor.md); embedders compile
  against `hull/embed.h` and the header tree). `cap.h` has been present since an
  early commit and is a valid `#include <hull/cap.h>` an external embedder could use
  to pull in every capability header at once.
- **Not** named in [`stability.md`](stability.md) tiers or any packaging / install
  manifest - so it is neither an explicitly-committed nor an explicitly-excluded
  surface.

Disposition: **RETAIN.** Zero in-repo use does NOT prove external embedders don't
depend on it; deleting a long-present public header is a source-compatibility risk
with no offsetting benefit (a 9-line umbrella costs nothing to keep). It is
correctly classified as "internally-unused public surface", not dead code. If the
project later decides to slim the public surface, the path is **deprecate-first** (a
documented deprecation note + one release cycle), never a silent delete - but this
audit finds no reason to do even that now.

## 4. Conclusion

The dead-code dimension of H1 has **nothing to delete**:
- build-system reference coverage: complete (275/275 referenced; 0 orphans),
  corroborated by the `build/*.d` actual-compile set for the built configuration;
- fixture-name reference coverage: complete (every fixture has a manually-validated
  executable consumer; 0 stale);
- `cap.h`: retained public surface, not dead code.

Each result states exactly what its command proves - reference coverage (+ a compile
corroboration and a manual wiring check), NOT the prerequisite closure of a linked
target; the latter is only warranted if a deletion is proposed, and none is.

**No code was changed in S1.** The commands are reproducible and could seed a future
coverage gate if desired. Next slice: S2a (consolidate only the verbatim
`feature.c` / `flavor.c` checksum comparison).
