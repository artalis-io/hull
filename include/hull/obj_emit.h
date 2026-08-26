/*
 * obj_emit.h - compiler-free app_registry object emitter
 *
 * Serializes hl_app_entries[] (the app's embedded-file registry) directly
 * as a relocatable object in the target's native format (ELF, Mach-O, and
 * COFF/PE are all implemented - see emit_elf / emit_macho / emit_coff), so
 * `hull build` never needs a C compiler for the data object - only a linker.
 * See docs/compiler_free_build.md.
 *
 * The emitter is HOST-INDEPENDENT: a macOS host emits ELF for a Linux
 * target and vice versa. All fields are written little-endian explicitly;
 * every supported target arch (x86-64, aarch64) is little-endian.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_OBJ_EMIT_H
#define HL_OBJ_EMIT_H

#include <stddef.h>

/* One registry entry to serialize: a name string + the file bytes it maps
 * to. Mirrors HlEntry (include/hull/entry.h) but as pure input (the emitter
 * owns neither the strings nor the blobs). */
typedef struct {
    const char          *name;   /* NUL-terminated entry name (non-NULL) */
    const unsigned char *data;   /* file bytes (may be NULL iff len == 0) */
    unsigned int         len;    /* byte count */
} HlEmitEntry;

typedef enum {
    HL_OBJ_ELF = 0,
    HL_OBJ_MACHO,   /* macOS/Darwin */
    HL_OBJ_COFF     /* Windows PE */
} HlObjFormat;

typedef enum {
    HL_OBJ_X86_64 = 0,
    HL_OBJ_AARCH64
} HlObjArch;

typedef struct {
    HlObjFormat  format;
    HlObjArch    arch;
    /* ELF-only knobs. Cosmo's linker wants specific values; 0/0 is the
     * standard SysV ELF a native ld accepts. */
    unsigned char elf_osabi;   /* EI_OSABI byte; 0 = ELFOSABI_NONE */
    unsigned int  elf_flags;   /* e_flags; 0 = none */
} HlObjTarget;

/*
 * Emit a relocatable object exporting the strong global
 *   const HlEntry hl_app_entries[N+1]
 * (the N real entries followed by a { NULL, NULL, 0 } sentinel), with the
 * name strings and file blobs living in the same read-only section and each
 * entry's name/data pointers as ABS64 relocations into it.
 *
 * On success returns 0 and sets *out to a malloc'd buffer of *out_len bytes
 * (caller frees). On error returns -1 (*out untouched). n may be 0 (emits
 * just the sentinel).
 */
int hl_obj_emit_app_registry(const HlObjTarget *tgt,
                             const HlEmitEntry *entries, size_t n,
                             unsigned char **out, size_t *out_len);

#endif /* HL_OBJ_EMIT_H */
