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
