# mbedTLS patches

Hull vendors mbedTLS in `vendor/mbedtls/` unmodified: **no Hull patch is
applied to it today.** This file exists so the invariant lint (INV-10,
`scripts/check_invariants.py`, [invariants/README.md](invariants/README.md))
has somewhere to check a future one against.

`vendor/mbedtls/hull_config.h` is Hull's own configuration file
(`MBEDTLS_CONFIG_FILE`), not upstream code, so it is exempt from the marker
rule.

**Adding a patch.** Mark every changed hunk with a `HULL PATCH NNNN` comment
(numbered, within 40 lines of each changed line) and add a
`## Patch NNNN - <what>` section here saying what it changes, why, and which
audit or issue it answers. INV-10 fails a marker this file does not list, a
section no marker uses, an unnumbered marker, and an added line in
`vendor/mbedtls/` with no marker near it.

**Upgrading.** Replace the tree wholesale. A diff that changes
`#define MBEDTLS_VERSION_STRING` is read as an upgrade, so its hunks are not
checked one by one; re-apply every patch listed here, keeping its marker.
