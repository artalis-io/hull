# Hull. SBOM

Software Bill of Materials. Self-describing vendored-deps manifest baked
into every hull binary, exposed as `hull sbom`.

## What it does

```sh
hull sbom                       # human-readable table (default)
hull sbom --format=json         # flat JSON object
hull sbom --format=cyclonedx    # CycloneDX 1.5 JSON
hull sbom --format=spdx         # SPDX 2.3 JSON

hull agent sbom                 # same as `hull sbom --format=json`
```

Each entry lists:

- **name**. Short identifier (`keel`, `lua`, `mozilla-ca-bundle`, etc.)
- **version**. Semver string, or empty if unknown / submodule-tracked
- **commit**. Git SHA at build time, or empty if a snapshot vendoring
- **license_spdx**. SPDX license identifier
- **url**. Upstream source URL
- **role**. One-line description of what this component does in Hull
- **embedded_blob_sha256**. SHA-256 of the embedded blob (CA bundle, etc.),
  computed at runtime from the actual bytes in the binary. Only present
  for components whose data is embedded.

## How auto-refresh works

The SBOM data table is built at compile time by `src/hull/sbom.c`.
Three sources contribute:

1. **Submodule commits.** The Makefile reads each submodule's HEAD SHA
   via `git rev-parse --short=12 HEAD` and passes them as
   `-DHULL_VENDOR_<NAME>_COMMIT="..."` defines. Current submodules:
   `keel`, `wamr`. Every `make` reads fresh; the resulting binary
   self-describes the actual vendored contents. If `git` is unavailable,
   the value falls back to `"unknown"` (build still succeeds).

2. **Snapshot versions.** Non-submodule vendored deps (Lua, QuickJS,
   SQLite, mbedTLS, TweetNaCl, pledge polyfill, etc.) have their version
   strings hardcoded in `sbom_entries[]` in `src/hull/sbom.c`. Bumping
   one means a one-line edit there alongside the actual vendor update.

3. **Build-flag gating.** Entries are wrapped in `#ifdef HL_ENABLE_*`
   guards. A `make HL_ENABLE_DB=0` build correctly omits SQLite from
   its SBOM; a build with `HL_ENABLE_GPU=1` includes wgpu-native.

Refresh cadence: **per-build**. No separate "regenerate SBOM" step.
The binary in your hand describes its own contents accurately.

## Why per-build (vs other refresh strategies)

The SBOM should describe **the binary in your hand**, not whatever git
state the developer has now. End-users don't have the source tree;
they need the binary to self-describe. Per-build auto-refresh delivers
that without any sync ceremony. Adding a new vendored dep is three
mechanical lines: `git submodule add`, one entry in `sbom_entries[]`,
one `-D` in the Makefile.

Per-release CI regeneration (alternative B) would be less responsive.
Per-vendor-update scripts (alternative C) introduce a manual sync step
that's easy to forget after a submodule bump. Per-build is the only
strategy where the binary cannot lie about its own contents.

## Embedded blob SHA-256 (tamper-detection signal)

For components whose actual bytes are embedded into the binary (Mozilla
CA bundle today; future: embedded platform sig), the SHA-256
is computed at runtime by hashing the embedded data with mbedTLS.
The first call caches the result; subsequent calls return the cached
string. This means:

- **Tamper detection**: if someone modifies the binary's embedded CA
  bundle, the SBOM's reported SHA-256 will reflect the modified bytes.
  not a stale build-time value.
- **Auditors** can compare the SBOM's SHA-256 against a known-good
  SHA-256 from upstream (e.g., the published Mozilla CA bundle).

## Architecture: orthogonal to the rest of the runtime

The SBOM module is a read-only data exporter:

- **No runtime dependencies on capabilities** (no db, fs, http, env,
  etc.). The cap layer doesn't know SBOM exists.
- **Self-contained C** in `src/hull/sbom.c` + the subcommand wrapper.
  Depends only on `cacert.h` (for embedded-blob SHA-256) and mbedTLS.
- **Compile-time gateable**: `make HL_ENABLE_SBOM=0` (when added) would
  compile out the entire feature without affecting any other subsystem.
- **No mutable global state** beyond the static caches for SHA-256
  results.

The unit-test suite (`tests/hull/test_sbom.c`) links the SBOM module
against the minimum surface (`sbom.o + cacert.o + mbedtls`). If SBOM
accidentally starts pulling in other Hull subsystems, the test link
line breaks immediately. Orthogonality canary.

## Test coverage

`tests/hull/test_sbom.c` covers:

- Entry table integrity (non-empty, hull entry present, required fields)
- Submodule commit hashes are valid lowercase hex
- Format-name parsing (known/unknown)
- All four output formats produce non-empty output
- Each format's structural invariants (JSON balance, CycloneDX top-level
  fields, SPDX top-level fields)
- Embedded-blob SHA-256 caching (two calls return identical pointer)
- Output **determinism**: calling `hl_sbom_format` twice on the same
  binary produces byte-identical output. This is the prerequisite for
  the binary in your hand and your colleague's binary to produce
  identical SBOMs that can be compared as artifacts.

14 unit tests; runs as part of `make test`.

## Adding a new vendored dep

1. `git submodule add <url> vendor/<name>` (or drop the snapshot into
   `vendor/<name>/`).
2. Add an entry to `sbom_entries[]` in `src/hull/sbom.c`:

   ```c
   {
       .name = "newdep",
       .version = "1.2.3",         /* OR leave empty for submodules */
       .commit = HULL_VENDOR_NEWDEP_COMMIT,  /* if submodule */
       .license_spdx = "MIT",
       .url = "https://example.com/newdep",
       .role = "one-line description",
       .embedded_blob_sha256 = NULL,  /* unless embedded as bytes */
   },
   ```
3. If it's a submodule, add to the Makefile:

   ```make
   HULL_VENDOR_NEWDEP_COMMIT := $(shell git -C vendor/newdep rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
   CFLAGS += -DHULL_VENDOR_NEWDEP_COMMIT=\"$(HULL_VENDOR_NEWDEP_COMMIT)\"
   ```
4. If it's optional behind a build flag, wrap the entry in `#ifdef`.

`make test` will catch missing fields via the integrity tests.

## Format choice guide

- **`human`**. Terminal output. Default. Use for ad-hoc inspection.
- **`json`**. Flat, agent-friendly. Use from scripts, dashboards, MCP.
- **`cyclonedx`**. CycloneDX 1.5 JSON. NTIA-aligned. Use when feeding
  into defense / government / regulated compliance pipelines that have
  CycloneDX consumers (most enterprise SBOM tooling does).
- **`spdx`**. SPDX 2.3 JSON. Use when the consumer is SPDX-native
  (some OSS-compliance tooling prefers SPDX).

CycloneDX and SPDX outputs include the same embedded-blob SHA-256 as
the JSON output, expressed via each spec's standard `hashes` /
`checksums` field.
