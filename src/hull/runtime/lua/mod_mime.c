/* mod_mime.c - hull.mime module: content-MIME sniffing
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "mod_buffer.h"
#include "hull/cap/mime.h"

/* ════════════════════════════════════════════════════════════════════
 * hull.mime module
 *
 * mime.sniff(bytes) -> string | nil
 *
 * Thin binding around hl_cap_mime_sniff. Returns the sniffed MIME
 * type string for the given byte buffer, or nil if no signature
 * matches. The caller typically passes the first chunk of an upload
 * (or the whole buffer if short). The sniffer reads up to a few
 * hundred bytes of magic and falls through to nil for unrecognised
 * content; that's the signal to either reject the upload or fall
 * back to the declared MIME from the Content-Type header.
 *
 * Pure capability - no fs/network/db access. No required_caps.
 * ════════════════════════════════════════════════════════════════════ */

static int lua_mime_sniff(lua_State *L)
{
    size_t len = 0;
    const char *bytes = luaL_checklstring(L, 1, &len);
    const char *mime = hl_cap_mime_sniff((const uint8_t *)bytes, len);
    if (mime) lua_pushstring(L, mime);
    else      lua_pushnil(L);
    return 1;
}

static const luaL_Reg mime_funcs[] = {
    {"sniff", lua_mime_sniff},
    {NULL, NULL}
};

int luaopen_hull_mime(lua_State *L)
{
    luaL_newlib(L, mime_funcs);
    return 1;
}
