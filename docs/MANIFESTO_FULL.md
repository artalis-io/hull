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

**Platform Features**
[Startup Sequence](#startup-sequence) · [Scheduled Tasks](#scheduled-tasks) · [Background Work](#background-work) · [Multi-Instance](#multi-instance) · [Application Updates](#application-updates) · [Email](#email) · [Secrets and API Keys](#secrets-and-api-keys) · [Logging](#logging) · [Authentication](#authentication) · [Development Mode](#development-mode) · [HTML Templating](#html-templating) · [PDF Generation](#pdf-generation) · [File Uploads](#file-uploads) · [Testing](#testing) · [App WebSockets](#app-websockets) · [CSV Export/Import](#csv-exportimport) · [Internationalization (i18n)](#internationalization-i18n) · [Full-Text Search](#full-text-search) · [Pagination](#pagination) · [Backup](#backup) · [Role-Based Access Control (RBAC)](#role-based-access-control-rbac) · [Data Validation](#data-validation) · [Rate Limiting](#rate-limiting)

**Limitations, Roadmap & Comparisons**
[Known Limitations](#known-limitations) · [Roadmap](#roadmap) · [How Hull Differs from Tauri](#how-hull-differs-from-tauri) · [How Hull Differs from Redbean](#how-hull-differs-from-redbean) · [Survivability](#survivability)

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

Hull ships a batteries-included Lua standard library embedded in the binary. Detailed documentation for each platform feature follows in the sections below.

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

## Startup Sequence

The order matters. Each step happens before the next, and the security boundary is explicit:

```
1.  Parse command-line arguments
2.  Open SQLite database (create if needed)
3.  Set SQLite pragmas (WAL mode, busy timeout)
4.  Bind HTTP socket (Keel server init)
5.  Validate license key (if licensing is enabled)
6.  Open browser (if --open flag, requires fork+exec)
    ─── SANDBOX BOUNDARY ───
7.  Apply pledge/unveil (no more exec, no filesystem beyond declared paths)
8.  Initialize Lua runtime
9.  Load embedded standard library
10. Load application code (app.lua)
11. Run database migrations
12. Enter Keel event loop
```

Everything above the sandbox boundary can access the full system. Everything below is restricted to exactly what the application declared. Lua code — including all user-written application logic — runs entirely within the sandbox.

## Scheduled Tasks

Hull applications often need to do work on a schedule: clean up expired sessions, send invoice reminders, sync data with external APIs, generate reports. Traditional solutions — system cron, background job queues, worker processes — all require infrastructure that Hull eliminates.

Hull's scheduler is built into the event loop. No threads, no external daemon, no message queue. The Keel event loop already wakes up on a timeout — Hull uses that timeout to fire timers at the right moment.

### Lua API

```lua
-- Interval — run every N minutes/hours
app.every("15m", function()
    db.exec("DELETE FROM sessions WHERE expires_at < ?", hull.time.now())
end)

-- Daily — run at a specific time
app.daily("08:00", function()
    local overdue = db.query("SELECT * FROM invoices WHERE due < ? AND paid = 0", today())
    for _, inv in ipairs(overdue) do
        send_reminder(inv)
    end
end)

-- Weekly — run on a specific day and time
app.weekly("Mon", "06:00", function()
    generate_weekly_report()
end)

-- One-shot delay — run once after a duration
app.after("5s", function()
    sync_pending_invoices()
end)
```

No cron expressions. Four functions cover 95% of real use cases. Readable, vibecoder-friendly, LLM-friendly.

### C Layer

The C layer maintains a min-heap of timers:

```c
typedef struct {
    int64_t fire_at;        // absolute time (monotonic clock)
    int64_t interval;       // 0 = one-shot, >0 = repeating
    int lua_callback_ref;   // Lua registry reference
} HullTimer;
```

Before each `kl_event_wait`, Hull computes the timeout as the minimum of the default timeout and the time until the next timer fires:

```c
int timeout = default_timeout;
if (timer_heap.count > 0) {
    int64_t next = timer_heap.timers[0].fire_at - now_ms();
    if (next < timeout) timeout = (int)next;
    if (next <= 0) timeout = 0;  // timer already due
}
int n = kl_event_wait(loop, events, max_events, timeout);

// Handle I/O events first (HTTP requests)
handle_io_events(events, n);

// Then fire any expired timers
while (timer_heap.count > 0 && timer_heap.timers[0].fire_at <= now_ms()) {
    HullTimer t = heap_pop(&timer_heap);
    lua_call_ref(L, t.lua_callback_ref);
    if (t.interval > 0) {
        t.fire_at += t.interval;   // reschedule
        heap_push(&timer_heap, t);
    }
}
```

No threads. No signals. No external cron daemon. The event loop wakes up at the right time, fires the callback, and goes back to handling HTTP requests.

### Time Functions

Hull removes the Lua `os` library (it contains `os.execute`, `os.remove`, `os.exit` — unsafe). Time functions are exposed selectively through the Hull namespace:

```lua
hull.time.now()          -- unix timestamp (seconds)
hull.time.now_ms()       -- unix timestamp (milliseconds)
hull.time.date(fmt)      -- formatted date string (strftime)
hull.time.clock()        -- monotonic clock for measuring intervals
```

Safe to expose. No process control, no filesystem access, no environment variables.

## Background Work

A scheduled task that takes 2 seconds (generate 50 PDFs, submit a batch to the tax authority API) blocks the entire server for 2 seconds. No HTTP requests are handled during that time. For a local tool serving one user, that might be acceptable. For a small office with 5 people, it's not.

Hull solves this with Lua coroutines — cooperative multitasking where background work voluntarily yields control back to the event loop between chunks of work.

### The Problem

```lua
-- This blocks the server for the entire batch:
app.every("15m", function()
    local pending = db.query("SELECT * FROM invoices WHERE submitted = 0")
    for _, inv in ipairs(pending) do
        submit_to_nav(inv)  -- each call takes 200ms, 50 invoices = 10 seconds blocked
    end
end)
```

### The Solution

```lua
-- This yields between items, keeping the server responsive:
app.every("15m", function()
    local pending = db.query("SELECT * FROM invoices WHERE submitted = 0")
    for _, inv in ipairs(pending) do
        submit_to_nav(inv)
        app.yield()  -- return control to event loop, resume next tick
    end
end)
```

`app.yield()` suspends the coroutine and returns to the event loop. The next iteration handles any pending HTTP requests, then resumes the coroutine where it left off.

### Spawning Background Tasks from Request Handlers

```lua
app.post("/reports/generate", function(req, res)
    local params = req.body
    res.json({ status = "generating" })

    -- This runs after the response is sent, in the background
    app.spawn(function()
        local rows = db.query("SELECT * FROM transactions WHERE year = ?", params.year)
        local chunks = chunk(rows, 100)
        for i, batch in ipairs(chunks) do
            process_batch(batch)
            app.yield()  -- stay responsive between chunks
        end
        db.exec("UPDATE reports SET status = 'done' WHERE year = ?", params.year)
    end)
end)

-- Sleep — yield and resume after a delay
app.spawn(function()
    while true do
        poll_external_api()
        app.sleep("30s")  -- yields, resumes after 30 seconds via timer
    end
end)
```

### C Layer

```c
typedef struct {
    lua_State *co;          // Lua coroutine (thread)
    int64_t resume_at;      // 0 = next tick, >0 = after delay (for app.sleep)
    int ref;                // Lua registry ref to prevent GC
} HullTask;
```

The event loop priority order:

```
Event Loop Iteration
─────────────────────────
1. kl_event_wait(timeout)     ← wakes on I/O or timer
2. Handle HTTP requests       ← always first, never starved
3. Fire expired timers        ← app.every, app.daily, app.after
4. Resume background tasks    ← app.spawn, app.yield, app.sleep
   (time-budgeted)
5. Repeat
```

HTTP requests are always handled first. Timers fire second. Background tasks get whatever time is left, capped at a per-iteration budget (e.g. 5ms) to guarantee the server stays responsive. If a background task forgets to yield, the budget cap forces a pause anyway.

Lua coroutines are not OS threads — they are ~200 bytes each. You can have thousands of them. No mutexes, no race conditions, no shared state bugs. The C layer just manages when to resume them.

### What This Handles

| Use case | Pattern |
|----------|---------|
| Session cleanup | `app.every("1h", fn)` |
| Daily report | `app.daily("06:00", fn)` |
| Invoice reminders | `app.daily("08:00", fn)` |
| API sync (polling) | `app.every("15m", fn)` with `app.yield()` between items |
| Batch export | `app.spawn(fn)` from request handler, yield between chunks |
| Retry with backoff | `app.spawn` + `app.sleep("30s")` in a loop |
| DB maintenance | `app.weekly("Sun", "03:00", fn)` for VACUUM |
| Deferred work | `app.after("0s", fn)` — runs after current request completes |

### Limits

**No true parallelism.** Background tasks run concurrently with HTTP requests (interleaved on the event loop) but not in parallel (no simultaneous execution). There are no threads.

**CPU-heavy work blocks the server — and that's usually fine.** Streamlit is single-threaded Python (slower than Lua) and people happily run pandas on 100K-row datasets, render charts, and do ML inference on it. It blocks the UI for a few seconds and nobody cares because it's a local tool for a small audience. Hull is the same: a report that takes 3 seconds to generate means the user sees a spinner for 3 seconds. That's "the app is thinking," not a production incident.

For work that can be chunked (batch API calls, row-by-row processing), `app.yield()` between chunks keeps the server responsive. For work that can't be chunked (a single expensive query, generating a complex PDF), the block is brief and the user is waiting for it anyway.

The actual limit is computation that takes *minutes* while the server must remain responsive — video transcoding, ML training, compressing gigabyte-scale files. That needs threads or a separate process, which Hull doesn't support. If your app regularly needs this, it has outgrown Hull.

### Browser Frontend and Network Access

The browser frontend (HTML5/JS) is not restricted by Hull's sandbox. JavaScript running in the browser tab can `fetch()` any URL the browser allows, subject to CORS — the same as any website. Even if the Hull backend is `Network: OFFLINE` with no `allowed_hosts`, the frontend JS can talk to external APIs directly (Stripe.js for payments, Google Maps for geocoding, OAuth redirect flows).

However, the frontend can only access data that the Hull backend has served to it via HTTP responses. The backend can only access its own SQLite database, its declared data directory, and its declared network hosts. The browser cannot reach into the server's sandbox — it sees only what the server chose to send. A malicious frontend script could exfiltrate *application data the user is already viewing* to an external endpoint, but it cannot exfiltrate system files, other applications' data, or anything outside Hull's sandbox. This is visible in the Lua source (`hull inspect`) and frontend JS source (embedded in the binary, extractable and readable).

## Multi-Instance

Hull applications are single-threaded by design. One instance handles 10,000+ requests per second on a single core. For the target use cases (local tools, small business applications, field data collection), one instance serves hundreds of concurrent users.

SQLite in WAL mode handles multiple processes accessing the same database file safely. Concurrent reads are unlimited. Writes are serialized via file locking with a configurable busy timeout. This is sufficient for any workload this platform targets.

If an application has outgrown a single Hull instance, it has outgrown Hull. Use something else. Hull is not a cloud platform and does not pretend to be.

## Application Updates

Hull applications are files. Updating an application means replacing the file with a newer version. There is no auto-updater, no background download, no silent replacement of the running binary. Self-updating binaries are a security anti-pattern — if the update server is compromised, every installation is silently backdoored. And it contradicts Hull's "you control the software" thesis: the user decides when to update, or whether to update at all. The old binary keeps working forever.

### Update Check — Browser-Side, Zero Backend Network

The update check happens entirely in the browser frontend. The Hull backend does not need network access — it can stay `Network: OFFLINE`. The browser's `fetch()` pings a static version endpoint on the developer's CDN:

```lua
-- app.lua
app.config({
    update_url = "https://acmecorp.com/myapp/version.json",
    version = "1.0.0",
})
```

When `update_url` is declared, Hull's embedded JS stdlib injects the update check automatically. No developer-side JavaScript required. The version endpoint is a static JSON file:

```json
{
    "version": "1.2.1",
    "download_url": "https://acmecorp.com/myapp.com",
    "changelog": "Fixed VAT calculation for reverse charge invoices",
    "sha256": "a3f8c1...e92d"
}
```

If a newer version is available, the user sees a banner:

```
┌─────────────────────────────────────────────────┐
│  Update available: v1.2.1                       │
│  Fixed VAT calculation for reverse charge.      │
│  Download: https://acmecorp.com/myapp.com       │
│  SHA-256: a3f8c1...e92d                         │
│                                     [Dismiss]   │
└─────────────────────────────────────────────────┘
```

If the user is offline, the `fetch()` fails silently. No error, no nag.

### Update Flow

```
1. User sees banner, clicks download link
2. Browser downloads the new binary (normal file download)
3. User verifies: hull verify myapp.com
   → platform signature valid (official Hull)
   → developer signature valid (same developer as before)
   → SHA-256 matches the version manifest
4. User replaces old binary with new binary
5. data.db and license.key are unchanged — just files next to the binary
6. User launches the new version — done
```

### Database Migrations on Update

A new version may change the database schema — add a column, create a new table, add an index. Hull's migration runner handles this automatically. Migrations are numbered SQL files embedded in the binary:

```
migrations/
  001_init.sql
  002_add_vat_column.sql
  003_customer_index.sql
```

On startup, Hull checks which migrations have already been applied (tracked in a `_hull_migrations` table) and runs any new ones. Version 1.0.0 ships with migrations 001-002. Version 1.2.1 ships with 001-003. When the user launches 1.2.1 for the first time, migration 003 runs automatically. The user's data is preserved and the schema is brought up to date.

SQLite schema changes are additive — `ALTER TABLE ADD COLUMN` never destroys existing data. Hull's migration convention enforces this: migrations add structure, they do not drop or rename columns. If a migration fails, it is rolled back and the application reports the error at startup rather than running with an inconsistent schema.

### What This Gets Right

- **No backend network access needed** — the backend stays sandboxed, the browser does the check
- **No self-modification** — the running binary never replaces itself (`pledge` blocks `exec` anyway)
- **User is in control** — they choose when to update, or never
- **No forced updates** — unlike SaaS where the vendor pushes changes without asking
- **Verifiable** — SHA-256 hash in the version manifest + `hull verify` on the downloaded binary confirms both platform integrity and developer identity
- **Offline-safe** — if the user is offline, the check fails silently
- **CDN-friendly** — `version.json` is a static file, hosted on GitHub Pages, Cloudflare, S3, anywhere — costs effectively nothing
- **Data-safe** — `data.db` and `license.key` are never touched by the update. Migrations are additive and automatic
- **Rollback is trivial** — if v1.2.1 breaks something, the user copies the old binary back. That's it. The application is a file — you can keep the previous version in a folder, rename it `myapp-v1.0.0.com`, or let Dropbox version it for you. Compare this to SaaS: when Salesforce pushes a broken update, you are along for the ride. You can't roll back. You can't stay on the old version. You file a support ticket and wait. With Hull, the user has the old binary, the new binary, and the choice. Their data is compatible with both — migrations are additive, so the old binary simply ignores columns it doesn't know about. The user is never trapped on a broken version with no way back.
- **Zero migration risk** — the database is a single file. Before updating, `cp data.db data.db.bak`. That's a complete backup — schema, data, indexes, everything. If the new version's migration does something unexpected, restore the backup and go back to the old binary. Total downtime: seconds. In a traditional stack, a database migration rollback means reverse-engineering SQL changes, restoring from a backup pipeline that may or may not work, coordinating application servers, and praying. In Hull, it's copying two files.

## Email

Business applications need email — invoice delivery, payment reminders, password resets, notification digests. Hull provides two email transports: an HTTP API relay (default, recommended) and a built-in SMTP client (for environments without external email services).

### HTTP API Relay (recommended)

Most email today goes through HTTP APIs — SendGrid, Postmark, Mailgun, Amazon SES, Resend. These services handle deliverability (DKIM, SPF, IP reputation), bounce processing, and compliance. Hull's existing `http_client.c` already speaks HTTPS — email is just a POST with a JSON body.

Hull ships an `email` standard library module that wraps common email API providers with a unified interface:

```lua
-- app.lua — configure once
app.config({
    email = {
        provider = "postmark",       -- or "sendgrid", "ses", "resend", "mailgun"
        api_key = "...",
        from = "invoices@acmecorp.com",
    },
    allowed_hosts = {"api.postmarkapp.com"},
})

-- Then anywhere in app code:
local email = require("email")
email.send({
    to = "customer@example.com",
    subject = "Invoice #1042",
    html = render_template("invoice_email", invoice),
    text = render_plaintext_invoice(invoice),  -- optional plaintext fallback
})
```

The `email` module translates `email.send()` into the correct HTTP API call for the configured provider. Adding a new provider is adding a Lua file — no C changes. The provider's host is declared in `allowed_hosts`, so the network allowlist and `hull verify` / verify.gethull.dev show exactly which email service the app uses.

This works with scheduled tasks and background work:

```lua
-- Send invoice reminders daily at 8am
app.daily("08:00", function()
    local overdue = db.query(
        "SELECT i.*, c.email FROM invoices i JOIN customers c ON i.customer_id = c.id " ..
        "WHERE i.due < ? AND i.reminded = 0", today())
    for _, inv in ipairs(overdue) do
        email.send({
            to = inv.email,
            subject = "Reminder: Invoice #" .. inv.number,
            html = render_template("reminder_email", inv),
        })
        db.exec("UPDATE invoices SET reminded = 1 WHERE id = ?", inv.id)
        app.yield()  -- stay responsive between sends
    end
end)
```

### Built-in SMTP Client

For environments where HTTP email APIs are unavailable — air-gapped networks, on-premise deployments, organizations that run their own mail servers — Hull includes a minimal SMTP client in the C layer. It handles EHLO, STARTTLS (via mbedTLS), AUTH (PLAIN/LOGIN), and MIME multipart message construction.

```lua
-- app.lua — SMTP configuration
app.config({
    email = {
        provider = "smtp",
        host = "mail.acmecorp.local",
        port = 587,
        username = "invoices@acmecorp.com",
        password = "...",
        starttls = true,
        from = "invoices@acmecorp.com",
    },
    allowed_hosts = {"mail.acmecorp.local"},
})

-- Same API — email.send() works identically regardless of transport
email.send({
    to = "customer@example.com",
    subject = "Invoice #1042",
    html = render_template("invoice_email", invoice),
})
```

The Lua API is identical for both transports. Switching from Postmark to a local SMTP server is changing `provider` and `host` in `app.config`. Application code doesn't change.

The SMTP client is deliberately minimal — it sends email. It does not handle bounce processing, DKIM signing, mailing list management, or inbox monitoring. Those are service-level concerns best handled by a mail server or an API provider. Hull's SMTP client covers the common case: "send this email to this address from this account."

### Offline Fallback — Browser mailto:

For fully offline applications that occasionally need to compose an email, the frontend can use `mailto:` links to open the user's default email client with pre-filled fields:

```lua
app.get("/invoices/:id/email", function(req, res)
    local inv = db.query("SELECT * FROM invoices WHERE id = ?", req.params.id)[1]
    local customer = db.query("SELECT * FROM customers WHERE id = ?", inv.customer_id)[1]
    res.json({
        mailto = string.format("mailto:%s?subject=%s&body=%s",
            customer.email,
            url_encode("Invoice #" .. inv.number),
            url_encode(render_plaintext_invoice(inv)))
    })
end)
-- Frontend JS: window.location.href = response.mailto
```

No network access needed. No API key. The user's own email client handles sending. Limited to plaintext and no background batch sends, but sufficient for "email this invoice to the customer" workflows.

## Secrets and API Keys

Any application that talks to external services — email providers, tax authorities, payment processors, government APIs — needs credentials. API keys, SMTP passwords, OAuth client secrets, signing keys.

### Declared Environment Access

Environment variable access follows the same pattern as network access: the application declares which variables it needs, and Hull only reads those. Undeclared variables return `nil`. This is part of the application's security manifest — visible at startup, in `hull inspect`, and on verify.gethull.dev.

```lua
-- app.lua — declare which env vars the app needs
app.config({
    allowed_env = {"POSTMARK_TOKEN", "NAV_API_KEY"},
    allowed_hosts = {"api.postmarkapp.com", "api.nav.gov.hu"},

    email = {
        provider = "postmark",
        api_key = hull.env("POSTMARK_TOKEN"),
    },
    nav_api_key = hull.env("NAV_API_KEY"),
})
```

The C layer enforces the allowlist:

```c
// C layer — hull.env() only reads declared variables
static int hull_env(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    if (!is_allowed_env(name)) {
        lua_pushnil(L);
        return 1;  // undeclared variable — silent nil
    }
    const char *val = getenv(name);
    if (val) lua_pushstring(L, val);
    else lua_pushnil(L);
    return 1;
}
```

The application cannot read `HOME`, `PATH`, `USER`, `SSH_AUTH_SOCK`, or any other environment variable it didn't declare. The startup banner and verify.gethull.dev show exactly which variables the app reads:

```
Hull v0.1.0 | port 8080 | db: data.db
Network:  api.postmarkapp.com, api.nav.gov.hu
Env:      POSTMARK_TOKEN, NAV_API_KEY
Sandbox:  pledge(stdio rpath wpath inet) unveil(data.db:rw, public/:r)
```

A user who sees `Env: POSTMARK_TOKEN, NAV_API_KEY` knows exactly what the application reads from their environment. An app that declares `Env: (none)` reads nothing. Same transparency model as network and filesystem access — declared, visible, enforced.

`hull.env()` is only available during startup (before the sandbox boundary at step 7). After that, the function returns `nil` for all variables — even declared ones. The values are captured once and stored in `app.config` for the lifetime of the process.

### Storage Options

The user provides the declared environment variables before launching the app:

```bash
POSTMARK_TOKEN=xxx NAV_API_KEY=yyy ./myapp.com
```

Or in a `.env` file next to the binary (read at startup, before sandbox). This is the right default for most deployments — secrets are not in the binary, not in the database, and not accessible after the sandbox is applied.

**Stored in the database.** For applications where the end user manages their own API keys (e.g. a settings page where the user enters their Postmark token), the key is stored in SQLite. If database encryption is enabled, the key is encrypted at rest along with everything else.

```lua
-- User enters API key in settings UI, stored in DB
app.post("/settings/email", function(req, res)
    db.exec("UPDATE settings SET value = ? WHERE key = 'postmark_token'", req.body.token)
    res.json({ ok = true })
end)

-- Read at startup or on demand
local function get_email_key()
    return db.query("SELECT value FROM settings WHERE key = 'postmark_token'")[1].value
end
```

**Embedded in the binary.** For single-tenant local apps where the developer controls the deployment and the user owns the machine, the key can be compiled into the binary at build time. The key is in the binary — extractable by anyone with the file. This is acceptable when the binary is distributed to a trusted user (e.g. a licensed customer) and the API key is scoped to their account.

```bash
hull build --output myapp.com --env POSTMARK_TOKEN=xxx
```

### What Hull Does NOT Do

**Hull does not phone home for secrets.** No Vault, no AWS Secrets Manager, no key management service. The secret is either in the environment, in the database, or in the binary. All local. All offline.

**Hull does not read undeclared environment variables.** The application's `allowed_env` is its complete list. `hull.env("PATH")` returns `nil` unless `PATH` is in `allowed_env`. This is enforced in compiled C — Lua code cannot bypass it. The user's environment is private by default.

**Hull does not log secrets.** The startup banner prints the variable *names* (e.g. `Env: POSTMARK_TOKEN`) but never prints values. The user can see *which* variables the app reads without the values being exposed in any log or display.

## Logging

Application logs go to SQLite. Not a log file — SQLite. The database is already there, it's queryable, it doesn't need rotation, and the developer can build a log viewer page in their admin UI with 10 lines of Lua.

### Lua API

```lua
local log = require("log")

log.info("Invoice created", {invoice_id = 42, customer = "Acme"})
log.warn("Rate limit approaching", {remaining = 5})
log.error("NAV API submission failed", {invoice_id = 42, status = 503})
log.debug("Query executed", {sql = "SELECT ...", ms = 12})
```

Four levels: `debug`, `info`, `warn`, `error`. Configurable minimum level. Debug is off by default in production.

### Storage

Hull maintains a `_hull_logs` table automatically:

```sql
CREATE TABLE _hull_logs (
    id INTEGER PRIMARY KEY,
    level TEXT NOT NULL,       -- debug, info, warn, error
    message TEXT NOT NULL,
    context TEXT,              -- JSON blob of structured data
    created_at INTEGER NOT NULL -- unix timestamp
);
CREATE INDEX idx_logs_level_time ON _hull_logs(level, created_at);
```

Writes go to both SQLite (structured, persistent, queryable) and stdout (formatted, for terminal during development). In production, stdout can be suppressed.

The developer can query logs from application code:

```lua
app.get("/admin/logs", function(req, res)
    local logs = db.query(
        "SELECT * FROM _hull_logs WHERE level = ? ORDER BY created_at DESC LIMIT 100",
        req.query.level or "error")
    res.json(logs)
end)
```

### Auto-cleanup

Hull ships a built-in scheduled task — no developer action needed:

```lua
-- Internal, runs automatically
app.daily("03:00", function()
    db.exec("DELETE FROM _hull_logs WHERE created_at < ?",
        hull.time.now() - (app.config.log_retention_days or 30) * 86400)
end)
```

Default retention: 30 days. Configurable via `app.config({ log_retention_days = 90 })`. Logs don't grow forever.

### Why Not a Log File

- Log files need rotation — `logrotate` doesn't exist on Windows
- Log files aren't queryable without external tools (`grep` is not a query language)
- Log files are a second artifact to manage alongside `data.db`
- SQLite in WAL mode handles the write load easily — a local business app generates maybe 100 log entries per hour, SQLite handles thousands of inserts per second
- The database is already there. Why add another file?

## Authentication

Hull is not an auth framework. It provides cryptographic primitives in compiled C and a pattern in the Lua standard library. The developer assembles them for their specific needs.

### Password Hashing (C layer)

mbedTLS already ships in the binary and includes PBKDF2-HMAC-SHA256. No new dependency. The C layer handles:

- Random salt generation (from mbedTLS entropy source)
- PBKDF2-HMAC-SHA256 with configurable iterations (default 600,000)
- Encoding salt + hash + parameters into a single storable string
- Timing-safe comparison (constant-time to prevent timing attacks)

```lua
local auth = require("auth")

-- Hash — returns a self-contained string with algorithm, iterations, salt, and hash
local hash = auth.hash_password("hunter2")
-- "pbkdf2:sha256:600000:base64salt:base64hash"

-- Verify — timing-safe comparison
local ok = auth.verify_password("hunter2", hash)  -- true
local ok = auth.verify_password("wrong", hash)    -- false
```

One function to hash, one to verify. The hash string contains everything needed to verify later — algorithm, iteration count, salt — so parameters can be upgraded in future versions without breaking existing stored hashes.

Hashed passwords are stored in the `users` table in `data.db`. If database encryption is enabled, they are encrypted at rest along with everything else — double protection. The password is hashed (PBKDF2, irreversible) and the hash itself is encrypted (AES-256, inaccessible without the license key). Even if someone gets the database file, they get an encrypted blob. Even if they decrypt it, they get hashes, not passwords.

### Secure Token Generation

For API tokens, password reset links, email verification codes, session IDs — the app needs cryptographically secure random bytes. TweetNaCl's `randombytes()` is already in the binary:

```lua
local token = auth.random_token(32)   -- 32 bytes → 64-char hex string
local short = auth.random_token(16)   -- 16 bytes → 32-char hex string
```

### The Auth Pattern

Hull provides: password hashing (C layer), sessions (Lua stdlib), CSRF (Lua stdlib), middleware (Keel). The developer writes the login logic:

```lua
local auth = require("auth")
local session = require("session")

-- Middleware: protect everything except public routes
app.use("*", "/*", function(req, res)
    if req.path == "/login" or req.path == "/register"
       or req.path:match("^/public/") then
        return 0  -- allow through
    end
    local user = session.get(req, "user")
    if not user then
        res.redirect("/login")
        return 1  -- short-circuit
    end
    req.ctx.user = user  -- available to handlers via req.ctx
    return 0
end)

-- Register
app.post("/register", function(req, res)
    local hash = auth.hash_password(req.body.password)
    db.exec("INSERT INTO users (email, password_hash) VALUES (?, ?)",
        req.body.email, hash)
    res.redirect("/login")
end)

-- Login
app.post("/login", function(req, res)
    local user = db.query(
        "SELECT * FROM users WHERE email = ?", req.body.email)[1]
    if user and auth.verify_password(req.body.password, user.password_hash) then
        session.set(req, res, "user", {id = user.id, email = user.email})
        res.redirect("/")
    else
        res.status(401).json({error = "Invalid credentials"})
    end
end)

-- Logout
app.post("/logout", function(req, res)
    session.clear(req, res)
    res.redirect("/login")
end)
```

~30 lines. No magic. No opinionated auth framework. The developer controls who can register, what roles exist, password requirements, lockout policy. Hull provides the hard parts that must be correct (hashing in compiled C with timing-safe comparison, cryptographic random, session management) and lets the developer write the easy parts that must be flexible (login flow, permissions model, UI) in Lua.

### What Hull Does NOT Provide

- **No OAuth/OIDC provider** — that's a cloud integration. Use browser-side redirect flows if needed (the browser can `fetch()` anywhere)
- **No JWT** — local apps don't need stateless tokens. Sessions in SQLite are simpler, revocable, and there's no distributed system to share tokens across
- **No MFA built-in** — the developer can implement TOTP using the crypto primitives (HMAC from mbedTLS + `auth.random_token` for secret generation), but Hull doesn't ship a TOTP module
- **No user management UI** — the developer builds this. User models are application-specific (roles, permissions, org structure). Hull provides the crypto, not the policy

## Development Mode

Hull's development mode is designed for two audiences: human developers looking at a terminal and browser, and LLM coding agents (Claude Code, OpenCode, Cursor) that only read terminal output. Both need the same thing — fast feedback with clear error messages.

### Console Output

Lua's `print()` is not part of the `io` library — it's a standalone global function. When Hull removes `io` and `os` from the Lua environment, `print()` survives. It is the most basic debugging tool for both humans and LLMs, and Hull keeps it available.

```lua
print("got here", invoice.id, invoice.total)  -- stdout, immediate
log.info("Invoice created", {id = 42})         -- structured, persistent
```

In development mode, both go to the terminal immediately. In production mode, `print()` is redirected to `log.debug` (goes to SQLite, not stdout) and can be suppressed entirely via `log_level`:

```lua
app.config({
    mode = "production",
    log_level = "info",        -- suppresses debug + print
    log_retention_days = 30,
})
```

### LLM-Friendly Error Output

When Lua code errors out, Hull catches it with `xpcall` and a custom error handler. The terminal output is designed to be parseable by both humans and LLMs:

```
[ERROR] routes/invoices.lua:47: attempt to index a nil value (local 'customer')

Stack trace:
  routes/invoices.lua:47  in function 'create_invoice'
  routes/invoices.lua:12  in function <handler POST /invoices>
  lib/app.lua:89          in function 'dispatch'

Context (routes/invoices.lua:44-50):
  44│  local customer = db.query(
  45│      "SELECT * FROM customers WHERE id = ?", req.body.customer_id)[1]
  46│
  47│  local name = customer.name  -- ERROR: customer is nil
  48│
  49│  local invoice = {
  50│      customer_name = name,

Request: POST /invoices
Body: {"customer_id": 999, "amount": 100}
```

The error output includes: file path with line number, the error message, a full stack trace, the source code context around the error line, and the HTTP request that triggered it. An LLM reading this immediately knows: the query returned no rows, `customer` is nil, line 47 needs a nil check. It fixes the code, saves the file, Hull hot-reloads, done.

In development mode, the same error also goes to the HTTP response — a debug error page (like Django/Flask debug mode) so a human in the browser sees it too. In production mode, the HTTP response is a generic `500 Internal Server Error` with no details, and the full error goes to `log.error` in SQLite.

### Hot Reload

In development mode, Lua files are loaded from the filesystem on each request. Change a file, refresh the browser (or re-run `curl`), see the result. No restart, no recompilation, no build step.

### Hull WebSocket — `/_hull/ws`

Hull uses a single WebSocket connection at `/_hull/ws` for all bidirectional communication between the browser and the server. Keel already supports WebSocket natively — no additional dependency. One connection handles everything: server→browser push events, browser→server console errors, heartbeat, shutdown, and hot reload.

The `/_hull/ws` endpoint is active in both development and production, because server-push and connection awareness are useful in any mode. The message types differ:

| Message | Direction | Development | Production | Purpose |
|---------|-----------|-------------|------------|---------|
| Heartbeat | server→browser | yes | yes | Detect server crash/stop, show "disconnected" overlay |
| Shutdown | server→browser | yes | yes | Clean "server stopped" message before connection drops |
| App push | server→browser | yes | yes | Server-to-browser notifications from Lua code |
| File reload | server→browser | yes | no | Auto-refresh browser when source files change on disk |
| Console error | browser→server | yes | no | Pipe JS errors/warnings to server terminal |

All messages are JSON:

```json
{"type": "heartbeat"}
{"type": "shutdown"}
{"type": "push", "event": "report-ready", "data": {"id": 42}}
{"type": "reload"}
{"type": "console", "level": "error", "message": "...", "source": "app.js", "line": 142}
```

**Heartbeat.** The server sends `{"type": "heartbeat"}` every 5 seconds. When the WebSocket closes (server stopped, crashed, or restarted), the browser shows an overlay:

```
┌─────────────────────────────────────────────┐
│  Server disconnected. Waiting for restart...│
└─────────────────────────────────────────────┘
```

The injected JS reconnects automatically with exponential backoff. When the server comes back, the overlay disappears and the page refreshes. In production, this means:

- The user accidentally closes the terminal → the browser shows "disconnected" instead of silently dying on the next click
- The machine reboots → the browser recovers when the app is relaunched
- Hull crashes → clear feedback instead of a broken page

**Server shutdown.** When Hull shuts down cleanly (Ctrl-C, SIGTERM), it sends `{"type": "shutdown"}` before closing. The browser shows "Server stopped" instead of waiting for a timeout.

**App push events (Lua → browser).** Lua code can push custom events to all connected browsers via `app.push()`. This is how background tasks and scheduled jobs communicate results to the UI without polling:

```lua
-- Backend: background task finishes, notify the browser
app.spawn(function()
    generate_report(params)
    app.push("report-ready", {id = report.id, name = "Q4 Summary"})
end)

-- Backend: scheduled sync completes, refresh the UI
app.every("15m", function()
    local count = sync_invoices_from_nav()
    if count > 0 then
        app.push("data-updated", {source = "nav", count = count})
    end
end)
```

```javascript
// Frontend: listen for server-pushed events
const ws = new WebSocket("ws://localhost:8080/_hull/ws");

ws.onmessage = function(e) {
    const msg = JSON.parse(e.data);
    switch (msg.type) {
        case "push":
            if (msg.event === "report-ready") {
                showNotification("Report ready: " + msg.data.name);
                refreshReportList();
            }
            if (msg.event === "data-updated") {
                showNotification(msg.data.count + " invoices synced");
                refreshInvoiceTable();
            }
            break;
        case "shutdown":
            showOverlay("Server stopped.");
            break;
    }
};

ws.onclose = function() {
    showOverlay("Server disconnected. Waiting for restart...");
    // auto-reconnect with backoff (handled by Hull's injected JS)
};
```

This eliminates polling. The browser doesn't repeatedly ask "is the report done yet?" — the server tells it when it's done. WebSocket is the right choice for localhost — bidirectional, low overhead, no proxy concerns, and Keel already has it.

**Browser console proxy (development only, browser → server).** In development mode, Hull injects a small `<script>` block into every HTML response that captures frontend errors and sends them to the server over the same WebSocket connection:

```javascript
// Injected by Hull in development mode only — stripped in production builds
window.onerror = function(msg, src, line, col) {
    ws.send(JSON.stringify({type: "console", level: "error",
        message: msg, source: src, line: line, col: col}));
};
window.onunhandledrejection = function(e) {
    ws.send(JSON.stringify({type: "console", level: "error",
        message: "Unhandled promise: " + e.reason}));
};
["error", "warn"].forEach(function(level) {
    var orig = console[level];
    console[level] = function() {
        orig.apply(console, arguments);
        ws.send(JSON.stringify({type: "console", level: level,
            message: Array.from(arguments).join(" ")}));
    };
});
```

The C layer receives these messages on the WebSocket and prints them to the terminal with a `[BROWSER]` prefix. The LLM sees frontend and backend errors in one stream. In production, the console proxy is not injected — no script, no browser→server messages.

**File-change hot reload (development only).** When Hull detects a file change on disk (Lua, HTML, JS, CSS), it sends `{"type": "reload"}` over the WebSocket. The browser refreshes automatically. The developer saves a file, the browser updates — no manual refresh needed. This message type does not exist in production builds where all code is embedded in the binary.

### Access Log

In development mode, every HTTP request is logged to the terminal with timing and response size:

```
[REQ] GET /api/invoices → 200 (15ms, 2.3KB)
[REQ] POST /api/invoices → 422 (3ms, 48B)
[REQ] GET /public/app.js → 200 (1ms, 8.1KB)
[REQ] GET /public/missing.css → 404 (0ms)
```

A 404 for a static asset is immediately visible. A slow query shows up as a high response time. The LLM doesn't need to guess what's happening — it sees every request and response status.

### The Unified Terminal Stream

Everything the LLM needs in one place — backend and frontend:

```
Hull v0.1.0 | port 8080 | db: data.db | mode: development
Ready.

[REQ] GET / → 200 (2ms, 4.2KB)
[REQ] GET /public/app.js → 200 (1ms, 8.1KB)
[REQ] GET /public/style.css → 404 (0ms)
[REQ] GET /api/invoices → 200 (15ms, 2.3KB)

[BROWSER] error: Cannot read property 'name' of undefined
          at renderInvoice (app.js:142:18)
          at fetchInvoices (app.js:98:5)

[REQ] POST /api/invoices → 422 (3ms)
[BROWSER] warn: Fetch failed: POST /api/invoices 422 {"error": "missing field"}

[ERROR] routes/invoices.lua:47: attempt to index a nil value (local 'customer')
  Stack trace:
    routes/invoices.lua:47  in function 'create_invoice'
  Context (routes/invoices.lua:44-50):
    45│      "SELECT * FROM customers WHERE id = ?", req.body.customer_id)[1]
    47│  local name = customer.name  -- ERROR: customer is nil
  Request: POST /api/invoices
  Body: {"customer_id": 999}
```

The LLM reads this single stream and sees:

1. `style.css` → 404 — needs to create the file or fix the HTML reference
2. JS error at `app.js:142` — `name` property of undefined, needs a null check in the frontend
3. Lua error at `invoices.lua:47` — same nil issue on the backend
4. The 422 response body tells it exactly which field is missing

The LLM fixes all three — backend Lua, frontend JS, missing asset — without opening a browser.

### The LLM Development Loop

This is why Hull is purpose-built for vibecoding. The LLM writes both backend and frontend code, tests via `curl`, and reads one terminal for all errors:

```
Terminal 1: hull dev
            Hull v0.1.0 | port 8080 | db: data.db | mode: development
            Ready.

Terminal 2: (LLM coding agent)
            > write routes/invoices.lua     ← LLM writes backend
            > write public/app.js           ← LLM writes frontend
            > curl localhost:8080/           ← triggers page load

            [REQ] GET / → 200 (2ms)
            [REQ] GET /public/app.js → 200 (1ms)
            [BROWSER] error: renderInvoice is not defined (app.js:12:3)

            > edit public/app.js            ← LLM fixes the JS bug
            > curl localhost:8080/           ← retrigger

            [REQ] GET / → 200 (2ms)
            [REQ] GET /api/invoices → 200 (15ms)
            (no errors)                      ← it works
```

No React component trees. No webpack build errors. No hydration mismatches. No browser DevTools the LLM can't access. Write Lua, write JS, curl, read the terminal. The feedback loop covers both backend and frontend.

### What This Does NOT Cover

**Visual and layout issues.** The LLM can't see that a button is misaligned or a color is wrong. CSS rendering is inherently visual and requires human eyes. Browsers do emit console warnings for some CSS issues (invalid properties, failed asset loads), which would be piped back, but "this doesn't look right" requires a person looking at it. This limitation applies to every stack, not just Hull.

**Hull does not render the frontend for the LLM.** There is no headless browser, no DOM serialization, no server-side rendering of the HTML for terminal consumption. The LLM reads the HTML/JS/CSS source files directly — it has filesystem access — and reasons about structure from source. If it needs to know what an API returns, it curls the endpoint. If it needs to know what the page does, it reads the HTML and JS. If something breaks at runtime, the browser console proxy tells it. The LLM decides what debug information it needs and how to get it. Adding a headless renderer would be massive complexity for marginal gain — the LLM is already good at reasoning about HTML structure from source code.

### Production Mode

In production, all development instrumentation is stripped:

| Behavior | Development | Production |
|----------|-------------|------------|
| `print()` | stdout (immediate) | redirected to `log.debug` or suppressed |
| `log.*` | stdout + SQLite | SQLite only (configurable) |
| Lua errors | terminal + HTTP response (debug page) | `log.error` in SQLite + generic HTTP 500 |
| Access log | stdout (`[REQ]` lines) | `log.info` in SQLite |
| Browser console proxy | active (JS errors piped to terminal via WebSocket) | disabled (no injection) |
| `/_hull/ws` WebSocket | all messages (heartbeat, shutdown, push, reload, console) | heartbeat + shutdown + app push only |
| Hot reload | yes (files loaded per-request, WebSocket triggers browser refresh) | no (code embedded in binary) |
| Startup banner | printed | printed |

## HTML Templating

Not every Hull app is an SPA. Many apps — especially the ones vibecoders build — just want to return HTML from the server. A form submits, the page reloads with results. No JavaScript framework, no client-side state management, no build step. Traditional web development that LLMs generate fluently.

Hull ships a template engine in the Lua stdlib (~150 lines). The syntax uses `{{ }}` for expressions and `{% %}` for Lua code — a pattern LLMs know well from Jinja2, Django, EJS, and ERB:

```html
<!-- templates/invoice.html -->
<h1>Invoice #{{invoice.number}}</h1>
<p>Date: {{invoice.date}}</p>
<p>Customer: {{invoice.customer_name}}</p>

<table>
  <tr><th>Item</th><th>Qty</th><th>Price</th><th>Total</th></tr>
  {% for _, item in ipairs(invoice.items) do %}
  <tr>
    <td>{{item.name}}</td>
    <td>{{item.qty}}</td>
    <td>€{{item.price}}</td>
    <td>€{{item.total}}</td>
  </tr>
  {% end %}
</table>

{% if invoice.paid then %}
  <p class="status paid">PAID</p>
{% else %}
  <p class="status unpaid">UNPAID — Due: {{invoice.due_date}}</p>
{% end %}
```

```lua
local tmpl = require("template")

app.get("/invoices/:id", function(req, res)
    local invoice = db.query("SELECT * FROM invoices WHERE id = ?", req.params.id)[1]
    invoice.items = db.query("SELECT * FROM invoice_items WHERE invoice_id = ?", invoice.id)
    res.html(tmpl.render("invoice", invoice))
end)
```

### Template Syntax

- `{{expr}}` — output, HTML-escaped by default (prevents XSS)
- `{{{expr}}}` — raw output, no escaping (for trusted HTML)
- `{% lua code %}` — execute Lua (loops, conditionals, variable assignment)

### Layouts

A base template that wraps pages — header, nav, footer defined once:

```html
<!-- templates/layout.html -->
<!DOCTYPE html>
<html>
<head>
  <title>{{title}} — MyApp</title>
  <link rel="stylesheet" href="/public/style.css">
</head>
<body>
  <nav>
    <a href="/">Dashboard</a>
    <a href="/invoices">Invoices</a>
    <a href="/customers">Customers</a>
  </nav>
  <main>{{{content}}}</main>
  <footer>MyApp v1.0 — {{user.email}}</footer>
</body>
</html>
```

```lua
res.html(tmpl.render("invoice", invoice, {layout = "layout"}))
```

The template engine renders the page template first, then injects the result into `{{{content}}}` in the layout. Standard pattern.

### Template Resolution

Templates are loaded from `templates/` in development (filesystem, hot-reload). In production, they're embedded in the binary alongside Lua files and static assets. Same resolution order as everything else in Hull.

### SPA vs Server-Rendered — Not a Choice

Hull supports both, and they can coexist in the same app:

```lua
-- Server-rendered HTML pages (template engine)
app.get("/invoices",     function(req, res) res.html(tmpl.render("invoice_list", data)) end)
app.get("/invoices/:id", function(req, res) res.html(tmpl.render("invoice",      data)) end)

-- JSON API endpoints (for SPA frontend or mobile clients)
app.get("/api/invoices",     function(req, res) res.json(invoices) end)
app.get("/api/invoices/:id", function(req, res) res.json(invoice)  end)

-- Static SPA frontend
app.static("/app", "public/")  -- serves index.html, app.js, etc.
```

A vibecoder can start with server-rendered HTML (simpler, fewer files, LLM generates it in one shot) and migrate to an SPA later if they need richer interactivity. Or use both: server-rendered for content pages, SPA for the dashboard.

## PDF Generation

Hull provides three layers of PDF output, from zero-code to fully programmatic:

### Browser Print (already works)

CSS `@media print` + `window.print()`. The user clicks "Print," the browser generates a PDF. Zero server-side code, handles complex layouts, supports images, custom fonts, and full CSS styling. Good for interactive "print this page" workflows. Does not work for batch generation, email attachments, or background tasks — the user must initiate it from the browser.

### Lua PDF Writer (stdlib module)

A pure Lua module (~500-600 lines) that generates PDF programmatically. No C dependency. PDF 1.4 is a text-based format at its core — for structured business documents (invoices, receipts, reports), you don't need a full page layout engine. You need text, tables, fonts, page breaks, and alignment.

Built-in PDF fonts (Helvetica, Times, Courier) are guaranteed by the PDF specification in every PDF reader — no font embedding needed. This covers 95% of business document use cases.

```lua
local pdf = require("pdf")

local doc = pdf.new({title = "Invoice #1042", author = "Acme Corp"})

doc:font("Helvetica", 12)
doc:text("INVOICE", {align = "center", size = 24, bold = true})
doc:text("Invoice #1042", {size = 14})
doc:text("Date: 2026-02-25")
doc:text("Customer: Kovacs Pekseg Kft.")
doc:line()

doc:table({
    headers = {"Item", "Qty", "Unit Price", "Total"},
    rows = {
        {"Flour (50kg)", "10", "€25.00", "€250.00"},
        {"Sugar (25kg)", "5",  "€18.00", "€90.00"},
    },
    widths = {0.4, 0.1, 0.25, 0.25},
})

doc:line()
doc:text("Subtotal: €340.00", {align = "right", bold = true})
doc:text("VAT (27%): €91.80", {align = "right"})
doc:text("Grand Total: €431.80", {align = "right", bold = true, size = 14})

local bytes = doc:render()
```

Three outputs from the same data:

```lua
-- Serve as download
app.get("/invoices/:id/pdf", function(req, res)
    local invoice = load_invoice(req.params.id)
    local bytes = render_invoice_pdf(invoice)
    res.header("Content-Type", "application/pdf")
    res.header("Content-Disposition", 'attachment; filename="invoice-1042.pdf"')
    res.body(bytes)
end)

-- Save to filesystem
hull.fs.write("exports/invoice-1042.pdf", bytes)

-- Attach to email
email.send({
    to = customer.email,
    subject = "Invoice #1042",
    html = tmpl.render("invoice_email", invoice),
    attachments = {
        {name = "invoice-1042.pdf", data = bytes, type = "application/pdf"},
    },
})

-- Batch generation in a background task
app.spawn(function()
    local invoices = db.query("SELECT * FROM invoices WHERE month = ?", month)
    for _, inv in ipairs(invoices) do
        local bytes = render_invoice_pdf(inv)
        hull.fs.write("exports/" .. inv.number .. ".pdf", bytes)
        app.yield()
    end
    app.push("batch-pdf-done", {count = #invoices})
end)
```

### C PDF Library (future, if needed)

If demand warrants complex PDFs (embedded images, custom fonts, complex multi-column layouts), Hull can vendor a C library like libharu (~200KB). The Lua API stays the same — `pdf.new()`, `doc:text()`, `doc:table()` — the backend changes underneath. Same pattern as the email module: unified Lua API, swappable implementation.

### What the Lua PDF Writer Does NOT Cover

Full-page graphic design, embedded raster images, custom font embedding, complex multi-column layouts. These are desktop publishing problems, not business document problems. For those, the browser's print-to-PDF with CSS `@media print` handles more complex layouts than the Lua module ever should.

### Same Data, Three Outputs

Templates, PDF, and browser print all serve the same data model in different formats:

```lua
local function load_invoice(id)
    local inv = db.query("SELECT * FROM invoices WHERE id = ?", id)[1]
    inv.items = db.query("SELECT * FROM invoice_items WHERE invoice_id = ?", id)
    return inv
end

-- HTML (browser, server-rendered)
app.get("/invoices/:id", function(req, res)
    res.html(tmpl.render("invoice", load_invoice(req.params.id), {layout = "layout"}))
end)

-- PDF (download or email attachment)
app.get("/invoices/:id/pdf", function(req, res)
    local bytes = render_invoice_pdf(load_invoice(req.params.id))
    res.header("Content-Type", "application/pdf")
    res.body(bytes)
end)

-- JSON (SPA frontend or API consumer)
app.get("/api/invoices/:id", function(req, res)
    res.json(load_invoice(req.params.id))
end)

-- Print (browser CSS @media print — zero server code)
```

One data model, four representations. The developer writes `load_invoice()` once.

## File Uploads

Hull exposes file uploads through `req.files`, populated by the C layer from Keel's multipart/form-data parser. No Lua-side parsing — the C layer handles RFC 2046 boundary detection, header extraction, and body accumulation before Lua ever sees the data.

### Lua API

```lua
app.post("/upload", function(req, res)
    if not req.files then
        return res.json({error = "no files"}, 400)
    end

    for _, file in ipairs(req.files) do
        -- file.name     = form field name (string)
        -- file.filename = original filename (string)
        -- file.type     = Content-Type (string, e.g. "image/png")
        -- file.size     = byte count (number)
        -- file.data     = raw bytes (string)

        hull.fs.write("uploads/" .. file.filename, file.data)
    end

    res.json({uploaded = #req.files})
end)
```

### Limits

Upload limits are declared in `app.config`, enforced by the C layer before Lua runs:

```lua
app.config = {
    upload_max_size  = 10 * 1024 * 1024,   -- 10 MB per file (default)
    upload_max_total = 50 * 1024 * 1024,   -- 50 MB per request (default)
    upload_max_files = 20,                  -- max files per request (default)
}
```

Exceeding any limit returns HTTP 413 (Payload Too Large) automatically. The handler never executes.

### How It Works

1. Browser sends `multipart/form-data` POST
2. Keel's C-layer multipart parser (`KlMultipartReader`) processes the stream
3. Each part's headers and body are accumulated into `KlMultipartPart` structs
4. `lua_bindings.c` converts the parts array into `req.files` — a Lua table of file objects
5. Handler receives fully-parsed files, no streaming or callbacks needed

File storage uses `hull.fs.write()`, which is path-restricted by the sandbox. Files can only be written to directories the application has declared access to via unveil.

### What's Explicitly Out of Scope

**Streaming uploads** — files are fully buffered before the handler runs. This matches the local-first model: uploads come from the local machine over localhost, so latency is negligible. A 10 MB file at localhost loopback speed takes milliseconds.

**Image processing** — resizing, thumbnails, format conversion are not built in. The developer can shell out to system tools or use a Lua library if needed. Most local-first apps display images at their original size.

## Testing

Hull includes a built-in test framework invoked via `hull test`. Tests run in the same process as the application — no TCP connections, no port binding, no timing dependencies. The test module simulates HTTP requests by calling route handlers directly through Keel's router.

### Running Tests

```bash
hull test                    # run all tests in tests/
hull test tests/test_auth.lua  # run a specific test file
```

Exit code 0 on success, 1 on failure. LLM-friendly output: test name, pass/fail, failure details with expected vs actual.

### Writing Tests

```lua
local test = require("test")
local db = require("db")

-- Runs before each test: clean slate
test.setup(function()
    db.exec("DELETE FROM users")
    db.exec("DELETE FROM sessions")
end)

test.test("register creates user", function(t)
    local res = t.post("/api/register", {
        json = {email = "alice@example.com", password = "secret123"}
    })

    t.equal(res.status, 200)
    t.match(res.json.token, "^%x+$")

    local user = db.query("SELECT * FROM users WHERE email = ?", "alice@example.com")[1]
    t.ok(user, "user exists in database")
    t.ok(user.password_hash ~= "secret123", "password is hashed")
end)

test.test("login with wrong password returns 401", function(t)
    -- Setup: create a user first
    t.post("/api/register", {
        json = {email = "bob@example.com", password = "correct"}
    })

    local res = t.post("/api/login", {
        json = {email = "bob@example.com", password = "wrong"}
    })

    t.equal(res.status, 401)
end)

test.test("upload accepts valid file", function(t)
    local res = t.post("/upload", {
        files = {
            {name = "doc", filename = "report.pdf", type = "application/pdf", data = "fake-pdf-bytes"}
        }
    })

    t.equal(res.status, 200)
    t.equal(res.json.uploaded, 1)
end)
```

### Test Helpers

| Helper | Purpose |
|--------|---------|
| `t.get(path, opts?)` | Simulate GET request |
| `t.post(path, opts?)` | Simulate POST request |
| `t.put(path, opts?)` | Simulate PUT request |
| `t.delete(path, opts?)` | Simulate DELETE request |
| `t.equal(got, want, msg?)` | Assert equality |
| `t.ok(val, msg?)` | Assert truthy |
| `t.match(str, pattern, msg?)` | Assert Lua pattern match |
| `t.fail(msg)` | Fail unconditionally |

Request options:

```lua
{
    json = {...},         -- sets body + Content-Type: application/json
    headers = {...},      -- custom headers
    files = {...},        -- simulated multipart upload
    cookies = {...},      -- request cookies
}
```

Response object:

```lua
res.status              -- HTTP status code (number)
res.headers             -- response headers (table)
res.body                -- raw response body (string)
res.json                -- parsed JSON body (table, nil if not JSON)
```

### Database Isolation

Each `hull test` run uses an in-memory SQLite database (`:memory:`). Migrations run automatically before the first test. `test.setup()` runs before each test function — typically used to clear tables.

This means:
- Tests never touch the production database
- Tests are fast (in-memory SQLite, no disk I/O)
- Tests are isolated (setup function provides clean state)
- No teardown needed (database is discarded when the process exits)

### LLM Testing Loop

The test framework is designed for LLM-driven development. The typical loop:

```
1. LLM writes feature code
2. LLM writes test
3. hull test → reads output
4. If failure: LLM reads error, fixes code, goto 3
5. If pass: move to next feature
```

Output format is plain text, parseable by any LLM:

```
PASS  register creates user (2ms)
FAIL  login with wrong password returns 401 (1ms)
      expected: 401
      got:      500
      at: tests/test_auth.lua:25
PASS  upload accepts valid file (1ms)

2 passed, 1 failed
```

## App WebSockets

Hull exposes Keel's WebSocket support to Lua with the same routing pattern as HTTP. The `/_hull/ws` connection handles platform-level events (heartbeat, shutdown, hot reload). App WebSockets are developer-defined endpoints for application-specific real-time communication.

### Lua API

```lua
app.ws("/chat/:room", {
    on_open = function(ws)
        -- ws.id       = unique connection ID (number)
        -- ws.params   = route params {room = "general"}
        -- ws.headers  = upgrade request headers
        -- ws.query    = query string params
        ws.send(json.encode({type = "welcome", id = ws.id}))
    end,

    on_message = function(ws, msg)
        -- msg = string (text frame) or bytes (binary frame)
        ws.broadcast(msg)           -- send to all others on this route
        ws.broadcast(msg, true)     -- send to all including sender
    end,

    on_close = function(ws, code, reason)
        -- cleanup
    end
})
```

### Connection Management

From any handler (HTTP or WebSocket):

```lua
app.ws_send("/chat/general", msg)                -- broadcast to all on route
app.ws_send("/chat/general", msg, {id = ws.id})  -- send to specific connection
app.ws_count("/chat/general")                    -- active connection count
```

### C Layer

The C layer handles:
- WebSocket upgrade handshake (already in Keel)
- Frame parsing, ping/pong, close frames (already in Keel)
- Per-route connection list (new: tracks which connections belong to which `app.ws` route)
- Calls into Lua for `on_open`, `on_message`, `on_close` events
- Enforces connection and message size limits before Lua sees the data

### Limits

```lua
app.config = {
    ws_max_connections   = 100,              -- per route (default)
    ws_max_message_size  = 1024 * 1024,      -- 1 MB per message (default)
}
```

Exceeding connection limit rejects the upgrade with HTTP 503. Exceeding message size closes the connection with WebSocket status 1009 (Message Too Big). Both enforced at C layer.

This covers: live dashboards, chat, notifications, real-time data feeds, collaborative editing. All over localhost — no scaling concerns.

## CSV Export/Import

CSV is the universal data interchange format for business tools. Hull includes a built-in `csv` module — pure Lua, RFC 4180 compliant, ~150 lines.

### Export

```lua
local csv = require("csv")

app.get("/export/invoices.csv", function(req, res)
    local rows = db.query("SELECT id, customer, amount, date FROM invoices")
    local data = csv.encode(rows, {
        headers = {"ID", "Customer", "Amount", "Date"}
    })
    res.header("Content-Type", "text/csv")
    res.header("Content-Disposition", 'attachment; filename="invoices.csv"')
    res.body(data)
end)
```

### Import

```lua
app.post("/import/invoices", function(req, res)
    local file = req.files[1]
    local rows, err = csv.decode(file.data, {
        headers = true,     -- first row is headers → returns array of tables
        types = {           -- optional type coercion
            amount = "number",
            date = "string",
        }
    })

    if err then return res.json({error = err}, 400) end

    for _, row in ipairs(rows) do
        db.exec("INSERT INTO invoices (customer, amount, date) VALUES (?, ?, ?)",
            row.Customer, row.amount, row.date)
    end

    res.json({imported = #rows})
end)
```

### API

**csv.encode(rows, opts):**
- `rows` — array of tables (column → value)
- `opts.headers` — explicit column order/names, or auto-detect from first row's keys
- `opts.separator` — default `","`, configurable for TSV or European `;`
- Returns a string

**csv.decode(str, opts):**
- `str` — CSV content
- `opts.headers` — `true` = first row is headers (returns array of tables), `false` = returns array of arrays
- `opts.separator` — auto-detect `,` vs `;` vs `\t`
- `opts.types` — optional column → type map for coercion
- Returns array of rows, or `nil, error_message`

Handles: quoted fields, embedded commas, embedded newlines, escaped double quotes (`""`). No Excel (XLSX) support — XLSX is a ZIP of XML files requiring zlib. If we add zlib later (~30 KB), basic XLSX export becomes feasible. For now, CSV covers 95% of business data interchange. Excel opens CSV natively.

## Internationalization (i18n)

Most local-first business tools serve one language per installation. A Hungarian accountant doesn't need runtime language switching — they need the app in Hungarian. But the developer might sell the same app in multiple markets.

Hull's i18n is compile-time locale selection with runtime string lookup. Simple key-value tables, no ICU, no CLDR, no plural rules engine.

### Setup

```lua
local i18n = require("i18n")

-- Load locale tables
i18n.load("en", require("locales.en"))
i18n.load("hu", require("locales.hu"))

-- Set active locale (from config, user preference, or auto-detect)
i18n.locale("hu")
```

### Locale Files

Plain Lua tables. No special format, no compilation step.

```lua
-- locales/en.lua
return {
    invoice = {
        title = "Invoice",
        total = "Total: ${amount}",
        status = {
            draft = "Draft",
            sent = "Sent",
            paid = "Paid",
        }
    },
    nav = {
        home = "Home",
        invoices = "Invoices",
        settings = "Settings",
    }
}
```

```lua
-- locales/hu.lua
return {
    invoice = {
        title = "Számla",
        total = "Összesen: ${amount} Ft",
        status = {
            draft = "Piszkozat",
            sent = "Elküldve",
            paid = "Fizetve",
        }
    },
    nav = {
        home = "Főoldal",
        invoices = "Számlák",
        settings = "Beállítások",
    }
}
```

### Usage

```lua
-- In handlers
i18n.t("invoice.title")                          --> "Számla"
i18n.t("invoice.total", {amount = "1 500"})       --> "Összesen: 1 500 Ft"

-- In templates
-- <h1>{{ t("invoice.title") }}</h1>
-- <p>{{ t("invoice.total", {amount = inv.total}) }}</p>
```

**Interpolation:** `${key}` in translation strings, replaced from the params table. For plurals, the developer writes Lua — it's a three-line conditional, not a framework:

```lua
i18n.t(count == 1 and "item.one" or "item.other", {count = count})
```

### Number/Date/Currency Formatting

Locale-aware formatting exposed as helpers:

```lua
i18n.number(1500.5)          --> "1 500,5" (Hungarian)
i18n.date(hull.time.now())   --> "2026.02.25." (Hungarian)
i18n.currency(1500, "HUF")  --> "1 500 Ft"
```

Each locale defines a small format table: decimal separator, thousands separator, date pattern, currency symbol/position. ~50 lines of Lua per locale format.

### Language Detection

```lua
-- Auto-detect from browser Accept-Language header
local lang = i18n.detect(req)  -- picks best match from loaded locales
i18n.locale(lang)
```

Pure Lua, ~200 lines for the core. Locale files are user-authored Lua tables.

## Full-Text Search

SQLite FTS5 is one of the best full-text search engines available — used by Apple's Spotlight, among others. It's compiled into SQLite by default. Hull wraps it with an ergonomic Lua API.

### Define an Index

```lua
local search = require("search")

-- Creates FTS5 virtual table + sync triggers if not exists
search.index("invoices_fts", {
    source = "invoices",                -- source table
    fields = {"customer", "notes"},     -- columns to index
    tokenizer = "unicode61",            -- default, handles accented chars + CJK
})
```

Under the hood this creates:

```sql
CREATE VIRTUAL TABLE IF NOT EXISTS invoices_fts
USING fts5(customer, notes, content=invoices, content_rowid=id,
           tokenize='unicode61');

-- Automatic sync triggers (INSERT, UPDATE, DELETE)
CREATE TRIGGER IF NOT EXISTS invoices_fts_ai AFTER INSERT ON invoices BEGIN
    INSERT INTO invoices_fts(rowid, customer, notes)
    VALUES (new.id, new.customer, new.notes);
END;
```

### Search

```lua
local results = search.query("invoices_fts", "kovács AND építkezés", {
    limit = 20,
    offset = 0,
    order = "rank",     -- FTS5 relevance ranking (default)
    snippet = true,     -- return highlighted snippets
})

-- results = {
--   {id = 42, rank = -2.5, snippet = "...<b>Kovács</b> Kft. — <b>építkezés</b>i anyag..."},
--   {id = 17, rank = -1.8, snippet = "...<b>Kovács</b> János..."},
-- }
```

All values in the generated SQL use `?` parameter binding — the search query string itself goes through FTS5's `MATCH` syntax, not string concatenation.

### Rebuild Index

After bulk import or schema change:

```lua
search.rebuild("invoices_fts")
```

### Multiple Indexes

Different field combinations for different search contexts:

```lua
search.index("customer_search", {source = "invoices", fields = {"customer"}})
search.index("full_search", {source = "invoices", fields = {"customer", "notes", "line_items"}})
```

Pure Lua wrapper, ~80 lines. All heavy lifting is SQLite FTS5. The `unicode61` tokenizer handles accented characters, CJK, and most Unicode correctly out of the box.

## Pagination

Every list view needs pagination. Rather than a separate module, this is a `db` helper.

### Offset-Based Pagination

```lua
app.get("/invoices", function(req, res)
    local page = db.paginate(
        "SELECT * FROM invoices ORDER BY date DESC", {},
        {page = tonumber(req.query.page) or 1, per_page = 25}
    )

    -- page.rows     = array of row tables (the data)
    -- page.page     = current page number (1-indexed)
    -- page.per_page = items per page
    -- page.total    = total matching rows (COUNT(*))
    -- page.pages    = total pages (ceil(total / per_page))
    -- page.has_prev = boolean
    -- page.has_next = boolean

    res.json({
        invoices = page.rows,
        pagination = {
            page = page.page,
            pages = page.pages,
            total = page.total,
            has_prev = page.has_prev,
            has_next = page.has_next,
        }
    })
end)
```

### In Templates

```html
{% for _, inv in ipairs(page.rows) do %}
    <tr><td>{{ inv.customer }}</td><td>{{ inv.amount }}</td></tr>
{% end %}

{% if page.pages > 1 then %}
<nav>
    {% if page.has_prev then %}<a href="?page={{ page.page - 1 }}">← Prev</a>{% end %}
    <span>Page {{ page.page }} of {{ page.pages }}</span>
    {% if page.has_next then %}<a href="?page={{ page.page + 1 }}">Next →</a>{% end %}
</nav>
{% end %}
```

### Cursor-Based Pagination

For large datasets where `OFFSET` performance degrades:

```lua
local page = db.paginate_cursor(
    "SELECT * FROM events ORDER BY id DESC",
    {after = req.query.after, limit = 50}
)
-- page.rows, page.next_cursor, page.has_more
```

Uses `WHERE id < ?` internally — constant-time regardless of offset depth.

### Implementation

Wraps the developer's query with `LIMIT ? OFFSET ?` and runs a parallel `SELECT COUNT(*)` with the same WHERE clause. Pagination values are parameter-bound, not interpolated. ~40 lines of Lua in `db.lua`.

## Backup

SQLite databases are single files. Backup is `cp`. But Hull provides a proper command that creates a consistent snapshot without locking the running server.

### CLI

```bash
hull backup                          # → data.db.2026-02-25T14-30-00.bak
hull backup --output invoices.bak    # custom filename
```

Uses SQLite's `VACUUM INTO` — creates a defragmented, consistent copy. Takes milliseconds for typical local-first databases.

### Scheduled Backups

```lua
app.daily("02:00", function()
    local filename = "backups/data-" .. hull.time.date("%Y-%m-%d") .. ".db"
    db.backup(filename)
    log.info("backup", {file = filename, size = hull.fs.size(filename)})
end)
```

### Restore

```bash
hull restore data.db.2026-02-25T14-30-00.bak
```

Copies the backup over `data.db` after verifying the SQLite header is valid. No migration needed — the schema is in the file.

The backup file is a fully self-contained SQLite database. You can open it with any SQLite client, query it, verify it. No proprietary format, no vendor lock-in, no special tooling required.

## Role-Based Access Control (RBAC)

Builds on top of the `auth` module. `auth` handles identity (who are you?), `rbac` handles authorization (what can you do?).

### Define Roles

```lua
local rbac = require("rbac")

rbac.role("admin", {"*"})                                -- all permissions
rbac.role("accountant", {"invoices.*", "reports.view"})   -- wildcard + specific
rbac.role("viewer", {"invoices.view", "reports.view"})    -- read-only
```

### Assign and Check

```lua
rbac.assign(user_id, "accountant")
rbac.revoke(user_id, "accountant")
rbac.can(user_id, "invoices.create")    --> true/false
rbac.roles(user_id)                     --> {"accountant"}
```

### Middleware Guard

```lua
-- Protect a route
app.post("/api/invoices", rbac.require("invoices.create"), function(req, res)
    -- only reaches here if user has permission
end)

-- Protect a group of routes
app.use("/api/admin/*", rbac.require("admin"))

-- Multiple permissions
app.delete("/api/invoices/:id",
    rbac.require_all("invoices.delete", "invoices.view"), handler)

app.get("/api/reports/:type",
    rbac.require_any("reports.view", "admin"), handler)
```

### How It Works

`rbac.require()` returns a middleware function that:

1. Reads `req.user_id` (set by auth middleware earlier in the chain)
2. Loads user roles from `_hull_user_roles`
3. Checks if any role grants the required permission
4. If no → returns 403 Forbidden
5. If yes → calls next handler

### Permission Format

Dot-separated namespaces with wildcard support:

- `invoices.create` — specific permission
- `invoices.*` — all invoice permissions
- `*` — superuser (all permissions)

### Storage

Two tables, auto-created on first use:

```sql
CREATE TABLE IF NOT EXISTS _hull_roles (
    name TEXT PRIMARY KEY,
    permissions TEXT NOT NULL  -- JSON array
);

CREATE TABLE IF NOT EXISTS _hull_user_roles (
    user_id INTEGER NOT NULL,
    role TEXT NOT NULL REFERENCES _hull_roles(name),
    PRIMARY KEY (user_id, role)
);
```

Pure Lua, ~120 lines. Role definitions and user-role assignments stored in the same encrypted SQLite database as everything else.

## Data Validation

The `valid` module is the input validation layer — the first line of defense before data reaches the database. It validates, type-coerces, and strips unknown fields in one pass.

### Usage

```lua
local valid = require("valid")

app.post("/api/invoices", function(req, res)
    local data, errors = valid.check(req.body, {
        customer    = {required = true, type = "string", min = 1, max = 200},
        amount      = {required = true, type = "number", min = 0},
        date        = {required = true, pattern = "^%d%d%d%d%-%d%d%-%d%d$"},
        email       = {type = "string", email = true},
        status      = {type = "string", one_of = {"draft", "sent", "paid"}},
        notes       = {type = "string", max = 5000},
        items       = {type = "table", min_items = 1, each = {
            description = {required = true, type = "string", max = 500},
            quantity    = {required = true, type = "number", min = 1},
            unit_price  = {required = true, type = "number", min = 0},
        }},
    })

    if errors then
        return res.json({errors = errors}, 422)
    end

    -- data is validated + cleaned: unknown fields stripped
    db.exec("INSERT INTO invoices (customer, amount, date, email, status, notes) VALUES (?, ?, ?, ?, ?, ?)",
        data.customer, data.amount, data.date, data.email, data.status, data.notes)
end)
```

### Error Format

```lua
-- errors = {
--   customer = "is required",
--   amount   = "must be a number",
--   email    = "is not a valid email",
--   items    = {
--     [2] = {quantity = "must be at least 1"}
--   }
-- }
```

Flat structure for simple fields, nested for table/array fields. LLM-readable, JSON-serializable, frontend-displayable.

### Built-in Validators

| Validator | Description |
|-----------|-------------|
| `required` | Field must be present and non-nil |
| `type` | `"string"`, `"number"`, `"boolean"`, `"table"` |
| `min` / `max` | Number range or string length |
| `pattern` | Lua pattern match |
| `email` | Basic email format check |
| `one_of` | Value must be in allowed list |
| `match` | Must equal another field (e.g. `match = "password"` for confirmation) |
| `each` | Validate each item in a table/array |
| `min_items` / `max_items` | Array length bounds |
| `custom` | `function(value) return true/false, "error msg" end` |

### Sanitization

`valid.check()` returns a *new* table containing only declared fields. Undeclared fields are stripped. This prevents mass assignment — you can safely pass the result to a database insert without worrying about extra fields being injected.

### Reusable Schemas

```lua
local invoice_schema = {
    customer = {required = true, type = "string", min = 1, max = 200},
    amount   = {required = true, type = "number", min = 0},
}

app.post("/api/invoices", function(req, res)
    local data, errors = valid.check(req.body, invoice_schema)
    -- ...
end)

app.put("/api/invoices/:id", function(req, res)
    local data, errors = valid.check(req.body, invoice_schema)
    -- ...
end)
```

Pure Lua, ~150 lines. Uses Lua patterns for format validation (no regex engine dependency).

## Rate Limiting

Even local UIs can go rogue. A broken `setInterval`, an infinite retry loop, a misbehaving SPA. The server protects itself.

### Basic Usage

```lua
local limit = require("limit")

-- Global: 100 requests per second
app.use("*", limit.per_second(100))

-- Per-route limits
app.use("/api/login", limit.per_minute(10))        -- brute force protection
app.use("/api/upload", limit.per_minute(30))        -- upload throttle
app.use("/api/export/*", limit.per_hour(50))        -- expensive operations
```

### Advanced Configuration

```lua
app.use("/api/*", limit.create({
    rate = 50,              -- requests
    period = 60,            -- per 60 seconds
    burst = 10,             -- allow burst above rate
    key = function(req)     -- grouping key (default: client IP)
        return req.headers["X-Session-Id"] or req.ip
    end,
    on_limited = function(req, res)   -- custom response (default: 429)
        res.json({error = "slow down", retry_after = 5}, 429)
    end,
}))
```

### Response on Limit Exceeded

```
HTTP/1.1 429 Too Many Requests
Retry-After: 5
Content-Type: application/json

{"error": "rate limit exceeded", "retry_after": 5}
```

### Implementation

Token bucket algorithm, in-memory (not SQLite — rate limiting needs to be fast). Each bucket tracks request count and last refill timestamp. Buckets are garbage-collected when idle.

Runs as Lua middleware, not C layer. For local-first apps the overhead is negligible — a table lookup + counter increment, sub-microsecond. The C layer already enforces `max_connections` for true fd exhaustion. Rate limiting handles the application-level concern: protecting routes from request floods.

Pure Lua, ~80 lines.

## Known Limitations

Hull is honest about its constraints. These are known trade-offs, not bugs.

**Concurrency.** Hull is single-threaded. SQLite serializes writes. Under real workloads with multiple concurrent users making writes, requests queue. For Hull's target use case — 1-5 simultaneous users running a local tool — this is not a bottleneck. SQLite handles tens of thousands of reads per second and hundreds of writes per second. If your workload needs concurrent writes from thousands of users, Hull is not the right tool (and you probably need a server, not a local app).

**Lua ecosystem.** Lua's ecosystem is smaller than Python's, JavaScript's, or Rust's. There is no equivalent of npm or pip with hundreds of thousands of packages. Hull mitigates this by shipping a batteries-included standard library (routing, auth, RBAC, email, CSV, i18n, FTS, PDF, templates, validation, rate limiting, sessions, CSRF). For modules that don't touch restricted APIs (no `io`, no `os`, no `ffi`), pure Lua libraries work fine. The trade-off is intentional: a small, auditable ecosystem vs. a vast one with supply chain risk.

**Performance.** Keel (Hull's HTTP server) is designed for performance — epoll/kqueue/io_uring, non-blocking I/O, pre-allocated connection pools. But Hull has not been formally benchmarked against other frameworks. For the target workload (local tool, 1-5 users), performance is not a concern. Benchmarks will be published when the platform is mature enough for them to be meaningful.

**Windows and macOS sandbox.** pledge/unveil provides kernel-enforced sandboxing on Linux (SECCOMP BPF + Landlock LSM) and OpenBSD (native syscalls). On macOS and Windows, the kernel sandbox is not available. Hull still runs — the Lua sandbox (restricted stdlib, C-level path validation, network allowlist) provides application-level security on all platforms. The strongest security posture requires Linux or OpenBSD. Windows App Container (available since Windows 8, default-deny for filesystem/network/registry/IPC) and macOS App Sandbox are viable future directions for platform-specific sandbox enforcement. Both are on the roadmap but not yet implemented.

**No mobile.** Hull produces desktop executables. The Lua + SQLite core could be embedded in native mobile apps (iOS/Android) as a library, and the HTML5/JS frontend could run in a mobile webview. This is a future roadmap item, not a current capability.

## Roadmap

Features that are planned but not yet implemented, in rough priority order.

### WASM Workers — Performance-Critical Computation

Lua is fast enough for HTTP handlers, business logic, and database queries. It is not fast enough for numerical computation, image processing, data transformation, or any CPU-bound workload where interpreted code hits a wall.

Hull's answer is **WebAssembly (WASM) workers** — sandboxed, compiled modules that Lua can call for heavy lifting:

```lua
-- Load a WASM module compiled from C, Rust, Zig, or anything
local matrix = require("wasm/matrix_ops.wasm")

-- Call exported functions — runs at near-native speed
local result = matrix.multiply(a, b, rows, cols)
local det = matrix.determinant(data, n)
```

**Why WASM, not native code:**

- **Sandboxed by design.** WASM modules run in a memory-isolated environment. A WASM worker cannot access the filesystem, the network, or the host process's memory. This maintains Hull's security model — the Lua sandbox restricts Lua, and the WASM sandbox restricts compiled code. No `ffi`, no `dlopen`, no escape hatches.
- **Any language.** WASM is a compilation target, not a language. Write performance-critical code in C, C++, Rust, Zig, or anything that compiles to WASM. The developer chooses the language that fits the problem. Hull doesn't care — it runs the `.wasm` binary.
- **Portable.** WASM modules are platform-independent by specification. The same `.wasm` file runs on every OS Hull supports, with no per-platform compilation. This preserves the single-binary story — the WASM modules embed into the APE binary alongside Lua and static assets.
- **Auditable.** WASM modules are small, self-contained, and have a well-defined interface (exported functions with typed signatures). They are significantly easier to audit than native shared libraries.

**Implementation path:** Embed a lightweight WASM runtime (wasm3 or wamr — both are small C libraries designed for embedding, under 100 KB). Expose a `require("wasm/module.wasm")` loader that returns a Lua table with the exported functions. WASM memory is separate from Lua memory. Data is passed via function arguments and return values (numbers, pointers into WASM linear memory for bulk data).

**Use cases:**

- PDF layout engine (compute-heavy text flow and table layout)
- CSV/Excel parsing for large files (millions of rows)
- Cryptographic operations beyond what TweetNaCl provides
- Image resizing/conversion for file upload processing
- Statistical calculations, financial modeling, Monte Carlo simulations
- Custom compression/encoding for domain-specific data formats

### LuaJIT (Alternative Performance Path)

An alternative to WASM for performance-critical Lua code: optionally ship with LuaJIT instead of plain Lua 5.4.

**Pros:** LuaJIT's tracing JIT compiler achieves near-C performance for numerical Lua code. No language boundary — the same Lua code just runs faster. FFI library enables zero-overhead calls to C functions.

**Cons:** LuaJIT is stuck on Lua 5.1 semantics (no integers, no generational GC, no `goto`). It is maintained by a single person (Mike Pall). Platform support is narrower than plain Lua. LuaJIT's FFI bypasses the Lua sandbox — any code using FFI can call arbitrary C functions, which breaks Hull's security model.

**Our position:** WASM workers are the preferred path because they maintain the sandbox. LuaJIT may be offered as an opt-in build flag for developers who need raw Lua speed and accept the security trade-off. Both approaches solve the same underlying problem (interpreted Lua is too slow for numerical work) via different trade-offs.

### Mobile Frontend (iOS/Android)

Hull currently produces desktop executables. The Lua + SQLite core could be embedded as a library in native iOS/Android apps, with the HTML5/JS frontend running in a platform webview (WKWebView on iOS, WebView on Android).

This would mean: write the backend logic once in Lua, share the SQLite database schema, and run the same application on desktop and mobile. The HTML5/JS frontend already works in mobile browsers — the missing piece is packaging.

**Not a priority for v1.0.** Desktop is the focus. Mobile comes after the core platform is stable.

### Windows Sandbox (App Container)

Windows App Container provides kernel-enforced process isolation (default-deny filesystem, network, registry, IPC) for standalone .exe files. It requires a broker/worker pattern — the process launches itself in a sandboxed child via `CreateProcess` with `SECURITY_CAPABILITIES`.

Combined with `SetProcessMitigationPolicy()` (which blocks dynamic code generation, child processes, and unsigned DLL loading), this would bring Windows close to Linux's pledge/unveil enforcement level.

**Implementation complexity is medium.** The Win32 API surface is well-documented but requires careful integration with Cosmopolitan's NT layer.

### macOS App Sandbox

macOS App Sandbox provides filesystem, network, and IPC restrictions for sandboxed processes. Unlike pledge/unveil, it requires an entitlements plist and code signing.

**Feasibility depends on Cosmopolitan APE compatibility with Apple's code signing.** If APE binaries can be code-signed with entitlements, App Sandbox becomes viable. If not, macOS remains on the Lua-sandbox-only security tier.

### Benchmarks

Formal performance benchmarks comparing Hull (Keel + Lua + SQLite) against comparable stacks. Will be published when the platform is mature enough for the numbers to be meaningful and reproducible.

### hull.com Marketplace

A curated directory of Hull applications — both free (AGPL) and commercial. Developers list their apps, users discover and download them. 15% commission on commercial sales. Signature verification built in — every app listed must pass `hull verify`.

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
