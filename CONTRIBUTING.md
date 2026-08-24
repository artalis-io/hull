# Contributing to Hull

Hull is dual-licensed (AGPL-3.0 + commercial — see [LICENSING.md](LICENSING.md)).
That model only works if a single legal entity holds the rights to every
line of code in the repository. So contributions to Hull come with one
mandatory term: **the contributor assigns copyright in their contribution
to Artalis Ltd. ("Artalis")**.

This document spells out exactly what that means, why it's required, and
how to actually submit a PR.

If you're not comfortable with the assignment, please do not open a PR.
We'd rather you fork (the AGPL guarantees that right) than file work we
can't merge.

---

## TL;DR

1. Read [the contributor terms](#contributor-terms-cct-10) below.
2. Make your change. Match the codebase style. Run `make test` and `make e2e`.
3. Add `Signed-off-by: Your Name <your@email>` to every commit
   (`git commit -s` does this automatically).
4. Open a PR.

A GitHub Action checks every commit in the PR for the `Signed-off-by`
line. PRs missing it cannot be merged.

---

## Contributor terms (CCT 1.0)

By adding `Signed-off-by: Your Name <your@email>` to a commit in this
repository, you certify all of the following:

> **(a) Authority.** You are the original author of the contribution, OR
> you have express written permission from the author and rights-holder
> to submit it under these terms. If your contribution was made in the
> course of employment, you have permission from your employer (or you
> are the employer).
>
> **(b) Copyright assignment.** You irrevocably and unconditionally
> assign to Artalis Ltd., to the maximum extent permitted by applicable
> law, all worldwide copyright in your contribution, including the right
> to enforce that copyright and to relicense the contribution under any
> terms Artalis chooses, including proprietary terms.
>
> **(c) Patent license.** You grant Artalis Ltd. and downstream
> recipients of the Hull software a perpetual, worldwide, royalty-free,
> irrevocable license under any patent claims you can license that
> would be infringed by your contribution alone or in combination with
> Hull.
>
> **(d) Original work.** Your contribution is your own original work
> (or, per clause (a), work you have rights over) and does not include
> third-party code unless that code is explicitly labelled with its
> upstream license and is compatible with both AGPL-3.0 and Artalis's
> commercial license.
>
> **(e) No warranty.** Your contribution is provided "AS IS" without
> warranty of any kind. Artalis is not obligated to use it.
>
> **(f) Moral rights.** To the extent permitted by your jurisdiction's
> law, you waive moral rights in your contribution as against Artalis
> and downstream recipients.

The `Signed-off-by` line is your electronic signature acknowledging
these terms. The terms are versioned ("CCT 1.0"); future revisions
apply only to commits made after the revision is published.

### Why copyright assignment?

Hull's commercial license pays for development. The AGPL alone does not
allow Artalis to relicense contributed code under proprietary terms —
contributors would individually retain copyright on their changes, and
selling a closed-source license to a customer would require permission
from every past contributor. That's an unworkable maintenance burden.

The same reason FSF requires assignment for GNU projects. We chose the
explicit grant rather than the FSF's separate paper form because the
sign-off model is easier to integrate with GitHub.

### Corporate contributors

If your employer owns the code (because you wrote it during work
hours, on work equipment, or under an employment agreement that assigns
work-product to the employer), you need your employer's permission
before you can sign off. That permission can take a few forms:

- A blanket policy letter from your employer authorising open-source
  contributions to Hull, signed by someone with authority. Email a copy
  to `licensing@artalis.io` and we'll keep it on file. You then sign
  off normally.
- A per-PR statement from your employer attached to the PR ("Acme Corp
  agrees to the Hull CCT 1.0 for this contribution"). Heavyweight; only
  use this for one-off contributions.
- Your employer signs off via their own account.

If you're contributing in a personal capacity (clearly outside work,
on your own hardware, no employment-agreement complications), no
employer step is needed.

---

## Code conventions

- C11. Compile with `-Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Werror`.
- `-fstack-protector-strong` for buffer-overflow detection.
- Vendor code (`vendor/**`) uses `-w` to suppress warnings — don't add
  your code there.
- Public functions prefixed `hl_*` (capabilities: `hl_cap_*`, tools:
  `hl_tool_*`, commands: `hl_cmd_*`).
- See `CLAUDE.md` for the architecture map and the conventions section
  near the bottom.

## Required checks before opening a PR

```sh
make            # build clean, zero non-vendor warnings
make test       # 35+ unit suites
make e2e        # 22+ end-to-end tests
make lint       # Lua + JS lint, SDK-header + docs-integrity gates
make cppcheck   # static analysis
```

`make lint` includes `check-docs-integrity` — it verifies that every
`docs/*.md` is catalogued in `docs/README.md`, that Markdown links resolve,
that archived docs are not presented as active specs, and that moved historical
paths cannot silently reappear. If you add or move a doc, update
`docs/README.md` (and, for a historical doc, `docs/archive/README.md`) or this
check will fail.

CI runs all of these (plus ASan, MSan, scan-build, and the Cosmopolitan
build). A green CI run is required to merge.

## Reporting a security vulnerability

Do **not** open a public issue or PR for a security vulnerability. Follow the
private disclosure process in [SECURITY.md](SECURITY.md).

## Sign-off mechanics

To enable automatic sign-off:

```sh
git config --global user.name "Your Name"
git config --global user.email "your@email"
git commit -s -m "your message"
```

The `-s` flag appends:

```
Signed-off-by: Your Name <your@email>
```

If you forget on a commit you've already made:

```sh
git commit --amend -s --no-edit                    # amend most recent
git rebase HEAD~N --signoff                        # last N commits
git filter-branch --msg-filter 'cat && echo "Signed-off-by: Your Name <your@email>"' HEAD~N..HEAD  # rewrite history
```

The DCO check in CI walks every commit on the PR head; one missing
sign-off blocks the merge.

The name and email in the sign-off must match the GitHub-author identity
of the commit (so we can correlate sign-offs with accounts).

---

## What we look for in PRs

- **Tight scope.** One change per PR; don't bundle unrelated cleanups.
- **Tests.** Bug fix → regression test. New capability → unit + e2e tests.
- **Manifest / capability discipline.** If you add a new capability,
  it must go through `hl_cap_*` and be declarable in `manifest.modules`.
  Don't add globals that bypass the capability layer.
- **Cross-runtime parity.** Stdlib helpers visible to both Lua and JS
  must behave identically; add tests in both `test_lua.c` and `test_js.c`.
- **Vendor changes.** Submit upstream first, then bump the vendored
  copy here. Don't fork in-tree.

## What we don't accept

- AI-generated code submitted under sign-off **without disclosure**.
  AI-assisted code is fine, but the sign-off requires you to be able to
  certify authority and originality. If the AI surface is doing most of
  the work, say so in the PR description — we evaluate case by case.
- Patches that are dual-licensed under a license incompatible with our
  commercial relicensing (e.g. GPLv2-only-without-"or later").
- Contributions from contributors on a US OFAC-sanctioned list, or from
  jurisdictions where Artalis is prohibited from doing business.

---

## Questions

Open an issue with the `governance` label, or email
`licensing@artalis.io`. We'll respond within a few business days.
