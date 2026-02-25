# Hull

Local-first application platform. Single binary, zero dependencies, runs anywhere.

Write backend logic in Lua, frontend in HTML5/JS, data in SQLite. `hull build` produces a single portable executable — under 2 MB — that runs on Linux, macOS, Windows, FreeBSD, OpenBSD, and NetBSD.

## Start here

| You are | Read this |
|---------|-----------|
| A developer who wants to build with Hull | [MANIFESTO.md](MANIFESTO.md) — design, architecture, security model, standard library |
| A developer who wants implementation details | [docs/MANIFESTO_FULL.md](docs/MANIFESTO_FULL.md) — full feature docs with C/Lua code examples |
| Exploring whether Hull fits your use case | [PERSONAS.md](PERSONAS.md) — eight real-world personas and how Hull solves their problems |
| An investor evaluating the opportunity | [INVESTORS.md](INVESTORS.md) — the problem, the product, economics, moat, risks |

## What it does

- **Ship as a file.** One binary = the entire application. No installer, no runtime, no Docker, no cloud.
- **Own your data.** SQLite file on the user's machine. Backup = copy a file.
- **Built-in everything.** Routing, auth, RBAC, email, CSV, i18n, FTS, PDF, templates, validation, rate limiting, WebSockets, sessions, CSRF.
- **Kernel sandbox.** pledge/unveil on Linux/OpenBSD. Apps declare what they can access; the kernel enforces it.
- **Ed25519 licensing.** Built-in license key system for commercial distribution. Offline verification, no activation server.
- **AI-friendly.** Lua is small, consistent, and LLMs generate it reliably. The entire dev loop — write, test, fix — works in one terminal with any AI assistant.

## Architecture

Six vendored C libraries, zero external dependencies:

| Component | Purpose |
|-----------|---------|
| [Keel](https://github.com/artalis-io/keel) | HTTP server (epoll/kqueue/io_uring/poll) |
| Lua 5.4 | Application scripting |
| SQLite | Database |
| mbedTLS | TLS client |
| TweetNaCl | Ed25519 signatures |
| pledge/unveil | Kernel sandbox |

## Status

Hull is in active development.

## License

AGPL-3.0. See [LICENSE](LICENSE).

Commercial licenses available for closed-source distribution. See the [Licensing](MANIFESTO.md#licensing) section of the manifesto.
