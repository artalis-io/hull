# Security Policy

Hull is a capability-secure application runtime: its security model (the
capability layer, the kernel sandbox, the signature/attestation chain) is the
product, not an afterthought. We take vulnerability reports seriously and want to
make responsible disclosure easy.

This file is the **vulnerability-reporting** policy. For the security
*architecture* (threat model, sandbox phases, capability-enforcement invariants,
the three-layer signature system) see [`docs/security.md`](docs/security.md).

## Supported versions

Hull is pre-1.0. Security fixes are made against the **latest released version**
and the `main` branch. Older tagged releases are not maintained - if you are on an
older `0.x`, upgrade to the latest release rather than expecting a backport.

| Version | Supported |
|---|---|
| Latest release (current `0.x`) | ✅ Security fixes |
| `main` | ✅ Security fixes |
| Any older tagged release | ❌ Upgrade to latest |

`hull update` moves an installed binary to the latest signed release; see
[`docs/release_signing.md`](docs/release_signing.md).

## Reporting a vulnerability

**Do not open a public GitHub issue, pull request, or discussion for a security
vulnerability.** Public disclosure before a fix is available puts every Hull user
at risk.

Report privately, using either channel:

1. **GitHub private vulnerability reporting (preferred).** On this repository, go
   to the **Security** tab → **Report a vulnerability** ("Advisories" →
   "Report a vulnerability"). This opens a private advisory visible only to you
   and the maintainers.
2. **Email.** If you cannot use GitHub's private reporting, email
   **security@artalis.io**. Encrypt if you can; if you need a key first, send a
   short unencrypted note asking for one (no vulnerability details).

Please include, to the extent you can:

- the affected component (capability layer, sandbox, signature/attestation,
  a specific `hull.*` / `hull_cap_*` surface, the build/release pipeline, …);
- Hull version (`hull version`) and platform (Linux / macOS / cosmo APE);
- a minimal reproduction (an `app.lua` / `app.js`, a manifest, and the exact
  commands), or a proof-of-concept;
- the impact you believe it has (sandbox escape, capability bypass, signature
  forgery, memory-safety defect, DoS, information disclosure, …).

You do **not** need a complete exploit to report - a credible description of a
weakness is enough to start.

## What to expect

| Stage | Target |
|---|---|
| Acknowledgement of your report | within **3 business days** |
| Initial triage + severity assessment | within **7 days** |
| Fix + coordinated disclosure | negotiated with you; typically within **90 days**, faster for actively-exploited or critical issues |

We will keep you updated as we triage and fix, tell you when a fix ships and in
which release, and - unless you prefer to stay anonymous - credit you in the
advisory and the release notes. We practice coordinated disclosure: please give
us a reasonable window to ship a fix before any public write-up.

## Scope

**In scope** - anything that breaks a security boundary Hull promises:

- **Capability escape / bypass** - reaching a resource (fs, db, network, env,
  compute) not granted by the app manifest, or defeating a `hull_cap_*` check
  (path traversal / authorization bypass, SQL injection past the parameterized
  layer, host-allowlist bypass, env-allowlist bypass).
- **Sandbox escape** - escaping the kernel sandbox (pledge/unveil/seatbelt) or
  the Lua/QuickJS interpreter sandbox (`eval`, `io`/`os`, `load*` reachability).
- **Signature / attestation forgery** - defeating the release, platform, or app
  signature layers, or the composed-feature attestation.
- **Memory-safety defects** in the trusted C core (`src/hull/**`, the public
  headers) reachable from app input.
- **Build / release supply-chain** defects that let unverified code into a
  produced binary.

**Out of scope** (report upstream, or not a Hull vulnerability):

- Bugs in vendored dependencies (WAMR, QuickJS, Lua, SQLite, mbedTLS, Keel) that
  do not change Hull's security posture - report to that project (Keel lives at
  [github.com/artalis-io/keel](https://github.com/artalis-io/keel)).
- Findings that require `--no-sandbox`, `--no-ca-bundle`, or an already-compromised
  host / build machine.
- Missing hardening that is documented as a deliberate trade-off (see
  `docs/security.md` and `docs/known_limitations.md`).
- Vulnerabilities in example apps (`examples/`) that do not reflect a runtime
  defect.

When in doubt, report it privately and let us classify - we would rather triage a
non-issue than miss a real one.
