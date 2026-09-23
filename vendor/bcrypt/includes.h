/*
 * includes.h - Hull's shim for the vendored OpenBSD bcrypt sources.
 *
 * bcrypt_pbkdf.c and blowfish.c are OpenSSH-portable files that open with
 * `#include "includes.h"` and then branch on autoconf HAVE_* macros. This
 * header stands in for the autoconf output so BOTH compile UNMODIFIED - Hull's
 * rule for vendored code is that it is not edited, and a file we have not
 * touched is a file that can be re-fetched and diffed against upstream.
 *
 * What the vendored sources need from outside, and where it comes from:
 *
 *   explicit_bzero      a zeroing memset the compiler may not elide. Defined
 *                       here rather than reused from cap/crypto.c, whose
 *                       hull_secure_zero is static to that file (cap/smtp.c
 *                       keeps a local copy for the same reason).
 *   arc4random_buf      bcrypt_pbkdf overwrites its output with RANDOM bytes
 *                       on the error path, deliberately, so a caller that
 *                       ignores the return code gets unusable key material
 *                       rather than a predictable buffer. Wired to Hull's
 *                       CSPRNG; that property is worth keeping, not stubbing.
 *   u_int8_t etc.       BSD spellings, from <sys/types.h> on every target Hull
 *                       builds for (glibc, macOS, and cosmo all provide them).
 *
 * Deliberately NOT defined: HAVE_BCRYPT_PBKDF, HAVE_BLOWFISH_INITSTATE,
 * HAVE_BLOWFISH_EXPAND0STATE, HAVE_BLF_ENC, HAVE_BLF_H. Each of those makes
 * the vendored file compile to NOTHING (it assumes the platform already has
 * the routine), which would leave the KDF silently absent.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_VENDOR_BCRYPT_INCLUDES_H
#define HULL_VENDOR_BCRYPT_INCLUDES_H

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* bcrypt_pbkdf.c guards its <stdlib.h> include on this. */
#define HAVE_STDLIB_H 1

/* The BSD integer spellings the vendored sources use. Mapped to the C99
 * fixed-width types rather than typedef'd, because C gives no way to ask
 * whether a typedef already exists and these names ARE those types by
 * definition. <sys/types.h> is included above, so any platform that does
 * provide them has already done so and the macro is a no-op rename.
 *
 * Needed because a cosmocc build has neither: u_int32_t is a BSD-ism that
 * Cosmopolitan does not carry, and blowfish.c is written entirely in it. */
#define u_int8_t  uint8_t
#define u_int16_t uint16_t
#define u_int32_t uint32_t

/* blowfish.c includes <blf.h> only under HAVE_BLF_H, which upstream's build
 * sets when the PLATFORM supplies one. Ours does not, and the file needs
 * blf_ctx regardless - so the local copy comes in here, after the types it is
 * written in. bcrypt_pbkdf.c reaches its own `#include "blf.h"` fallback. */
#include "blf.h"

/* Zero memory in a way the optimiser may not remove. The volatile pointer is
 * what stops it: writes through a volatile lvalue are observable behaviour, so
 * the store cannot be treated as dead even though nothing reads it back. */
static inline void hull_bcrypt_explicit_bzero(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}
#ifndef explicit_bzero
#define explicit_bzero(p, n) hull_bcrypt_explicit_bzero((p), (n))
#endif

/* Hull's CSPRNG. Declared rather than #included so no Hull header reaches the
 * vendored translation units. */
int hl_cap_crypto_random(void *buf, size_t len);

static inline void hull_bcrypt_arc4random_buf(void *buf, size_t n)
{
    if (hl_cap_crypto_random(buf, n) != 0) {
        /* The point of the call is that a caller ignoring the return code must
         * not receive USABLE key material. Random is better; zeroed is still
         * unusable. Never leave the buffer as it was. */
        hull_bcrypt_explicit_bzero(buf, n);
    }
}
#ifndef arc4random_buf
#define arc4random_buf(b, n) hull_bcrypt_arc4random_buf((b), (n))
#endif

/* OpenBSD's free-after-zeroing. bcrypt_pbkdf uses it on the heap buffer that
 * held the salt, so the pairing matters: zero FIRST, then release. Splitting
 * it into a plain free() would leave the salt readable in freed memory. */
static inline void hull_bcrypt_freezero(void *p, size_t n)
{
    if (p == NULL) return;
    hull_bcrypt_explicit_bzero(p, n);
    free(p);
}
#ifndef freezero
#define freezero(p, n) hull_bcrypt_freezero((p), (n))
#endif

#endif /* HULL_VENDOR_BCRYPT_INCLUDES_H */
