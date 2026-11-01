/*
 * mod_encoding.c: Lua binding for the C codecs (utils/hex, utils/base64).
 *
 * Exposes an internal module `hull.encoding._native`. The stdlib
 * hull.encoding is the sole caller: it takes this fast path when it is
 * present and keeps its pure-Lua codecs for everything else - reason strings
 * on refused input, lenient decoding, and a vanilla Lua state (the stdlib test
 * harness) where no native module exists.
 *
 * Why it exists: the pure-Lua codecs build a table of one small string per
 * byte or group, so a 3 MB value costs tens of MB of Lua heap (the heap is
 * capped at 64 MB). Here the result is written straight into one Lua buffer.
 *
 *   hex_encode(bytes)                  -> text
 *   hex_decode(text)                   -> bytes | nil
 *   base64_encode(bytes, url, pad)     -> text
 *   base64_decode(text, url)           -> bytes | nil
 *
 * A decoder returns nil for ANY refused input and says nothing about why;
 * hull.encoding then asks its pure codec, which has the reason. The C and the
 * Lua decoders accept exactly the same inputs (asserted by a differential test
 * in tests/hull/runtime/lua/test_lua.c), so the answer never depends on which
 * one ran. The url alphabet takes no padding, as in hull.encoding.
 *
 * No capability and no authority: pure computation over the caller's bytes,
 * so it needs no manifest gate. Each codec charges its input and output, 1
 * unit per 8 bytes, before the work (enc_charge; the hull:encoding rate in
 * JS): the buffer growth alone charged the output at the allocator's rate
 * and the input scan not at all (audit 18).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"        /* luaopen decls */
#include "../../utils/base64.h"
#include "../../utils/hex.h"
#include "hull/shared/work_charge.h"   /* hl_work_bytes_units */

#include <stdint.h>

/* Charge reading `in_len` bytes and writing `out_len`, before the work; the
 * count hook raises the limit here when the run is over it. */
static void enc_charge(lua_State *L, size_t in_len, size_t out_len)
{
    uint64_t units = hl_work_bytes_units((uint64_t)in_len) +
                     hl_work_bytes_units((uint64_t)out_len);
    lua_hlwork(L, units > SIZE_MAX ? SIZE_MAX : (size_t)units, 0);
}

static int l_hex_encode(lua_State *L)
{
    size_t n;
    const char *in = luaL_checklstring(L, 1, &n);
    if (n > (SIZE_MAX - 1) / 2)
        return luaL_error(L, "encoding: value too large");
    enc_charge(L, n, n * 2);
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, n * 2 + 1);
    if (hl_hex_encode((const uint8_t *)in, n, out, n * 2 + 1) != 0)
        return luaL_error(L, "encoding: hex encode failed");
    luaL_pushresultsize(&b, n * 2);
    return 1;
}

static int l_hex_decode(lua_State *L)
{
    size_t n;
    const char *in = luaL_checklstring(L, 1, &n);
    enc_charge(L, n, n / 2);
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, n / 2 + 1);
    int got = hl_hex_decode(in, n, (uint8_t *)out, n / 2 + 1);
    if (got < 0) {
        lua_pushnil(L);
        return 1;
    }
    luaL_pushresultsize(&b, (size_t)got);
    return 1;
}

static int l_base64_encode(lua_State *L)
{
    size_t n;
    const char *in = luaL_checklstring(L, 1, &n);
    unsigned flags = 0;
    if (lua_toboolean(L, 2)) flags |= HL_BASE64_URL;
    if (!lua_toboolean(L, 3)) flags |= HL_BASE64_NOPAD;
    size_t len = hl_base64_encoded_len(n, flags);
    if (len == 0 && n != 0)
        return luaL_error(L, "encoding: value too large");
    enc_charge(L, n, len);
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, len + 1);
    if (hl_base64_encode(in, n, out, len + 1, flags) < 0)
        return luaL_error(L, "encoding: base64 encode failed");
    luaL_pushresultsize(&b, len);
    return 1;
}

static int l_base64_decode(lua_State *L)
{
    size_t n;
    const char *in = luaL_checklstring(L, 1, &n);
    /* The url alphabet is unpadded, so any '=' is refused there. */
    unsigned flags = lua_toboolean(L, 2) ? (HL_BASE64_URL | HL_BASE64_NOPAD) : 0;
    size_t cap = n / 4 * 3 + 3;
    enc_charge(L, n, cap);
    luaL_Buffer b;
    char *out = luaL_buffinitsize(L, &b, cap);
    size_t got = 0;
    if (hl_base64_decode(in, n, (uint8_t *)out, cap, &got, flags) != 0) {
        lua_pushnil(L);
        return 1;
    }
    luaL_pushresultsize(&b, got);
    return 1;
}

static const luaL_Reg encoding_native[] = {
    { "hex_encode",    l_hex_encode },
    { "hex_decode",    l_hex_decode },
    { "base64_encode", l_base64_encode },
    { "base64_decode", l_base64_decode },
    { NULL, NULL },
};

int luaopen_hull_encoding_native(lua_State *L)
{
    hl_lua_newlib(L, encoding_native);
    return 1;
}
