/*
 * compilers.h - Canonical compiler defaults for hull build toolchain
 *
 * Single source of truth for the default compiler used by hull build,
 * hull verify, and other tool-mode commands.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_COMPILERS_H
#define HL_COMPILERS_H

/* Default compiler for tool-mode commands (hull build, hull verify, etc.).
 * Must be present in the allowed_prefixes[] table in cap/tool.c. */
#define HL_DEFAULT_CC "cosmocc"

#endif /* HL_COMPILERS_H */
