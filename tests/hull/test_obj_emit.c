/*
 * test_obj_emit.c — unit tests for the compiler-free object emitter (ELF).
 *
 * Emits app_registry ELF objects and validates them structurally by parsing
 * the bytes back: header, sections, the exported hl_app_entries symbol, the
 * ABS64 relocations, and that each entry's len is inline while its name/data
 * pointer slots are zeroed and their reloc addends point at the right bytes
 * in .data.rel.ro. Host-independent (no linking), so it runs on every CI arch;
 * the link+run round-trip lives in the e2e.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/obj_emit.h"
#include "utest.h"

#include <stdint.h>
#include <string.h>

/* ── little-endian readers over the emitted buffer ── */
static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1]<<8); }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)(p[0] | p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24); }
static uint64_t rd64(const unsigned char *p) { uint64_t v=0; for (int i=0;i<8;i++) v |= (uint64_t)p[i]<<(8*i); return v; }

/* Section header accessors (Elf64_Shdr = 64 bytes). */
#define SH_NAME(sh)    rd32((sh)+0)
#define SH_TYPE(sh)    rd32((sh)+4)
#define SH_FLAGS(sh)   rd64((sh)+8)
#define SH_OFFSET(sh)  rd64((sh)+24)
#define SH_SIZE(sh)    rd64((sh)+32)
#define SH_LINK(sh)    rd32((sh)+40)
#define SH_INFO(sh)    rd32((sh)+44)
#define SH_ENTSIZE(sh) rd64((sh)+56)

typedef struct {
    const unsigned char *o;
    uint64_t shoff; uint16_t shnum, shstrndx, machine, etype;
    const unsigned char *shstr;
} Elf;

static const unsigned char *shdr(const Elf *e, int i) { return e->o + e->shoff + (uint64_t)i*64; }

static int find_sec(const Elf *e, const char *name) {
    for (int i = 0; i < e->shnum; i++) {
        const char *nm = (const char *)e->shstr + SH_NAME(shdr(e, i));
        if (strcmp(nm, name) == 0) return i;
    }
    return -1;
}

static void parse(Elf *e, const unsigned char *o) {
    memset(e, 0, sizeof *e);
    e->o = o;
    e->etype    = rd16(o + 16);
    e->machine  = rd16(o + 18);
    e->shoff    = rd64(o + 40);
    e->shnum    = rd16(o + 60);
    e->shstrndx = rd16(o + 62);
    e->shstr    = o + SH_OFFSET(shdr(e, e->shstrndx));
}

/* ── the shared assertion body (one arch) ── */
static void check_emit(int *utest_result,
                       HlObjArch arch, uint16_t want_machine, uint32_t want_rtype) {
    HlEmitEntry ents[] = {
        { "./app",              (const unsigned char *)"return 1", 8 },
        { "templates/base.html",(const unsigned char *)"<html>",   6 },
        { "migrations/001.sql", (const unsigned char *)"",         0 },  /* zero-len ok */
    };
    size_t n = sizeof ents / sizeof ents[0];
    HlObjTarget tgt = { HL_OBJ_ELF, arch, 0, 0 };

    unsigned char *o = NULL; size_t olen = 0;
    ASSERT_EQ(0, hl_obj_emit_app_registry(&tgt, ents, n, &o, &olen));
    ASSERT_TRUE(o != NULL);
    ASSERT_GT(olen, (size_t)64);

    /* ELF ident + type + machine. */
    ASSERT_EQ(0x7f, o[0]); ASSERT_EQ('E', o[1]); ASSERT_EQ('L', o[2]); ASSERT_EQ('F', o[3]);
    ASSERT_EQ(2, o[4]);   /* ELFCLASS64 */
    ASSERT_EQ(1, o[5]);   /* ELFDATA2LSB */
    ASSERT_EQ(1, rd16(o + 16));            /* ET_REL */
    ASSERT_EQ(want_machine, rd16(o + 18)); /* e_machine */

    Elf e; parse(&e, o);
    int i_sec = find_sec(&e, ".data.rel.ro");
    int i_rela   = find_sec(&e, ".rela.data.rel.ro");
    int i_sym    = find_sec(&e, ".symtab");
    int i_str    = find_sec(&e, ".strtab");
    ASSERT_TRUE(i_sec > 0 && i_rela > 0 && i_sym > 0 && i_str > 0);

    /* The entry array holds relocated pointers, so the section must be
     * RELRO-eligible: SHF_ALLOC|SHF_WRITE (0x3), NOT read-only .rodata. A
     * plain-.rodata placement (0x2) puts the RELATIVE reloc targets in a
     * truly RO page and musl's loader SIGSEGVs applying them. */
    ASSERT_EQ((uint64_t)0x3, SH_FLAGS(shdr(&e, i_sec)) & 0x3);

    const unsigned char *sec = o + SH_OFFSET(shdr(&e, i_sec));
    const unsigned char *strtab = o + SH_OFFSET(shdr(&e, i_str));

    /* Find the hl_app_entries global in .symtab. */
    const unsigned char *symtab = o + SH_OFFSET(shdr(&e, i_sym));
    uint64_t nsym = SH_SIZE(shdr(&e, i_sym)) / 24;
    uint64_t array_off = ~0ull, sym_size = 0; int found = 0; int sym_shndx = -1;
    for (uint64_t s = 0; s < nsym; s++) {
        const unsigned char *sy = symtab + s*24;
        const char *nm = (const char *)strtab + rd32(sy + 0);
        if (strcmp(nm, "hl_app_entries") == 0) {
            ASSERT_EQ((uint8_t)((1<<4)|1), sy[4]);   /* GLOBAL | OBJECT */
            sym_shndx = rd16(sy + 6);
            array_off = rd64(sy + 8);
            sym_size  = rd64(sy + 16);
            found = 1;
        }
    }
    ASSERT_EQ(1, found);
    ASSERT_EQ(i_sec, sym_shndx);
    ASSERT_EQ((uint64_t)(n + 1) * 24, sym_size);  /* N real + sentinel */

    /* Array: each real entry's len inline; name/data slots zeroed; sentinel zero. */
    for (size_t k = 0; k < n; k++) {
        const unsigned char *ent = sec + array_off + k*24;
        ASSERT_EQ((uint64_t)0, rd64(ent + 0));   /* name slot (reloc-filled) */
        ASSERT_EQ((uint64_t)0, rd64(ent + 8));   /* data slot */
        ASSERT_EQ(ents[k].len, rd32(ent + 16));  /* len inline */
    }
    const unsigned char *sentinel = sec + array_off + n*24;
    for (int b = 0; b < 24; b++) ASSERT_EQ(0, sentinel[b]);

    /* Relocs: exactly 2N ABS64, targeting the array slots, addends pointing
     * at the entry's name / data bytes in .data.rel.ro. */
    const unsigned char *rela = o + SH_OFFSET(shdr(&e, i_rela));
    uint64_t nrel = SH_SIZE(shdr(&e, i_rela)) / 24;
    ASSERT_EQ((uint64_t)2 * n, nrel);
    ASSERT_EQ((uint64_t)24, SH_ENTSIZE(shdr(&e, i_rela)));
    ASSERT_EQ((uint32_t)i_sym, SH_LINK(shdr(&e, i_rela)));    /* → .symtab */
    ASSERT_EQ((uint32_t)i_sec, SH_INFO(shdr(&e, i_rela))); /* applies to .data.rel.ro */

    for (size_t k = 0; k < n; k++) {
        const unsigned char *rn = rela + (2*k)   * 24;  /* name reloc */
        const unsigned char *rd = rela + (2*k+1) * 24;  /* data reloc */
        ASSERT_EQ(array_off + k*24 + 0, rd64(rn + 0));
        ASSERT_EQ(array_off + k*24 + 8, rd64(rd + 0));
        ASSERT_EQ(want_rtype, (uint32_t)(rd64(rn + 8) & 0xffffffff));
        ASSERT_EQ((uint64_t)1, rd64(rn + 8) >> 32);     /* sym idx 1 = .data.rel.ro */
        uint64_t name_add = rd64(rn + 16);
        uint64_t data_add = rd64(rd + 16);
        ASSERT_STREQ(ents[k].name, (const char *)sec + name_add);
        if (ents[k].len)
            ASSERT_EQ(0, memcmp(sec + data_add, ents[k].data, ents[k].len));
    }

    free(o);
    (void)want_rtype;
}

UTEST(obj_emit, elf_x86_64) {
    check_emit(utest_result, HL_OBJ_X86_64, 62 /*EM_X86_64*/, 1 /*R_X86_64_64*/);
}

UTEST(obj_emit, elf_aarch64) {
    check_emit(utest_result, HL_OBJ_AARCH64, 183 /*EM_AARCH64*/, 257 /*R_AARCH64_ABS64*/);
}

UTEST(obj_emit, empty_is_just_sentinel) {
    HlObjTarget tgt = { HL_OBJ_ELF, HL_OBJ_X86_64, 0, 0 };
    unsigned char *o = NULL; size_t olen = 0;
    ASSERT_EQ(0, hl_obj_emit_app_registry(&tgt, NULL, 0, &o, &olen));
    ASSERT_TRUE(o != NULL);
    Elf e; parse(&e, o);
    int i_sym = find_sec(&e, ".symtab"), i_rela = find_sec(&e, ".rela.data.rel.ro");
    /* one hl_app_entries sym of size 24 (sentinel only), zero relocs */
    ASSERT_EQ((uint64_t)0, SH_SIZE(shdr(&e, i_rela)));
    const unsigned char *strtab = o + SH_OFFSET(shdr(&e, find_sec(&e, ".strtab")));
    const unsigned char *symtab = o + SH_OFFSET(shdr(&e, i_sym));
    uint64_t nsym = SH_SIZE(shdr(&e, i_sym)) / 24; int found = 0;
    for (uint64_t s = 0; s < nsym; s++) {
        if (strcmp((const char *)strtab + rd32(symtab + s*24), "hl_app_entries") == 0) {
            ASSERT_EQ((uint64_t)24, rd64(symtab + s*24 + 16)); found = 1;
        }
    }
    ASSERT_EQ(1, found);
    free(o);
}

UTEST(obj_emit, rejects_unknown_format) {
    HlObjTarget tgt = { (HlObjFormat)99, HL_OBJ_X86_64, 0, 0 };
    unsigned char *o = NULL; size_t olen = 0;
    ASSERT_EQ(-1, hl_obj_emit_app_registry(&tgt, NULL, 0, &o, &olen));
}

/* Mach-O + COFF: the utest cross-checks only the container magic + machine
 * (the full structural cross-check runs against llvm-readobj/otool in the
 * e2e). rd16/rd32 already read little-endian. */
static void check_container(int *utest_result, HlObjFormat fmt, HlObjArch arch,
                            uint32_t magic_le, size_t magic_bytes,
                            size_t machine_off, uint32_t want_machine) {
    HlEmitEntry ents[] = {
        { "./app", (const unsigned char *)"x", 1 },
        { "static/logo.png", (const unsigned char *)"\x89PNG", 4 },
    };
    HlObjTarget tgt = { fmt, arch, 0, 0 };
    unsigned char *o = NULL; size_t olen = 0;
    ASSERT_EQ(0, hl_obj_emit_app_registry(&tgt, ents, 2, &o, &olen));
    ASSERT_TRUE(o != NULL);
    ASSERT_GT(olen, (size_t)32);
    uint32_t got_magic = (magic_bytes == 4) ? rd32(o) : rd16(o);
    ASSERT_EQ(magic_le, got_magic);
    uint32_t got_machine = rd32(o + machine_off);
    if (want_machine <= 0xffff) got_machine = rd16(o + machine_off);
    ASSERT_EQ(want_machine, got_machine);
    free(o);
}

UTEST(obj_emit, macho_x86_64) {   /* MH_MAGIC_64=0xFEEDFACF, cputype@4 */
    check_container(utest_result, HL_OBJ_MACHO, HL_OBJ_X86_64, 0xFEEDFACF, 4, 4, 0x01000007);
}
UTEST(obj_emit, macho_arm64) {
    check_container(utest_result, HL_OBJ_MACHO, HL_OBJ_AARCH64, 0xFEEDFACF, 4, 4, 0x0100000C);
}
UTEST(obj_emit, coff_x86_64) {    /* machine@0 (u16): AMD64=0x8664 */
    check_container(utest_result, HL_OBJ_COFF, HL_OBJ_X86_64, 0x8664, 2, 0, 0x8664);
}
UTEST(obj_emit, coff_arm64) {     /* ARM64=0xAA64 */
    check_container(utest_result, HL_OBJ_COFF, HL_OBJ_AARCH64, 0xAA64, 2, 0, 0xAA64);
}

UTEST_MAIN();
