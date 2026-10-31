## Summary

<!-- What changes and why. For an audit fix: the finding, its attacker
(A1 remote client / A2 planted file / A3 app author) and severity, per
docs/audit_process.md section 1. -->

## Checklist

- [ ] **Regression test.** Each fixed finding has a test named
      `<area>_audit<N>.<case>` that fails on the pre-fix code (I ran it before
      the fix). Non-audit bug fixes: a regression test all the same.
- [ ] **Independent regression review** done by someone other than the author
      (docs/audit_process.md section 2.3): regressions in the fix, false
      refusals of ordinary use, flavor builds, docs drift.
- [ ] **Flavor builds** pass for what this touches (the CI `flavors` matrix;
      cosmo for shared code).
- [ ] **Invariants.** `make check-invariants` passes. A new "a new X must Y"
      rule in CLAUDE.md comes with a lint rule and fixtures in this PR; any
      new allowlist entry has a reason the reviewer agrees with.
- [ ] **Docs updated**: CLAUDE.md, docs/*, CHANGELOG `[Unreleased]`.
- [ ] **Engine / protocol patches land alone** (vendored engines, wire
      clients, transaction or run machinery), and audit batches respect the
      cap (docs/audit_process.md section 3).
- [ ] **No AI attribution lines** in commits or this description (no
      `Co-Authored-By` agent trailers, no "Generated with" lines). Commits are
      signed off (`git commit -s`).
