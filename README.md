# Hull

## Why

AI coding assistants solved code generation. Millions of people can now describe software and have it built. But the output is always the same: a React frontend, a Node.js backend, a Postgres database, and a cloud deployment problem. The vibecoder swapped one dependency — coding skill — for another: the cloud. They don't own anything more than before. They just rent different things.

Hull is the missing piece. The AI writes Lua, `hull build` produces a single file. That file is the product. No cloud. No hosting. No dependencies. The developer owns it.

## What

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
- **Sandboxed.** Kernel sandbox (pledge/unveil) on Linux/OpenBSD. Windows App Container and macOS App Sandbox are on the roadmap. Lua sandbox is always in effect on all platforms.
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
