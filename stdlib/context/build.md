<!-- minimal -->
## Build System

`hull build` compiles a Hull application into a single self-contained binary.

```bash
hull build myapp/              # produces myapp/build/app
hull build myapp/ -o server    # custom output name
hull verify myapp/build/app    # verify Ed25519 signature
```

The binary embeds: app code, templates, static files, migrations, stdlib, SQLite, and the HTTP server. No runtime dependencies.

**Signing:**
```bash
hull keygen                     # generate Ed25519 keypair
hull build myapp/ --sign        # sign with default key
hull verify myapp/build/app     # verify signature
```

<!-- compact -->
## Build Pipeline

1. Discovers all app files (`.lua`/`.js`, `templates/`, `static/`, `migrations/`)
2. Generates `app_registry.c` with embedded file data (sorted by name for VFS binary search)
3. Generates `app_main.c` entry point
4. Compiles with system C compiler and links against `libhull_platform.a`
5. Optionally signs the binary with Ed25519

## Platform Library

The platform library (`libhull_platform.a`) contains all vendored dependencies. Build it once:

```bash
make platform                   # build libhull_platform.a
```

When hull has an embedded platform (`EMBED_PLATFORM=1`), `hull build` extracts it automatically. Otherwise it looks in the `build/` directory next to the hull binary.

## Signing and Verification

**Dual-layer Ed25519 signatures:**
- **Platform layer (inner):** Signed by the Hull distribution key. Proves platform library is authentic.
- **App layer (outer):** Signed by developer key. Proves app hasn't been tampered with.

```bash
hull keygen                     # creates hull_key.pub + hull_key.sec
hull build myapp/ --sign        # signs with hull_key.sec
hull verify myapp/build/app     # verifies with hull_key.pub
hull inspect myapp/build/app    # show embedded metadata
```

## Cosmopolitan Builds

Build a single binary that runs on Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD:

```bash
make platform-cosmo             # build x86_64 + aarch64 platform archives
make CC=cosmocc                 # build hull as APE binary
hull build myapp/               # produces portable APE binary
```

<!-- full -->
## Self-Build Chain

Hull can build itself reproducibly:

```bash
make self-build
# hull  -> builds hull2
# hull2 -> builds hull3
# diff hull2 hull3  (should be identical)
```

## Distribution Mode

Embed the platform library in the hull binary for zero-dependency distribution:

```bash
make EMBED_PLATFORM=1           # embed native platform
make CC=cosmocc EMBED_PLATFORM=cosmo  # embed multi-arch cosmo platform
```

Users receive a single `hull` binary that can build apps without needing a separate platform library or build tools (beyond a C compiler).

## Build Output Structure

```
myapp/
  app.lua             # entry point
  templates/          # HTML templates
  static/             # static assets
  migrations/         # SQL migrations
  build/
    app               # compiled binary
    app_registry.c    # generated (embedded files)
    app_main.c        # generated (entry point)
```

## Manifest in Built Binaries

The app's manifest (capabilities declared in the entry point) is embedded in the binary and enforced by the kernel sandbox at runtime:

```lua
app.manifest = {
    fs = { "data/" },               -- allowed filesystem paths
    env = { "DATABASE_URL" },       -- allowed environment variables
    hosts = { "api.example.com" },  -- allowed HTTP hosts
}
```

## Build Flags

The compiled binary supports runtime flags:
- `--audit` — enable capability audit logging (JSON to stderr)
- `--no-sandbox` — disable kernel sandbox (debugging only)
- `--no-migrate` — skip automatic migration on startup
- `--max-instructions N` — override per-request instruction limit
