/*
 * app_entries_default.c — Default empty app entries
 *
 * Provides the sentinel-terminated empty array. When building with
 * APP_DIR or via hull build, the generated app_registry.o overrides
 * this with a populated hl_app_entries[] (linker sees the strong
 * symbol first).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/entry.h"

const HlEntry hl_app_entries[] = {
    { 0, 0, 0 }
};
