/*
 * asset_checksum.c - the fixed-64 constant-time checksum compare shared by
 * `hull feature install` and `hull flavor install`. See asset_checksum.h for the
 * contract and the rationale for keeping this out of the public header tree.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "asset_checksum.h"

int hl_asset_checksum_eq(const char a[64], const char b[64])
{
    unsigned char diff = 0;
    for (int i = 0; i < 64; i++)
        diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}
