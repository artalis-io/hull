/*
 * errno_name.h - the symbolic name of an errno value.
 *
 * For an error an application branches on: `"EHOSTUNREACH"` is a value a
 * program compares, where strerror's prose is locale text a person reads.
 * libc has no portable name table (glibc's strerrorname_np is glibc 2.32+,
 * nothing on macOS / musl / cosmo), so this is the one small table Hull keeps.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef HULL_SHARED_ERRNO_NAME_H
#define HULL_SHARED_ERRNO_NAME_H

#include <stddef.h>

/* Enough for "E" + any int's decimal digits + NUL. */
#define HL_ERRNO_NAME_MAX 16

/**
 * The symbolic name of `err` ("ECONNREFUSED", "EHOSTUNREACH", ...), as a
 * static string, for the errno values a network dial can produce and the
 * common POSIX ones. A value the table does not know is formatted into `buf`
 * as "E<number>" (buf must hold HL_ERRNO_NAME_MAX bytes) and buf is returned.
 * Never NULL; 0 is "E0".
 */
const char *hl_errno_name(int err, char *buf, size_t n);

#endif
