# Hull. A Hardened, Capability-Secure Runtime for AI-Native Systems

> **Code became disposable. Trust is not.** AI generates code endlessly. Hull constrains what that code can actually do, at a kernel-enforced boundary the script cannot cross.
>
> For the operational vocabulary, target audiences, tone, and canonical phrasings used across the site, README, and external materials, see [docs/POSITIONING.md](POSITIONING.md). This document is the long-form rationale; POSITIONING.md is the style reference.

## Contents

**Vision & Market**
[Mission & Vision](#mission--vision) · [Key Selling Points](#key-selling-points) · [Thesis](#thesis) · [The False Choice](#the-false-choice)

**What & Why**
[What Hull Is](#what-hull-is) · [Why It Works This Way](#why-it-works-this-way) · [What Gap It Fills](#what-gap-it-fills) · [The Vibecoding Problem](#the-vibecoding-problem)

**Audience**
[Who Hull Is For](#who-hull-is-for) · [What Hull Is Not](#what-hull-is-not)

**Closing**
[Survivability](#survivability) · [Philosophy](#philosophy)

## Mission & Vision

**Mission:** Digital independence for everyone. Make it possible for anyone (developer or not) to build, own, and distribute software that runs anywhere, requires nothing, and answers to nobody. You own the data. You own the code. You own the outcome. You own the build pipeline. The entire chain (from development to distribution) can run air-gapped on your own hardware, with your own AI, disconnected from every cloud and every third party. No dependence on hyperscalers. No dependence on frontier LLM providers. No dependence on hosting platforms. No dependence on anyone.

**Vision:** Make Software Great Again. A world where local-first, single-file applications are the default for tools that don't need the cloud. Where the person who uses the software owns the software. The binary, the data, the build pipeline, and the business outcome. Where an AI assistant and a single command produce a product, not a deployment problem.

**The status quo is broken.** AI coding assistants solved one problem (you don't need to know how to code) and created two others: you now depend on cloud infrastructure to run the result, and nobody has verified what the AI-generated code actually does. Vibecoded apps are cloud apps by default: React + Node.js + Postgres + Vercel/AWS. The vibecoder swapped one dependency (coding skill) for two others (cloud infrastructure and blind trust in generated code). They don't own anything more than before; they just rent different things and hope the AI didn't introduce something they wouldn't have written themselves.

Hull breaks this chain: the AI writes Lua or JavaScript, the output is a file instead of a deployment, and the runtime constrains what that code can do. The developer owns the product. The user can verify its boundaries.

Four core beliefs:

1. Software should be an artifact you own, not a service you rent
2. Data should live where the owner lives. On their machine, encrypted, backed up by copying a file
3. Building software should be as easy as describing what you want. And the result should be yours
4. When AI writes the code, the runtime must be the trust boundary. Not the developer's ability to audit every line

## Key Selling Points

### Digital Independence

- **Own your data**. SQLite file on your machine, not someone else's server
- **Own your backups** (your database is a file. Copy it to a USB stick, Dropbox, S3, rsync it to a NAS, email it to yourself) whatever you want. No vendor backup UI, no export-and-pray, no "please contact support to restore." Your data, your backup strategy, your choice.
- **Own your pipeline** (hull.com is ejectable, source is AGPL, build from scratch if you want. Even the AI coding step can be fully air-gapped and offline) OpenCode + Ollama + open-weight models (minimax-m2.5, Llama, Qwen, etc.) on your own hardware. Source code never leaves your machine. No cloud IDE, no API calls, no telemetry. The entire chain from "describe what you want" to "here's your binary" can run disconnected from the internet.
- **Own your distribution** (Hull's Ed25519 licensing system is yours to configure. Perpetual, monthly, annual, seat-based, site-licensed, free trial with expiry) you choose the model. No app store cut, no payment platform lock-in, no third party between you and your customer. You ship a file, you deliver a license key, you keep the revenue.
- **Own your security.** Hull apps declare exactly what they can access (files, hosts, env vars) and the kernel enforces it. No AI agent with free rein over your computer. When Meta's AI safety director Summer Yue gave [OpenClaw](https://techcrunch.com/2026/02/23/a-meta-ai-security-researcher-said-an-openclaw-agent-ran-amok-on-her-inbox/) access to her inbox, it ignored her instructions and bulk-deleted hundreds of emails while she ran to her Mac Mini to physically stop it. That's what happens when software has unconstrained access to your system. Hull apps can't do this: pledge/unveil means the process physically cannot touch files outside its declared paths, connect to undeclared hosts, or spawn other processes. The sandbox isn't a policy. It's a syscall-level wall.
- **Own your business outcome** (no hosting costs, no vendor lock-in, no platform risk. No usage-based surprise bills. When [jmail.world](https://peerlist.io/scroll/post/ACTHQ7M7REKP7R67DHOOQ8JOQ7NNDM)) a Next.js app on Vercel (went viral, the creator woke up to a $49,000 monthly bill from edge functions and bandwidth charges. With Hull, there is no bill. The app runs on the user's machine. Traffic costs nothing because there is no traffic) just a file on a computer.
- **Free from hyperscalers**. AWS, Azure, GCP are not needed, not wanted, not involved
- **Free from SaaS companies**. Your software doesn't stop when they raise prices or shut down
- **Free from prompt injection** (cloud-based AI coding assistants fetch context from remote skill files, MCP servers, web search results, and third-party tool outputs. Every one of these is a prompt injection surface) an attacker can embed instructions in a webpage, a GitHub issue, a package README, or a skill file that the LLM follows silently. An air-gapped local model running on your own hardware with Ollama has no attack surface: no network fetches, no remote skill files, no MCP servers phoning home, no third-party context. The model sees only what you give it. No injection vector exists because no external input exists.

### Security & Trust

- **Self-declaring apps**. Every Hull app exposes the files, hosts, env vars, and resources it will access. The startup banner, `hull inspect`, and verify.gethull.dev show exactly what the app can touch. Hull helps you verify that what the app claims is what the app does.
- **Defense in depth**. Six independent layers (seven with optional GPU compute), each enforced separately:
  1. **Runtime sandboxes**. Lua: `os.execute`, `io.popen`, `loadfile`, `dofile` removed entirely. JS: `eval()`, `Function` constructor, `std`, `os` modules removed. Both: restricted to C-level capability APIs only. Per-request instruction limits (default 100M) via `lua_sethook` and `JS_SetInterruptHandler` cap script-side cost.
  2. **C-level enforcement**. Allowlist checks before every outbound connection and file access. SQL is always parameterized via `sqlite3_bind_*`. Internal `_hull_*` tables are protected by call-stack inspection. Compiled code, not bypassable from Lua or JS.
  3. **Allocator model**. Keel's `KlAllocator` vtable routes all allocations through a pluggable interface with `old_size`/`size` tracking on every realloc and free. Enables arena/pool allocation, bounded memory, and leak detection. No raw `malloc`/`free` in Hull's own code.
  4. **Kernel sandbox**. Pledge/unveil syscall filtering on Linux (SECCOMP BPF + Landlock LSM) and OpenBSD (native). Cosmopolitan libc provides libc-level pledge/unveil emulation on Windows. macOS uses Seatbelt (`sandbox_init_with_parameters`) with a deny-default SBPL profile built dynamically from the manifest. The process physically cannot exceed its declared capabilities.
  5. **Digital signatures**. Three independent Ed25519 layers: platform (gethull.dev key), app (developer key), and release (`hull.sha256.sig` verified by `hull update`). Together they prove the C layer is legitimate Hull, the app hasn't been tampered with, and the binary you downloaded matches what was published.
  6. **WASM sandbox**. Compute plugins run in WAMR's isolated linear memory with no I/O imports, gas-metered execution, configurable memory/instruction caps, and an AOT pipeline that delivers near-native speed without leaving the sandbox.
  7. **GPU sandbox** *(when `HL_ENABLE_GPU=1`)*. Wgpu-native compute shaders run with manifest-gated device access, a 5-second timeout per dispatch, persistent buffers scoped to the process, and texture/buffer bindings declared at compile time.
- **Sanitizer-hardened C runtime**. Keel (Hull's HTTP server) is developed and tested under the full sanitizer suite:
  - **ASan** (AddressSanitizer). Heap/stack buffer overflow, use-after-free, double-free, memory leaks
  - **UBSan** (UndefinedBehaviorSanitizer). Signed overflow, null dereference, misaligned access, shift overflow
  - **MSan** (MemorySanitizer). Reads of uninitialized memory
  - **TSan** (ThreadSanitizer). Data races (relevant for future multi-threaded extensions)
  - Every commit runs `make debug` (ASan + UBSan enabled) against the full test suite. Every CI build runs under sanitizers. Bugs found by sanitizers are treated as release blockers.
- **Static analysis**. Clang `scan-build` (static analyzer) and `cppcheck --enable=all` run on every commit. Both must exit clean with zero findings before merging.
- **Fuzz-tested**. LibFuzzer targets cover the primary attack surface (untrusted network input): HTTP parser + chunked decoder, multipart/form-data parser. Fuzz targets run with ASan + UBSan enabled. Corpus-driven, crash-reproducing, continuous.
- **Auditable**. 11 vendored C libraries (plus 2 optional: WAMR for WASM compute, wgpu-native for GPU). All vendored, all in-tree, all auditable. No `npm`, no `pip`, no `crates.io`, no transitive dependency graphs. The C surface that mediates capabilities is a small, well-bounded set of `hl_cap_*` functions.
- **Zero supply chain risk**. No package managers, no transitive dependencies, no auto-updates fetching code from the internet at build time.
- **Encryption at rest** *(planned)*. License-key derivation infrastructure (Ed25519 + Curve25519 from TweetNaCl) already exists; transparent SQLite-level encryption (SEE-compatible or SQLCipher-style) is a roadmap item, not currently shipped. Apps that need it today can encrypt values with the `crypto.secretbox_*` capability before writing them.

### AI-First Development

- **Dual runtime (Lua and JavaScript**) write in whichever language you (or your AI) prefer. Lua is small (~60 keywords) and LLM-friendly. JavaScript (via QuickJS ES2023) is familiar to every web developer. Same API, same capabilities, same output binary. One app, one language. Your choice.
- **Works with any AI assistant** (Claude Code, OpenAI Codex, OpenCode, Cursor) anything that writes code
- **Air-gapped development**. OpenCode + local model (minimax-m2.5, Llama, etc.) on your own hardware. Code never leaves your premises. Develop on M3 Ultra 512GB, fully offline.
- **No frontier model dependency**. Hull doesn't require GPT-4 or Claude. Any model that generates Lua or JavaScript works. Run your own.
- **LLM-friendly errors**. File:line, stack trace, source context, request details piped to terminal
- **LLM testing loop** (write, test, read output, fix, repeat) all in one terminal
- **Vibecoded apps are NOT cloud apps**. Hull breaks the default pipeline where AI produces React + Node + Postgres + Vercel. With Hull, AI produces Lua or JS and the output is a single file. The developer owns the output.

### Runtime

- **~5 MB single binary, ~3.66 MB compute-only.** The default build (Lua + QuickJS + SQLite + mbedTLS + TweetNaCl + pledge/unveil + WAMR + embedded Mozilla CA bundle + embedded TinyCC) is around 5 MB on aarch64. `make HL_ENABLE_DB=0` drops SQLite and the DB-backed stdlib for a ~3.66 MB compute-focused build. Add `HL_ENABLE_GPU=1` (wgpu-native) for GPU compute.
- **Fast enough. And native speed when you need it.** Lua 5.4 and QuickJS are 10-30× faster than Python and 5-10× faster than Ruby for application logic, sustaining 70-100K req/s on a single core for I/O-bound routes. For CPU-bound work, WASM compute plugins (interpreter or AOT, gas-metered, no I/O) close the gap to ~1.2-1.9× native C. For massively parallel workloads, GPU compute via wgpu-native (Vulkan/Metal/DX12) runs WGSL shaders with persistent buffers and pipeline support. Apps drop down only when they need to; the sandbox boundary doesn't move.
- **Batteries included**. Routing, auth, JWT, sessions, CSRF, RBAC, templates, CORS, rate limiting, WebSockets, SSE, full-text search, CSV, i18n, idempotency, transactional outbox/inbox, health checks, ETags, image codecs, SMTP. All in the stdlib, all sandboxed, all auditable.
- **Single-threaded event loop + bounded worker pool**. Keel's epoll/kqueue/io_uring/poll event loop drives one Lua/JS handler at a time per request; async DB and HTTP work runs on a configurable thread pool (`--workers N`) so the loop stays responsive. Easy to reason about, no shared mutable state, no race conditions in app code.
- **Cooperative multitasking**. Background timers (`app.every`, `app.daily`), long-running batches, and async I/O via coroutines (Lua) or Promises (JS).
- **Runs everywhere**. Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD from one Cosmopolitan APE binary.
- **Air-gapped operation**. Works offline, no phone-home, no telemetry, no activation server.

### Modern Stack

- **HTML5/JS frontend**. Any framework or vanilla JS; every widget and chart library already exists
- **The UI is the user's own browser**. Chrome, Edge, Safari, Opera. Already installed on every machine. No Electron, no embedded Chromium, no webview dependency.
- **WebSocket support**. Real-time dashboards, live updates, push notifications
- **Full-text search**. SQLite FTS5, relevance ranking, Unicode, highlighted snippets
- **PDF, CSV, email** (export, import, send) all built in

### Distribution

- **Ship as a file**. One binary, runs on any OS, no installer, no runtime, no admin privileges
- **Built-in licensing**. Ed25519 signed keys, offline verification, tax-ID binding for compliance
- **Digital signatures**. Platform and app signatures prove integrity and authorship
- **Reproducible builds**. Three independent properties, all CI-gated on Linux and macOS:
  1. **`make` is deterministic.** Same source tree → byte-identical `build/hull` between rebuilds.
  2. **`hull build` is deterministic.** Same source + same hull version → byte-identical app binary (built to the same output path; macOS `ld64` hashes output path into LC_UUID, so the test snapshots between rebuilds to the same path. Which is the right question to ask anyway, since end-users build to a known target name).
  3. **`make self-build` proves hull is self-hostable.** Hull can build hull2 can build hull3 across all platforms.

  The three together close the "verifiable provenance" loop: anyone with the source can rebuild bit-for-bit, and anyone with hull can rebuild any app bit-for-bit. Vendored `ar` archives are kept deterministic via `ZERO_AR_DATE=1`; system linker defaults (`--build-id=sha1` on Linux, path-hashed LC_UUID on macOS) provide content-addressed identifiers without further flags.

## Thesis

The software industry spent 15 years pushing everything to the cloud. The result: subscription fatigue, vendor lock-in, privacy erosion, applications that stop working when the internet goes down, and products that vanish when startups die.

Now AI is writing the code (and nobody is asking what that code can do once it's running. Traditional frameworks assume trusted code written by developers who understand every line. That assumption is gone. The vibecoder describes what they want, the AI produces hundreds of lines, and the result runs with full system access. This is not a hypothetical risk) it's the default behavior of every major framework.

People want local tools they can control. And in a world where AI writes the code, they need a runtime that proves what the code can and cannot do.

Hull is a capability-secure runtime for building single-file, zero-dependency, run-anywhere applications. You write your backend logic in Lua or JavaScript, your frontend in HTML5/JS, your data lives in SQLite, and Hull compiles everything into one executable. The app declares its capabilities in a manifest (files, hosts, environment variables, GPU access) and the kernel enforces those boundaries. No servers. No cloud. No npm. No pip. No Docker. No hosting. No unconstrained access. Just a file that does what it says and nothing more.

## The False Choice

The industry keeps offering people the same two options for managing data and workflows:

**Excel** (your data is trapped in a format that mixes data, logic, and presentation into one file. Excel's fundamental problem isn't that it's bad) it's that the data, the logic, and the presentation are all the same thing. Change a column width and you might break a formula. Copy a sheet and the references break. Email a spreadsheet and now there are 15 conflicting versions. It corrupts when it gets too large. Every business has spreadsheets that should be applications. They stay as spreadsheets because building an application was too hard.

**SaaS** (your data is trapped on someone else's server, behind a subscription, with a Terms of Service that can change tomorrow. The vendor can raise prices, shut down, get acquired, or lose your data in a breach. You don't own anything) you rent access to your own information.

**Hull**. Your data is a SQLite file on your computer. Your application is a file next to it. You own both. Forever.

Hull splits what Excel conflates:

```
Excel:     data + logic + UI = one .xlsx file (everything coupled)

Hull:      data    = SQLite       (queryable, relational, no corruption)
           logic   = Lua or JS    (version-controlled, testable, separate)
           UI      = HTML5/JS     (forms, tables, charts, print layouts)
```

A bookkeeper using a Hull app doesn't know this separation exists. They see forms, tables, and buttons. But under the hood:

- The data can't be accidentally corrupted by dragging a cell
- The logic can't be broken by inserting a row
- The UI can be changed without touching the data
- Multiple users can't create conflicting copies because the database handles concurrency
- Backup is copying one file, not "which version of Q4_report_FINAL_v3_ACTUAL.xlsx is the right one"

Hull is the third option nobody's offering: properly structured data that you still control. No coupling of data and presentation. No subscription. No server. No lock-in. Just two files (one is the tool, one is your data) and they're both yours.

## What Hull Is

Hull is a capability-secure application runtime that embeds the entire stack (HTTP server, dual scripting runtime, database, TLS, signatures, kernel sandbox, optional WASM and GPU compute) into a single binary, built on [Cosmopolitan libc](https://github.com/jart/cosmopolitan) for cross-platform APE binaries. It is not a general-purpose framework. It is a sandboxed environment where application code runs inside declared capability boundaries enforced by the kernel.

| Component | Purpose |
|-----------|---------|
| [Keel](https://github.com/artalis-io/keel) | HTTP server (epoll/kqueue/io_uring/poll), routing, middleware, async, thread pool |
| [Lua 5.4](https://www.lua.org/) | Application scripting (default runtime, 1.9× faster than QuickJS) |
| [QuickJS](https://bellard.org/quickjs/) | Application scripting (ES2023 JavaScript) |
| [SQLite](https://sqlite.org/) | Embedded database (WAL mode, parameterized queries, FTS5) |
| [mbedTLS](https://github.com/Mbed-TLS/mbedtls) | TLS client for outbound HTTPS / SMTP |
| [TweetNaCl](https://tweetnacl.cr.yp.to/) | Ed25519 signatures, XSalsa20+Poly1305, Curve25519 |
| [pledge/unveil](https://github.com/jart/pledge) | Kernel sandbox (Justine Tunney's polyfill) |
| [log.c](https://github.com/rxi/log.c), [sh_arena](https://github.com/sailorhg/sh_arena), [sh_json](https://github.com/sailorhg/sh_json) | Logging, arena allocator, streaming JSON |
| [miniz](https://github.com/richgel999/miniz) | gzip response compression + client decompression |
| Mozilla CA bundle | ~145 root certificates embedded for HTTPS without a system store |
| Embedded TinyCC | ~400 KB compiler embedded so `hull build` does not require a separately-installed C compiler (the system linker is still used for the link step) |
| [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime) *(optional)* | WebAssembly compute plugins (interpreter + AOT, SIMD128, Memory64) |
| [wgpu-native](https://github.com/gfx-rs/wgpu-native) *(optional)* | GPU compute shaders (Vulkan/Metal/DX12) |

Total: **~5 MB** with the default build (everything except GPU), **~3.66 MB** with `HL_ENABLE_DB=0` for compute-only deployments, **~5.2 MB** with the optional WAMR runtime included. Runs on Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD from a single APE binary.

Hull is not a web framework. It is not a do-everything platform. It is a deliberately constrained runtime for building local-first desktop applications that use an HTML5/JS frontend served to the user's browser. The constraints are the feature. They guarantee that the application can only do what it declared. The user double-clicks a file, a browser tab opens, and they have a working application. Their data never leaves their machine. The app physically cannot access anything beyond its declared boundaries.

## Why It Works This Way

Every design decision follows from one constraint: **the end user should be able to run, back up, and control the application without technical knowledge.**

**Single binary** because installation is "put file on computer." No installer, no runtime, no package manager, no PATH configuration, no admin privileges. Works from a USB stick. Works from Dropbox.

**Dual runtime: Lua and JavaScript.** Hull ships both Lua 5.4 and QuickJS (ES2023). Each app picks one (`app.lua` or `app.js`. Lua was designed for embedding in C: 280 KB, clean C API refined over 30 years, LLMs generate it reliably. QuickJS brings JavaScript to the same model: 350 KB, ES2023 compliant, familiar to every web developer. Both share the same C capability layer, the same sandbox, the same stdlib API. The developer (or their AI) writes in whichever language they prefer) the output is the same single-file binary.

**SQLite** because it's the most deployed database in the world, it's a single file, it works offline, it handles concurrent access via file locking, and backup is copying one file. No database server, no connection strings, no ports, no administration.

**mbedTLS** because applications that talk to external APIs (government tax systems, payment processors) need outbound HTTPS. mbedTLS is designed for embedding, small, and has no external dependencies. This is a TLS client, not a server. Hull uses Keel for inbound HTTP, which has its own pluggable TLS vtable for deployments that need inbound TLS.

**TweetNaCl** because applications sold commercially need license key verification. Ed25519 signatures in 770 lines of C, public domain, audited. No OpenSSL dependency. License verification happens locally, offline, in compiled C that is difficult to tamper with.

**Cosmopolitan C** because "runs anywhere" must mean anywhere. A single APE (Actually Portable Executable) binary runs on every major operating system without recompilation. The developer builds once. The user runs it on whatever they have.

**HTML5/JS frontend** because every widget, form element, date picker, and print stylesheet already exists. The developer writes standard HTML, CSS, and JavaScript. Any framework or none. Building a native GUI toolkit would be a massive scope increase for no user benefit. The user doesn't know or care that localhost is involved. They see a browser tab with a form. That's an application to them.

**pledge/unveil** because "your data is safe" should be a provable technical guarantee, not a policy promise. A Hull application declares at startup exactly what it can access: its own database file, its own directory, one network port. The kernel enforces this via syscall filtering. The application physically cannot exfiltrate data, access other files, or spawn processes. This is not a sandbox configuration (it is a property of the binary, verifiable by inspection. Hull vendors [Justine Tunney's pledge/unveil polyfill](https://github.com/jart/pledge) which ports OpenBSD's sandbox APIs to Linux using SECCOMP BPF for syscall filtering and Landlock LSM (Linux 5.13+) for filesystem restrictions. On OpenBSD, the native pledge/unveil syscalls are used directly. On macOS and Windows, native kernel sandboxing is unavailable) but [Cosmopolitan libc](https://github.com/jart/cosmopolitan), the cross-platform runtime that makes APE binaries possible, provides libc-level pledge/unveil emulation that restricts the process at the C library layer. The security model degrades gracefully: full kernel sandbox on Linux and OpenBSD, libc-level enforcement via Cosmopolitan on other platforms, application-level safety guarantees everywhere.

## What Gap It Fills

There is currently no way for a non-technical person to get a custom local application without hiring a developer to build a native app, set up a server, or configure a cloud deployment.

There is currently no way for a developer (or an AI coding assistant) to produce a single file that is a complete, self-contained, database-backed application with a web UI that runs on any operating system.

And there is currently no runtime that treats AI-generated code as untrusted by default. That constrains the code to declared capabilities and proves containment to the end user. Every existing framework gives the application full system access and trusts the developer to have written safe code. When the developer is an AI and the user is a vibecoder who can't audit the output, that trust model is broken.

The closest alternatives and why they fall short:

**Electron** (produces local apps with web UIs, but each one is 200+ MB because it bundles an entire Chromium browser. Requires per-platform builds. No built-in database. No security sandboxing. Chromium alone pulls in hundreds of third-party dependencies) any one of which is a supply-chain attack vector. The `node_modules` tree for a typical Electron app contains 500-1,500 packages from thousands of maintainers. A single compromised dependency (event-stream, ua-parser-js, colors.js. All real incidents) can exfiltrate data from every app that includes it. The attack surface is too large for any team to audit.

**Docker**. Solves deployment consistency but requires Docker Desktop (2+ GB), is Linux-only in production, and is a server deployment tool, not a local application tool.

**Go/Rust single binaries** (produce cross-platform executables but require per-platform compilation, have no built-in scripting layer (changes require recompilation), and ship no application framework. Both ecosystems rely on centralized package registries (crates.io, pkg.go.dev) with deep transitive dependency trees. A typical Rust web application pulls 200-400 crates; a typical Go web service pulls 50-150 modules. Each dependency is maintained by an independent author, auto-updated by bots, and trusted implicitly by the build system. The security of the entire application depends on the weakest link in a chain nobody has fully audited. Go's `go.sum` and Rust's `Cargo.lock` provide reproducibility but not auditability) they pin the versions of dependencies you haven't read.

**Datasette**. Closest in spirit (SQLite + web UI), but requires Python installation, pip, and is read-oriented rather than a full application platform.

**Traditional web frameworks (Express, Django, Rails, Laravel)**. Require runtime installation, package managers, database servers, and hosting. Designed for cloud deployment, not local-first applications. Every one of these stacks depends on a package manager ecosystem (npm, pip, composer, bundler) with the same supply-chain risks as Electron and Rust/Go, plus the operational attack surface of a production server.

Hull fills the gap: **a capability-secure application runtime around 5 MB that produces a single file containing an HTTP server, a dual-language scripting engine, a database, signature verification, and a web UI, runnable on any operating system, with kernel-enforced capability boundaries, requiring zero installation.** The app declares what it can access. The kernel enforces it. The user can verify it. Optional WASM compute plugins (interpreter + AOT, gas-metered, no I/O) and GPU compute shaders (wgpu-native) provide near-native speed without leaving the sandbox.

## The Vibecoding Problem

AI coding assistants (Claude Code, Cursor, OpenCode) have made it possible for anyone to build software by describing what they want in natural language. But there are two gaps between "the AI wrote my code" and "someone can use my application": **deployment** and **trust.**

Today, every vibecoded project hits the same walls. The AI generates a React frontend and a Node.js backend. Now what? The vibecoder must learn about hosting, DNS, SSL certificates, database servers, environment variables, CI/CD pipelines, and monthly billing. They're forced to choose between:

- **Own your infrastructure (AWS, GCP, DigitalOcean)**. EC2 instances, RDS databases, load balancers, S3 buckets, IAM roles, security groups, CloudWatch alerts. A simple CRUD app becomes a distributed systems problem with a $50-200/month floor.

- **Platform-as-a-Service (Vercel, Netlify, Railway)** (simpler until it isn't. Usage-based pricing means you don't know your bill until it arrives. When [jmail.world](https://jmail.world)) a searchable archive of the Jeffrey Epstein emails built as a Next.js app on Vercel (went viral in late 2025, the creator woke up to a [$49,000 monthly bill](https://peerlist.io/scroll/post/ACTHQ7M7REKP7R67DHOOQ8JOQ7NNDM). Every hover, click, and page view triggered edge functions and bandwidth charges. Vercel's CEO personally covered the bill for PR reasons) but that's not a business model anyone can rely on.

- **Give up**. The most common outcome. The project stays on localhost, shown to nobody. The vibecoder's idea dies in a terminal window.

The core absurdity: a vibecoder who just wants to build a small tool for a small audience is funnelled into the same cloud infrastructure designed for applications serving millions. There is no middle ground between "runs on my laptop" and "deployed to the cloud."

And even if the deployment problem is solved, there's a second gap: **trust.** The vibecoder described what they wanted. The AI produced hundreds of lines of code. Nobody read every line. Traditional frameworks assume the developer trusts the code they wrote. But the vibecoder didn't write it, and they can't audit it. The result runs with full system access: file system, network, environment variables, everything. If the AI hallucinated an outbound HTTP call, added an overly broad file glob, or introduced a dependency that phones home, the vibecoder would never know. No existing framework treats this as a problem.

Hull closes both gaps. The AI writes Lua or JavaScript (whichever it (or the developer) prefers. The data lives in SQLite instead of Postgres. The frontend is HTML5/JS served from the binary instead of a CDN. The app declares its capabilities in a manifest, and the kernel enforces them) so even if the AI-generated code tries to access undeclared files or connect to undeclared hosts, it physically cannot. And when it's done, `hull build --output myapp.com` produces a single file the vibecoder can share, sell, or put in Dropbox. No server. No hosting bill. No $49,000 surprise. No deployment step at all. No unconstrained code.

The code the AI generates is the product. Not a codebase that needs infrastructure to become a product. The actual, distributable, runnable, sandboxed product.

## Who Hull Is For

### Developers & Software Houses

Professional developers and small teams (2-10 devs) building commercial local-first tools.

**The pain:** Deployment complexity. Hosting costs. Supply chain anxiety. Subscription model fatigue (both theirs and their users'. Every app they ship comes with an infrastructure bill and an operational burden that never ends. Small teams competing with VC-funded SaaS face margin pressure from hosting) every customer they add increases their AWS bill.

**Why Hull:** Single binary distribution. Ed25519 licensing built in. $0 infrastructure cost. Zero hosting means 100% gross margin from sale one. AGPL + commercial dual license. Team license $299 for up to 5 developers. Sell a product, not a service.

**The outcome:** Ship to customers as a file. Zero DevOps, zero monthly bill, zero scaling anxiety. Revenue from day one.

### Vibecoders

Non-developers (or developer-adjacent) using LLMs to build their own tools.

**The pain:** They can describe what they want to an AI, but they can't deploy or distribute the result. The AI writes React + Node.js, and now they need to learn Docker, AWS, DNS, SSL, and CI/CD just to share what they built.

**Why Hull:** The LLM writes Lua or JavaScript. No React, no Node, no deployment pipeline. `hull build` creates a binary. Zero technical knowledge required for distribution. The output is a file, not a deployment problem.

**Air-gapped development:** Works with OpenCode + minimax-m2.5 on M3 Ultra 512GB (code never leaves your premises. Also works with Claude Code, OpenAI Codex, Cursor) if you're OK with cloud-based AI.

**The outcome:** Describe a tool, get a file, share it.

### SMBs (Small & Medium Businesses)

Small businesses trapped between Excel and SaaS.

**The pain:** Excel corruption, version chaos, SaaS subscription costs, data sovereignty concerns, vendor lock-in, GDPR compliance headaches. They're paying monthly for tools they don't fully control, storing sensitive business data on servers they don't own. They need an inventory tracker, an appointment scheduler, an invoice generator, a job costing calculator. Small apps that solve their daily lives. Each one is either an overengineered SaaS at $30/month/seat or a fragile spreadsheet one wrong click away from disaster.

**Why Hull:** Own your data (single SQLite file). Own your tools (single binary). One-time purchase. Works offline. Encrypted database. Back up by copying a file. A vibecoder or local IT consultant describes the tool to an AI, writing Lua or JavaScript, `hull build` produces a file, and the business has software that works like enterprise software without the enterprise price tag or complexity.

**The outcome:** Custom tools that replace spreadsheets and SaaS subscriptions, owned outright, backed up by copying a file. No IT department required.

---

All three groups share a need: **turn application logic into a self-contained, distributable, controllable artifact.** Hull is the machine that does this.

## What Hull Is Not

**Hull is not a web framework.** It is a local application runtime that uses HTTP as its UI transport. The browser is the display layer, not the deployment target.

**Hull is not a cloud platform.** The application runs on the user's machine. Planned hosted services (Hull Build, Hull Verify, Hull Sync) will provide convenience (hosted compilation, binary integrity verification, encrypted multi-user relay) but every one is designed to be replaceable and ejectable. No managed databases. No deployment pipelines. No lock-in.

**Hull is not a general-purpose server.** It is optimized for single-user or small-team local use. It does not include load balancing, horizontal scaling, caching layers, or message queues.

**Hull is not a mobile framework.** It targets desktop operating systems (Linux, macOS, Windows, BSDs). The Lua+SQLite core can be embedded in native mobile apps separately, but Hull itself produces desktop executables.

**Hull is not a replacement for SaaS.** Some applications genuinely need cloud infrastructure. Real-time collaboration, massive datasets, global distribution. Hull is for the other applications: the ones that work better when they're local, private, and under the user's control.

## Survivability

Hull's value proposition depends on the platform being maintained. What happens if artalis-io disappears?

**The code is AGPL.** The entire Hull source (C runtime, build tool, standard library) is open source under AGPL-3.0. Anyone can fork, build, and distribute Hull from source. The Makefile builds the entire platform from scratch without hull.com (`make CC=cosmocc`). No proprietary component exists that couldn't be rebuilt from the published source.

**The dependencies are vendored.** All core C libraries (plus optional WAMR and wgpu-native) are included in the Hull repository or pulled as git submodules pinned to specific commits. No package manager fetches at build time, no URLs that could go offline. A `git clone --recursive` of the Hull repo contains everything needed to build the platform.

**The ejection path is permanent.** `hull eject` copies hull.com into the project. An ejected project is fully self-contained. It can build production binaries forever, even if hull.com's website, CDN, and every artalis-io server vanishes. The ejected binary is signed and functional indefinitely.

**Dead man's switch.** If the Hull project publishes no release (no tagged version on GitHub) for 24 consecutive months, the license automatically converts from AGPL-3.0 to MIT. This is a legally binding clause in the Hull license. It means:

- If artalis-io abandons the project, the community can fork under MIT (no copyleft obligation)
- Commercial license holders retain their existing rights regardless
- The conversion is triggered by a verifiable, objective condition (no GitHub release tag in 24 months)
- Anyone can check: look at the GitHub releases page

**Existing applications keep working.** A built Hull application is a standalone binary. It does not phone home, check for updates, validate its license against a server, or depend on any artalis-io infrastructure at runtime. If Hull the project dies tomorrow, every Hull application ever built continues to work exactly as it does today. Forever. That's the point of local-first.

## Philosophy

Software should be an artifact you own, not a service you rent. Hull exists to make that possible (in Lua or JavaScript, your choice) for a class of applications that the industry forgot about: small, local, private, single-purpose tools that just work.

No accounts. No subscriptions. No telemetry. No updates that break things. No Terms of Service. No "we're shutting down, export your data by Friday." Just a file on your computer that does what you need, for as long as you need it, answerable to nobody.

That's what software used to be. Hull makes it that way again.
