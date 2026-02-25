# Hull

True digital freedom has arrived.

## Why

AI coding assistants solved code generation. Millions of people can now describe software and have it built. But the output is always the same: a React frontend, a Node.js backend, a Postgres database, and a cloud deployment problem. The vibecoder swapped one dependency — coding skill — for another: the cloud. They don't own anything more than before. They just rent different things.

Hull is the missing piece. The AI writes Lua, `hull build` produces a single file. That file is the product. No cloud. No hosting. No dependencies. The developer owns it.

Never depend on hyperscalers. Never depend on cloud vendors, database vendors, deployment pipelines, the software supply chain, or even a specific LLM provider — bring your own model, local or cloud, any that writes Lua. Six vendored C libraries. One build command. One file. That's the entire stack.

Your data lives in an encrypted-at-rest SQLite file. Copy it, back it up, move it — it's yours. And Hull itself is built with Hull. `hull eject` copies the build tool into your project. No vendor lock-in. Walk away anytime and maintain your own copy.

The person who receives that file can trust it. Every Hull app declares exactly what it can access — files, hosts, resources. The kernel enforces it. Ed25519 signatures prove integrity. `hull inspect` and [verify.gethull.dev](https://verify.gethull.dev) let anyone verify that what the app claims is what the app does. Verifiable by guarantees, auditable by design.

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
- **Own the business outcome.** Deploy it on your terms. License it on your terms. Built-in Ed25519 license key system for commercial distribution. Offline verification, no activation server. No platform takes a cut.
- **Native speed when you need it.** Lua is faster than Python and Ruby for application logic — fast enough for HTTP handlers, business logic, and database queries. When you need more, optional WASM compute plugins let you write performance-critical code in C, Rust, or Zig. Sandboxed, gas-metered, no I/O — pure computation at near-native speed. Lua stays in control.
- **AI-friendly.** Zero compilation — instant iteration speed. Lua is small, consistent, and LLMs generate it reliably. Errors include file, line, stack trace, and request context — piped straight to the LLM. The entire frontend and backend conversation is auditable, so you can steer the LLM to adhere to your specs. Ralph loop it, Gastown it, or just let your agentic tool harness do its job — Hull's zero-compilation feedback loop is built for autonomous iteration.

## Architecture

Six vendored C libraries, zero external dependencies. Built on [Cosmopolitan libc](https://github.com/jart/cosmopolitan) for cross-platform APE binaries.

| Component | Purpose |
|-----------|---------|
| [Keel](https://github.com/artalis-io/keel) | HTTP server (epoll/kqueue/io_uring/poll) |
| [Lua 5.4](https://www.lua.org/) | Application scripting |
| [SQLite](https://sqlite.org/) | Database |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | TLS client |
| [TweetNaCl](https://tweetnacl.cr.yp.to/) | Ed25519 signatures |
| [pledge/unveil](https://github.com/jart/pledge) | Kernel sandbox |
| [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime) | WebAssembly compute plugins *(optional)* |

## Status

Hull is in active development.

## License

AGPL-3.0. See [LICENSE](LICENSE).

Commercial licenses available for closed-source distribution. See the [Licensing](MANIFESTO.md#licensing) section of the manifesto.
