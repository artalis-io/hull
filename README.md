# Hull

Local-first application platform. Single binary, zero dependencies, runs anywhere.

Write backend logic in Lua or JavaScript, frontend in HTML5, data in SQLite. `hull build` produces a single portable executable — under 2 MB — that runs on Linux, macOS, Windows, FreeBSD, OpenBSD, and NetBSD.

## Why

AI coding assistants solved code generation. But the output is always the same: a React frontend, a Node.js backend, a Postgres database, and a cloud deployment problem. Hull is the missing piece. The AI writes Lua, `hull build` produces a single file. That file is the product. No cloud. No hosting. No dependencies.

Six vendored C libraries. One build command. One file. That's the entire stack.

## Quick Start

```bash
# Build hull
make
make test

# Create a new project
./build/hull new myapp
cd myapp

# Run in development mode (hot reload)
../build/hull dev app.lua

# Build a standalone binary
../build/hull build -o myapp .

# Run it
./myapp -p 8080 -d app.db
```

## Hull Tools

Hull ships 10 subcommands for the full development lifecycle:

| Command | Purpose |
|---------|---------|
| `hull new <name>` | Scaffold a new project with example routes and tests |
| `hull dev <app>` | Development server with hot reload |
| `hull build -o <out> <dir>` | Compile app into a standalone binary |
| `hull test <dir>` | In-process test runner (no TCP, memory SQLite) |
| `hull inspect <dir>` | Display declared capabilities and signature status |
| `hull verify [--developer-key <key>]` | Verify Ed25519 signatures and file integrity |
| `hull eject <dir>` | Export to a standalone Makefile project |
| `hull keygen <name>` | Generate Ed25519 signing keypair |
| `hull sign-platform <key>` | Sign platform library with per-arch hashes |
| `hull manifest <app>` | Extract and print manifest as JSON |

### Build Pipeline

```
Source files (Lua/JS/HTML/CSS)
        ↓
hull build: collect → generate app_registry.c → compile → link → sign
        ↓
Single binary + package.sig (Ed25519 signed)
```

The build links against `libhull_platform.a` — a static archive containing Keel HTTP server, Lua 5.4, QuickJS, SQLite, mbedTLS, TweetNaCl, and the kernel sandbox. The platform library is signed separately with the gethull.dev key.

### Cross-Platform Builds

Hull supports three compiler targets:

| Compiler | Target | Binary Type |
|----------|--------|-------------|
| `gcc` / `clang` | Linux | ELF |
| `gcc` / `clang` | macOS | Mach-O |
| `cosmocc` | Any x86_64/aarch64 | APE (Actually Portable Executable) |

Cosmopolitan APE binaries run on Linux, macOS, Windows, FreeBSD, OpenBSD, and NetBSD from a single file. Hull builds multi-architecture platform archives (`make platform-cosmo`) so the resulting APE binary is a true fat binary for both x86_64 and aarch64.

## Architecture

```
┌─────────────────────────────────────────────┐
│  Application Code (Lua / JS)                │  ← Developer writes this
├─────────────────────────────────────────────┤
│  Standard Library (stdlib/)                 │  ← cors, ratelimit, csrf, auth, jwt, session
├─────────────────────────────────────────────┤
│  Runtimes (Lua 5.4 + QuickJS)              │  ← Sandboxed interpreters
├─────────────────────────────────────────────┤
│  Capability Layer (src/hull/cap/)           │  ← C enforcement boundary
│  fs, db, crypto, time, env, http, tool      │
├─────────────────────────────────────────────┤
│  Hull Core                                  │  ← Manifest, sandbox, signatures
├─────────────────────────────────────────────┤
│  Keel HTTP Server (vendor/keel/)            │  ← Event loop + routing
├─────────────────────────────────────────────┤
│  Kernel Sandbox (pledge + unveil)           │  ← OS enforcement
└─────────────────────────────────────────────┘
```

Each layer only talks to the one directly below it. Application code cannot bypass the capability layer.

### Standard Library

Hull ships a full set of middleware and utility modules for building secure backends:

| Module | Lua | JS | Purpose |
|--------|-----|-----|---------|
| `cors` | `hull.cors` | `hull:cors` | CORS headers + preflight handling |
| `ratelimit` | `hull.ratelimit` | `hull:ratelimit` | In-memory rate limiting with configurable windows |
| `csrf` | `hull.csrf` | `hull:csrf` | Stateless CSRF token generation/verification |
| `auth` | `hull.auth` | `hull:auth` | Session-based and JWT-based authentication middleware |
| `session` | `hull.session` | `hull:session` | Server-side sessions backed by SQLite |
| `cookie` | `hull.cookie` | `hull:cookie` | Cookie parse/serialize helpers |
| `jwt` | `hull.jwt` | `hull:jwt` | JWT sign/verify (HMAC-SHA256) |
| `template` | `hull.template` | `hull:template` | HTML template engine with inheritance, includes, filters |
| `json` | `hull.json` | (built-in) | JSON encode/decode |

All middleware modules follow the same factory pattern: `module.middleware(opts)` returns a function `(req, res) -> 0|1` where `0` = continue, `1` = short-circuit.

#### Backend Best Practices

Recommended middleware stack for a typical API backend:

```lua
local cors = require("hull.cors")
local ratelimit = require("hull.ratelimit")
local auth = require("hull.auth")
local session = require("hull.session")

session.init()

-- Order matters: rate limit → CORS → auth → routes
app.use("*", "/api/*", ratelimit.middleware({ limit = 100, window = 60 }))
app.use("*", "/api/*", cors.middleware({ origins = {"https://myapp.com"} }))
app.use("*", "/api/*", auth.session_middleware({}))

app.get("/api/me", function(req, res)
    res:json({ user = req.ctx.session })
end)
```

Key principles: rate limit before auth (reject early), CORS before auth (preflight must not require credentials), scope middleware to paths (`"/api/*"` not `"/*"`). See [examples/middleware/](examples/middleware/) and [CLAUDE.md](CLAUDE.md) for full API reference.

### Vendored Libraries

| Component | Purpose |
|-----------|---------|
| [Keel](https://github.com/artalis-io/keel) | HTTP server (epoll/kqueue/io_uring/poll), routing, middleware, TLS vtable |
| [Lua 5.4](https://www.lua.org/) | Application scripting (1.9x faster than QuickJS) |
| [QuickJS](https://bellard.org/quickjs/) | ES2023 JavaScript runtime with instruction-count gas metering |
| [SQLite](https://sqlite.org/) | Embedded database (WAL mode, parameterized queries) |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | TLS client for outbound HTTPS |
| [TweetNaCl](https://tweetnacl.cr.yp.to/) | Ed25519 signatures, XSalsa20+Poly1305, Curve25519 |
| [pledge/unveil](https://github.com/jart/pledge) | Kernel sandbox (Linux seccomp/landlock) |

## Security Model

Hull apps declare a manifest of exactly what they can access — files, hosts, environment variables. The kernel enforces it.

```lua
app.manifest({
    fs = { read = {"data/"}, write = {"data/uploads/"} },
    env = {"PORT", "DATABASE_URL"},
    hosts = {"api.stripe.com"}
})
```

**Three verification points:**

| When | Tool | Checks |
|------|------|--------|
| Before download | [verify.gethull.dev](https://verify.gethull.dev) (offline browser tool) | Platform sig, app sig, canary, manifest |
| Before install | `hull verify --developer-key dev.pub` | Both signatures + file hashes |
| At startup | `./myapp --verify-sig dev.pub` | Signatures verified before accepting connections |

**Defense depth by platform:**

| Platform | Kernel Sandbox | Violation | Static Binary |
|----------|---------------|-----------|---------------|
| Linux (gcc/clang) | seccomp-bpf + Landlock | SIGKILL | No |
| Cosmopolitan APE | Native pledge/unveil | SIGKILL | Yes (no LD_PRELOAD) |
| macOS | C-level validation only | Error return | No |

See [docs/security.md](docs/security.md) for the full attack model and [docs/architecture.md](docs/architecture.md) for implementation details.

## Performance

77,000–86,000 requests/sec on a single core. ~15% overhead vs raw C (Keel baseline: 101,000 req/s). SQLite write-heavy routes sustain 19,000 req/s.

| Route | Lua 5.4 | QuickJS |
|-------|--------:|--------:|
| GET /health (no DB) | 98,531 req/s | 52,263 req/s |
| GET / (DB write + JSON) | 6,866 req/s | 4,588 req/s |
| GET /greet/:name (params) | 102,204 req/s | 57,405 req/s |

See [docs/benchmark.md](docs/benchmark.md) for methodology.

## Examples

Ten example apps in both Lua and JavaScript:

| Example | What it demonstrates |
|---------|---------------------|
| [hello](examples/hello/) | Routing, query strings, route params, body echo |
| [rest_api](examples/rest_api/) | CRUD API with JSON bodies |
| [auth](examples/auth/) | Session-based authentication (register, login, logout) |
| [jwt_api](examples/jwt_api/) | JWT Bearer authentication with refresh tokens |
| [crud_with_auth](examples/crud_with_auth/) | Task CRUD with per-user isolation |
| [middleware](examples/middleware/) | Request ID, logging, rate limiting, CORS |
| [webhooks](examples/webhooks/) | Webhook delivery with HMAC-SHA256 signatures |
| [templates](examples/templates/) | Template engine: inheritance, includes, filters |
| [todo](examples/todo/) | Full CRUD todo app with HTML frontend (template engine) |
| [bench_db](examples/bench_db/) | SQLite performance benchmarks |

```bash
# Run an example
./build/hull -p 8080 -d /tmp/test.db examples/hello/app.lua

# Run its tests
./build/hull test examples/hello
```

## Documentation

| Document | Content |
|----------|---------|
| [MANIFESTO.md](MANIFESTO.md) | Design philosophy, architecture, security model |
| [docs/architecture.md](docs/architecture.md) | System layers, capability API, build pipeline |
| [docs/security.md](docs/security.md) | Trust model, attack model, sandbox enforcement |
| [docs/roadmap.md](docs/roadmap.md) | What's built, what's next |
| [docs/benchmark.md](docs/benchmark.md) | Performance methodology and results |
| [docs/keel_audit.md](docs/keel_audit.md) | Keel HTTP server C code audit report |
| [CLAUDE.md](CLAUDE.md) | Development guide for contributors |

## Building Hull

```bash
make                    # build hull binary
make test               # run 79 unit tests
make e2e                # end-to-end tests (all examples, both runtimes)
make e2e-templates      # template engine tests (40 tests, both runtimes)
make debug              # ASan + UBSan build
make msan               # MSan + UBSan (Linux clang only)
make check              # full validation (clean + ASan + test + e2e)
make analyze            # Clang static analyzer
make cppcheck           # cppcheck static analysis
make platform           # build libhull_platform.a
make platform-cosmo     # build multi-arch cosmo platform archives
make self-build         # reproducible build verification (hull→hull2→hull3)
make CC=cosmocc         # build with Cosmopolitan (APE binary)
make clean              # remove all build artifacts
```

## Status

Hull is in active development. All core features are implemented and tested across Linux, macOS, and Cosmopolitan APE. See [docs/roadmap.md](docs/roadmap.md) for what's next.

## License

AGPL-3.0. See [LICENSE](LICENSE).

Commercial licenses available for closed-source distribution. See the [Licensing](MANIFESTO.md#licensing) section of the manifesto.
