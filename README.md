# Hull

Local-first application platform. Single binary, zero dependencies, runs anywhere.

Hull embeds six C libraries into one portable executable:

| Component | Purpose |
|-----------|---------|
| [Keel](https://github.com/artalis-io/keel) | HTTP server (epoll/kqueue/io_uring/poll) |
| Lua 5.4 | Application scripting |
| SQLite | Database |
| mbedTLS | TLS client |
| TweetNaCl | Ed25519 signatures |
| pledge/unveil | Kernel sandbox |

Write backend logic in Lua, frontend in HTML5/JS, data in SQLite. `hull build` produces a single APE binary that runs on Linux, macOS, Windows, FreeBSD, OpenBSD, and NetBSD. Under 2 MB.

## What it does

- **Ship as a file.** One binary = the entire application. No installer, no runtime, no Docker, no cloud.
- **Own your data.** SQLite file on the user's machine. Backup = copy a file.
- **Built-in everything.** Routing, auth, RBAC, email, CSV, i18n, FTS, PDF, templates, validation, rate limiting, WebSockets, sessions, CSRF.
- **Kernel sandbox.** pledge/unveil on Linux/OpenBSD. Apps declare what they can access; the kernel enforces it.
- **Ed25519 licensing.** Built-in license key system for commercial distribution. Offline verification, no activation server.
- **AI-friendly.** Lua is small, consistent, and LLMs generate it reliably. The entire dev loop — write, test, fix — works in one terminal with any AI assistant.

## Status

Hull is in active development. The [MANIFESTO.md](MANIFESTO.md) is the complete design document. [docs/MANIFESTO_FULL.md](docs/MANIFESTO_FULL.md) includes detailed feature documentation with implementation examples.

## License

AGPL-3.0. See [LICENSE](LICENSE).

Commercial licenses available for closed-source distribution. See the [Licensing](MANIFESTO.md#licensing) section of the manifesto.
