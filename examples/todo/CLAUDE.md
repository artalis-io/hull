# Todo Example — Development Guide

## Build Commands

```bash
make dev                    # dev server, hot reload, HTTPS (Lua)
make dev ENTRY=app.js       # dev server with JS backend
make prod                   # native production binary
make prod CC=cosmocc        # portable APE binary
make run                    # run production binary
make test                   # run app tests
```

## File Structure

- `app.lua` / `app.js` — Dual backends with identical functionality
- `templates/` — HTML templates (shared by both backends)
- `static/` — Static assets
- `tests/` — App tests run via `hull test .`
- `certs/` — Auto-generated self-signed TLS certs (gitignored)

## Architecture

- Database schema (sessions, users, todos) is defined inline in the app files
- Auth uses bcrypt password hashing + secure session cookies
- CSRF protection via per-session tokens
- Templates use Hull's built-in template engine

## Cosmo Builds

Cosmo builds require `~/cosmocc/bin` in PATH (`export PATH="$HOME/cosmocc/bin:$PATH"`) — the root Makefile calls `x86_64-unknown-cosmo-cc` and `aarch64-unknown-cosmo-cc` directly.

Clean build when switching toolchains (avoids stale objects):
```bash
export PATH="$HOME/cosmocc/bin:$PATH"
make -C ../.. clean && make -C ../../vendor/keel clean
make prod CC=cosmocc
```

`make prod CC=cosmocc` handles the full cosmo workflow:
1. Builds `platform-cosmo` from root (if archives missing)
2. Rebuilds hull binary (platform-cosmo wipes it)
3. Runs `hull build --cc cosmocc -o todo .`

## Variables

Override via make: `make dev PORT=9443 DB=./local.db ENTRY=app.js`
