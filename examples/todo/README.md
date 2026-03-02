# Todo App

Full-featured todo application with user auth, sessions, CSRF protection, HTML templates, and static file serving. Demonstrates most Hull features in a single app.

Both Lua (`app.lua`) and JavaScript (`app.js`) backends are provided with identical functionality.

## Quick Start

```bash
make dev                    # https://localhost:8443 (Lua, hot reload)
make dev ENTRY=app.js       # JS backend
```

## Build & Run

```bash
make prod                   # native production binary
make prod CC=cosmocc        # portable APE binary (runs on Linux + macOS + Windows)
make run                    # run the production binary
make run ENTRY=app.js       # run with JS backend
```

## All Targets

| Target | Description |
|--------|-------------|
| `dev` | Dev server with hot reload + HTTPS |
| `prod` | Build standalone production binary |
| `run` | Run the production binary |
| `test` | Run app tests (`hull test .`) |
| `certs` | Generate self-signed TLS certs |
| `clean` | Remove production binary |

## Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `ENTRY` | `app.lua` | Entry point (`app.lua` or `app.js`) |
| `CC` | `cc` | Compiler (`cc`, `gcc`, `cosmocc`) |
| `PORT` | `8443` | Server port |
| `DB` | `/tmp/todo.db` | SQLite database path |

## Cosmo Builds

[Cosmopolitan](https://cosmo.zip) produces Actually Portable Executable (APE) binaries that run on Linux, macOS, Windows, and BSDs from a single file.

```bash
# Install cosmocc (one-time)
mkdir -p ~/cosmocc && cd ~/cosmocc
curl -sLO https://cosmo.zip/pub/cosmocc/cosmocc.zip && unzip cosmocc.zip

# Add to PATH (also add to ~/.zshrc or ~/.bashrc)
export PATH="$HOME/cosmocc/bin:$PATH"

# Clean build (recommended for first cosmo build)
make -C ../.. clean && make -C ../../vendor/keel clean
make prod CC=cosmocc
```

The `platform-cosmo` target uses `x86_64-unknown-cosmo-cc` and `aarch64-unknown-cosmo-cc` directly, so the full `~/cosmocc/bin` must be in PATH — not just `cosmocc` itself.

A clean build is recommended when switching between native and cosmo compilers to avoid stale object files from a different toolchain.

## TLS Certificates

`make dev` and `make run` auto-generate self-signed certs in `certs/` on first run. To regenerate:

```bash
rm -rf certs && make certs
```

## File Structure

```
app.lua          # Lua backend
app.js           # JavaScript backend (same functionality)
templates/       # HTML templates (shared by both backends)
static/          # Static assets (CSS, JS)
tests/           # App tests (run via hull test)
certs/           # Auto-generated TLS certs (gitignored)
```
