# H1 / S1 - dead-code evidence audit (result: no deletion)

Status: **AUDIT COMPLETE. ZERO CODE CHANGE.** S1 of the ratified
[H1 cleanup freeze](h1_cleanup_inventory.md). It proves the freeze's 1.1 survey
observations with recorded, reproducible commands, and settles the
`include/hull/cap.h` disposition. **Outcome: retain as-is - there is nothing safe
to delete in the dead-code dimension.** ("Finding no safe cleanup is a valid
result.")

## 1. Source / build closure - COMPLETE (no orphan `.c`)

Claim (freeze 1.1): every `src/hull/**/*.c` is part of the build under some
configuration. Proven by diffing the full source set against every `.c` make
resolves across the default build plus every feature flag:

```sh
{ make -pn 2>/dev/null
  make -pn HL_ENABLE_GPU=1 HL_ENABLE_DUCKDB=1 HL_ENABLE_POSTGRES=1 \
           HL_ENABLE_MYSQL=1 HL_ENABLE_TUI=1 HL_ENABLE_VALKEY=1 2>/dev/null
} | grep -oE 'src/hull/[A-Za-z0-9_/.-]+\.c' | sort -u > /tmp/make_known
find src/hull -name '*.c' | sort -u > /tmp/all_c
comm -23 /tmp/all_c /tmp/make_known   # -> empty
```

Result: **275 source files, 275 build-referenced, 0 orphans.** One file
(`cap/valkey_register.c`) is referenced ONLY under `HL_ENABLE_VALKEY=1` - it is
feature-gated, not dead (this is exactly the "optional composition" case the freeze
flagged: a default-only oracle would have mis-reported it). `make -pn` expands the
`$(wildcard …)` globs and feature `ifeq` blocks, so the oracle is the build system's
own resolved source set, not a basename grep.

## 2. Fixture-reference closure - COMPLETE (no stale fixtures)

Claim (freeze 1.1): every `tests/fixtures/*` is used. Proven repo-wide (the search
MUST include the Makefile / mk / scripts, not just `tests/` - the first pass missed
two because it was `tests/`-scoped):

```sh
for fx in tests/fixtures/*; do b=$(basename "$fx")
  grep -rq "$b" Makefile mk scripts tests CLAUDE.md \
    --exclude-dir="$(basename "$fx")" 2>/dev/null \
    || echo "UNREFERENCED $fx"
done   # -> no output
```

Result: **every fixture is referenced.** The two the `tests/`-only pass flagged are
driven from the Makefile: `tests/fixtures/selfbuild_app` (self-build e2e,
`Makefile:3008`) and `tests/fixtures/null_app` (compiler-free / null-app e2e,
`Makefile:3036,3038`).

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
- source/build closure: complete (0 orphans);
- fixture-reference closure: complete (0 stale);
- `cap.h`: retained public surface, not dead code.

**No code was changed in S1.** The commands above are reproducible and could seed a
future closure gate if desired. Next slice: S2a (consolidate only the verbatim
`feature.c` / `flavor.c` checksum comparison).
