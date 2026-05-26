# Hull tools — `hull tools install`

Status: **Shipped** (v0.1.2) | Tracked in: [`roadmap_next.md`](roadmap_next.md)

This is the design for shipping **optional Hull-native tools** as
separately-downloadable, separately-installable artefacts covered by
the same Ed25519 release signature that protects the `hull` binary
itself.

The first concrete user is `wamrc` (WAMR AOT compiler), but the
machinery is generic — adding a new tool means publishing one
extra entry in `hull.sha256` and one line in the tool registry.

---

## 1. Why side-loaded tools, not bundled

The `hull` binary is one cohesive product — runtimes, capability layer,
sandbox, HTTP, DB, WASM. Everyone gets the same surface. The optional
extras (`wamrc`, `wgpu-native`, future stuff) serve narrow audiences:

| Tool | Audience | Cost if bundled | Cost if missed |
|---|---|---|---|
| `wamrc` | Compute-heavy WASM workloads (ML, image pipelines, tight loops) | +~150 MB to hull binary (LLVM-backed AOT compilation) | Modules run via fast interpreter, ~50× slower than AOT for the few workloads where it matters |
| `wgpu-native` | GPU compute (ML inference, large parallel) | +~12 MB + Vulkan/Metal runtime dep | Out of scope for v0.1.2 (needs runtime dlopen — separate design) |

Bundling either into the main hull binary would inflate it for the 80%
who don't need them. Side-loading is the clean separation.

The trust chain stays identical: tools are listed in `hull.sha256`, so
the same Ed25519 signature that authorises a hull binary download also
authorises a tool download. Zero new keys, zero new signature formats.

## 2. Surface

```
hull tools list                 list available tools + their install state
hull tools list --json          machine-readable, for agents / hull doctor
hull tools install <tool>       download + verify + install (or upgrade)
hull tools install --all        install everything published for this hull
hull tools uninstall <tool>     remove a tool
```

`hull doctor` consults the same registry to render the WASM AOT row.

### CLI naming rationale

Verb-namespace structure (`hull tools <verb>`) was preferred over the
flatter `hull install <tool>` so the surface is unambiguously
hull-native: nobody will mistake `hull tools install` for an
application-package operation. The tradeoff (one extra word) buys
clear semantics and room for future operations
(`hull tools sync`, `hull tools verify`, `hull tools migrate`) without
crowding the top-level command space.

## 3. On-disk layout

Tools live under a hull-owned directory; they are NOT placed on the
user's PATH alongside system binaries:

```
~/.hull/
├── keys/                       (existing — release/platform keys, maintainer-only)
│   ├── release.{key,pub}
│   └── platform.{key,pub}
└── tools/                      (new)
    └── wamrc                   mode 0755, executable binary
```

Hull invokes tools by absolute path (`$HOME/.hull/tools/wamrc`), so
nothing else on the system ever depends on a Hull-managed binary
being on PATH. Uninstall is mechanically just `rm`.

### Why isolated instead of next-to-hull

`~/.local/bin/` is the user's general PATH directory; it carries every
binary the user installs from anywhere. Treating it as Hull's tool
store means:

- `which wamrc` could resolve to a Hull-managed or system-managed
  binary depending on PATH order. Brittle.
- `hull tools uninstall wamrc` would need to be careful not to remove
  a user-installed wamrc that happens to live in the same directory.
- Users who never wanted Hull on their PATH at all (e.g. invoke it
  via `~/.hull/hull`) shouldn't have Hull's tools polluting it.

`~/.hull/tools/` solves all three: the directory is hull's, only hull
writes there, only hull reads from there.

### Lookup order

When hull needs `wamrc` (e.g. for AOT compilation during
`hull build`):

1. `$HOME/.hull/tools/wamrc` — the canonical install location
2. Same directory as the running hull binary (`dirname(argv[0])/wamrc`)
   — convenience for ejected/portable installs
3. `wamrc` on `PATH` — for users who already have it via their distro
   or `brew install wamr`
4. Error: clear hint to run `hull tools install wamrc`

The fallbacks (2) and (3) mean self-built / system-packaged wamrc
keeps working. The new directory is just the preferred location.

## 4. Release pipeline

### New CI matrix job

`.github/workflows/release.yml` gains a `build-wamrc` job, running
in parallel with `build-native`:

```yaml
build-wamrc:
  name: Build wamrc (${{ matrix.name }})
  runs-on: ${{ matrix.os }}
  strategy:
    fail-fast: false
    matrix:
      include:
        - { name: linux-x86_64,  os: ubuntu-latest,    artifact: hull-wamrc-linux-x86_64 }
        - { name: linux-aarch64, os: ubuntu-24.04-arm, artifact: hull-wamrc-linux-aarch64 }
        - { name: darwin-arm64,  os: macos-latest,     artifact: hull-wamrc-darwin-arm64 }
  steps:
    - uses: actions/checkout@v4
      with: { submodules: true }

    - name: Install LLVM + cmake (Linux)
      if: runner.os == 'Linux'
      run: |
        sudo apt-get update
        sudo apt-get install -y llvm-18-dev cmake

    - name: Install LLVM + cmake (macOS)
      if: runner.os == 'macOS'
      run: brew install llvm@18 cmake

    - name: Build wamrc
      run: |
        export WAMRC_CMAKE_FLAGS="-DLLVM_DIR=$(llvm-config --cmakedir)"
        make wamrc
        strip build/wamrc 2>/dev/null || true

    - name: Rename artifact
      run: cp build/wamrc ${{ matrix.artifact }}

    - uses: actions/upload-artifact@v4
      with:
        name: ${{ matrix.artifact }}
        path: ${{ matrix.artifact }}
```

### Release job changes

```yaml
release:
  needs: [build-cosmo, build-native, build-wamrc]   # add build-wamrc
  ...
  - name: Flatten artifacts
    run: |
      mkdir -p dist
      mv artifacts/hull-cosmo/hull-cosmo                          dist/
      mv artifacts/hull-linux-x86_64/hull-linux-x86_64            dist/
      mv artifacts/hull-linux-aarch64/hull-linux-aarch64          dist/
      mv artifacts/hull-darwin-arm64/hull-darwin-arm64            dist/
      mv artifacts/hull-wamrc-linux-x86_64/hull-wamrc-linux-x86_64    dist/
      mv artifacts/hull-wamrc-linux-aarch64/hull-wamrc-linux-aarch64  dist/
      mv artifacts/hull-wamrc-darwin-arm64/hull-wamrc-darwin-arm64    dist/

  - name: Compute SHA-256 checksums
    working-directory: dist
    run: |
      sha256sum hull-cosmo hull-linux-x86_64 hull-linux-aarch64 hull-darwin-arm64 \
                hull-wamrc-linux-x86_64 hull-wamrc-linux-aarch64 hull-wamrc-darwin-arm64 \
                > hull.sha256

  ...
  assets+=(dist/hull-wamrc-linux-x86_64 dist/hull-wamrc-linux-aarch64 dist/hull-wamrc-darwin-arm64)
```

### No cosmo wamrc

`wamrc` links LLVM, which doesn't reasonably fit into a cosmocc fat
APE binary (LLVM is too large and platform-specific). Cosmo users who
want AOT do `make wamrc` from source. `hull tools install wamrc` on
a cosmo install reports "no wamrc binary published for cosmo —
build from source: `make wamrc`".

## 5. Trust chain

Identical to the hull-update chain:

1. Tool binaries land in `hull.sha256` alongside the hull binaries.
2. `hull.sha256.sig` is an Ed25519 signature over the manifest bytes,
   signed by the offline release key (`HULL_RELEASE_KEY` GitHub
   secret).
3. `hull tools install` reuses `hl_release_verify_manifest_sig()` to
   verify the signature against `HL_RELEASE_PUBKEY_HEX`.
4. Per-tool SHA-256 verification uses `crypto.subtle.digest`
   equivalent in C (already wired for `hull update`).

No new keys, no new ceremonies, no new tooling. The same browser
verifier at `https://gethull.dev/verify.html` already covers any
download in `hull.sha256`.

## 6. Install flow

```
hull tools install wamrc
```

1. Determine running hull's version (`HL_VERSION`) and platform
   (`update_platform()`).
2. Compute asset name: `hull-wamrc-<platform>`. Skip cosmo.
3. Fetch release metadata from
   `api.github.com/repos/artalis-io/hull/releases/tags/v<VERSION>`
   (NOT `latest` — we want the wamrc that matches the running hull).
4. Download `hull.sha256` + `hull.sha256.sig`.
5. Verify signature via `hl_release_verify_manifest_sig()`.
6. Locate the asset entry in the parsed manifest. If absent for this
   platform: "no wamrc binary published for hull v<VERSION> on
   <platform>".
7. Download the asset to `$HOME/.hull/tools/wamrc.tmp` over HTTPS via
   the embedded CA bundle.
8. SHA-256 verify against the manifest entry.
9. `chmod 0755 wamrc.tmp && rename(2) wamrc.tmp wamrc`.
10. Print: `installed wamrc (~/.hull/tools/wamrc, X MB, v<VERSION>)`.

### Version coupling

`hull tools install` always pulls from the SAME release as the running
hull binary, not `latest`. This way `wamrc` stays at the WAMR commit
the hull binary was compiled against — no ABI / canary / module-format
drift.

Upgrade path:
- `hull update` → new hull version. The locally-installed wamrc is now
  stale (built against old WAMR).
- `hull doctor` post-update shows `wamrc: ⚠  outdated (installed
  v0.1.2, hull is v0.1.3 — run `hull tools install wamrc` to refresh)`.
- `hull tools install wamrc` re-pulls the matching version, atomic
  rename, done.

(Future polish: `hull update` could auto-upgrade installed tools,
gated by a `--with-tools` flag. v0.1.3+, not v0.1.2.)

## 7. `hull tools list`

Human output:
```
Available tools for hull 0.1.2 on linux-aarch64:

  wamrc      ✓  installed   2.6 MB at ~/.hull/tools/wamrc
                WAMR AOT compiler — produces compute/*.aot.<arch>
                modules for ~50× speedup vs interpreter on
                compute-heavy WASM.

(Run `hull tools install <name>` to install. `--all` to install
everything. `--json` for machine-readable output.)
```

For an empty install:
```
  wamrc      ○  not installed
                hint: `hull tools install wamrc`
```

JSON output (for agents and `hull doctor`'s reuse):
```json
{
  "tools": [
    {
      "name": "wamrc",
      "description": "WAMR AOT compiler for compute/*.wasm modules",
      "available": true,                  // published for this platform
      "installed": true,
      "path": "/home/user/.hull/tools/wamrc",
      "installed_version": "0.1.2",       // hull version the binary came from
      "current_hull_version": "0.1.2",
      "outdated": false,
      "size_bytes": 2620416,
      "asset_name": "hull-wamrc-linux-aarch64"
    }
  ]
}
```

## 8. `hull doctor` update

Today's WASM section:
```
Compute (WASM)  (compute/<name>.wasm modules)
  runtime     ✓  WAMR enabled
  wamrc       ✗  not found in PATH or next to hull
                hint: `make wamrc` (requires cmake + LLVM)
                without wamrc, modules run via the fast interpreter
                (~50x slower than AOT for compute-heavy work)
```

After:
```
Compute (WASM)  (compute/<name>.wasm modules)
  runtime     ✓  WAMR enabled
  wamrc       ○  not installed
                hint: `hull tools install wamrc` (downloads ~3 MB
                AOT compiler signed against the release key)
                without wamrc, modules run via the fast interpreter
                (~50x slower than AOT for compute-heavy work)
```

When installed:
```
  wamrc       ✓  installed at ~/.hull/tools/wamrc (v0.1.2, 2.6 MB)
```

When mismatched after `hull update`:
```
  wamrc       ⚠  outdated (installed v0.1.2, hull is v0.1.3)
                hint: `hull tools install wamrc`
```

## 9. Implementation tasks

In order:

1. **`hl_tools_dir()` helper** in `src/hull/tools_install.c` (new file).
   Resolves `$HOME/.hull/tools/`, creates with `0755` if absent.
2. **`hl_tools_install_lookup_path(name, out, size)`** — implements the
   four-step lookup order above. Used by `cap/wasm.c` for the wamrc
   resolution path.
3. **`hl_cmd_tools()`** in `src/hull/commands/tools.c` — dispatcher for
   `install`, `list`, `uninstall`. Reuses
   `hl_release_verify_manifest_sig()` and the keel HTTPS client
   already imported by `hull update`.
4. **Tool registry** — static array of `HlTool` entries (name,
   description, asset-name template, platforms supported). Currently
   one entry: wamrc.
5. **Doctor integration** — update `commands/doctor.c` WASM section to
   render the new states (installed / outdated / not installed) using
   the registry.
6. **Release workflow** — add the `build-wamrc` matrix, extend the
   release job's flatten / sha256 / gh-release-create steps.
7. **e2e test** `tests/e2e_tools.sh` — install wamrc (against a
   mock release endpoint or skipped if `--skip-network`), verify
   the binary works (`wamrc --help`), uninstall, verify removed.
8. **Docs** — this file + a new section in `CLAUDE.md` ("Tools and
   side-loading"), bump the roadmap entry from "planned" to "shipped".

Out of scope for v0.1.2 (explicit non-goals):

- **wgpu-native**: requires runtime `dlopen` architecture change.
  Tracked separately.
- **Cosmo wamrc**: LLVM doesn't fit cleanly into cosmocc fat binaries.
  Cosmo users build from source.
- **System-wide (`/usr/local/lib/hull/tools/`) installation**: stay
  user-scoped for v0.1.2.
- **`hull update --with-tools`** (auto-refresh installed tools on hull
  upgrade): polish for v0.1.3+.
- **Third-party tools**: only first-party (gethull.dev-signed) tools
  install via this path. Third-party Hull modules continue to use the
  module-declaration system in app manifests.

## 10. Out-of-scope: what this is NOT

- **NOT a package manager for application dependencies.** Hull apps
  declare their `manifest.modules` from the first-party stdlib
  registry; that's a separate, source-baked-in mechanism. `hull
  tools install` is for hull-native tools that are too platform- or
  size-sensitive to bundle in the main binary.
- **NOT a way to ship out-of-band patches.** The `hull` binary itself
  is updated via `hull update`. Tools are independent extras.
- **NOT a way to extend the capability surface.** Tools don't bypass
  the sandbox or add new `hl_cap_*` entry points. They're invoked as
  subprocesses (wamrc) or future-`dlopen`'d libraries (wgpu) called
  through existing capability code paths.

## 11. Cross-references

- Trust chain + release signing: [`release_signing.md`](release_signing.md)
- Roadmap entry: [`roadmap_next.md`](roadmap_next.md) (section to be
  added when this lands).
- Browser verifier (covers tool downloads since they're in
  `hull.sha256`): `site/verify.html` — Release Binary section.
