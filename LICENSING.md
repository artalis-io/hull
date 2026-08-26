# Licensing

Hull is dual-licensed: **AGPL-3.0** for open source use, **commercial license** for
proprietary embedding.

## Open source: AGPL-3.0

Hull is licensed under the [GNU Affero General Public License v3.0](LICENSE).
You may use, modify, and distribute Hull and applications built with `hull build`
under the terms of the AGPL-3.0, including its source-disclosure obligations for
network services that interact with users.

The AGPL-3.0 is the right license when:

- Your project is open source under an AGPL-compatible license.
- You run Hull internally inside your organisation and the source-disclosure
  obligations don't apply.
- You're evaluating Hull or contributing changes back upstream.
- You're publishing a SaaS where you're prepared to make the modified source
  available to users.

## Commercial license

A commercial license is available for organisations that need to embed Hull
in a proprietary product, distribute modified Hull without source disclosure,
or ship a hosted service without AGPL's source-availability obligations.

The commercial license:

- Removes the AGPL-3.0's copyleft and source-disclosure requirements.
- Permits embedding Hull in closed-source products and distributions.
- Is priced by team size and use case. No per-end-user fees.

Contact **<licensing+site@artalis.io>** with a short description of your use case
and we'll send terms.

## Which one do I need?

| Use case | License |
|----------|---------|
| Open source project (AGPL-compatible) | AGPL-3.0 |
| Internal tooling inside your organisation | AGPL-3.0 |
| Public SaaS where you're willing to publish modified Hull source | AGPL-3.0 |
| Closed-source product that embeds Hull | Commercial |
| Distributing modified Hull binaries without source | Commercial |
| Hosted service where you don't want to publish modifications | Commercial |

If you're not sure which applies, email <licensing+site@artalis.io> and describe
what you're building.

## What's in each tier

### Community tier (AGPL-3.0-or-later)

Everything in the public `artalis-io/hull` repo. Includes the full runtime
(Lua + JS), capability layer, kernel sandbox, trust artifacts (SBOM,
Sigstore + Rekor, SLSA attestations, `hull verify-self`, fork playbook),
basic web stdlib (sessions, basic CSRF, basic role-based RBAC, HTTP
server/client, templates, forms, validation, ETag, health, i18n, CSV),
HTMX core profile, basic multipart + attachment helpers, build / sign /
verify / update / doctor commands, agent platform, examples, and
documentation.

The community tier never shrinks. Nothing currently shipped in the OSS
repo will ever move behind a commercial gate. Enterprise-tier features
are *additions*, not *removals*.

### Enterprise tier (commercial license)

Built in the separate `artalis-io/hull-enterprise` repository (private,
commercial-license only). Hull binaries built with enterprise features
have the relevant commercial code statically linked at build time via
the same `HL_ENABLE_*` flag pattern. The OSS Hull binary runs without
any commercial code; community-tier users get a 100% AGPL binary.

Planned / shipped enterprise-tier features:

- **Enterprise SSO middleware.** OIDC, SAML, LDAP, SCIM connectors with
  group / claim mapping, JIT provisioning, and session-bridge helpers.
  Basic session and JWT auth stays in the community tier.
- **Advanced RBAC.** ABAC (attribute-based access control), policy-as-code,
  role hierarchies, dynamic permission resolution, audit-grade decision
  logging. Basic role-based RBAC stays in the community tier.
- **Compliance audit log.** Retention policies, structured export (SIEM,
  S3, Splunk), tamper-evident hash chain, redaction rules, regulator-ready
  retrieval. Basic per-request audit logging stays in the community tier.
- **Hardware-token + KMS signing.** YubiKey (OpenPGP / PIV), AWS KMS, GCP
  KMS, Azure Key Vault integrations for `hull sign-release` and
  `hull sign-platform`. Multi-party signing ceremonies. Software-key
  signing stays in the community tier.

This list grows over time as additional enterprise-only capabilities ship.
See `docs/roadmap_next.md` items tagged `[COMMERCIAL]` for what's planned
but not yet built.

### Hosted tier (subscription)

Hosted services run on infrastructure operated by Artalis. Live in the
separate `artalis-io/hull-hosted-services` repo. Not distributed as
source under any license. Planned services include:

- Hull Build (hosted compile)
- Hull Verify with enterprise SSO + audit log
- Hosted rebuild-attestation service (see `docs/security.md` §7.4)
- Hosted SBOM repository with continuous CVE alerts

## Vendored dependencies

Hull statically links the following third-party libraries (all in `vendor/`).
Every license below is AGPL-3.0-compatible.

| Library | License | Notes |
|---|---|---|
| Keel | MIT | Hull's HTTP server (own project, separate repo). |
| Lua 5.4 | MIT | |
| QuickJS | MIT | |
| SQLite | Public domain | |
| mbedTLS | Apache-2.0 / GPL-2.0 dual | TLS client. |
| TweetNaCl | Public domain | Ed25519 + secretbox. |
| pledge/unveil | ISC | jart's polyfill. |
| log.c | MIT | |
| sh_arena, sh_json | MIT | |
| WAMR | Apache-2.0 | Optional (`HL_ENABLE_WASM=1`, default on). |
| wgpu-native | MPL-2.0 / Apache-2.0 dual | Optional (`HL_ENABLE_GPU=1`, default off). |
| miniz | MIT | gzip compression. |
| Mozilla CA bundle | MPL-2.0 | Embedded for HTTPS without a system store. |
| **TinyCC** | **LGPL-2.1+** | Embedded so `hull build` does not require a separately-installed C compiler. The system linker is still used for the link step. |

**The TinyCC note.** TinyCC is the one LGPL component. LGPL is GPL/AGPL-compatible,
but static linking under LGPL §6 requires either (a) the recipient's ability to
relink against a modified LGPL version, or (b) bundling LGPL source. Since Hull
is AGPL-3.0 and ships full source (including `vendor/tcc/` at the pinned commit),
both conditions are satisfied: the recipient has the source needed to rebuild
hull against a modified TinyCC. This holds equally for the commercial license -
commercial customers receive (or can pull) the same `vendor/tcc/` tree.

## Why dual-license

Hull is built to be useful in two places that don't share a license model.
AGPL-3.0 keeps the open source ecosystem healthy: anything built on top of
Hull that becomes a service has to share what it built. Commercial licensing
keeps Hull viable as a project, funds maintenance, and unblocks the
serious-developer-at-a-real-company use cases that AGPL would otherwise
shut out.

See [`docs/MANIFESTO.md`](docs/MANIFESTO.md) for the longer rationale and
the dead-man's-switch clause that converts the project license to MIT if
Hull stops shipping for 24 consecutive months.
