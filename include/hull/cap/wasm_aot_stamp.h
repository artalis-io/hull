/*
 * cap/wasm_aot_stamp.h - provenance stamp of a Hull-patched wamrc (round-6 M2)
 *
 * AOT code is bounded by the wall-clock watchdog only because WAMR patch 0007
 * makes wamrc emit a terminate check at every loop header: WAMR never meters
 * AOT code, so an .aot from any other wamrc runs `(loop br 0)` forever whatever
 * the watchdog does. The same patch has wamrc stamp every AOT file it writes:
 * the target-info section's `reserved` u64 (zero in upstream WAMR, read and
 * ignored by its loader) carries HL_AOT_STAMP_MAGIC in its high 32 bits and
 * HL_AOT_STAMP_VERSION in its low 32. hl_cap_wasm_load refuses an AOT file
 * without the current stamp (falling back to the module's .wasm when there is
 * one), and `hull build` refuses to embed one (stdlib/cli/lua/hull/build.lua
 * checks the same bytes).
 *
 * The stamp is a guard against an UNPATCHED wamrc (a distro or Homebrew
 * build, an older ~/.hull/tools/wamrc), not against a hostile one: AOT is
 * native code, and whoever can hand Hull a crafted .aot can also stamp it.
 * Provenance of embedded AOT code is the app signature's job.
 *
 * Bump HL_AOT_STAMP_VERSION (here AND in patch 0007's HULL_AOT_STAMP_VERSION)
 * whenever the code-generation contract the stamp vouches for changes, so
 * artifacts from an older patched wamrc are refused too.
 *
 * Header-only and WAMR-free, so the tool VM, the runtime and the tests share
 * one parser.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_WASM_AOT_STAMP_H
#define HL_CAP_WASM_AOT_STAMP_H

#include <stddef.h>
#include <stdint.h>

#define HL_AOT_STAMP_MAGIC   0x48554C4Cu   /* "HULL" */
#define HL_AOT_STAMP_VERSION 1u            /* 1: loop-header terminate check (0007) */

/* WAMR's AOT layout (core/config.h AOT_MAGIC_NUMBER, aot_emit_aot_file.c):
 * the magic "\0aot", a u32 version, then - always first, at offset 8 - the
 * target-info section: u32 type 0, u32 size 48, and its fields, with
 * `reserved` at body + 24 (low half: version, high half: magic). */
#define HL_AOT_FILE_MAGIC        0x746F6100u
#define HL_AOT_TARGET_INFO_SIZE  48u
#define HL_AOT_STAMP_OFFSET      (8u + 8u + 24u)

enum {
    HL_AOT_STAMP_OK = 0,
    HL_AOT_STAMP_NOT_AOT,     /* too short, wrong magic, or no target info */
    HL_AOT_STAMP_MISSING,     /* well-formed AOT without a Hull stamp */
    HL_AOT_STAMP_OLD_VERSION, /* stamped by a different patch version */
};

static inline uint32_t hl_aot_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Classify @p buf (the start of an AOT file, @p len bytes of it). Hull's AOT
 * targets (x86_64, aarch64) are little-endian, as is every field read here. */
static inline int hl_aot_stamp_check(const uint8_t *buf, size_t len)
{
    if (!buf || len < 16u + HL_AOT_TARGET_INFO_SIZE)
        return HL_AOT_STAMP_NOT_AOT;
    if (hl_aot_le32(buf) != HL_AOT_FILE_MAGIC)
        return HL_AOT_STAMP_NOT_AOT;
    if (hl_aot_le32(buf + 8) != 0u ||                      /* target info */
        hl_aot_le32(buf + 12) != HL_AOT_TARGET_INFO_SIZE)
        return HL_AOT_STAMP_NOT_AOT;
    uint32_t version = hl_aot_le32(buf + HL_AOT_STAMP_OFFSET);
    uint32_t magic   = hl_aot_le32(buf + HL_AOT_STAMP_OFFSET + 4);
    if (magic != HL_AOT_STAMP_MAGIC)
        return HL_AOT_STAMP_MISSING;
    if (version != HL_AOT_STAMP_VERSION)
        return HL_AOT_STAMP_OLD_VERSION;
    return HL_AOT_STAMP_OK;
}

#endif /* HL_CAP_WASM_AOT_STAMP_H */
