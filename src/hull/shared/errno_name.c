/*
 * errno_name.c - the symbolic name of an errno value. See errno_name.h.
 *
 * Every entry is guarded: the set is POSIX, but a cosmo APE or a minimal libc
 * need not define each one, and a name must never be mapped to a number the
 * platform gives another meaning.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "hull/shared/errno_name.h"

#include <errno.h>
#include <stdio.h>

#define HL_ERRNO_CASE(e) case e: return #e;

const char *hl_errno_name(int err, char *buf, size_t n)
{
    switch (err) {
    /* What a dial reports. */
#ifdef ECONNREFUSED
    HL_ERRNO_CASE(ECONNREFUSED)
#endif
#ifdef EHOSTUNREACH
    HL_ERRNO_CASE(EHOSTUNREACH)
#endif
#ifdef ENETUNREACH
    HL_ERRNO_CASE(ENETUNREACH)
#endif
#ifdef ENETDOWN
    HL_ERRNO_CASE(ENETDOWN)
#endif
#ifdef EHOSTDOWN
    HL_ERRNO_CASE(EHOSTDOWN)
#endif
#ifdef ETIMEDOUT
    HL_ERRNO_CASE(ETIMEDOUT)
#endif
#ifdef ECONNRESET
    HL_ERRNO_CASE(ECONNRESET)
#endif
#ifdef ECONNABORTED
    HL_ERRNO_CASE(ECONNABORTED)
#endif
#ifdef EADDRNOTAVAIL
    HL_ERRNO_CASE(EADDRNOTAVAIL)
#endif
#ifdef EADDRINUSE
    HL_ERRNO_CASE(EADDRINUSE)
#endif
#ifdef EAFNOSUPPORT
    HL_ERRNO_CASE(EAFNOSUPPORT)
#endif
#ifdef EPROTONOSUPPORT
    HL_ERRNO_CASE(EPROTONOSUPPORT)
#endif
#ifdef EISCONN
    HL_ERRNO_CASE(EISCONN)
#endif
#ifdef ENOTCONN
    HL_ERRNO_CASE(ENOTCONN)
#endif
#ifdef EPIPE
    HL_ERRNO_CASE(EPIPE)
#endif
#ifdef EALREADY
    HL_ERRNO_CASE(EALREADY)
#endif
#ifdef EINPROGRESS
    HL_ERRNO_CASE(EINPROGRESS)
#endif
    /* What making the socket reports. */
#ifdef EACCES
    HL_ERRNO_CASE(EACCES)
#endif
#ifdef EPERM
    HL_ERRNO_CASE(EPERM)
#endif
#ifdef EMFILE
    HL_ERRNO_CASE(EMFILE)
#endif
#ifdef ENFILE
    HL_ERRNO_CASE(ENFILE)
#endif
#ifdef ENOBUFS
    HL_ERRNO_CASE(ENOBUFS)
#endif
#ifdef ENOMEM
    HL_ERRNO_CASE(ENOMEM)
#endif
#ifdef EINVAL
    HL_ERRNO_CASE(EINVAL)
#endif
#ifdef EBADF
    HL_ERRNO_CASE(EBADF)
#endif
#ifdef EINTR
    HL_ERRNO_CASE(EINTR)
#endif
#ifdef EAGAIN
    HL_ERRNO_CASE(EAGAIN)
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || EWOULDBLOCK != EAGAIN)
    HL_ERRNO_CASE(EWOULDBLOCK)
#endif
#ifdef EIO
    HL_ERRNO_CASE(EIO)
#endif
#ifdef ENOENT
    HL_ERRNO_CASE(ENOENT)
#endif
#ifdef ENOTSOCK
    HL_ERRNO_CASE(ENOTSOCK)
#endif
#ifdef EOPNOTSUPP
    HL_ERRNO_CASE(EOPNOTSUPP)
#endif
#ifdef EMSGSIZE
    HL_ERRNO_CASE(EMSGSIZE)
#endif
#ifdef EFAULT
    HL_ERRNO_CASE(EFAULT)
#endif
    default:
        break;
    }
    if (!buf || n == 0) return "E?";
    snprintf(buf, n, "E%d", err);
    return buf;
}
