# Platform-sig over composed features — design

**Status:** design (not implemented). Closes roadmap
[roadmap_next.md §0.4](roadmap_next.md). Extends the inner platform-signature
layer so a composed app's `package.sig` attests **every** archive linked into
it, not just `libhull_platform.a`.

## The gap

Today `hull build` gives an app two signature layers in `package.sig`
(see [security.md](security.md)):

- **platform layer** (inner, gethull-owned): proves the **`libhull_platform.a`**
  the app was built against is gethull.dev-built. Mechanism: a per-arch manifest
  `<sha256>  <arch>` signed by the **platform key** (`HL_PLATFORM_PUBKEY_HEX`),
  embedded in `hull` (`embedded_platform_sig.h`), cross-checked at compose, and
  recorded in `package.sig.gethull` so runtime `--verify-sig` re-attests it.
- **app layer** (outer, developer-owned): proves the app bytes weren't tampered.

Since #113 (runtime featurify) and #114 (HTTP as a feature), a plain `hull build`
composes **more than the platform lib**: exactly one runtime archive, the HTTP
core + per-runtime web bindings (when the app needs HTTP), a per-runtime tui
bridge (`--with=tui`), and any `--with=` feature (duckdb/gpu/postgres/mysql).
**None of these are in the platform manifest**, so the inner-layer "provably
gethull-built" guarantee stops at the platform lib. Authenticity is still
anchored elsewhere (below), so this is a **defense-in-depth completion, not a
live vulnerability** — but the second signature layer is incomplete for a
composed binary.

Where each composed archive's authenticity is anchored **today**:

| Archive | Source | Anchored by (today) | In `package.sig`? |
|---|---|---|---|
| `libhull_platform.a` | embedded in `hull` | platform manifest (platform key) | **yes** (`gethull`) |
| `libhull_feature-{lua,js}.a` | embedded in `hull` | the hull binary's own release signature (`hull.sha256`) | no |
| `libhull_feature-http[-<rt>].a` | embedded in `hull` | " | no |
| `libhull_feature-tui-<rt>.a` | embedded in `hull` | " | no |
| `libhull_feature-<name>.a` (`--with`) | `~/.hull/feature/` | release sig at `hull feature install` + compose re-verify (`hl_release_io_verify_local_asset`) | no |
| `libhull_platform-<flavor>.a` (`--flavor`) | `~/.hull/platform/` | release sig at `hull flavor install` + compose re-verify | no |

## Principle

**Every archive linked into the app must have its SHA-256 present in a
gethull-signed manifest, and `package.sig` must record (signed manifest,
signature, composed hashes) so `--verify-sig` re-attests the full composed set
and refuses to boot on any mismatch.**

Decisions taken (2026-07-27): **cover every composed archive** (embedded +
externally-installed), and make a missing/mismatched attestation **fatal** under
`--verify-sig` (same fail-closed posture as the platform layer).

## Two signing domains

There is no single signed manifest that already lists everything, because there
are two signing keys with two purposes:

1. **What ships *inside* `hull`** — the platform lib AND the embedded feature
   archives (runtime, http core, web bindings, tui bridges). Signed by the
   **platform key** at release (the `sign-platform-manifest` job). Today this
   manifest covers only the platform lib; this design **extends it** to also
   carry the embedded feature-archive hashes, per arch.
2. **What is *separately installed*** — `--with` feature libs and `--flavor`
   platform libs. Already in the **release manifest** (`hull.sha256`, signed by
   the **release key**). Nothing new to publish; the design just **records** the
   composed entries in `package.sig`.

`package.sig.gethull` grows a `composed` block that carries an attestation from
whichever domain each archive belongs to. Both anchors are already embedded
pubkeys (`HL_PLATFORM_PUBKEY_HEX`, `HL_RELEASE_PUBKEY_HEX`), so no new key.

## Release-side changes

**Extend the platform manifest to an "embedded-archive manifest."** Reuse the
`hl_platform_sig_*` machinery (canonical sorted `<sha256>  <name>` lines, platform
key). Today it emits one line per arch for `libhull_platform.a`; extend the
`sign-platform-manifest` job (`.github/workflows/release.yml`) to also hash and
list, per arch, every embedded feature archive that ships in that release's
`hull`:

```
<sha256>  libhull_platform.<arch>.a
<sha256>  libhull_feature-lua.<arch>.a
<sha256>  libhull_feature-js.<arch>.a
<sha256>  libhull_feature-http.<arch>.a
<sha256>  libhull_feature-http-lua.<arch>.a
<sha256>  libhull_feature-http-js.<arch>.a
<sha256>  libhull_feature-tui-lua.<arch>.a
<sha256>  libhull_feature-tui-js.<arch>.a
```

Signed as one blob per the existing scheme; embedded in `hull` alongside the
current platform-sig (`embedded_platform_sig.h` becomes an embedded-archive
manifest). The bytes hashed here MUST be **byte-identical** to what
`build_assets.c` extracts from the embedded copy — the manifest is generated from
the same archives the embed step consumes, so the reproducible-build chain keeps
them in lockstep (same discipline as the platform lib today). The `--with`
feature libs and `--flavor` libs stay covered by `hull.sha256` (unchanged).

## Compose-side changes (`hull build`)

For each archive it links, the build records an attestation entry, sourced by
domain:

- **Embedded archives** (platform + runtime + http + web + tui bridges): after
  extracting each from the embedded copy, look up its expected hash in the
  embedded-archive manifest (`hl_platform_sig_extract_for_arch`, generalized from
  one asset to N), cross-check the extracted bytes' SHA-256, and record
  `{name, sha256, domain: "platform"}`. A mismatch fails the build (the extracted
  archive isn't what gethull signed).
- **Externally-installed archives** (`--with` / `--flavor`): the compose already
  re-verifies the cache-sourced lib against the release-signed `hull.sha256` via
  `hl_release_io_verify_local_asset` (`tool.platform_verify`). Extend it to
  *return* the matched `{name, sha256}` and record `{name, sha256, domain:
  "release"}`, plus (once) the cached `hull.sha256` + `.sig` bytes.

`package.sig.gethull` gains:

```jsonc
"gethull": {
  // existing platform layer stays as-is for back-compat
  "manifest": "<embedded-archive manifest text>",   // now multi-line
  "signature": "<platform-key sig hex>",
  "arch_hashes": { "<arch>": "<platform-lib sha256>" },  // kept; platform lib

  // NEW: the full composed set
  "composed": {
    "platform_domain": {                 // verified vs HL_PLATFORM_PUBKEY_HEX
      "manifest": "<embedded-archive manifest text>",
      "signature": "<hex>",
      "assets": [ { "name": "libhull_feature-lua.<arch>.a", "sha256": "..." }, ... ]
    },
    "release_domain": {                  // verified vs HL_RELEASE_PUBKEY_HEX
      "manifest": "<hull.sha256 text>",
      "signature": "<hex>",
      "assets": [ { "name": "libhull_feature-duckdb-<arch>.a", "sha256": "..." }, ... ]
    }
  }
}
```

Covered by the app-layer signature (like the rest of `package.sig`), so the
developer's outer signature also seals the composed attestation against tamper.

## Runtime (`--verify-sig`)

Extend the existing platform-sig check (`src/hull/serve.c`, run before sandbox
phase 2). For each domain in `gethull.composed`:

1. Verify `manifest`'s `signature` against the embedded pubkey for that domain
   (`HL_PLATFORM_PUBKEY_HEX` / `HL_RELEASE_PUBKEY_HEX`).
2. For each `assets[]` entry, confirm its `sha256` appears in that verified
   `manifest`.

**Any failure is fatal**: log a `[sig]` error and exit non-zero before serving,
identical to the platform-layer posture. `--no-verify-platform` still bypasses
the whole gethull layer (dev builds, forks). Pre-v0.1.0 all-zero placeholder
pubkeys skip verification with the existing one-time warning.

Note the guarantee is an **attestation**, not a re-hash: the runtime can't re-hash
archives already linked into its own image, exactly as the platform layer works
today. It proves "this binary was built composing archives whose hashes match a
gethull-signed manifest," and the outer app signature proves the attestation
itself wasn't altered.

## Cosmo

The cosmo (fat APE) base compiles both runtimes + HTTP in, so it composes **no**
embedded feature archives — its `composed.platform_domain` degenerates to the
platform lib only (dual-arch, as today). `--with` features are native-only, so
`composed.release_domain` is empty on cosmo. No cosmo-specific work beyond the
existing dual-arch platform-sig handling.

## Effort + risk

**Moderate.** It reuses every existing primitive (the `hl_platform_sig_*` codec,
`hl_release_io_verify_local_asset`, the `package.sig` gethull block, the runtime
`--verify-sig` path) — the work is *extending* them from "one platform lib" to "N
archives across two domains," plus the release-job manifest extension. No new
key, no new crypto, no new trust root.

- **Release (`release.yml`, platform-sig job):** hash + list the embedded feature
  archives per arch. ~contained.
- **Compose (`build.lua` + `platform_sig.c` + `release_io.c`):** generalize the
  extract/cross-check to N assets; thread the `{name, sha256}` results into the
  `package.sig` writer. Moderate, mechanical.
- **Runtime (`serve.c` + `platform_sig.c`):** loop the two domains; parse +
  verify. Contained.

Main risk is **byte-identity discipline**: the release-signed embedded-archive
hashes must equal what the build extracts and what gets linked. The reproducible
build chain + the existing platform-lib precedent (already solved for
`libhull_platform.a`) contain it; the new archives just join the same pipeline.

## Test plan

- Unit (`test_platform_sig` / `test_signature`): multi-asset manifest build/parse
  /verify; a mismatched asset hash fails.
- E2E: build a `--with=duckdb` app + a plain HTTP app + a pure-compute CLI, run
  each with `--verify-sig <pubkey>` and assert boot; then corrupt one composed
  archive's recorded hash in `package.sig` and assert a fatal `[sig]` refusal.
  (Mirror `tests/e2e_build.sh`'s existing platform-sig assertions.)
- Release smoke (`release_smoke.sh`): a live-installed `--with=<feature>` build,
  run with `--verify-sig`, asserts the composed feature is attested.

## Relationship to §0.5 (composed-app SBOM)

Orthogonal but complementary: §0.5 makes a composed app's **SBOM** enumerate its
`--with` features (transparency); this makes `package.sig` **attest** them
(provenance). Both read the same compose-time feature identity
(`FEATURE_SPECS` / the composed asset list), so they can share the plumbing that
surfaces "what was composed" at build time.
