/*
 * app_entries_default.c — Default empty app entries
 *
 * Provides the sentinel-terminated empty array. When building with
 * APP_DIR or via hull build, the generated app_registry.o overrides
 * this with a populated array (linker sees the strong symbol first).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

typedef struct {
    const char *name;
    const unsigned char *data;
    unsigned int len;
} HlStdlibEntry;

const HlStdlibEntry hl_app_lua_entries[] = {
    { 0, 0, 0 }
};

const HlStdlibEntry hl_app_template_entries[] = {
    { 0, 0, 0 }
};
