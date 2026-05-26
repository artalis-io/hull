<!-- minimal -->
## Side-loaded Hull tools (`hull tools install`)

Hull ships some optional executables (currently: `wamrc`, the WAMR
AOT compiler) as separately-downloadable artifacts. They install to
`$HOME/.hull/tools/` under the same Ed25519-signed release manifest
that protects the `hull` binary itself.

```bash
hull tools list                  # see registry + install state
hull tools install wamrc         # download, verify, install
hull tools uninstall wamrc       # remove
hull tools install --all         # install everything for this platform
```

### Why side-load instead of bundle

Bundling `wamrc` into the main binary would add ~150 MB (LLVM
backend). Most users don't need AOT — modules run via Hull's fast
WASM interpreter just fine. AOT is ~50× faster than interpreter for
compute-heavy workloads, but only matters if you're doing
compute-heavy work.

If `hull doctor` reports
`wamrc ○ not installed` and you have `compute/*.wasm` modules that
matter for performance, install it.

### Agent introspection

```bash
hull agent tools                 # JSON registry + install state per tool
hull agent compute [app_dir]     # includes wamrc state per app
```

The compute output's `wamrc` block tells an agent whether to
recommend `hull tools install wamrc` when it sees modules with no
AOT artifacts.

<!-- compact -->
## Trust chain

Identical to `hull update`:

1. Tool binaries are listed in the same `hull.sha256` release
   manifest as the hull binary.
2. `hull.sha256.sig` is an Ed25519 signature over the manifest
   bytes.
3. `hull tools install` downloads the asset over HTTPS using the
   embedded Mozilla CA bundle, verifies the manifest signature
   against the embedded `HL_RELEASE_PUBKEY_HEX`, then verifies the
   asset's SHA-256 (constant-time compare) against the manifest.
4. Atomic install via `rename(2)` — no half-written files.

No new keys, no new ceremonies. Same key that signs `hull` itself
signs the tool binaries.

## Version coupling

`hull tools install` always pulls from the SAME release as the
running hull binary, not "latest". This way `wamrc` stays at the
WAMR commit hull was compiled against — no ABI / canary / module-
format drift.

Upgrade path:
```bash
hull update                      # → new hull version
hull tools install wamrc         # → re-pull to match
```

## What gets where

```
~/.hull/
  tools/
    wamrc      ← mode 0755, executable binary
```

Hull invokes tools by absolute path (`$HOME/.hull/tools/wamrc`), so
nothing on your `$PATH` is touched. Uninstall is mechanically just
`rm $HOME/.hull/tools/<name>`.

### Lookup order

When hull needs a tool (e.g. `wamrc` for AOT compilation during
`hull build`):

1. `$HOME/.hull/tools/<name>` — canonical install location
2. `dirname(hull_exe)/<name>` — for ejected / portable installs
3. `<name>` on `$PATH` — for users who have it via distro / brew

The fallbacks mean a system-managed `wamrc` keeps working;
`$HOME/.hull/tools/` is just the preferred location.

## Registered tools

Run `hull agent tools` for current state. Today:

| Tool | Purpose | Platforms |
|---|---|---|
| `wamrc` | WAMR AOT compiler | linux-x86_64, linux-aarch64, darwin-arm64 |

Cosmo is unsupported for tools that need LLVM (LLVM doesn't fit a
fat APE binary). Cosmo users: `make wamrc` from source.

## When you need wamrc

You need wamrc if BOTH are true:

- You have `compute/*.wasm` modules in your app, AND
- You care about ~50× speedup on hot paths

You DON'T need wamrc for development — the WAMR interpreter is fast
enough for everything except production-grade compute. `hull build`
auto-AOT-compiles when wamrc is available and silently skips
otherwise.

To check: `hull doctor` → "Compute (WASM)" → wamrc row.

## Failure modes

| Error | Cause | Fix |
|---|---|---|
| `failed to download checksum manifest (404)` | No release with this tag (dev build) | Build against a tagged release, or `hull update` first |
| `release signature verification FAILED` | Manifest tampered with on GitHub | Don't install; report to release maintainer |
| `SHA-256 mismatch` | Transport corruption or middle-box meddling | Retry; persistent → MITM / corruption |
| `no checksum entry for hull-<tool>-<platform> in hull.sha256` | Tool not published for this OS/arch | `make <tool>` from source if available |
| `unknown tool 'X'` | Typo or not in the registry | `hull tools list` |

<!-- full -->
## CI / hermetic builds

For CI environments that need wamrc deterministically:

```yaml
- name: Install wamrc
  run: hull tools install wamrc
- name: Build app
  run: hull build .
```

This pulls the exact `wamrc` version your `hull` binary expects, so
AOT codegen is reproducible across CI runs. Pin the hull version
itself with `HULL_VERSION=0.1.2 sh install.sh`.

## Adding a new tool (maintainer reference)

Three edits required:

1. **Registry entry** in `src/hull/tools_install.c`:
   ```c
   {
       .name              = "your-tool",
       .description       = "...",
       .has_linux_x86_64  = 1,
       .has_linux_aarch64 = 1,
       .has_darwin_arm64  = 1,
       .has_cosmo         = 0,
   },
   ```

2. **CI matrix** in `.github/workflows/release.yml` — add a
   `build-your-tool` job that produces `hull-your-tool-<platform>`
   artifacts on each platform, and extend the release job's flatten
   / sha256 / asset list.

3. **Optional: subsystem panel callout** — if the tool serves a
   specific subsystem (e.g. wamrc → compute), add a state block to
   that subsystem's `hl_agent_*` reporter so agents see actionable
   hints without separately querying the tool registry.

The registry itself is the trust boundary: every tool name passing
through `hl_tools_install_path()` / `hl_tools_lookup_path()` is
restricted to `[A-Za-z0-9_-]+`, so adding an entry won't open path-
traversal vectors.

## Why this design over alternatives

- **vs bundling everything**: keeps the base binary lean (~5 MB)
  for the 80% who don't need optional tools.
- **vs PATH-managed external installs**: tool versions stay locked
  to the hull binary that wants them — no ABI drift, no "works on
  my machine".
- **vs a separate package manager**: reuses the existing release
  trust chain; one signed manifest covers everything.
- **vs `dlopen` shared libraries**: tools are subprocesses
  (`wamrc input.wasm output.aot`), not loaded into the hull address
  space. Failures stay isolated; tools can be any language.

## Release-time smoke test

`tests/release_smoke.sh` runs `hull tools install wamrc` against
the just-published release as part of the post-release validation.
That's the only end-to-end test of the live install path; the unit
tests (`test_tools_install.c`, `test_release_io.c`) cover the
trust/IO primitives, and `tests/e2e_tools.sh` covers everything
except the actual GitHub download.

## See also

- `hull agent context --task=compute` — when AOT actually matters
- `hull agent context --task=build` — `hull build` flags incl. `--no-aot`
- `docs/tools_install.md` — full design doc
- `docs/release_signing.md` — the shared trust chain
