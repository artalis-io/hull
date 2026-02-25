# Hull — Local-First Application Platform

## Contents

**Vision & Market**
[Mission & Vision](#mission--vision) · [Key Selling Points](#key-selling-points) · [Thesis](#thesis) · [The False Choice](#the-false-choice)

**What & Why**
[What Hull Is](#what-hull-is) · [Why It Works This Way](#why-it-works-this-way) · [What Gap It Fills](#what-gap-it-fills) · [The Vibecoding Problem](#the-vibecoding-problem)

**Audience**
[Who Hull Is For](#who-hull-is-for) · [What Hull Is Not](#what-hull-is-not)

**Architecture & Build**
[Architecture](#architecture) · [Why C and Lua](#why-c-and-lua) · [Build System](#build-system) · [hull.com](#hullcom--the-build-tool) · [Licensing](#licensing) · [Security Model](#security-model) · [Database Encryption](#database-encryption-optional) · [Standard Library Reference](#standard-library-reference)

**Limitations & Comparisons**
[Known Limitations](#known-limitations) · [How Hull Differs from Tauri](#how-hull-differs-from-tauri) · [How Hull Differs from Redbean](#how-hull-differs-from-redbean) · [Survivability](#survivability)

**Reference**
[Project Structure](#project-structure) · [Build](#build)

**Business**
[Business Plan & Monetization](#business-plan--monetization)

**Closing**
[Philosophy](#philosophy)

## Mission & Vision

**Mission:** Digital independence for everyone. Make it possible for anyone — developer or not — to build, own, and distribute software that runs anywhere, requires nothing, and answers to nobody. You own the data. You own the code. You own the outcome. You own the build pipeline. The entire chain — from development to distribution — can run air-gapped on your own hardware, with your own AI, disconnected from every cloud and every third party. No dependence on hyperscalers. No dependence on frontier LLM providers. No dependence on hosting platforms. No dependence on anyone.

**Vision:** Make Software Great Again. A world where local-first, single-file applications are the default for tools that don't need the cloud. Where the person who uses the software owns the software — the binary, the data, the build pipeline, and the business outcome. Where an AI assistant and a single command produce a product, not a deployment problem.

**The status quo is broken.** AI coding assistants solved one problem (you don't need to know how to code) and created another (you now depend on cloud infrastructure to run the result). Vibecoded apps are cloud apps by default — React + Node.js + Postgres + Vercel/AWS. The vibecoder swapped one dependency (coding skill) for another (the cloud). They don't own anything more than before — they just rent different things. Hull breaks this chain: the AI writes Lua instead of JavaScript, the output is a file instead of a deployment, and the developer owns the product instead of renting infrastructure to run it.

Three core beliefs:

1. Software should be an artifact you own, not a service you rent
2. Data should live where the owner lives — on their machine, encrypted, backed up by copying a file
3. Building software should be as easy as describing what you want — and the result should be yours

## Key Selling Points

### Digital Independence

- **Own your data** — SQLite file on your machine, not someone else's server
- **Own your backups** — your database is a file. Copy it to a USB stick, Dropbox, S3, rsync it to a NAS, email it to yourself — whatever you want. No vendor backup UI, no export-and-pray, no "please contact support to restore." Your data, your backup strategy, your choice.
- **Own your pipeline** — hull.com is ejectable, source is AGPL, build from scratch if you want. Even the AI coding step can be fully air-gapped and offline — OpenCode + Ollama + open-weight models (minimax-m2.5, Llama, Qwen, etc.) on your own hardware. Source code never leaves your machine. No cloud IDE, no API calls, no telemetry. The entire chain from "describe what you want" to "here's your binary" can run disconnected from the internet.
- **Own your distribution** — Hull's Ed25519 licensing system is yours to configure. Perpetual, monthly, annual, seat-based, site-licensed, free trial with expiry — you choose the model. No app store cut, no payment platform lock-in, no third party between you and your customer. You ship a file, you deliver a license key, you keep the revenue.
- **Own your security** — Hull apps declare exactly what they can access — files, hosts, env vars — and the kernel enforces it. No AI agent with free rein over your computer. When Meta's AI safety director Summer Yue gave [OpenClaw](https://techcrunch.com/2026/02/23/a-meta-ai-security-researcher-said-an-openclaw-agent-ran-amok-on-her-inbox/) access to her inbox, it ignored her instructions and bulk-deleted hundreds of emails while she ran to her Mac Mini to physically stop it. That's what happens when software has unconstrained access to your system. Hull apps can't do this — pledge/unveil means the process physically cannot touch files outside its declared paths, connect to undeclared hosts, or spawn other processes. The sandbox isn't a policy. It's a syscall-level wall.
- **Own your business outcome** — no hosting costs, no vendor lock-in, no platform risk. No usage-based surprise bills. When [jmail.world](https://peerlist.io/scroll/post/ACTHQ7M7REKP7R67DHOOQ8JOQ7NNDM) — a Next.js app on Vercel — went viral, the creator woke up to a $49,000 monthly bill from edge functions and bandwidth charges. With Hull, there is no bill. The app runs on the user's machine. Traffic costs nothing because there is no traffic — just a file on a computer.
- **Free from hyperscalers** — AWS, Azure, GCP are not needed, not wanted, not involved
- **Free from SaaS companies** — your software doesn't stop when they raise prices or shut down
- **Free from prompt injection** — cloud-based AI coding assistants fetch context from remote skill files, MCP servers, web search results, and third-party tool outputs. Every one of these is a prompt injection surface — an attacker can embed instructions in a webpage, a GitHub issue, a package README, or a skill file that the LLM follows silently. An air-gapped local model running on your own hardware with Ollama has no attack surface: no network fetches, no remote skill files, no MCP servers phoning home, no third-party context. The model sees only what you give it. No injection vector exists because no external input exists.

### Security & Trust

- **Self-declaring apps** — every Hull app exposes the files, hosts, env vars, and resources it will access. The startup banner, `hull inspect`, and verify.gethull.dev show exactly what the app can touch. Hull helps you verify that what the app claims is what the app does.
- **Defense in depth** — five independent layers, each enforced separately:
  1. **Lua sandbox** — `os.execute`, `io.popen`, `loadfile`, `dofile` removed from runtime entirely. Restricted `hull.fs.*` API with C-level path validation.
  2. **C-level enforcement** — allowlist checks before every outbound connection and file access. Compiled code, not bypassable from Lua.
  3. **Allocator model** — Keel's `KlAllocator` vtable routes all allocations through a pluggable interface with `old_size`/`size` tracking on every realloc and free. Enables arena/pool allocation, bounded memory, and leak detection. No raw `malloc`/`free` anywhere in the codebase.
  4. **Kernel sandbox** — pledge/unveil syscall filtering on Linux (SECCOMP BPF + Landlock LSM) and OpenBSD (native). The process physically cannot exceed its declared capabilities.
  5. **Digital signatures** — Ed25519 platform + app signatures prove the C layer is legitimate Hull and hasn't been tampered with.
- **Sanitizer-hardened C runtime** — Keel (Hull's HTTP server) is developed and tested under the full sanitizer suite:
  - **ASan** (AddressSanitizer) — heap/stack buffer overflow, use-after-free, double-free, memory leaks
  - **UBSan** (UndefinedBehaviorSanitizer) — signed overflow, null dereference, misaligned access, shift overflow
  - **MSan** (MemorySanitizer) — reads of uninitialized memory
  - **TSan** (ThreadSanitizer) — data races (relevant for future multi-threaded extensions)
  - Every commit runs `make debug` (ASan + UBSan enabled) against the full test suite. Every CI build runs under sanitizers. Bugs found by sanitizers are treated as release blockers.
- **Static analysis** — Clang `scan-build` (static analyzer) and `cppcheck --enable=all` run on every commit. Both must exit clean with zero findings before merging.
- **Fuzz-tested** — libFuzzer targets cover the primary attack surface (untrusted network input): HTTP parser + chunked decoder, multipart/form-data parser. Fuzz targets run with ASan + UBSan enabled. Corpus-driven, crash-reproducing, continuous.
- **LLM-based C audit** — Claude Code's `c-audit` skill reviews Hull's C modules for memory safety, buffer handling, integer overflow, and undefined behavior. Automated review catches patterns that static analysis misses. Used during development, not as a replacement for traditional tools — as a layer on top.
- **Lua-side quality** — Selene (Lua linter) and `luacheck` for static analysis of the Lua standard library. Hull's `hull test` framework runs the application test suite. LLM-friendly error output (file:line, stack trace, source context) enables AI-assisted debugging and review of Lua code.
- **Auditable** — 6 vendored C libs, ~1,500 lines of binding code. One person can review in a day.
- **Zero supply chain risk** — no npm, no pip, no crates.io, no package managers, no transitive dependencies
- **Encrypted at rest** — AES-256 database encryption, license-key-derived

### AI-First Development

- **LLM-first dev experience** — Lua is small (~60 keywords), consistent, LLM-friendly
- **Works with any AI assistant** — Claude Code, OpenAI Codex, OpenCode, Cursor — anything that writes code
- **Air-gapped development** — OpenCode + local model (minimax-m2.5, Llama, etc.) on your own hardware. Code never leaves your premises. Develop on M3 Ultra 512GB, fully offline.
- **No frontier model dependency** — Hull doesn't require GPT-4 or Claude. Any model that generates Lua works. Run your own.
- **LLM-friendly errors** — file:line, stack trace, source context, request details piped to terminal
- **LLM testing loop** — write, test, read output, fix, repeat — all in one terminal
- **Vibecoded apps are NOT cloud apps** — Hull breaks the default pipeline where AI produces React + Node + Postgres + Vercel. With Hull, AI produces Lua and the output is a single file. The developer owns the output.

### Runtime

- **2 MB total binary** — Keel + Lua + SQLite + mbedTLS + TweetNaCl + pledge/unveil
- **Batteries included** — routing, auth, RBAC, email, CSV, i18n, FTS, PDF, templates, validation, rate limiting, WebSockets
- **Single-threaded event loop** — easy to reason about, no race conditions, no deadlocks
- **Cooperative multitasking** — cron jobs, long-running batch, background tasks via Lua coroutines
- **Runs everywhere** — Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD from one binary
- **Air-gapped operation** — works offline, no phone-home, no telemetry, no activation server

### Modern Stack

- **HTML5/JS frontend** — any framework or vanilla JS; every widget and chart library already exists
- **The UI is the user's own browser** — Chrome, Edge, Safari, Opera. Already installed on every machine. No Electron, no embedded Chromium, no webview dependency.
- **WebSocket support** — real-time dashboards, live updates, push notifications
- **Full-text search** — SQLite FTS5, relevance ranking, Unicode, highlighted snippets
- **PDF, CSV, email** — export, import, send — all built in

### Distribution

- **Ship as a file** — one binary, runs on any OS, no installer, no runtime, no admin privileges
- **Built-in licensing** — Ed25519 signed keys, offline verification, tax-ID binding for compliance
- **Digital signatures** — platform and app signatures prove integrity and authorship
- **Reproducible builds** — same source + same Hull version = same binary, verifiable by anyone

## Thesis

The software industry spent 15 years pushing everything to the cloud. The result: subscription fatigue, vendor lock-in, privacy erosion, applications that stop working when the internet goes down, and products that vanish when startups die.

People want local tools they can control.

Hull is a platform for building single-file, zero-dependency, run-anywhere applications. You write your backend logic in Lua, your frontend in HTML5/JS, your data lives in SQLite, and Hull compiles everything into one executable. No servers. No cloud. No npm. No pip. No Docker. No hosting. Just a file.

## The False Choice

The industry keeps offering people the same two options for managing data and workflows:

**Excel** — your data is trapped in a format that mixes data, logic, and presentation into one file. Excel's fundamental problem isn't that it's bad — it's that the data, the logic, and the presentation are all the same thing. Change a column width and you might break a formula. Copy a sheet and the references break. Email a spreadsheet and now there are 15 conflicting versions. It corrupts when it gets too large. Every business has spreadsheets that should be applications. They stay as spreadsheets because building an application was too hard.

**SaaS** — your data is trapped on someone else's server, behind a subscription, with a Terms of Service that can change tomorrow. The vendor can raise prices, shut down, get acquired, or lose your data in a breach. You don't own anything — you rent access to your own information.

**Hull** — your data is a SQLite file on your computer. Your application is a file next to it. You own both. Forever.

Hull splits what Excel conflates:

```
Excel:     data + logic + UI = one .xlsx file (everything coupled)

Hull:      data    = SQLite    (queryable, relational, no corruption)
           logic   = Lua       (version-controlled, testable, separate)
           UI      = HTML5/JS  (forms, tables, charts, print layouts)
```

A bookkeeper using a Hull app doesn't know this separation exists. They see forms, tables, and buttons. But under the hood:

- The data can't be accidentally corrupted by dragging a cell
- The logic can't be broken by inserting a row
- The UI can be changed without touching the data
- Multiple users can't create conflicting copies because the database handles concurrency
- Backup is copying one file, not "which version of Q4_report_FINAL_v3_ACTUAL.xlsx is the right one"

Hull is the third option nobody's offering: properly structured data that you still control. No coupling of data and presentation. No subscription. No server. No lock-in. Just two files — one is the tool, one is your data — and they're both yours.

## What Hull Is

Hull is a self-contained application runtime that embeds six C libraries into a single binary:

| Component | Purpose | Size |
|-----------|---------|------|
| [Keel](https://github.com/artalis-io/keel) | HTTP server (epoll/kqueue/io_uring/poll) | ~60 KB |
| Lua 5.4 | Application scripting | ~280 KB |
| SQLite | Database | ~600 KB |
| mbedTLS | TLS client for external API calls | ~400 KB |
| TweetNaCl | Ed25519 license key signatures | ~8 KB |
| [pledge/unveil](https://github.com/jart/pledge) | Kernel sandbox (Justine Tunney's polyfill) | ~30 KB |

Total: under 2 MB. Runs on Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD from a single binary via Cosmopolitan C (Actually Portable Executable).

Hull is not a web framework. It is a platform for building local-first desktop applications that use an HTML5/JS frontend served to the user's browser. The user double-clicks a file, a browser tab opens, and they have a working application. Their data never leaves their machine.

## Why It Works This Way

Every design decision follows from one constraint: **the end user should be able to run, back up, and control the application without technical knowledge.**

**Single binary** because installation is "put file on computer." No installer, no runtime, no package manager, no PATH configuration, no admin privileges. Works from a USB stick. Works from Dropbox.

**Lua** because it was designed for exactly this: embedding in a C host to add scripting. It's 280 KB, compiles everywhere, has a clean C API refined over 30 years, and LLMs generate correct Lua reliably. The alternative was QuickJS (JavaScript), but Lua is smaller, simpler to embed, and doesn't carry the expectation of npm and async/await that would confuse the mental model.

**SQLite** because it's the most deployed database in the world, it's a single file, it works offline, it handles concurrent access via file locking, and backup is copying one file. No database server, no connection strings, no ports, no administration.

**mbedTLS** because applications that talk to external APIs (government tax systems, payment processors) need outbound HTTPS. mbedTLS is designed for embedding, small, and has no external dependencies. This is a TLS client, not a server — Hull uses Keel for inbound HTTP, which has its own pluggable TLS vtable for deployments that need inbound TLS.

**TweetNaCl** because applications sold commercially need license key verification. Ed25519 signatures in 770 lines of C, public domain, audited. No OpenSSL dependency. License verification happens locally, offline, in compiled C that is difficult to tamper with.

**Cosmopolitan C** because "runs anywhere" must mean anywhere. A single APE (Actually Portable Executable) binary runs on every major operating system without recompilation. The developer builds once. The user runs it on whatever they have.

**HTML5/JS frontend** because every widget, form element, date picker, and print stylesheet already exists. The developer writes standard HTML, CSS, and JavaScript — any framework or none. Building a native GUI toolkit would be a massive scope increase for no user benefit. The user doesn't know or care that localhost is involved. They see a browser tab with a form. That's an application to them.

**pledge/unveil** because "your data is safe" should be a provable technical guarantee, not a policy promise. A Hull application declares at startup exactly what it can access: its own database file, its own directory, one network port. The kernel enforces this via syscall filtering. The application physically cannot exfiltrate data, access other files, or spawn processes. This is not a sandbox configuration — it is a property of the binary, verifiable by inspection. Hull vendors [Justine Tunney's pledge/unveil polyfill](https://github.com/jart/pledge) which ports OpenBSD's sandbox APIs to Linux using SECCOMP BPF for syscall filtering and Landlock LSM (Linux 5.13+) for filesystem restrictions. On OpenBSD, the native pledge/unveil syscalls are used directly. On macOS and Windows, the sandbox is not available at the kernel level — Hull still runs, but without the syscall-level enforcement. The security model degrades gracefully: full kernel sandbox on Linux and OpenBSD, application-level safety guarantees everywhere else.

## What Gap It Fills

There is currently no way for a non-technical person to get a custom local application without hiring a developer to build a native app, set up a server, or configure a cloud deployment.

There is currently no way for a developer (or an AI coding assistant) to produce a single file that is a complete, self-contained, database-backed application with a web UI that runs on any operating system.

The closest alternatives and why they fall short:

**Electron** — produces local apps with web UIs, but each one is 200+ MB because it bundles an entire Chromium browser. Requires per-platform builds. No built-in database. No security sandboxing. Chromium alone pulls in hundreds of third-party dependencies — any one of which is a supply-chain attack vector. The `node_modules` tree for a typical Electron app contains 500-1,500 packages from thousands of maintainers. A single compromised dependency (event-stream, ua-parser-js, colors.js — all real incidents) can exfiltrate data from every app that includes it. The attack surface is too large for any team to audit.

**Docker** — solves deployment consistency but requires Docker Desktop (2+ GB), is Linux-only in production, and is a server deployment tool, not a local application tool.

**Go/Rust single binaries** — produce cross-platform executables but require per-platform compilation, have no built-in scripting layer (changes require recompilation), and ship no application framework. Both ecosystems rely on centralized package registries (crates.io, pkg.go.dev) with deep transitive dependency trees. A typical Rust web application pulls 200-400 crates; a typical Go web service pulls 50-150 modules. Each dependency is maintained by an independent author, auto-updated by bots, and trusted implicitly by the build system. The security of the entire application depends on the weakest link in a chain nobody has fully audited. Go's `go.sum` and Rust's `Cargo.lock` provide reproducibility but not auditability — they pin the versions of dependencies you haven't read.

**Datasette** — closest in spirit (SQLite + web UI), but requires Python installation, pip, and is read-oriented rather than a full application platform.

**Traditional web frameworks (Express, Django, Rails, Laravel)** — require runtime installation, package managers, database servers, and hosting. Designed for cloud deployment, not local-first applications. Every one of these stacks depends on a package manager ecosystem (npm, pip, composer, bundler) with the same supply-chain risks as Electron and Rust/Go, plus the operational attack surface of a production server.

Hull fills the gap: **a complete application runtime in under 2 MB that produces a single file containing an HTTP server, a scripting engine, a database, and a web UI, runnable on any operating system, with kernel-enforced security, requiring zero installation.**

## The Vibecoding Problem

AI coding assistants (Claude Code, Cursor, OpenCode) have made it possible for anyone to build software by describing what they want in natural language. But there's a gap between "the AI wrote my code" and "someone can use my application."

Today, every vibecoded project hits the same wall: **deployment.** The AI generates a React frontend and a Node.js backend. Now what? The vibecoder must learn about hosting, DNS, SSL certificates, database servers, environment variables, CI/CD pipelines, and monthly billing. They're forced to choose between:

- **Own your infrastructure (AWS, GCP, DigitalOcean)** — EC2 instances, RDS databases, load balancers, S3 buckets, IAM roles, security groups, CloudWatch alerts. A simple CRUD app becomes a distributed systems problem with a $50-200/month floor.

- **Platform-as-a-Service (Vercel, Netlify, Railway)** — simpler until it isn't. Usage-based pricing means you don't know your bill until it arrives. When [jmail.world](https://jmail.world) — a searchable archive of the Jeffrey Epstein emails built as a Next.js app on Vercel — went viral in late 2025, the creator woke up to a [$49,000 monthly bill](https://peerlist.io/scroll/post/ACTHQ7M7REKP7R67DHOOQ8JOQ7NNDM). Every hover, click, and page view triggered edge functions and bandwidth charges. Vercel's CEO personally covered the bill for PR reasons — but that's not a business model anyone can rely on.

- **Give up** — the most common outcome. The project stays on localhost, shown to nobody. The vibecoder's idea dies in a terminal window.

The core absurdity: a vibecoder who just wants to build a small tool for a small audience is funnelled into the same cloud infrastructure designed for applications serving millions. There is no middle ground between "runs on my laptop" and "deployed to the cloud."

Hull is that middle ground. The AI writes Lua instead of JavaScript. The data lives in SQLite instead of Postgres. The frontend is HTML5/JS served from the binary instead of a CDN. And when it's done, `hull build --output myapp.com` produces a single file the vibecoder can share, sell, or put in Dropbox. No server. No hosting bill. No $49,000 surprise. No deployment step at all.

The code the AI generates is the product. Not a codebase that needs infrastructure to become a product — the actual, distributable, runnable product.

## Who Hull Is For

### Developers & Software Houses

Professional developers and small teams (2-10 devs) building commercial local-first tools.

**The pain:** Deployment complexity. Hosting costs. Supply chain anxiety. Subscription model fatigue — both theirs and their users'. Every app they ship comes with an infrastructure bill and an operational burden that never ends. Small teams competing with VC-funded SaaS face margin pressure from hosting — every customer they add increases their AWS bill.

**Why Hull:** Single binary distribution. Ed25519 licensing built in. $0 infrastructure cost — zero hosting means 100% gross margin from sale one. AGPL + commercial dual license. Team license $299 for up to 5 developers. Sell a product, not a service.

**The outcome:** Ship to customers as a file. Zero DevOps, zero monthly bill, zero scaling anxiety. Revenue from day one.

### Vibecoders

Non-developers (or developer-adjacent) using LLMs to build their own tools.

**The pain:** They can describe what they want to an AI, but they can't deploy or distribute the result. The AI writes React + Node.js, and now they need to learn Docker, AWS, DNS, SSL, and CI/CD just to share what they built.

**Why Hull:** The LLM writes Lua instead of JavaScript. `hull build` creates a binary. Zero technical knowledge required for distribution. The output is a file, not a deployment problem.

**Air-gapped development:** Works with OpenCode + minimax-m2.5 on M3 Ultra 512GB — code never leaves your premises. Also works with Claude Code, OpenAI Codex, Cursor — if you're OK with cloud-based AI.

**The outcome:** Describe a tool, get a file, share it.

### SMBs (Small & Medium Businesses)

Small businesses trapped between Excel and SaaS.

**The pain:** Excel corruption, version chaos, SaaS subscription costs, data sovereignty concerns, vendor lock-in, GDPR compliance headaches. They're paying monthly for tools they don't fully control, storing sensitive business data on servers they don't own. They need an inventory tracker, an appointment scheduler, an invoice generator, a job costing calculator — small apps that solve their daily lives. Each one is either an overengineered SaaS at $30/month/seat or a fragile spreadsheet one wrong click away from disaster.

**Why Hull:** Own your data (single SQLite file). Own your tools (single binary). One-time purchase. Works offline. Encrypted database. Back up by copying a file. A vibecoder or local IT consultant describes the tool to an AI, `hull build` produces a file, and the business has software that works like enterprise software without the enterprise price tag or complexity.

**The outcome:** Custom tools that replace spreadsheets and SaaS subscriptions, owned outright, backed up by copying a file. No IT department required.

---

All three groups share a need: **turn application logic into a self-contained, distributable, controllable artifact.** Hull is the machine that does this.

## What Hull Is Not

**Hull is not a web framework.** It is a local application runtime that uses HTTP as its UI transport. The browser is the display layer, not the deployment target.

**Hull is not a cloud platform.** There are no hosted services, no managed databases, no deployment pipelines. The application runs on the user's machine and nowhere else.

**Hull is not a general-purpose server.** It is optimized for single-user or small-team local use. It does not include load balancing, horizontal scaling, caching layers, or message queues.

**Hull is not a mobile framework.** It targets desktop operating systems (Linux, macOS, Windows, BSDs). The Lua+SQLite core can be embedded in native mobile apps separately, but Hull itself produces desktop executables.

**Hull is not a replacement for SaaS.** Some applications genuinely need cloud infrastructure — real-time collaboration, massive datasets, global distribution. Hull is for the other applications: the ones that work better when they're local, private, and under the user's control.

## Architecture

Hull is a classic three-tier architecture — presentation, application logic, data — stripped to its minimum viable form and packaged into a single binary.

```
┌─────────────────────────────────────────────┐
│              Presentation Layer              │
│                  HTML5 / JS                  │
│                                              │
│  Forms, tables, charts, print layouts        │
│  Talks to backend via fetch() to localhost   │
│  Any framework or none (vanilla JS works)    │
└──────────────────────┬──────────────────────┘
                       │ HTTP (localhost)
┌──────────────────────┴──────────────────────┐
│              Application Layer               │
│                    Lua                        │
│                                              │
│  Routes, middleware, validation              │
│  Business logic, calculations, rules         │
│  Session management, auth                    │
│  External API calls (NAV, SDI, etc.)         │
└──────────────────────┬──────────────────────┘
                       │ Lua ↔ C bindings
┌──────────────────────┴──────────────────────┐
│                Data Layer                    │
│                 SQLite                        │
│                                              │
│  Schema, migrations, queries                 │
│  Parameterized access only (no raw SQL)      │
│  WAL mode, encryption at rest (optional)     │
│  Backup = copy one file                      │
└─────────────────────────────────────────────┘

All three layers packaged into a single binary by:

┌─────────────────────────────────────────────┐
│             Platform Layer (C)               │
│         Keel + Lua + SQLite + mbedTLS        │
│         + TweetNaCl + pledge/unveil          │
│                                              │
│  HTTP server, Lua runtime, DB engine         │
│  TLS client, license verification            │
│  Kernel sandbox, embedded assets             │
│  Build system (hull build → APE binary)      │
└─────────────────────────────────────────────┘
```

This is the same three-tier pattern that enterprise software got right 20 years ago with C#/WinForms/.NET + SQL Server. The architecture was proven — the weight was the problem. You don't need IIS, SQL Server, and a .NET runtime to separate data from logic from presentation. You need SQLite, Lua, and HTML — three things that fit in 2 MB.

| | C# 3-tier (2005) | Hull (2026) |
|---|---|---|
| Presentation | WinForms / WPF | HTML5/JS |
| Application | C# / .NET | Lua |
| Data | SQL Server | SQLite |
| Deployment | MSI installer + SQL Server + .NET runtime + IIS + days of IT work | Copy one file |
| Binary size | ~200 MB + runtime | ~2 MB total |
| Cross-platform | Windows only | Linux, macOS, Windows, BSDs |
| Licensing | Per-seat Windows + SQL Server + Visual Studio | Ed25519 key in a text file |
| Security | Windows ACLs | pledge/unveil, Lua sandbox, encrypted DB |
| Change logic | Recompile C#, redeploy | Edit Lua file, refresh browser |
| AI-buildable | No | Yes — LLMs generate Lua fluently |

Same proven pattern, 1/100th the weight, runs anywhere, buildable by an AI.

### Runtime (C layer)

The C layer handles everything Lua cannot do:

- **main.c** — startup sequence: parse arguments, open database, bind socket, open browser (if `--open`), apply pledge/unveil sandbox, initialize Lua, enter event loop
- **lua_bindings.c** — bridge between Keel's `KlRequest`/`KlResponse` and Lua tables. Exposes request properties (method, path, headers, params, query, body) and response methods (status, header, json, html, file, redirect) to Lua
- **lua_db.c** — bridge between SQLite and Lua. Exposes `db.query()` (returns rows as Lua tables), `db.exec()` (for mutations), prepared statement caching, migration runner. **SQL injection is structurally impossible:** all queries go through `sqlite3_prepare_v2` + `sqlite3_bind_*`. There is no string-concatenated SQL path exposed to Lua. The `?` parameter binding is the only way to pass values into queries
- **lua_crypto.c** — bridge between TweetNaCl and Lua. Exposes signature verification, hashing. Used internally by the license system, available to application code
- **license.c** — Ed25519 license key verification. Reads `license.key`, validates signature against the public key embedded at build time, extracts licensed modules and expiry date. Checks are scattered across multiple C functions to resist patching
- **http_client.c** — minimal HTTPS client built on mbedTLS. Connects to declared API endpoints, POSTs data, reads responses. Enforces the `allowed_hosts` allowlist at the C layer before connecting — Lua code cannot bypass it
- **smtp.c** — minimal SMTP client built on mbedTLS. Handles EHLO, STARTTLS, AUTH (PLAIN/LOGIN), and MIME multipart message construction. For on-premise and air-gapped environments that run their own mail servers. Subject to the same `allowed_hosts` enforcement as HTTP
- **timer.c** — min-heap timer scheduler. Manages `app.every`, `app.daily`, `app.weekly`, `app.after` callbacks. Integrates with the event loop timeout to wake up at the right moment without polling
- **task.c** — background task runner. Manages Lua coroutines spawned by `app.spawn`, resumes them between HTTP requests with a per-iteration time budget. Implements `app.yield` and `app.sleep`
- **embed.c** — Hull's Lua standard library compiled into the binary as byte arrays. Loaded by a custom `require()` searcher before the filesystem loader, so the standard library is always available regardless of what files exist on disk

### Standard Library (embedded Lua layer)

These ship inside the binary. The user never manages them.

- **app** — route registration, middleware chain, static file serving, response helpers, lifecycle management
- **db** — query helpers, migration runner, backup, pagination (`db.paginate`), transaction wrappers. All queries use parameterized binding (`?` placeholders) — no string-concatenated SQL path exists
- **json** — JSON encode/decode (pure Lua)
- **csv** — RFC 4180 CSV encode/decode. Handles quoted fields, embedded commas/newlines, separator auto-detection. ~150 lines
- **email** — unified email API with provider abstraction (Postmark, SendGrid, SES, Resend, Mailgun, SMTP). `email.send()` works identically regardless of transport
- **auth** — password hashing (`auth.hash_password`, `auth.verify_password`), secure token generation (`auth.random_token`). Wraps PBKDF2-HMAC-SHA256 from mbedTLS and `randombytes` from TweetNaCl
- **rbac** — role-based access control. Role definitions, permission wildcards, `rbac.require()` middleware guard. Stores roles in SQLite
- **log** — structured logging to SQLite (`log.info`, `log.warn`, `log.error`, `log.debug`). Auto-cleanup via built-in scheduled task
- **i18n** — internationalization. Key-value string lookup with `${var}` interpolation, locale-aware number/date/currency formatting, `Accept-Language` detection. ~200 lines
- **search** — SQLite FTS5 wrapper. Declarative index definition, automatic sync triggers, relevance-ranked results with highlighted snippets. ~80 lines
- **template** — HTML template engine with `{{ }}` expressions (HTML-escaped), `{{{ }}}` raw output, `{% %}` Lua code blocks, and layout inheritance. ~150 lines
- **pdf** — PDF document builder for business documents (text, tables, built-in fonts, page breaks, alignment). Pure Lua, ~500-600 lines, no C dependency
- **test** — test framework for `hull test`. Request simulation (`t.get`, `t.post`), assertions (`t.equal`, `t.ok`, `t.match`), per-test database isolation
- **valid** — input validation with schema declaration. Type checking, bounds, patterns, nested table validation, unknown field stripping. ~150 lines
- **limit** — rate limiting middleware. Token bucket algorithm, per-route configuration, in-memory counters. ~80 lines
- **session** — cookie-based sessions stored in SQLite
- **csrf** — CSRF token generation and verification middleware

### User Code (filesystem Lua layer)

The developer writes these. In development they live on the filesystem for hot-reload. In production they are embedded into the binary.

```
my-app/
  app.lua             # entry point: routes, middleware, config
  routes/
    invoices.lua      # domain routes
    customers.lua
    settings.lua
  middleware/
    auth.lua          # custom middleware
  modules/
    nav_hu.lua        # external API integration
  templates/
    layout.html       # base layout (nav, footer, head)
    invoice.html      # invoice page template
    invoice_list.html # invoice listing template
    invoice_email.html# email body template
  locales/
    en.lua            # English strings
    hu.lua            # Hungarian strings
  migrations/
    001_settings.sql  # database schema
    002_invoices.sql
  tests/
    test_auth.lua     # test files (hull test)
    test_invoices.lua
  public/
    index.html        # SPA frontend (if using SPA)
    app.js
    style.css
```

### Resolution Order

When Lua calls `require("something")`:

1. **Compiled-in Hull standard library** (app, db, json, session, valid, csrf)
2. **Compiled-in user modules** (only in production builds)
3. **Filesystem** (development, or hot-patching production)

This means development is live-reload from files, production is self-contained in the binary, and emergency patches can override embedded code by dropping a file next to the executable.

## Why C and Lua

Hull is a C core with a Lua scripting layer. Not Rust. Not Go. Not TypeScript. Here's why.

### Why C for the runtime

**Cosmopolitan requires C.** The entire "single binary, runs anywhere" capability comes from Cosmopolitan C. There is no Cosmopolitan Rust, no Cosmopolitan Go. APE binaries are a C toolchain feature. Choosing any other language for the runtime would mean giving up the core premise of the project.

**The vendored libraries are C.** Lua is C. SQLite is C. mbedTLS is C. TweetNaCl is C. Keel is C. There is no FFI boundary, no marshalling overhead, no ABI compatibility layer. One compiler, one toolchain, one language, one binary. A Rust runtime wrapping five C libraries would spend more lines on `unsafe` FFI bindings than on actual logic.

**The C surface is small.** Hull's own C code is ~1,500 lines — the binding layer between Lua and the vendored libraries. This is not a 100,000-line C codebase where memory safety is a daily concern. It's a thin integration layer where every allocation is visible, every buffer has a bound, and one person can audit the whole thing in a day. The risk profile of 1,500 lines of carefully written C is lower than the risk profile of pulling in a Rust async runtime with 200 transitive crates.

**C compiles in seconds.** A clean build of Hull takes under 3 seconds. A comparable Rust project with serde, tokio, hyper, rusqlite, and rlua would take 60-120 seconds. During development, this matters. When a vibecoder's AI assistant is iterating on the platform, sub-second rebuilds are a feature.

### Why not Rust

Rust's safety guarantees are real. For a large-team, high-churn application codebase, they pay for themselves. For Hull, they don't — and they cost more than they save.

**No Cosmopolitan support.** This alone is disqualifying. Hull without APE binaries is not Hull. It's just another framework that produces per-platform executables.

**FFI defeats the safety model.** Hull's runtime is glue between C libraries. Every Lua API call crosses `unsafe`. Every SQLite call crosses `unsafe`. Every mbedTLS call crosses `unsafe`. The borrow checker protects the 10% of code between FFI boundaries while the 90% that does actual work is `unsafe` anyway.

**Cargo is a supply chain.** Rust's package ecosystem is excellent for application development and problematic for a project whose security story is "six vendored libraries, readable in an afternoon." A Rust rewrite would pull in rusqlite (depends on libsqlite3-sys), rlua or mlua (depends on lua-src), rustls or native-tls, serde, tokio or async-std — each with its own transitive dependency tree. The auditable, zero-dependency property vanishes.

**Complexity budget.** Rust's type system, lifetime annotations, and trait bounds are powerful tools that consume cognitive budget. In Hull's C code, a Lua binding function is: get arguments from Lua stack, call C function, push results to Lua stack. In Rust it becomes: generic over lifetime parameters, wrapped in a `UserData` impl, with `FromLua`/`ToLua` trait bounds, error types converted through `From` impls. The code is safer. It's also 3x longer and harder for a new contributor (or an AI) to understand and modify.

### Why not Go

**No Cosmopolitan support.** Go produces per-platform binaries. No APE, no single-binary-runs-everywhere.

**Go embeds Lua poorly.** Go's goroutine scheduler and garbage collector conflict with Lua's C-stack-based coroutines. The gopher-lua pure-Go implementation exists but is 5-10x slower than C Lua and doesn't support the full C API that existing Lua libraries depend on.

**Go embeds SQLite poorly.** The standard go-sqlite3 binding uses CGo, which disables cross-compilation, slows builds, and defeats Go's deployment simplicity. Pure-Go SQLite ports exist but are slower and less battle-tested.

**Runtime overhead.** Go's garbage collector, goroutine scheduler, and runtime add ~10 MB to the binary and introduce GC pauses. Hull's total binary is under 2 MB with no GC.

### Why not TypeScript / Bun / Deno

**Runtime size.** Bun is ~50 MB. Deno is ~80 MB. Node is ~40 MB. Hull is under 2 MB. The JavaScript runtime alone is 20-40x larger than the entire Hull binary.

**No Cosmopolitan support.** These runtimes produce per-platform executables (or require installation).

**Dependency culture.** The npm ecosystem normalises pulling in hundreds of packages for trivial functionality. A Hull application has zero dependencies beyond what's vendored in the binary. A typical Next.js project has 800+ transitive dependencies in `node_modules`. One malicious or compromised package in that tree undermines the entire security story.

**V8/JavaScriptCore are not auditable.** These JavaScript engines are millions of lines of C++. They are large, complex attack surfaces maintained by browser vendor teams with hundreds of engineers. Lua 5.4 is 30 files of ANSI C, maintained for 30 years, and genuinely auditable by a single person.

### Why Lua for application code

**Designed for embedding.** Lua exists because Roberto Ierusalimschy and his team at PUC-Rio needed a scripting language that could be embedded in C programs. That was the design goal from day one in 1993. Thirty years of refinement for exactly this use case. The C API (stack-based value passing, registry, metatables) is clean, stable, and complete.

**Battle-tested in hostile environments.** Lua runs inside Redis (handling untrusted scripts from network clients), inside OpenResty/Nginx (processing HTTP requests at scale), inside game engines (running player-authored mods), inside Wireshark (parsing network packets), inside industrial controllers (where crashes mean physical damage). These are environments where the scripting layer must be sandboxable, deterministic, and reliable. Hull's use case — running application logic in a local HTTP server — is tame by comparison.

**LLM-friendly.** Lua has a small grammar (~60 keywords and operators), minimal syntax, consistent semantics, and no gotchas like JavaScript's type coercion or Python's significant whitespace. LLMs generate correct Lua more reliably than most languages. For a platform whose primary audience includes vibecoders working with AI assistants, this matters.

**Sandboxable from C.** The C host creates the Lua environment and controls what's in it. Remove `os.execute` and it doesn't exist — not deprecated, not blocked by policy, removed from the runtime entirely. No other mainstream scripting language gives the host this level of control with this little code.

**Small and fast enough.** Lua 5.4 compiles to ~280 KB. Plain Lua (not LuaJIT) is interpreted, roughly 5-15x slower than C for pure computation — and significantly faster than CPython (~10-30x faster) and Ruby (~5-10x faster). Lua consistently ranks among the fastest interpreted scripting languages in existence. Hull deliberately uses plain Lua 5.4 rather than LuaJIT: LuaJIT is faster (near C speed for hot paths) but is stuck on Lua 5.1 semantics, is maintained by a single person, has limited platform support, and adds complexity to the build. Plain Lua 5.4 has integers, generational GC, and a clean C API — and it compiles everywhere Cosmopolitan does.

For Hull's workloads, Lua speed is irrelevant to overall performance. A typical request cycle is: parse HTTP (C, Keel), run a Lua handler (microseconds of Lua), query SQLite (C, milliseconds of disk I/O), format a JSON response (microseconds of Lua), send HTTP (C, Keel). The bottleneck is always I/O — SQLite queries, network writes, external API calls — never Lua execution. OpenResty proved this at scale: plain Lua (and LuaJIT) scripting inside Nginx handles millions of requests per second. Hull's local-first use case with 1-5 users is trivial by comparison.

**The 1-indexed arrays are annoying.** Yes. Live with it.

## Build System

The build tool is `hull.com` — itself a Hull APE binary. See the **hull.com — The Build Tool** section for full details on how hull.com works, its signature model, ejection, and bootstrapping.

### Development

```bash
hull dev                        # run from source files, hot-reload
hull dev --open                 # same, but open browser automatically
hull dev --port 9090            # custom port
```

Lua files are loaded from the filesystem on each request (in dev mode). Change a file, refresh the browser, see the result. No restart, no compilation.

### Production Build

```bash
hull build --output myapp.com
hull build --output myapp.com --sign developer.key   # + developer signature
```

This:

1. Verifies hull.com's own platform signature (ensures untampered C runtime)
2. Scans the project directory for all Lua, SQL, template, locale, and static files
3. Stamps the verified Hull C runtime into a new APE binary
4. Embeds all collected artifacts as byte arrays
5. Optionally signs the app layer with the developer's Ed25519 key
6. If `license.key` exists, embeds the commercial license
7. The output is one file that contains the entire application

### What Gets Embedded

```
app.lua              -> embedded
routes/*.lua         -> embedded
middleware/*.lua      -> embedded
modules/*.lua        -> embedded
templates/*.html     -> embedded
locales/*.lua        -> embedded (i18n string tables)
migrations/*.sql     -> embedded
public/**            -> embedded (HTML, CSS, JS, images)
```

### Distribution

The developer ships:

```
myapp.com            # the entire application
license.key          # per-customer (if licensing is used)
```

The user creates at runtime:

```
data.db              # SQLite database (their data)
```

Three files. One is the application, one proves ownership, one is their data. Backup means copying these files. Moving to a new computer means copying these files.

## hull.com — The Build Tool

hull.com is an APE binary that scaffolds, develops, builds, and verifies Hull applications. It is itself a Hull app — the Hull C runtime plus a Lua application layer that handles CLI commands instead of HTTP routes. Hull is built with Hull.

### Commands

```
hull new myapp                  # scaffold new project
hull dev                        # development server (hot reload, debug)
hull build                      # build APE binary
hull build --sign key.pem       # build + sign with developer key
hull test                       # run tests
hull backup                     # backup database
hull restore file.bak           # restore database
hull verify app.com             # verify signatures on any Hull binary
hull inspect app.com            # show sandbox declarations + embedded files
hull eject                      # copy build pipeline into project
hull version                    # show version + signature info
hull license activate KEY       # activate commercial license
```

### What's Inside hull.com

```
hull.com (APE binary, ~2 MB)
├── Hull C Runtime (signed by artalis-io)
│   ├── Keel          # HTTP server
│   ├── Lua 5.4       # scripting
│   ├── SQLite         # database
│   ├── mbedTLS        # TLS
│   ├── TweetNaCl      # signatures
│   └── pledge/unveil  # sandbox
├── Build Tool Lua Layer (signed by artalis-io)
│   ├── cli.lua        # command dispatch
│   ├── scaffold.lua   # project templates
│   ├── build.lua      # artifact collection, binary assembly
│   ├── dev.lua        # development server wrapper
│   └── verify.lua     # signature verification
├── Platform Signature  # Ed25519 over C runtime
└── App Signature       # Ed25519 over Lua build tool layer
```

Both layers are signed independently using the same dual-signature model described in the Security section. The platform signature proves the C runtime is the official Hull build. The app signature proves the build tool's Lua code is unmodified. hull.com eats its own dogfood.

### The Build Guarantee

When you run `hull build`:

```
1. hull.com verifies its OWN platform signature
   → "Am I running on an official, untampered Hull runtime?"
   → If verification fails: WARN (ejected copies may be unsigned)

2. Collect project artifacts:
   ├── app.lua + routes/*.lua + modules/*.lua    → Lua source or bytecode
   ├── templates/*.html                          → embedded strings
   ├── locales/*.lua                             → embedded Lua tables
   ├── migrations/*.sql                          → embedded SQL
   ├── public/*                                  → embedded static assets
   └── license.key (if exists)                   → embedded license

3. Stamp the verified Hull C runtime into a new APE binary
   → The runtime is byte-identical to the signed runtime in hull.com
   → Not recompiled, not modified, not patched — stamped directly

4. Embed all collected artifacts as byte arrays in the binary

5. If --sign key.pem: sign the app layer with developer's Ed25519 key

6. If AGPL (no commercial license): embed Lua source (AGPL requires it)
   If commercial: embed Lua bytecode (source optional via --include-source)

7. Output: myapp.com (single APE binary, runs everywhere)
```

The critical guarantee: **the Hull runtime in your built app is byte-identical to the signed runtime in hull.com.** `hull verify myapp.com` can confirm the platform layer is exactly the one artalis-io signed, regardless of what Lua code the developer embedded.

### Versioning

hull.com follows semver. Version is embedded in the binary.

```
$ hull version
Hull 1.2.0 (2026-03-15)
Platform:   signed by artalis-io (ed25519:a1b2c3...)  ✓
Build tool: signed by artalis-io (ed25519:d4e5f6...)  ✓
License:    Standard — expires 2027-03-15 (mark@artalis.io)
```

### Scaffolding

```bash
$ hull new invoicing
Created invoicing/
  app.lua             # entry point with example route
  migrations/
    001_init.sql      # example schema
  templates/
    layout.html       # base layout
  locales/
    en.lua            # English strings
  public/
    index.html        # SPA starter (or empty for server-rendered)
    style.css
  tests/
    test_app.lua      # example test
  .gitignore
```

The scaffolded project is immediately runnable: `cd invoicing && hull dev --open`.

### Ejection

`hull eject` copies the hull.com binary into the project directory, making the project fully self-contained:

```
Before:
myapp/
  app.lua
  routes/
  templates/
  ...

After:
myapp/
  app.lua
  routes/
  templates/
  ...
  .hull/
    hull.com            # frozen copy of the build tool
    EJECTED.md          # explains the trade-offs
```

After ejection:
- `./.hull/hull.com build` instead of `hull build`
- Project is fully self-contained — no global install needed
- Zero external dependencies: the repo contains everything to build the final binary
- CI can clone and build without installing anything

**What you lose:**
- The ejected hull.com is frozen at the version you ejected
- No automatic security patches for the runtime
- Updating requires manually replacing `.hull/hull.com` or running `hull eject --update`

**What you keep:**
- The ejected binary is still signed — its signatures don't expire
- All functionality works identically
- `hull verify` still validates the platform signature

The trade-off is explicit: **portability vs. freshness.** Non-ejected projects always build with the latest verified hull.com. Ejected projects are self-contained but frozen. This mirrors the vendoring vs. package manager tension — Hull makes both paths first-class.

### Bootstrapping

How is hull.com itself built the first time?

```
1. First build (Makefile + cosmocc):
   make CC=cosmocc         → builds C runtime from source
   Embed build tool Lua    → hull.com
   Sign with artalis-io's Ed25519 key

2. Subsequent builds (self-hosting):
   hull build              → hull.com builds the next version of itself
   Sign with artalis-io's Ed25519 key

3. CI verification:
   Build from source (make) AND from hull.com (hull build)
   Compare outputs          → must be identical (reproducible builds)
```

After the first bootstrap, hull.com is self-hosting. But the Makefile path always exists as an escape hatch — anyone can build from source without hull.com. The CI pipeline runs both paths and verifies they produce identical output.

## Licensing

Hull is dual-licensed: **AGPL-3.0 + Commercial**.

### The AGPL Side (Free)

The Hull source code — C runtime, build tool, standard library — is AGPL-3.0. This means:

- Anyone can read, build, audit, and fork Hull
- Anyone can use Hull for free if their application is also AGPL (source distributed with every binary)
- Open-source projects use Hull at zero cost

### The Commercial Side (Paid)

A commercial license exempts the developer's Lua application layer from AGPL obligations. Distribute closed-source apps without sharing source code.

| | Standard | Team | Perpetual |
|---|---|---|---|
| Price | **$99** one-time | **$299** one-time | **$499** one-time |
| Developers | 1 | Up to 5 | 1 |
| Commercial license | Perpetual | Perpetual | Perpetual |
| Hull updates | 1 year | 1 year | Lifetime |
| Update renewal | $49/year | $149/year | N/A |
| Apps you can ship | Unlimited | Unlimited | Unlimited |
| End-user seats | Unlimited | Unlimited | Unlimited |

**Key details:**

- The **commercial license itself is perpetual** — you never lose the right to distribute closed-source apps. Even if you don't renew updates, your existing apps and the last hull.com version you received continue to work forever.
- **Update access** is what has a term. Standard gets 1 year of hull.com updates. After that, you keep using the last version you downloaded. It still works. It still builds. You just don't get new features or security patches until you renew.
- **Per-developer, not per-app, not per-end-user.** One Standard license = one developer = unlimited Hull apps = unlimited end users of those apps.
- **No feature gating.** AGPL and commercial builds have identical features. The license is a legal instrument, not a DRM mechanism. There is no "community edition" vs "enterprise edition."

### What Changes With vs. Without a License

| | AGPL (free) | Commercial ($99/$299/$499) |
|---|---|---|
| Build apps | Yes | Yes |
| All features | Yes | Yes |
| Distribute binaries | Yes, with Lua source | Yes, without source |
| `hull inspect` shows | Security profile + Lua source | Security profile only (bytecode) |
| Startup banner | "Hull (AGPL-3.0)" | No branding requirement |
| End users see source | Required (AGPL) | Not required |

AGPL builds embed Lua source (the license requires it). Commercial builds embed Lua bytecode by default — the developer can opt in to including source with `hull build --include-source`.

### How the License Key Works

The license key is an Ed25519-signed JSON payload:

```json
{
    "developer": "mark@artalis.io",
    "type": "perpetual",
    "issued": "2026-03-01",
    "updates_until": "forever",
    "signature": "ed25519:..."
}
```

`hull build` embeds it into the output binary. The built app can display license info (`hull --license`). Verification is offline — no phone-home, no activation server, no internet required. Same Ed25519 cryptography used for platform signatures (TweetNaCl).

```bash
hull license activate HULL-XXXX-XXXX-XXXX-XXXX
```

### App-Level Licensing (Optional)

Separate from the Hull platform license, developers who sell their Hull apps can use the built-in Ed25519 licensing system for their own customers:

The developer generates a key pair. The private key stays on their machine. The public key is embedded in the binary at build time. A customer license key is a signed payload containing:

```
company=Kovacs Pekseg Kft.
tax_id=12345678-2-42
modules=invoicing,nav_hu
expires=2027-01-01
signature=<Ed25519 signature of the above>
```

At startup, the compiled C code verifies the signature against the embedded public key. No phone-home, no activation server, no internet required.

**Why tax ID binding works for business apps:** For invoicing and compliance applications, the license is bound to the company's tax identifier. Every document the application generates must legally contain the issuer's tax ID. If company A shares their license with company B, every document company B generates carries company A's tax identity — tax fraud. The government enforces your licensing for you.

**What stops piracy:** Nothing stops a determined reverse engineer. The threat model is a business owner's nephew, not a hacker. Compiled C verification with checks scattered across multiple functions is sufficient to prevent casual sharing. The person who would disassemble and patch the binary was never going to pay. The real protection is the update pipeline: compliance modules change when regulations change. A copied binary is a snapshot.

## Security Model

Hull's security is layered:

**Kernel sandbox (pledge/unveil)** — the application declares its capabilities at startup, before entering the event loop. After the sandbox is applied, the process cannot:

- Access files outside its declared paths
- Open network connections beyond its declared scope
- Spawn child processes
- Execute other programs
- Escalate privileges

This is enforced by the operating system kernel. It is not a policy — it is a syscall-level guarantee. Hull vendors Justine Tunney's [pledge/unveil polyfill](https://github.com/jart/pledge) for Linux (SECCOMP BPF + Landlock LSM). On OpenBSD, native syscalls are used. On macOS and Windows, the sandbox is unavailable at the kernel level — the application runs without syscall restrictions, relying on OS-level permissions and application-layer safety instead. The strongest security posture is achieved on Linux and OpenBSD.

**Supply chain** — six vendored C libraries, all designed for embedding, all auditable. No package manager, no transitive dependencies, no lockfiles, no supply chain attacks. The entire dependency tree is readable in an afternoon.

**SQL injection prevention** — all database access goes through parameterized queries at the C binding layer. Lua code cannot construct raw SQL that reaches SQLite without parameter binding.

**License verification in compiled C** — not in Lua scripts that can be trivially edited. Verification checks are distributed across multiple functions in the compiled binary.

**Minimal, auditable C surface** — Hull's own C code (the binding layer, license verification, HTTP client, startup sequence) is roughly 1,500 lines. Not 150,000 — fifteen hundred. A single security auditor can read the entire codebase in a day. The vendored libraries (Keel, Lua, SQLite, mbedTLS, TweetNaCl) are all established, battle-tested projects with decades of combined security scrutiny. The total attack surface is small and enumerable.

**Lua sandbox and restricted file I/O** — Lua was designed to be embedded in hostile environments (game engines, network equipment, industrial controllers). The C host controls exactly which Lua standard library functions are available. Hull removes the entire `io` and `os` standard libraries, plus `loadfile` and `dofile`, from the Lua environment before any user code runs. These functions do not exist in the runtime — not deprecated, not blocked by policy, removed entirely.

In their place, Hull exposes a restricted file API implemented in compiled C. Every file operation validates the path against the application's declared data directory before touching the filesystem:

```c
// Hull's C layer — exposed to Lua as hull.fs.*
// Every function resolves the path and checks it against the allowed directory

static int hull_fs_read(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    char resolved[PATH_MAX];
    if (!resolve_and_check(path, resolved, allowed_data_dir))
        return luaL_error(L, "access denied: %s", path);
    // safe — read and return contents
    ...
}

static int hull_fs_write(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    const char *data = luaL_checkstring(L, 2);
    char resolved[PATH_MAX];
    if (!resolve_and_check(path, resolved, allowed_data_dir))
        return luaL_error(L, "access denied: %s", path);
    // safe — write data
    ...
}

static int hull_fs_exists(lua_State *L) { ... }   // path-checked
static int hull_fs_delete(lua_State *L) { ... }   // path-checked
static int hull_fs_list(lua_State *L)   { ... }   // path-checked
```

The `resolve_and_check` function resolves symlinks, rejects `..` traversal, and verifies the resolved absolute path starts with the allowed data directory. This is compiled C — Lua code cannot override, monkey-patch, or bypass it.

Lua application code uses the restricted API:

```lua
-- These work — path is inside the data directory
local csv = hull.fs.read("exports/report.csv")
hull.fs.write("exports/q4.csv", csv_data)
local files = hull.fs.list("uploads/")

-- These fail — path resolves outside the data directory
hull.fs.read("/etc/passwd")              -- error: access denied
hull.fs.read("../../secrets.txt")        -- error: access denied
hull.fs.write("/tmp/exfiltrated.db", d)  -- error: access denied
```

File uploads from the browser arrive via HTTP POST (Keel handles this). File downloads go out as HTTP responses. The browser mediates all user-facing file selection — Hull never needs access to the user's general filesystem.

This Lua sandbox is the **primary security layer on all platforms**, including macOS and Windows where pledge/unveil is not available at the kernel level. On Linux and OpenBSD, pledge/unveil provides a second, kernel-enforced layer — even if a bug in Hull's C code or the Lua VM itself were exploited, the kernel would still block unauthorized filesystem access.

```
                Linux/OpenBSD    macOS         Windows
                ─────────────    ─────         ───────
Lua sandbox:    active           active        active
(C path check,  (same)           (same)
 io/os removed)

pledge/unveil:  active           —             —
(kernel-level
 enforcement)
```

The honest assessment: Linux with pledge/unveil is the most secure deployment. macOS and Windows with only the Lua sandbox are secure enough for the threat model — accidental file access, malicious Lua modules, path traversal bugs. You would need to exploit a vulnerability in Hull's own C code or the Lua VM to bypass the sandbox, at which point you have arbitrary code execution and no application-level sandbox of any kind would help.

**Restricted network access** — Hull applications declare their network needs at build time. The C layer enforces a host allowlist — Lua code can only make outbound HTTP requests to explicitly declared endpoints:

```lua
-- app.lua — declared at startup, before sandbox
app.config({
    allowed_hosts = {
        "api.nav.gov.hu",        -- Hungarian tax authority
        "fatturapa.gov.it",      -- Italian SDI
    },
})
```

The C-layer HTTP client checks every outbound request against this list before connecting:

```c
// http_client.c — compiled C, not bypassable from Lua
static int hull_http_request(lua_State *L) {
    const char *url = luaL_checkstring(L, 1);
    char host[256];
    if (parse_host(url, host, sizeof(host)) < 0)
        return luaL_error(L, "invalid URL");

    if (!is_allowed_host(host))
        return luaL_error(L, "network access denied: %s", host);

    // safe — proceed with mbedTLS connection
    ...
}
```

Three tiers of network access:

| Mode | `allowed_hosts` | What it means |
|------|-----------------|---------------|
| Offline (default) | `{}` (empty) | No outbound network. Pure local app. |
| Specific APIs | `{"api.nav.gov.hu", ...}` | Can only reach declared endpoints. |
| Open | `{"*"}` | Unrestricted outbound (developer's choice). |

On Linux, the offline default is reinforced by omitting the `inet` pledge promise entirely. Applications that declare `allowed_hosts` get `inet` in their pledge. The kernel enforces what the C layer already checks — defense in depth.

The `allowed_hosts` list is visible in the application's `app.lua` — readable by anyone who has the source, and inspectable in development builds. In production builds, it is embedded in the binary but still printed at startup:

```
Hull v0.1.0 | port 8080 | db: data.db
Network: api.nav.gov.hu, fatturapa.gov.it
Sandbox: pledge(stdio rpath wpath inet) unveil(data.db:rw, public/:r)
```

**User-facing transparency (trust model)** — Hull's security model protects users not only from bugs and accidents, but provides tools to evaluate trust in the application developer themselves.

The fundamental question: *"I downloaded a Hull app. How do I know it's not stealing my data?"*

Hull makes the answer inspectable rather than requiring blind trust:

**1. Visible sandbox declarations.** Every Hull application prints its security posture at startup — the filesystem paths it can access, the network hosts it can reach, and its pledge promises. A user (or their IT department, or a reviewer) can see exactly what the application is allowed to do:

```
Hull v0.1.0 | port 8080 | db: data.db
Network: OFFLINE (no outbound connections)
Sandbox: pledge(stdio rpath wpath) unveil(data.db:rw, public/:r)
```

An invoicing app that shows `Network: OFFLINE` physically cannot phone home. An app that shows `Network: api.nav.gov.hu` can only reach the Hungarian tax authority. This is not a policy claim — it is what the kernel enforces.

**2. Readable Lua source.** In development mode, all application logic is plain Lua files on disk — readable, searchable, auditable. In production builds, `hull inspect` always shows the security profile (sandbox declarations, network access, filesystem paths). For AGPL builds, it also extracts the full Lua source (required by the license). For commercial builds, source extraction is available only if the developer opted in with `--include-source`:

```bash
hull inspect myapp.com              # list embedded files + security profile
hull inspect myapp.com app.lua      # print a specific file (AGPL or opted-in)
hull inspect myapp.com --extract    # extract all files to disk
hull inspect myapp.com --security   # show only security profile (always works)
```

Lua is a simple, readable language. A competent reviewer can read an entire Hull application's logic in an hour. This is not true of a minified React bundle or compiled Go binary.

**3. Deterministic, reproducible builds.** Given the same source files and Hull version, `hull build` produces the same binary. A third party can verify that a distributed binary matches its claimed source:

```bash
hull build --output myapp-verify.com
sha256sum myapp.com myapp-verify.com    # must match
```

This enables community-verified builds: the developer publishes source, anyone can reproduce the binary and confirm it matches what's distributed.

**4. No hidden capabilities.** Hull applications cannot:
- Load native code or shared libraries (no `ffi`, no `dlopen`)
- Execute shell commands (no `os.execute`, no `io.popen` — removed from Lua)
- Access files outside their declared directory (C-layer path validation + unveil)
- Connect to undeclared network hosts (C-layer allowlist + pledge)
- Spawn child processes (pledge blocks `exec`)

What the application *can* do is the union of its `allowed_hosts`, its `unveil` paths, and its `pledge` promises — all visible at startup and all kernel-enforced on Linux/OpenBSD.

**5. Community review and trust signals.** Hull applications are small enough to review. The entire attack surface of a Hull app is:
- ~500-2000 lines of Lua (the application logic)
- The `app.lua` config (filesystem paths, network hosts, features)
- The SQL migrations (database schema)

This is reviewable by a single person in an afternoon. Open-source Hull apps can be community-audited. Commercial Hull apps can be reviewed by a customer's IT team before deployment. The small surface area makes meaningful code review practical — unlike a 200MB Electron app or a cloud SaaS where you can't see the code at all.

**The honest assessment:** A malicious developer with `allowed_hosts = {"*"}` (unrestricted network) and broad filesystem access could exfiltrate data. But this is visible — the startup banner shows `Network: * (unrestricted)`, and `hull inspect` reveals exactly what the code does. The point is not that Hull prevents all malice — it's that Hull makes malice *detectable*. A user who sees `Network: OFFLINE` knows their data stays local. A user who sees `Network: *` knows to ask why. Contrast this with any SaaS where you have zero visibility into what the server does with your data.

### Platform Integrity — Verifying the C Layer

Everything above assumes the Hull platform itself is honest — that the compiled C code actually enforces pledge/unveil, actually checks the host allowlist, actually restricts file I/O. But what if a malicious app developer forks Hull, guts the sandbox enforcement, and ships a binary that *looks* like Hull but doesn't enforce anything? `hull inspect` shows the Lua source, but it can't prove the C layer underneath is legitimate.

This is the deepest trust problem. Checksums and hashes are not enough — they tell you the file hasn't been *corrupted*, not that it came from a *trusted source*. You need digital signatures: a cryptographic proof that ties a binary to the identity that produced it.

Hull solves this with two independent signatures, each answering a different question:

#### Signature 1: Hull project signs the platform (mandatory)

The Hull project maintains an Ed25519 key pair. The public key is published in the Hull repository, on the project website, and in every Hull release announcement. The private key is held by the project maintainers and used only in CI.

When the Hull project builds an official release, the CI pipeline signs a **platform attestation** — a signature over the compiled Hull platform code (the C layer: Keel, Lua, SQLite, mbedTLS, TweetNaCl, pledge polyfill, and Hull's own binding code). This signature is embedded in every binary built with that Hull release.

`hull build` does not re-sign the platform. It embeds application code (Lua, SQL, HTML/JS) into an already-signed platform binary. The platform signature is carried forward unchanged. This means every application built with official Hull contains a verifiable proof that its C layer came from the Hull project.

```bash
hull verify myapp.com
```
```
Platform:  Hull v0.1.0
Signed by: Hull Project (Ed25519: kl8x...q2m4)
Signature: VALID — platform code matches official Hull v0.1.0 release

App code:  3 Lua files, 2 migrations, 12 static assets
App signer: unsigned (no developer signature)
```

If someone forks Hull, modifies the C layer, and builds an application with it, the platform signature will either be missing or invalid:

```bash
hull verify sketchy-app.com
```
```
Platform:  Hull v0.1.0 (claimed)
Signed by: UNSIGNED — no valid Hull project signature
Status:    WARNING — platform is not signed by the Hull project.
           This binary was built with a modified or unofficial Hull.
           The sandbox, network allowlist, and file I/O restrictions
           may not be enforced. Do not trust this binary unless you
           trust the developer and have audited the source.
```

This is the critical check. A user doesn't need to understand C code — they run `hull verify` and either the platform is signed by the Hull project or it isn't. Binary answer. No ambiguity.

**How the signature works internally:**

```
┌─────────────────────────────────────────────┐
│ Hull APE Binary                             │
│                                             │
│ ┌─────────────────────────────────────────┐ │
│ │ Platform Code (C layer)                 │ │
│ │ Keel + Lua + SQLite + mbedTLS +         │ │
│ │ TweetNaCl + pledge + Hull bindings      │ │
│ │                                         │ │
│ │ SHA-256 hash ──┐                        │ │
│ └────────────────┼────────────────────────┘ │
│                  │                           │
│ ┌────────────────▼────────────────────────┐ │
│ │ Platform Attestation                    │ │
│ │ hull_version = "0.1.0"                  │ │
│ │ platform_hash = sha256:a3f8c1...e92d    │ │
│ │ signed_by = Hull Project Ed25519 pubkey │ │
│ │ signature = Ed25519(private, hash)      │ │
│ └─────────────────────────────────────────┘ │
│                                             │
│ ┌─────────────────────────────────────────┐ │
│ │ Embedded App Code                       │ │
│ │ Lua files, SQL migrations, HTML/JS/CSS  │ │
│ │ (not covered by platform signature —    │ │
│ │  this is the developer's code)          │ │
│ └─────────────────────────────────────────┘ │
│                                             │
│ ┌─────────────────────────────────────────┐ │
│ │ App Signature (optional)                │ │
│ │ Covers: platform hash + app content     │ │
│ │ Signed by: app developer's Ed25519 key  │ │
│ └─────────────────────────────────────────┘ │
└─────────────────────────────────────────────┘
```

#### Signature 2: App developer signs the application (optional)

For commercial applications or high-trust environments, the app developer can sign the complete binary — platform code plus embedded application code — with their own Ed25519 key:

```bash
hull build --output myapp.com --sign developer.key
```

This produces a second signature, independent of the Hull project's platform signature. It covers both the platform hash and the embedded content, proving:

- The binary was produced by this specific developer
- Neither the platform nor the application code has been modified since signing
- The developer vouches for the entire package

```bash
hull verify myapp.com
```
```
Platform:  Hull v0.1.0
Signed by: Hull Project (Ed25519: kl8x...q2m4)
Signature: VALID — platform code matches official Hull v0.1.0 release

App code:  3 Lua files, 2 migrations, 12 static assets
App signer: Kovacs Software Kft. (Ed25519: m9p2...x7w1)
Signature: VALID — binary matches developer signature
```

The developer's public key can be published on their website, in their repository, or distributed with their product documentation. Enterprise customers can pin the key and reject updates signed by a different key.

This signature is optional because many Hull use cases don't need it — an internal tool built by the IT department, an open-source project where the source is public, a personal tool for one person. The platform signature (mandatory) provides the baseline: "this is real Hull with real sandbox enforcement." The developer signature adds: "and this specific person/company produced this specific application."

#### Why two signatures matter

| | Platform signature (Hull project) | App signature (developer) |
|---|---|---|
| Answers | "Is the sandbox real?" | "Who made this?" |
| Trust anchor | Hull open-source project | App developer identity |
| Mandatory? | Yes — embedded in every official Hull build | No — opt-in for commercial/enterprise |
| Covers | C layer (Keel, Lua, SQLite, mbedTLS, etc.) | Entire binary (platform + app code) |
| Verification | `hull verify` checks automatically | `hull verify` checks if present |
| Forgeable? | Only with Hull project's private key | Only with developer's private key |

A forked Hull with the sandbox gutted would fail the platform signature check. A legitimate Hull with tampered Lua code would fail the developer signature check (if signed). Both checks are offline, instant, and use the same Ed25519 cryptography already vendored for licensing (TweetNaCl — no additional dependencies).

#### The audit chain

```
Hull source code        ← open source, ~1,500 lines of C, auditable in a day
        │
        ▼
Hull CI build           ← GitHub Actions, public logs, deterministic
        │
        ▼
Hull release binary     ← signed by Hull project Ed25519 key
        │                  published with source hash + build hash
        ▼
hull build myapp.com    ← embeds Lua/SQL/HTML into signed platform
        │                  optionally signed by developer
        ▼
Distributed binary      ← user runs hull verify:
                           ✓ platform signature valid (sandbox is real)
                           ✓ developer signature valid (author is known)
                           ✓ hull inspect shows security profile (+ source for AGPL builds)
                           ✓ startup banner shows sandbox declarations
```

Every link in this chain is verifiable. No link requires trust in something you can't inspect.

#### Web verifier — verify.gethull.dev

The Hull project publishes a static, single-page HTML5 application at **verify.gethull.dev** (CDN-hosted, no backend, no server). The user drags and drops a Hull binary onto the page. The JavaScript running in their browser:

1. Parses the binary structure to extract the platform attestation and app signature blocks
2. Verifies the platform Ed25519 signature against the Hull project's public key (hardcoded in the page)
3. Verifies the developer's Ed25519 signature if present
4. Extracts and displays the embedded `app.lua` configuration — `allowed_hosts`, filesystem paths, pledge promises
5. Lists all embedded files (Lua source, SQL migrations, static assets) with sizes
6. Shows a summary verdict

```
┌─────────────────────────────────────────────────────────┐
│  verify.gethull.dev                                        │
│                                                         │
│  ┌───────────────────────────────────────────────────┐  │
│  │  Drop a Hull binary here to verify                │  │
│  │              [ myapp.com ]                        │  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│  Platform                                               │
│  Hull v0.1.0 — SIGNED by Hull Project ✓                │
│                                                         │
│  Developer                                              │
│  Kovacs Software Kft. — SIGNED ✓                       │
│  Key: m9p2...x7w1                                       │
│                                                         │
│  Security Profile                                       │
│  ┌───────────────────────────────────────────────────┐  │
│  │ Network:    api.nav.gov.hu (outbound only)        │  │
│  │ Env:        NAV_API_KEY (1 variable)              │  │
│  │ Filesystem: data.db (rw), public/ (r)             │  │
│  │ Pledge:     stdio rpath wpath inet                │  │
│  │ Exec:       blocked                               │  │
│  │ Encryption: enabled (AES-256, license-key-derived)│  │
│  └───────────────────────────────────────────────────┘  │
│                                                         │
│  License                                                │
│  Hull platform: AGPL-3.0 (source included)              │
│  — or —                                                 │
│  Hull platform: Commercial (bytecode only)              │
│                                                         │
│  Embedded Files (17 files, 42 KB)                       │
│  ├── app.lua                     1.2 KB  [view]        │
│  ├── routes/invoices.lua         3.4 KB  [view]        │
│  ├── routes/customers.lua        2.1 KB  [view]        │
│  ├── locales/hu.lua              0.6 KB  [view]        │
│  ├── migrations/001_init.sql     0.8 KB  [view]        │
│  ├── public/index.html           4.2 KB  [view]        │
│  └── ...                                                │
│  ([view] available for AGPL builds or --include-source) │
│                                                         │
│  Verdict: ✓ Official Hull platform, signed by known     │
│  developer, limited network access, sandboxed.          │
└─────────────────────────────────────────────────────────┘
```

**Why this matters:**

- **No installation required.** The user doesn't need Hull, a terminal, or any technical knowledge. They open a webpage and drop a file.
- **No trust in the developer's website.** The verifier is hosted by the Hull project, not the app developer. The developer cannot tamper with the verification.
- **No server.** The page is static HTML + JavaScript. The binary never leaves the browser — it's parsed client-side using the File API. No upload, no network call, no privacy concern. The page works offline once loaded.
- **Readable source.** For AGPL builds (or commercial builds with `--include-source`), the [view] links display the embedded Lua files in the browser. A non-technical user's IT advisor can click through every file in the application without extracting anything. For commercial bytecode-only builds, the security profile is still fully visible.
- **Shareable.** A user can screenshot or link to the verification result. "Before I install this, here's what it can access" becomes a normal part of evaluating software.

The verifier page itself is open source, hosted on a CDN (GitHub Pages or Cloudflare Pages), and contains the Hull project's public key as a JavaScript constant. It could also be downloaded and run locally as a standalone HTML file — zero dependencies, works in any browser.

Ed25519 signature verification in JavaScript is a solved problem (~300 lines using existing public-domain implementations like tweetnacl-js). Parsing the Hull binary format to extract the attestation block and embedded file list is straightforward since Hull controls the format.

This closes the trust loop for non-technical users. You don't need to understand C, Lua, cryptography, or sandboxing. You drop a file on a webpage and get a plain-language answer: "this application was built with real Hull, signed by this developer, can only access these files and these network hosts, and here's the source code if you want to read it."

**Contrast with the alternatives:**

| | Hull | Electron | Go/Rust binary | SaaS |
|---|---|---|---|---|
| Can you read the app logic? | Yes, AGPL builds (`hull inspect`) | Minified JS, impractical | Compiled, no | No |
| Can you verify the platform? | Yes (Ed25519 signature, 1,500 LOC) | No (millions of LOC in Chromium) | Partially (compiler is open, but 300 crates aren't all audited) | No |
| Can you verify the author? | Yes (optional Ed25519 app signature) | Code signing (expensive, opaque CAs) | Code signing (same) | No |
| Can you verify the build? | Yes (reproducible, CI-built, signed) | Theoretically (practically impossible) | Yes (reproducible builds possible) | No |
| Can you see what it accesses? | Yes (startup banner, kernel-enforced) | No | No | No |
| Supply chain deps | 6 vendored C libs | 500-1,500 npm packages | 50-400 crates/modules | Unknown |
| Can one person audit it? | Yes, in a day | No | No | No |
| Cost to sign | Free (Ed25519 key pair) | $99-299/yr (Apple/MS certificates) | $99-299/yr (same) | N/A |

**The remaining trust boundary:** You trust that the Hull project itself is not malicious — the same way you trust GCC, the Linux kernel, SQLite, or any other foundational open-source project. This trust is earned through: open source code, public CI, reproducible builds, a small auditable codebase, community scrutiny over time, and a published signing key whose every use is tied to a public CI build log. This is the best the industry knows how to do. It is the same foundation everything else rests on. The difference is that Hull keeps the auditable surface small enough that the trust is *practically verifiable*, not just theoretically possible — and the signature model gives you a one-command answer (`hull verify`) instead of asking you to audit code yourself.

## Database Encryption (Optional)

Hull optionally encrypts the SQLite database at rest using a key derived from the license key. This is not a license enforcement mechanism — it is a data protection feature.

### Why It Matters

The license key and the database are separate files. This separation enables a physical security model:

```
On the computer:     data.db          (encrypted, useless alone)
On a USB stick:      license.key      (key material, no data)
```

Neither file is useful without the other. If the computer is stolen, the attacker gets an encrypted blob. If the USB stick is lost, no data is exposed. This is meaningful for:

- **GDPR Article 32** — encryption at rest is an explicit technical measure for protecting personal data. A Hull application storing customer records, patient notes, or employee data can demonstrate compliance by design.
- **Stolen/lost devices** — a laptop left in a taxi, a tablet taken from a job site, a computer seized in a burglary. The database is unreadable without the license key.
- **Multi-user machines** — the database file is opaque to other users on the same system, even if file permissions are misconfigured.

### How It Works

The license key contains a signed payload. A deterministic key derivation function (using HMAC from TweetNaCl or mbedTLS) derives a 256-bit database encryption key from the license payload and a salt embedded in the database header:

```
db_key = HKDF(license_payload, db_salt, "hull-db-encryption")
```

Hull uses [SQLite3 Multiple Ciphers](https://github.com/nicedecisions/sqlcipher) or SQLite's built-in codec API to apply AES-256 encryption transparently. All reads and writes go through the encryption layer — Lua code and the application logic are completely unaware.

### What This Does NOT Do

**It does not prevent a licensed user from reading their own data.** They have the license key. They can decrypt. This is by design — it's their data.

**It does not replace pledge/unveil.** Encryption protects data at rest (powered off, stolen, copied). The sandbox protects data at runtime (running application can't exfiltrate). They are complementary layers.

**It does not make backups harder.** The user copies `data.db` and `license.key` together. Both files are needed for restore. This is the same backup story as without encryption — copy the files, done.

### Activation

Encryption is opt-in, controlled by the application developer:

```lua
-- app.lua
app.config({
    db_encrypt = true,   -- encrypt database at rest
})
```

When enabled, Hull creates encrypted databases from the start. Existing unencrypted databases can be migrated with `hull encrypt`. Encrypted databases can be exported back to plain SQLite with `hull decrypt`.

```bash
hull encrypt --db data.db --key license.key    # encrypt existing database
hull decrypt --db data.db --key license.key    # export to plain SQLite
```

`hull decrypt` ensures the user is never locked in. If they stop using Hull, switch to a different tool, or simply want to inspect their data with a standard SQLite client, they can extract a plain database at any time. Their data, their choice. This is a core principle — Hull does not hold data hostage.

When disabled (the default), the database is plain SQLite, readable by any SQLite tool. This is the right default — encryption adds complexity that most local tools don't need.

## Standard Library Reference

Hull ships a batteries-included Lua standard library embedded in the binary. Full API documentation for each module is in [docs/MANIFESTO_FULL.md](docs/MANIFESTO_FULL.md).

| Module | Purpose | Size |
|--------|---------|------|
| **app** | Route registration, middleware chain, static files, response helpers, lifecycle | core |
| **db** | Query helpers, migrations, backup, pagination, transactions (parameterized only) | core |
| **json** | JSON encode/decode | ~200 lines |
| **csv** | RFC 4180 CSV encode/decode with separator auto-detection | ~150 lines |
| **email** | Unified API with provider abstraction (Postmark, SendGrid, SES, Resend, Mailgun, SMTP) | ~300 lines |
| **auth** | Password hashing (PBKDF2-HMAC-SHA256), secure token generation | ~100 lines |
| **rbac** | Role-based access control, permission wildcards, middleware guard | ~150 lines |
| **log** | Structured logging to SQLite + stdout, auto-cleanup | ~80 lines |
| **i18n** | Internationalization, locale-aware formatting, language detection | ~200 lines |
| **search** | SQLite FTS5 wrapper, declarative indexes, relevance ranking, highlighted snippets | ~80 lines |
| **template** | HTML template engine with expressions, code blocks, layout inheritance | ~150 lines |
| **pdf** | PDF document builder for business documents (text, tables, fonts, pages) | ~500 lines |
| **test** | Test framework for \ with request simulation and DB isolation | ~200 lines |
| **valid** | Input validation with schema declaration, type checking, sanitization | ~150 lines |
| **limit** | Rate limiting middleware (token bucket algorithm) | ~80 lines |
| **session** | Cookie-based sessions stored in SQLite | ~60 lines |
| **csrf** | CSRF token generation and verification middleware | ~40 lines |

**Platform features documented in detail:**

- **Startup Sequence** — arg parsing, DB init, sandbox application, Lua init, event loop
- **Scheduled Tasks** — cron-like scheduling via \, \, \
- **Background Work** — cooperative multitasking via Lua coroutines (\, \, \)
- **Browser Frontend** — HTML5/JS served to user's browser, localhost HTTP, any framework or vanilla JS
- **Multi-Instance** — port conflict detection, automatic port selection
- **Application Updates** — browser-side update checks, no backend network required
- **Email** — HTTP API relay (recommended) + built-in SMTP client + browser mailto: fallback
- **Secrets & API Keys** — declared environment variables, \ file support, startup validation
- **Logging** — structured logging to SQLite with auto-cleanup
- **Authentication** — PBKDF2 password hashing, secure tokens, session pattern
- **Development Mode** — hot reload, LLM-friendly error output, WebSocket live reload, access log
- **Production Mode** — embedded assets, bytecode compilation, startup banner
- **HTML Templating** — server-rendered templates with layout inheritance, SPA support
- **PDF Generation** — browser print + Lua PDF writer for programmatic generation
- **File Uploads** — multipart handling with configurable size limits
- **Testing** — request simulation, assertions, per-test database isolation
- **App WebSockets** — real-time communication with connection management
- **CSV Export/Import** — RFC 4180 compliant with streaming support
- **Internationalization** — string lookup, number/date/currency formatting, locale detection
- **Full-Text Search** — SQLite FTS5, declarative indexes, relevance ranking
- **Pagination** — offset-based and cursor-based with template helpers
- **Backup** — CLI + scheduled backups, consistent SQLite snapshots
- **RBAC** — role definitions, permission wildcards, middleware guards
- **Data Validation** — schema declaration, type checking, sanitization, reusable schemas
- **Rate Limiting** — token bucket, per-route configuration, configurable responses

## Known Limitations

Hull is honest about its constraints. These are known trade-offs, not bugs.

**Concurrency.** Hull is single-threaded. SQLite serializes writes. Under real workloads with multiple concurrent users making writes, requests queue. For Hull's target use case — 1-5 simultaneous users running a local tool — this is not a bottleneck. SQLite handles tens of thousands of reads per second and hundreds of writes per second. If your workload needs concurrent writes from thousands of users, Hull is not the right tool (and you probably need a server, not a local app).

**Lua ecosystem.** Lua's ecosystem is smaller than Python's, JavaScript's, or Rust's. There is no equivalent of npm or pip with hundreds of thousands of packages. Hull mitigates this by shipping a batteries-included standard library (routing, auth, RBAC, email, CSV, i18n, FTS, PDF, templates, validation, rate limiting, sessions, CSRF). For modules that don't touch restricted APIs (no `io`, no `os`, no `ffi`), pure Lua libraries work fine. The trade-off is intentional: a small, auditable ecosystem vs. a vast one with supply chain risk.

**Performance.** Keel (Hull's HTTP server) is designed for performance — epoll/kqueue/io_uring, non-blocking I/O, pre-allocated connection pools. But Hull has not been formally benchmarked against other frameworks. For the target workload (local tool, 1-5 users), performance is not a concern. Benchmarks will be published when the platform is mature enough for them to be meaningful.

**Windows and macOS sandbox.** pledge/unveil provides kernel-enforced sandboxing on Linux (SECCOMP BPF + Landlock LSM) and OpenBSD (native syscalls). On macOS and Windows, the kernel sandbox is not available. Hull still runs — the Lua sandbox (restricted stdlib, C-level path validation, network allowlist) provides application-level security on all platforms. The strongest security posture requires Linux or OpenBSD. Windows App Container (available since Windows 8, default-deny for filesystem/network/registry/IPC) and macOS App Sandbox are viable future directions for platform-specific sandbox enforcement. Both are on the roadmap but not yet implemented.

**No mobile.** Hull produces desktop executables. The Lua + SQLite core could be embedded in native mobile apps (iOS/Android) as a library, and the HTML5/JS frontend could run in a mobile webview. This is a future roadmap item, not a current capability.

## How Hull Differs from Tauri

[Tauri](https://tauri.app/) is the most prominent modern framework for building desktop applications with web frontends. It uses Rust for the backend, the system's native webview for rendering (WKWebView on macOS, WebView2 on Windows, WebKitGTK on Linux), and produces relatively small binaries compared to Electron.

Hull and Tauri solve overlapping problems but make fundamentally different trade-offs:

| | Tauri | Hull |
|---|---|---|
| Backend language | Rust | Lua (scripted, hot-reloadable) |
| Frontend rendering | System webview (WKWebView, WebView2, WebKitGTK) | User's browser (any browser) |
| Binary output | Per-platform (separate macOS, Windows, Linux builds) | Single APE binary (runs on all platforms) |
| Database | None built-in (bring your own) | SQLite embedded, with migrations, encryption, FTS |
| Dependencies | 200-400 crates from crates.io | 6 vendored C libraries, zero package manager |
| Build time | 60-120 seconds (Rust compilation) | Under 3 seconds (C compilation) |
| Binary size | 5-15 MB (varies by platform and webview) | Under 2 MB (all platforms, all features) |
| Sandbox | Inherits from OS webview sandbox | pledge/unveil (kernel-enforced on Linux/OpenBSD) |
| Built-in licensing | No | Ed25519 license keys, offline verification |
| Mobile support | Yes (Tauri v2 — iOS/Android via native webview) | No (desktop only, mobile on roadmap) |
| LLM-friendliness | Rust is verbose and complex for LLMs | Lua is small (~60 keywords), LLMs generate it reliably |
| App framework | Minimal (routing, IPC between Rust and JS) | Batteries included (auth, RBAC, email, CSV, i18n, FTS, PDF, sessions, validation) |
| Distribution | Per-platform installers (.dmg, .msi, .deb) | Single file, no installer, no admin privileges |
| Digital signatures | OS-level code signing ($99-299/year) | Ed25519 (free, self-managed, verify.gethull.dev) |

**Where Tauri is stronger:**

- **Mobile support.** Tauri v2 targets iOS and Android through native webviews. Hull is desktop-only.
- **Native integration.** System webviews provide platform-native features (notifications, file pickers, menus, system tray) without HTTP overhead. Hull uses the browser, which means system integration is limited to what the browser exposes.
- **Mature ecosystem.** Tauri has a larger community, more plugins, and more production deployments. Hull is new.
- **Rust safety guarantees.** For teams with Rust expertise, the borrow checker provides compile-time memory safety across the entire backend.

**Where Hull is stronger:**

- **Single binary, all platforms.** One APE file runs on Linux, macOS, Windows, FreeBSD, OpenBSD, NetBSD. No per-platform build pipeline, no CI matrix, no platform-specific testing.
- **Zero supply chain.** Six vendored C libraries vs. hundreds of crates. Auditable in a day vs. auditable in months.
- **Built-in application framework.** Auth, database, migrations, email, PDF, CSV, i18n, search, sessions, validation, rate limiting, RBAC — all included. Tauri ships IPC and a webview; everything else is bring-your-own.
- **AI-first development.** Lua is dramatically simpler than Rust for LLM code generation. A vibecoder can describe an app to an AI and get working Lua. Getting correct Rust with proper lifetime annotations and trait bounds from an AI is significantly harder.
- **Agentic iteration speed.** AI-assisted development is a generate-test-fix loop: the LLM writes code, the tool compiles and runs it, reads the output, and iterates. This loop runs dozens to hundreds of times per feature. Rust's 60-120 second compile times make each iteration a minute-long wait — multiplied across hundreds of cycles, a single feature takes hours of wall-clock compilation time alone. Hull's C runtime compiles in under 3 seconds; Lua changes require zero compilation. The agentic loop runs at the speed of thought, not the speed of `cargo build`. For vibecoders who rely entirely on AI iteration, this isn't a minor inconvenience — it's the difference between building something in an afternoon and giving up after an hour of watching a progress bar.
- **Built-in licensing and signatures.** Ed25519 license key verification and dual-signature verification built into the platform. Tauri has no built-in licensing.
- **Hot reload.** Lua reloads on every request in dev mode. Tauri requires Rust recompilation for backend changes.

**The deepest difference is philosophical, not technical.** Tauri does not focus on digital independence — and by design, it never will. Tauri is a framework for building desktop apps. It has no opinion on who owns the data, who controls distribution, or whether the user depends on cloud infrastructure. Hull's entire architecture is built around one premise: the person who uses the software should own everything about it — the binary, the data, the build pipeline, and the business outcome. This isn't a feature Hull adds on top; it's the reason Hull exists.

Tauri is a framework for professional developers who know Rust and want to build cross-platform desktop apps. Hull is a platform for anyone — including vibecoders who don't know any programming language — who wants to build, distribute, and sell local-first applications. Tauri optimizes for developer power. Hull optimizes for digital independence.

## How Hull Differs from Redbean

[Redbean](https://redbean.dev/) by Justine Tunney is the closest thing to Hull that exists. It's a single-file web server built on Cosmopolitan that embeds Lua — and it proved the concept that a useful web application can ship as a single portable binary.

Hull is not built on Redbean. Hull is built on [Keel](https://github.com/artalis-io/keel), an independent HTTP server library with pluggable event backends (epoll/kqueue/io_uring/poll), pluggable TLS, pluggable parsers, middleware, body readers, and WebSocket support. Keel provides the HTTP transport layer; Hull adds everything above it. Redbean is a reference point, not a dependency.

Hull takes the same single-binary thesis in a different direction:

| | Redbean | Hull |
|---|---|---|
| Core identity | Web server with Lua scripting | Application platform with build tool |
| Database | None built-in | SQLite embedded, with Lua bindings, migrations, and optional encryption |
| License model | ISC (permissive) | AGPL-3.0 + Commercial dual license |
| App licensing | None | Ed25519 license key system for commercial app distribution |
| Security | Cosmopolitan sandbox | pledge/unveil polyfill + Lua function restriction + encrypted database + digital signatures |
| Build tool | Zip files appended to binary | `hull build` — signed build tool produces signed APE binaries, ejectable |
| Target audience | Developers who want a portable web server | Vibecoders and developers who want to build and sell local-first applications |
| Framework | Bring your own | Ships with routing, middleware, sessions, auth, RBAC, validation, i18n, search, CSV, email, PDF |
| Data protection | None | Optional AES-256 database encryption tied to license key |
| Distribution model | Open source tool | Platform for building commercial products |

Redbean proved that a Lua web application can run anywhere from a single file. Hull takes that thesis to its conclusion: not just "serve web pages from a portable binary" but "build, package, license, encrypt, sandbox, distribute, and sell complete applications as a single binary."

Redbean is a tool. Hull is a platform for building products.

## Survivability

Hull's value proposition depends on the platform being maintained. What happens if artalis-io disappears?

**The code is AGPL.** The entire Hull source — C runtime, build tool, standard library — is open source under AGPL-3.0. Anyone can fork, build, and distribute Hull from source. The Makefile builds the entire platform from scratch without hull.com (`make CC=cosmocc`). No proprietary component exists that couldn't be rebuilt from the published source.

**The dependencies are vendored.** All six C libraries are included in the Hull repository. No external downloads, no package manager fetches, no URLs that could go offline. A git clone of the Hull repo contains everything needed to build the platform.

**The ejection path is permanent.** `hull eject` copies hull.com into the project. An ejected project is fully self-contained — it can build production binaries forever, even if hull.com's website, CDN, and every artalis-io server vanishes. The ejected binary is signed and functional indefinitely.

**Dead man's switch.** If the Hull project publishes no release (no tagged version on GitHub) for 24 consecutive months, the license automatically converts from AGPL-3.0 to MIT. This is a legally binding clause in the Hull license. It means:

- If artalis-io abandons the project, the community can fork under MIT (no copyleft obligation)
- Commercial license holders retain their existing rights regardless
- The conversion is triggered by a verifiable, objective condition (no GitHub release tag in 24 months)
- Anyone can check: look at the GitHub releases page

**Existing applications keep working.** A built Hull application is a standalone binary. It does not phone home, check for updates, validate its license against a server, or depend on any artalis-io infrastructure at runtime. If Hull the project dies tomorrow, every Hull application ever built continues to work exactly as it does today. Forever. That's the point of local-first.

## Project Structure

```
hull/
  src/
    main.c               # startup, sandbox, arg parsing
    lua_bindings.c        # Lua <-> Keel bridge
    lua_db.c              # Lua <-> SQLite bridge (parameterized queries only)
    lua_crypto.c          # Lua <-> TweetNaCl bridge
    license.c             # Ed25519 license verification (platform + app)
    http_client.c         # minimal HTTPS client (mbedTLS, allowlist-enforced)
    smtp.c                # minimal SMTP client (STARTTLS via mbedTLS)
    timer.c               # min-heap timer scheduler (app.every, app.daily, etc.)
    task.c                # background task runner (app.spawn, app.yield, app.sleep)
    embed.c               # embedded Lua standard library
  lib/                    # standard library (embedded in binary)
    app.lua               # routing, middleware, response helpers
    db.lua                # query helpers, migrations, backup, pagination
    json.lua              # JSON encode/decode
    csv.lua               # RFC 4180 CSV encode/decode
    email.lua             # unified email API (Postmark, SendGrid, SES, SMTP)
    auth.lua              # password hashing, token generation, verify helpers
    rbac.lua              # role-based access control + middleware guard
    log.lua               # structured logging to SQLite + stdout
    i18n.lua              # internationalization (string lookup, formatting)
    search.lua            # SQLite FTS5 wrapper (index, query, rebuild)
    template.lua          # HTML template engine ({{ }}, {% %}, layouts)
    pdf.lua               # PDF document builder (text, tables, fonts)
    test.lua              # test framework (hull test)
    valid.lua             # input validation + schema declaration
    limit.lua             # rate limiting middleware (token bucket)
    session.lua           # cookie sessions in SQLite
    csrf.lua              # CSRF middleware
  tool/                   # hull.com build tool Lua layer (also embedded)
    cli.lua               # command dispatch (new, dev, build, test, etc.)
    scaffold.lua          # project templates for hull new
    build.lua             # artifact collection, binary assembly, signing
    dev.lua               # development server wrapper
    verify.lua            # signature verification (hull verify, hull inspect)
  vendor/
    keel/                 # HTTP server
    lua/                  # Lua 5.4
    sqlite/               # SQLite amalgamation
    mbedtls/              # TLS client
    tweetnacl/            # Ed25519 signatures
    pledge/               # pledge/unveil polyfill (Justine Tunney)
  Makefile                # bootstrap build (make CC=cosmocc → hull.com)
  README.md
  LICENSE                 # AGPL-3.0
```

## Build

```bash
# Bootstrap (first build, or building from source):
make                    # native build for development
make CC=cosmocc         # build hull.com APE binary
make test               # run platform tests
make clean              # remove artifacts

# After hull.com exists (normal workflow):
hull new myapp          # scaffold project
hull dev                # development server
hull build              # production APE binary
hull test               # run app tests
```

## Business Plan & Monetization

### The Opportunity

Three market forces converging simultaneously:

1. **AI coding assistants are mainstream (2025-2026)** — millions of people can now describe software and have it written for them
2. **Local-first is a movement driven by privacy regulation** — GDPR, CCPA, digital sovereignty laws are pushing data back to the edge
3. **Supply chain security is a board-level concern** — SolarWinds, Log4j, xz-utils made "how many dependencies does this have?" a question executives ask

Hull is positioned at the intersection: the platform that turns AI-generated code into secure, distributable, local-first products.

### Revenue Model

**Primary:** Hull commercial licenses (removes AGPL obligation for closed-source distribution)

| Tier | Price | Renewal | Target |
|------|-------|---------|--------|
| Standard | $99 one-time | $49/year for updates | Solo developers, indie hackers |
| Team | $299 one-time | $149/year for updates | Small teams (up to 5 devs) |
| Perpetual | $499 one-time | Lifetime updates | Serious developers, long-term bet |

**Secondary (future, Year 2+):**

- **Enterprise tier:** custom contracts, SOC2 compliance docs, priority support, SLA. $2,000-10,000/year
- **Hull Marketplace:** curated directory of Hull applications. 15% commission on sales
- **Training & certification:** Hull Developer Certification. $199 per course
- **Migration consulting:** Excel/SaaS → Hull for enterprise. Project-based pricing

### Unit Economics

- **COGS:** ~$0 per license (download + signed key file, no infrastructure per customer)
- **Gross margin:** ~95% (cost is salaries + CI + CDN for downloads/docs/verify)
- **No hosting obligation** — customers self-host by design
- **Support burden is low** — platform is simple, runtime is stable, community handles most Q&A
- **License delivery** is an Ed25519-signed key file — no license server, no activation infrastructure

### Market Sizing

We don't have credible TAM/SAM/SOM numbers because this category doesn't exist yet. Hull is creating a new product category — "vibecoder-to-product platform" — and market sizing for a category that doesn't exist is fiction.

What we know qualitatively:

- **AI coding assistants are growing fast.** Millions of people are using Claude Code, Cursor, Copilot, and similar tools. Some fraction of these users want to distribute what they build. Today, that fraction hits the deployment wall. Hull removes the wall.
- **Local-first is a growing movement.** Driven by regulation (GDPR, CCPA, digital sovereignty laws), privacy concerns, and subscription fatigue. The demand for tools that work offline and keep data local is increasing.
- **SMBs are underserved.** Millions of small businesses run on spreadsheets because custom software was too expensive and complex. A vibecoder or local IT consultant building a Hull app is cheaper than any SaaS subscription over 2-3 years.

**High-value verticals where Hull's architecture is a natural fit:**

- **Defense and government** — air-gapped networks are the norm, not the exception. Hull apps run offline, require no internet, no phone-home, no activation server. Data stays on classified networks. Zero supply chain dependencies means fewer items on the software bill of materials (SBOM). Kernel sandbox (pledge/unveil) provides provable containment. Single-binary distribution simplifies accreditation — one artifact to certify, not a dependency tree of thousands. Even development can be fully air-gapped: OpenCode + a local model (minimax-m2.5, Llama, etc.) on isolated hardware — source code never leaves the secure facility. No cloud IDE, no GitHub Copilot, no data exfiltration vector from the development pipeline itself.
- **Medical and healthcare** — HIPAA, patient data sovereignty, and air-gapped clinical environments. Hull's encrypted SQLite database, offline operation, and self-declaring security model align with compliance requirements by design. A Hull app managing patient records on a clinic's local network never exposes data to the internet. Backup is copying a file. Development itself can be air-gapped — a hospital's internal dev team or contractor can build and iterate on Hull apps using a local LLM without patient data or source code ever touching an external network.
- **Legal and financial** — client confidentiality, regulatory compliance (SOX, MiFID II), data residency requirements. Tools that handle sensitive financial data should not phone home to cloud servers. Hull's network allowlist and kernel sandbox make this provable.
- **SMBs broadly** — the largest underserved market. Millions of small businesses run on Excel spreadsheets and manual processes because custom software meant hiring developers and paying for hosting. Hull makes it possible for a single person (or an AI) to build a tool that replaces a spreadsheet — inventory tracker, appointment scheduler, invoice generator, job costing calculator — and distribute it as a file. No IT department required.

### Comparable Business Models

- **Sublime Text:** one developer, ~$30M+ lifetime revenue, one-time purchase
- **JetBrains:** $600M+ annual revenue, developer tooling, perpetual fallback model
- **Panic (Transmit, Nova):** small team, premium developer tools, one-time purchase, profitable
- **Laravel (Spark, Forge, Vapor):** open-source framework + commercial SaaS tools around it

### Growth Flywheel

```
Vibecoders discover Hull through AI communities
    → build tools, share them (AGPL = visible source = awareness)
        → some tools go commercial → developer buys license
            → success stories attract boutique software houses
                → boutique houses build vertical products
                    → SMBs discover Hull through the tools they use
                        → some SMBs build their own → cycle repeats
```

### Competitive Moat

The technical stack is not the moat. Someone with sufficient motivation could replicate the C integration, the Cosmopolitan build, and the sandbox model. It would take significant effort, but it's not impossible.

The moat is the ecosystem:

- **First mover in a new category.** The "vibecoder-to-product" pipeline doesn't exist yet. Hull is building it. Whoever gets there first accumulates trust, community, documentation, tutorials, and real-world applications. Catching up requires not just matching the technology but replicating the ecosystem.
- **Trust accumulation.** Every signed Hull binary, every verify.gethull.dev check, every AGPL application with visible source builds a trust network. Trust compounds over time and doesn't transfer to competitors.
- **Community gravity.** AGPL means every Hull application is a showcase. Commercial license is the natural upgrade path. The more apps that exist, the more discoverable Hull becomes, the more developers build with it.
- **Ejection as trust signal.** Developers stay because Hull is useful, not because they're locked in. `hull eject` means you can leave anytime. This paradoxically increases loyalty — people trust platforms that let them leave.
- **Simplicity as durability.** Fewer than 10 moving parts means a 2-3 person team can maintain the entire platform. Competitors building on larger stacks need larger teams to maintain parity. Hull's simplicity is a structural cost advantage.

### Why Invest Now

1. **The vibecoding wave is NOW** — Claude Code, Cursor, OpenCode, Codex are mainstream in 2025-2026
2. **The timing window is narrow:** whoever builds the vibecoder→product pipeline first wins the category
3. **Local-first is being driven by regulation, not just ideology** — GDPR, CCPA, EU Digital Markets Act
4. **Supply chain attacks are front-page news** — organizations actively want fewer dependencies
5. **Hull is the only platform combining:** AI-friendly scripting + single-binary distribution + kernel sandbox + zero supply chain + built-in licensing + digital signatures
6. **Platform plays compound:** every app built on Hull increases the ecosystem value
7. **Minimal burn:** 6 vendored C libraries = small maintenance surface. A 2-3 person team can build and maintain Hull v1.0

### Path to Profitability

Hull's cost structure means profitability is achievable at small scale:

- **Break-even is low.** With near-zero COGS and a 2-3 person team, Hull needs hundreds of licenses — not thousands — to cover costs. At $99-499 per license with ~95% gross margin, the math works early.
- **Revenue is front-loaded.** One-time purchases mean revenue arrives at the point of sale, not drip-fed over months. No churn risk. No retention marketing.
- **Costs stay flat.** No servers to run for customers. No per-customer infrastructure. Adding the 10,000th customer costs the same as adding the 10th.
- **Expansion revenue from renewals.** Update renewals ($49-149/year) provide recurring revenue from customers who want the latest platform. This is optional — customers can stay on their last version forever — which means renewal revenue represents genuine value delivery, not lock-in.

We don't project specific license numbers because the category is new and projections would be fabricated. The Sublime Text model ($30M+ lifetime from one developer with one-time purchases) demonstrates that developer tooling with one-time pricing can build a substantial business. Hull's cost structure is even leaner.

## Philosophy

Software should be an artifact you own, not a service you rent. Hull exists to make that possible for a class of applications that the industry forgot about: small, local, private, single-purpose tools that just work.

No accounts. No subscriptions. No telemetry. No updates that break things. No Terms of Service. No "we're shutting down, export your data by Friday." Just a file on your computer that does what you need, for as long as you need it, answerable to nobody.

That's what software used to be. Hull makes it that way again.
