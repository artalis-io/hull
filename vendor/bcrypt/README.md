# bcrypt_pbkdf + Blowfish (vendored)

The key-derivation half of an OpenSSH private key that carries a passphrase.
`ssh-keygen` protects the private section with `bcrypt` as the KDF and
`aes256-ctr` as the cipher; this is the KDF.

## Provenance

Taken verbatim from [openssh-portable](https://github.com/openssh/openssh-portable),
`openbsd-compat/`, which carries OpenBSD's originals:

| file | origin | licence |
|------|--------|---------|
| `bcrypt_pbkdf.c` | OpenBSD, Ted Unangst | ISC |
| `blowfish.c` | OpenBSD, Niels Provos | BSD-3-Clause |
| `blf.h` | OpenBSD, Niels Provos | BSD-3-Clause |

Refresh with `make fetch-bcrypt`, which pins the SHA-256 of each file and
refuses a mismatch.

## Why vendored rather than written

`bcrypt_pbkdf`'s entire value is agreeing byte-for-byte with what `ssh-keygen`
wrote. A subtly wrong implementation does not fail loudly - it derives a
different key, the decrypt produces garbage, and the failure looks like a
wrong passphrase. Blowfish is also ~4 KiB of constant tables that cannot be
derived from the algorithm description, only transcribed.

Correctness is pinned by a known-answer test against OpenBSD's own regress
vector (`tests/hull/cap/test_bcrypt.c`), not by inspection.

## The two shim headers

`includes.h` and `crypto_api.h` are **Hull's**, not upstream's. They exist so
the two `.c` files compile with no edits at all - a file we have not touched
can be re-fetched and diffed against upstream, which is the point of
vendoring. They supply what upstream's autoconf would have:

- `explicit_bzero` / `freezero` - zeroing that the optimiser may not elide
- `arc4random_buf` - wired to Hull's CSPRNG. `bcrypt_pbkdf` overwrites its
  output with random bytes on the error path so a caller who ignores the
  return code gets unusable material rather than something predictable; that
  property is preserved rather than stubbed
- `u_int8_t` / `u_int16_t` / `u_int32_t` - BSD spellings a cosmocc build lacks
- SHA-512 - routed to Hull's existing `hl_cap_crypto_sha512` rather than
  carrying a second implementation that could disagree with the first

Deliberately NOT defined: `HAVE_BCRYPT_PBKDF`, `HAVE_BLOWFISH_INITSTATE`,
`HAVE_BLOWFISH_EXPAND0STATE`, `HAVE_BLF_ENC`, `HAVE_BLF_H`. Each makes its
file compile to nothing on the assumption the platform already provides the
routine, which here would leave the KDF silently absent.
