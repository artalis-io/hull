/*
 * obj_emit.c — compiler-free app_registry object emitter
 *
 * Serializes hl_app_entries[] as a relocatable object directly, so
 * `hull build` needs only a linker, not a C compiler. Host-independent:
 * every field is written little-endian by hand, and each container format
 * is built from self-defined constants (no elf.h or mach-o headers - a macOS
 * host must emit ELF for a Linux target and vice versa).
 *
 * A single planner lays out one section:
 *   [ name strings | file blobs | pad-to-8 | (N+1)-entry array ]
 * Each real entry's name/data pointer slots become ABS64 relocation sites
 * (target offset = where the string/blob lives); `len` is inline. The
 * trailing sentinel entry is 24 zero bytes with no relocations. Three
 * format backends serialize that plan (ELF, Mach-O, COFF/PE); cosmo reuses
 * the ELF backend, parameterized by (osabi, flags).
 *
 * The section holds relocated pointers, so it must be RELRO-eligible, NOT
 * plain read-only: at load time the dynamic linker writes the relocated
 * name/data addresses into the entry array, then (with -z relro) remaps it
 * read-only. ELF expresses this as `.data.rel.ro` (SHF_ALLOC|SHF_WRITE);
 * Mach-O as `__DATA,__const`. Placing it in `.rodata` (SHF_ALLOC only) puts
 * the relocation targets in a truly read-only page - glibc tolerates the
 * resulting text relocations, but musl's stricter loader SIGSEGVs applying
 * them. This is what `cc` does for a `const T[]` with pointer initializers.
 *
 * See docs/compiler_free_build.md.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/obj_emit.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ENTRY_SZ 24   /* sizeof(HlEntry) on LP64: name(8) data(8) len(4) pad(4) */
#define ENT_NAME_OFF 0
#define ENT_DATA_OFF 8

/* ── growable little-endian byte buffer ── */
typedef struct {
    unsigned char *p;
    size_t         len, cap;
    int            oom;
} Buf;

static void buf_reserve(Buf *b, size_t extra) {
    if (b->oom) return;
    if (b->len + extra <= b->cap) return;
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->len + extra) ncap *= 2;
    unsigned char *np = realloc(b->p, ncap);
    if (!np) { b->oom = 1; return; }
    b->p = np; b->cap = ncap;
}
static void buf_put(Buf *b, const void *src, size_t n) {
    buf_reserve(b, n); if (b->oom) return;
    memcpy(b->p + b->len, src, n); b->len += n;
}
static void buf_zero(Buf *b, size_t n) {
    buf_reserve(b, n); if (b->oom) return;
    memset(b->p + b->len, 0, n); b->len += n;
}
static void buf_u8(Buf *b, uint8_t v)  { buf_put(b, &v, 1); }
static void buf_u16(Buf *b, uint16_t v){ unsigned char t[2]; t[0]=(unsigned char)v; t[1]=(unsigned char)(v>>8); buf_put(b, t, 2); }
static void buf_u32(Buf *b, uint32_t v){ unsigned char t[4]; for (int i=0;i<4;i++) t[i]=(unsigned char)(v>>(8*i)); buf_put(b, t, 4); }
static void buf_u64(Buf *b, uint64_t v){ unsigned char t[8]; for (int i=0;i<8;i++) t[i]=(unsigned char)(v>>(8*i)); buf_put(b, t, 8); }
static void buf_pad(Buf *b, size_t align) { while (align && (b->len % align)) buf_u8(b, 0); }
/* Write strlen(str) bytes then zero-pad to a fixed field of `field` bytes. */
static void buf_fixed(Buf *b, const char *str, size_t field) {
    size_t n = strlen(str); if (n > field) n = field;
    buf_put(b, str, n); buf_zero(b, field - n);
}
/* Overwrite an already-appended u64 at absolute offset off (little-endian). */
static void buf_patch64(Buf *b, size_t off, uint64_t v) {
    for (int i = 0; i < 8; i++) b->p[off + i] = (unsigned char)(v >> (8*i));
}
static uint32_t strtab_add(Buf *b, const char *s) {
    uint32_t off = (uint32_t)b->len; buf_put(b, s, strlen(s) + 1); return off;
}

/* ── format-agnostic plan ── */
typedef struct { uint64_t site, target; } Reloc;

typedef struct {
    Buf      sec;          /* names ++ blobs ++ pad8 ++ array; slots zeroed */
    uint64_t array_off;    /* offset of the (N+1)-entry array within sec */
    uint64_t sym_size;     /* (N+1) * ENTRY_SZ */
    Reloc   *relocs;       /* 2N sites, or NULL when n == 0 */
    size_t   nreloc;       /* 2 * n */
} Plan;

static void plan_free(Plan *pl) { free(pl->sec.p); free(pl->relocs); }

static int plan_build(const HlEmitEntry *entries, size_t n, Plan *pl) {
    memset(pl, 0, sizeof *pl);
    uint64_t *name_off = NULL, *blob_off = NULL;
    if (n) {
        name_off = calloc(n, sizeof *name_off);
        blob_off = calloc(n, sizeof *blob_off);
        pl->relocs = calloc(2 * n, sizeof *pl->relocs);
        if (!name_off || !blob_off || !pl->relocs) goto oom;
    }
    for (size_t i = 0; i < n; i++) {
        name_off[i] = pl->sec.len;
        buf_put(&pl->sec, entries[i].name, strlen(entries[i].name) + 1);
    }
    for (size_t i = 0; i < n; i++) {
        blob_off[i] = pl->sec.len;
        if (entries[i].len) buf_put(&pl->sec, entries[i].data, entries[i].len);
    }
    buf_pad(&pl->sec, 8);
    pl->array_off = pl->sec.len;
    for (size_t i = 0; i < n; i++) {
        buf_u64(&pl->sec, 0);                  /* name slot ← reloc */
        buf_u64(&pl->sec, 0);                  /* data slot ← reloc */
        buf_u32(&pl->sec, entries[i].len);     /* len inline */
        buf_u32(&pl->sec, 0);                  /* pad */
    }
    buf_zero(&pl->sec, ENTRY_SZ);              /* { NULL, NULL, 0 } sentinel */
    pl->sym_size = (uint64_t)(n + 1) * ENTRY_SZ;
    for (size_t i = 0; i < n; i++) {
        pl->relocs[2*i]   = (Reloc){ pl->array_off + i*ENTRY_SZ + ENT_NAME_OFF, name_off[i] };
        pl->relocs[2*i+1] = (Reloc){ pl->array_off + i*ENTRY_SZ + ENT_DATA_OFF, blob_off[i] };
    }
    pl->nreloc = 2 * n;
    free(name_off); free(blob_off);
    if (pl->sec.oom) { plan_free(pl); return -1; }
    return 0;
oom:
    free(name_off); free(blob_off); plan_free(pl); return -1;
}

/* ── ELF64 backend ── */
static int emit_elf(const HlObjTarget *tgt, const Plan *pl,
                    unsigned char **out, size_t *out_len) {
    enum { EM_X86_64 = 62, EM_AARCH64 = 183,
           R_X86_64_64 = 1, R_AARCH64_ABS64 = 257 };
    uint16_t machine = (tgt->arch == HL_OBJ_AARCH64) ? EM_AARCH64 : EM_X86_64;
    uint32_t rtype   = (tgt->arch == HL_OBJ_AARCH64) ? R_AARCH64_ABS64 : R_X86_64_64;

    /* .rela.data.rel.ro: explicit addend, targeting the section symbol (1). */
    Buf rela = {0};
    for (size_t i = 0; i < pl->nreloc; i++) {
        buf_u64(&rela, pl->relocs[i].site);
        buf_u64(&rela, ((uint64_t)1 << 32) | rtype);        /* r_info: sym 1 */
        buf_u64(&rela, pl->relocs[i].target);               /* r_addend */
    }
    Buf strtab = {0}; buf_u8(&strtab, 0);
    uint32_t sym_name = strtab_add(&strtab, "hl_app_entries");

    Buf symtab = {0};
    buf_zero(&symtab, 24);                                  /* [0] null */
    buf_u32(&symtab, 0); buf_u8(&symtab, 0x03 /*LOCAL|SECTION*/);
    buf_u8(&symtab, 0);  buf_u16(&symtab, 1); buf_u64(&symtab, 0); buf_u64(&symtab, 0);
    buf_u32(&symtab, sym_name); buf_u8(&symtab, 0x11 /*GLOBAL|OBJECT*/);
    buf_u8(&symtab, 0);  buf_u16(&symtab, 1);
    buf_u64(&symtab, pl->array_off); buf_u64(&symtab, pl->sym_size);

    Buf shstr = {0}; buf_u8(&shstr, 0);
    uint32_t nm_sec    = strtab_add(&shstr, ".data.rel.ro");
    uint32_t nm_rela   = strtab_add(&shstr, ".rela.data.rel.ro");
    uint32_t nm_symtab = strtab_add(&shstr, ".symtab");
    uint32_t nm_strtab = strtab_add(&shstr, ".strtab");
    uint32_t nm_shstr  = strtab_add(&shstr, ".shstrtab");

    if (rela.oom || strtab.oom || symtab.oom || shstr.oom) {
        free(rela.p); free(strtab.p); free(symtab.p); free(shstr.p); return -1;
    }

    Buf o = {0};
    buf_u8(&o, 0x7f); buf_u8(&o, 'E'); buf_u8(&o, 'L'); buf_u8(&o, 'F');
    buf_u8(&o, 2 /*ELFCLASS64*/); buf_u8(&o, 1 /*LSB*/); buf_u8(&o, 1 /*EV_CURRENT*/);
    buf_u8(&o, tgt->elf_osabi); buf_u8(&o, 0);
    for (int i = 0; i < 7; i++) buf_u8(&o, 0);
    buf_u16(&o, 1 /*ET_REL*/); buf_u16(&o, machine); buf_u32(&o, 1);
    buf_u64(&o, 0); buf_u64(&o, 0);
    size_t shoff_field = o.len; buf_u64(&o, 0 /*e_shoff patched*/);
    buf_u32(&o, tgt->elf_flags);
    buf_u16(&o, 64); buf_u16(&o, 0); buf_u16(&o, 0);
    buf_u16(&o, 64); buf_u16(&o, 6); buf_u16(&o, 5);

    uint64_t off_sec, off_rela, off_symtab, off_strtab, off_shstr, off_shdrs;
    buf_pad(&o, 8); off_sec = o.len; buf_put(&o, pl->sec.p, pl->sec.len);
    buf_pad(&o, 8); off_rela   = o.len; buf_put(&o, rela.p, rela.len);
    buf_pad(&o, 8); off_symtab = o.len; buf_put(&o, symtab.p, symtab.len);
    off_strtab = o.len; buf_put(&o, strtab.p, strtab.len);
    off_shstr  = o.len; buf_put(&o, shstr.p, shstr.len);
    buf_pad(&o, 8); off_shdrs = o.len;

    /* Elf64_Shdr: name,type,flags,addr,offset,size,link,info,align,entsize */
    #define SHDR(nm,ty,fl,of,sz,lk,inf,al,es) do { \
        buf_u32(&o,(nm)); buf_u32(&o,(ty)); buf_u64(&o,(fl)); buf_u64(&o,0); \
        buf_u64(&o,(of)); buf_u64(&o,(sz)); buf_u32(&o,(lk)); buf_u32(&o,(inf)); \
        buf_u64(&o,(al)); buf_u64(&o,(es)); } while (0)
    SHDR(0,0,0,0,0,0,0,0,0);
    SHDR(nm_sec, 1 /*PROGBITS*/, 0x3 /*ALLOC|WRITE → RELRO*/, off_sec, pl->sec.len, 0,0, 8, 0);
    SHDR(nm_rela, 4 /*RELA*/, 0x40 /*INFO_LINK*/, off_rela, rela.len, 3,1, 8, 24);
    SHDR(nm_symtab, 2 /*SYMTAB*/, 0, off_symtab, symtab.len, 4, 2, 8, 24);
    SHDR(nm_strtab, 3 /*STRTAB*/, 0, off_strtab, strtab.len, 0,0, 1, 0);
    SHDR(nm_shstr, 3 /*STRTAB*/, 0, off_shstr, shstr.len, 0,0, 1, 0);
    #undef SHDR

    free(rela.p); free(strtab.p); free(symtab.p); free(shstr.p);
    if (o.oom) { free(o.p); return -1; }
    buf_patch64(&o, shoff_field, off_shdrs);
    *out = o.p; *out_len = o.len; return 0;
}

/* ── Mach-O 64 backend (MH_OBJECT) ── */
static int emit_macho(const HlObjTarget *tgt, const Plan *pl,
                      unsigned char **out, size_t *out_len) {
    uint32_t cputype = (tgt->arch == HL_OBJ_AARCH64) ? 0x0100000C : 0x01000007;
    uint32_t cpusub  = (tgt->arch == HL_OBJ_AARCH64) ? 0 : 3;
    /* X86_64_RELOC_UNSIGNED and ARM64_RELOC_UNSIGNED are both type 0. */

    /* Section data with in-place addends (Mach-O has no explicit r_addend). */
    Buf sec = {0}; buf_put(&sec, pl->sec.p, pl->sec.len);
    if (sec.oom) return -1;
    for (size_t i = 0; i < pl->nreloc; i++)
        buf_patch64(&sec, pl->relocs[i].site, pl->relocs[i].target);

    uint32_t nreloc = (uint32_t)pl->nreloc;
    /* Layout: header+loadcmds (208) | secdata | relocs | symtab | strtab */
    uint64_t off_sec = 208;                          /* 208 % 8 == 0 */
    uint64_t off_rel = off_sec + sec.len; off_rel = (off_rel + 3) & ~3ull;
    uint64_t off_sym = off_rel + (uint64_t)nreloc * 8; off_sym = (off_sym + 7) & ~7ull;
    uint64_t off_str = off_sym + 16;                 /* one nlist_64 */
    uint32_t strsize = 1 + 16;                       /* "\0" + "_hl_app_entries\0" */

    Buf o = {0};
    /* mach_header_64 */
    buf_u32(&o, 0xFEEDFACF); buf_u32(&o, cputype); buf_u32(&o, cpusub);
    buf_u32(&o, 1 /*MH_OBJECT*/); buf_u32(&o, 2 /*ncmds*/); buf_u32(&o, 176 /*sizeofcmds*/);
    buf_u32(&o, 0 /*flags*/); buf_u32(&o, 0 /*reserved*/);
    /* LC_SEGMENT_64 (one __DATA,__const section) */
    buf_u32(&o, 0x19); buf_u32(&o, 152 /*cmdsize*/);
    buf_fixed(&o, "", 16 /*segname empty for MH_OBJECT*/);
    buf_u64(&o, 0 /*vmaddr*/); buf_u64(&o, sec.len /*vmsize*/);
    buf_u64(&o, off_sec /*fileoff*/); buf_u64(&o, sec.len /*filesize*/);
    buf_u32(&o, 7 /*maxprot*/); buf_u32(&o, 7 /*initprot*/);
    buf_u32(&o, 1 /*nsects*/); buf_u32(&o, 0 /*flags*/);
    /* section_64 */
    buf_fixed(&o, "__const", 16); buf_fixed(&o, "__DATA", 16);
    buf_u64(&o, 0 /*addr*/); buf_u64(&o, sec.len /*size*/);
    buf_u32(&o, (uint32_t)off_sec /*offset*/); buf_u32(&o, 3 /*align=2^3*/);
    buf_u32(&o, (uint32_t)off_rel /*reloff*/); buf_u32(&o, nreloc /*nreloc*/);
    buf_u32(&o, 0 /*S_REGULAR*/);
    buf_u32(&o, 0); buf_u32(&o, 0); buf_u32(&o, 0); /*reserved1..3*/
    /* LC_SYMTAB */
    buf_u32(&o, 0x2); buf_u32(&o, 24 /*cmdsize*/);
    buf_u32(&o, (uint32_t)off_sym); buf_u32(&o, 1 /*nsyms*/);
    buf_u32(&o, (uint32_t)off_str); buf_u32(&o, strsize);

    /* section data */
    buf_pad(&o, 8); buf_put(&o, sec.p, sec.len);
    /* relocations (relocation_info: r_address + packed word) */
    buf_pad(&o, 4);
    for (size_t i = 0; i < pl->nreloc; i++) {
        buf_u32(&o, (uint32_t)pl->relocs[i].site);
        /* symbolnum(24)=1 (section) | pcrel(1)=0 | length(2)=3 | extern(1)=0 | type(4)=0 */
        buf_u32(&o, 1u | (3u << 25));
    }
    /* symbol table: one nlist_64 for _hl_app_entries */
    buf_pad(&o, 8);
    buf_u32(&o, 1 /*n_strx*/); buf_u8(&o, 0x0f /*N_SECT|N_EXT*/);
    buf_u8(&o, 1 /*n_sect*/); buf_u16(&o, 0 /*n_desc*/); buf_u64(&o, pl->array_off);
    /* string table: [0]='\0' then "_hl_app_entries\0" */
    buf_u8(&o, 0); buf_put(&o, "_hl_app_entries", 16);

    free(sec.p);
    if (o.oom) { free(o.p); return -1; }
    *out = o.p; *out_len = o.len; return 0;
}

/* ── COFF/PE backend ── */
static int emit_coff(const HlObjTarget *tgt, const Plan *pl,
                     unsigned char **out, size_t *out_len) {
    uint16_t machine = (tgt->arch == HL_OBJ_AARCH64) ? 0xAA64 : 0x8664;
    /* ADDR64 differs by arch: AMD64=0x0001, ARM64=0x000E (0x0001 on ARM64
     * is ADDR32, which would truncate a 64-bit pointer). */
    uint16_t addr64 = (tgt->arch == HL_OBJ_AARCH64) ? 0x000E : 0x0001;

    Buf sec = {0}; buf_put(&sec, pl->sec.p, pl->sec.len);
    if (sec.oom) return -1;
    for (size_t i = 0; i < pl->nreloc; i++)
        buf_patch64(&sec, pl->relocs[i].site, pl->relocs[i].target);

    uint64_t nreloc = pl->nreloc;
    int ovfl = nreloc > 0xffff;                     /* IMAGE_SCN_LNK_NRELOC_OVFL */
    uint64_t reloc_recs = nreloc + (ovfl ? 1 : 0);
    uint32_t nsyms = 3;                              /* .rdata sym + aux + hl_app_entries */

    uint64_t off_hdrs = 20 + 40;                    /* COFF header + 1 section header */
    uint64_t off_sec  = (off_hdrs + 7) & ~7ull;
    uint64_t off_rel  = off_sec + sec.len; off_rel = (off_rel + 3) & ~3ull;
    uint64_t off_sym  = off_rel + reloc_recs * 10;  /* IMAGE_RELOCATION = 10 bytes */
    /* string table immediately follows the symbol table. */

    Buf o = {0};
    /* COFF header */
    buf_u16(&o, machine); buf_u16(&o, 1 /*NumberOfSections*/);
    buf_u32(&o, 0 /*TimeDateStamp*/); buf_u32(&o, (uint32_t)off_sym);
    buf_u32(&o, nsyms); buf_u16(&o, 0 /*SizeOfOptionalHeader*/); buf_u16(&o, 0 /*Characteristics*/);
    /* section header: .rdata */
    uint32_t scn_char = 0x40 /*CNT_INITIALIZED_DATA*/ | 0x00400000 /*ALIGN_8BYTES*/
                      | 0x40000000 /*MEM_READ*/;
    if (ovfl) scn_char |= 0x01000000;               /* LNK_NRELOC_OVFL */
    buf_fixed(&o, ".rdata", 8);
    buf_u32(&o, 0 /*VirtualSize*/); buf_u32(&o, 0 /*VirtualAddress*/);
    buf_u32(&o, (uint32_t)sec.len /*SizeOfRawData*/); buf_u32(&o, (uint32_t)off_sec /*PtrToRawData*/);
    buf_u32(&o, (uint32_t)off_rel /*PtrToRelocations*/); buf_u32(&o, 0 /*PtrToLinenumbers*/);
    buf_u16(&o, (uint16_t)(ovfl ? 0xffff : nreloc)); buf_u16(&o, 0 /*NumberOfLinenumbers*/);
    buf_u32(&o, scn_char);

    /* section data */
    buf_pad(&o, 8); buf_put(&o, sec.p, sec.len);
    /* relocations (IMAGE_RELOCATION = VA(4) + SymIdx(4) + Type(2), packed 10B) */
    buf_pad(&o, 4);
    if (ovfl) { buf_u32(&o, (uint32_t)(nreloc + 1)); buf_u32(&o, 0); buf_u16(&o, 0); }
    for (size_t i = 0; i < pl->nreloc; i++) {
        buf_u32(&o, (uint32_t)pl->relocs[i].site);
        buf_u32(&o, 0 /*SymbolTableIndex → .rdata section symbol*/);
        buf_u16(&o, addr64);
    }
    /* symbol table (IMAGE_SYMBOL = 18 bytes, packed) */
    /* [0] .rdata static section symbol (+1 aux) */
    buf_fixed(&o, ".rdata", 8);
    buf_u32(&o, 0 /*Value*/); buf_u16(&o, 1 /*SectionNumber*/);
    buf_u16(&o, 0 /*Type*/); buf_u8(&o, 3 /*IMAGE_SYM_CLASS_STATIC*/); buf_u8(&o, 1 /*NumberOfAuxSymbols*/);
    /* [1] aux section definition (18 bytes) */
    buf_u32(&o, (uint32_t)sec.len /*Length*/);
    buf_u16(&o, (uint16_t)(ovfl ? 0xffff : nreloc) /*NumberOfRelocations*/);
    buf_u16(&o, 0 /*NumberOfLinenumbers*/); buf_u32(&o, 0 /*CheckSum*/);
    buf_u16(&o, 0 /*Number*/); buf_u8(&o, 0 /*Selection*/);
    buf_u8(&o, 0); buf_u16(&o, 0); /*unused pad to 18*/
    /* [2] hl_app_entries external (name > 8 chars → string table) */
    buf_u32(&o, 0 /*Name.Zeroes*/); buf_u32(&o, 4 /*Name.Offset (after 4-byte size)*/);
    buf_u32(&o, (uint32_t)pl->array_off /*Value*/); buf_u16(&o, 1 /*SectionNumber*/);
    buf_u16(&o, 0 /*Type*/); buf_u8(&o, 2 /*IMAGE_SYM_CLASS_EXTERNAL*/); buf_u8(&o, 0 /*aux*/);
    /* string table: size prefix + "hl_app_entries\0" */
    buf_u32(&o, 4 + 15); buf_put(&o, "hl_app_entries", 15);

    free(sec.p);
    if (o.oom) { free(o.p); return -1; }
    *out = o.p; *out_len = o.len; return 0;
}

int hl_obj_emit_app_registry(const HlObjTarget *tgt,
                             const HlEmitEntry *entries, size_t n,
                             unsigned char **out, size_t *out_len) {
    if (!tgt || !out || !out_len || (n && !entries)) return -1;
    Plan pl;
    if (plan_build(entries, n, &pl)) return -1;
    int rc;
    switch (tgt->format) {
        case HL_OBJ_ELF:   rc = emit_elf(tgt, &pl, out, out_len);   break;
        case HL_OBJ_MACHO: rc = emit_macho(tgt, &pl, out, out_len); break;
        case HL_OBJ_COFF:  rc = emit_coff(tgt, &pl, out, out_len);  break;
        default:           rc = -1;                                 break;
    }
    plan_free(&pl);
    return rc;
}
