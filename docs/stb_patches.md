# stb patches

Hull vendors Sean Barrett's stb_image and stb_image_write in `vendor/stb/`
unmodified: **no Hull patch is applied to them today.** This file exists so the
invariant lint (INV-10, `scripts/check_invariants.py`,
[invariants/README.md](invariants/README.md)) has somewhere to check a future
one against.

`vendor/stb/stb_impl.c` is Hull's own file (the implementation TU and the
size-prefixed, capped allocator both libraries use), not upstream code, so it
is exempt from the marker rule.

**Adding a patch.** Mark every changed hunk with a `HULL PATCH NNNN` comment
(numbered, within 40 lines of each changed line) and add a
`## Patch NNNN - <what>` section here saying what it changes, why, and which
audit or issue it answers. INV-10 fails a marker this file does not list, a
section no marker uses, an unnumbered marker, and an added line in
`vendor/stb/` with no marker near it.

**Upgrading.** Replace the headers wholesale. A diff that changes the
`stb_image - vN.NN` / `stb_image_write - vN.NN` header line is read as an
upgrade, so its hunks are not checked one by one; re-apply every patch listed
here, keeping its marker.
