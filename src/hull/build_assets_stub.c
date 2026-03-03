/*
 * build_assets_stub.c — No-op stubs for platform archive
 *
 * The real build_assets.o contains embedded platform data and is linked
 * into the hull binary. For standalone app binaries (produced by hull
 * build), these functions are never called but must be present to satisfy
 * references from cap_tool.o. This stub provides no-op implementations.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/build_assets.h"

int hl_build_get_platforms(const HlEmbeddedPlatform **out)
{
    (void)out;
    return 0;
}

int hl_build_extract_platform_arch(const char *dir, const char *arch)
{
    (void)dir; (void)arch;
    return -1;
}

int hl_build_extract_platform(const char *dir)
{
    (void)dir;
    return -1;
}

int hl_build_get_template(const char **data, size_t *len)
{
    (void)data; (void)len;
    return -1;
}

int hl_build_get_entry_header(const char **data, size_t *len)
{
    (void)data; (void)len;
    return -1;
}
