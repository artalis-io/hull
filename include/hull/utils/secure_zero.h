/*
 * hull/utils/secure_zero.h - zero secret memory where the optimizer cannot
 * elide it, and free a secret string after zeroing it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HL_UTILS_SECURE_ZERO_H
#define HL_UTILS_SECURE_ZERO_H

#include <stdlib.h>
#include <string.h>

static inline void hl_secure_zero(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

/* free() a NUL-terminated secret (a DSN carrying a password) after zeroing
 * it: a plain free left the password in the freed chunk. */
static inline void hl_secure_free_str(char *s)
{
    if (!s) return;
    hl_secure_zero(s, strlen(s));
    free(s);
}

#endif /* HL_UTILS_SECURE_ZERO_H */
