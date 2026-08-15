/*
 * cap/wasm_data.h — WASM shared data management
 *
 * Shared heap segments, chain management, and option clamping.
 * Split from wasm.h/wasm.c to keep the module manageable.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_WASM_DATA_H
#define HL_CAP_WASM_DATA_H

#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm.h"

/* ── Internal helpers (used by wasm.c) ────────────────────────────── */

/* Rebuild the WAMR shared heap chain from segment array.
 * Must be called under mod->mutex. Drains pool first. */
int hl_wasm_rebuild_chain(HlWasmModule *mod);

/* Free a single segment's resources: destroy the shared-heap descriptor, then
 * (only on success) release Hull-owned backing and clear the slot. Returns 0 when
 * fully reclaimed, or -1 when descriptor destruction failed -- in which case the
 * descriptor, backing, and metadata are all RETAINED (never a dangling descriptor)
 * and the caller must keep the slot + any MappedBuffer pin. */
int hl_wasm_free_segment(HlWasmDataSegment *seg);

/* Free all shared data for a module. Caller holds mod->mutex. Returns 0 when every
 * segment was reclaimed (shared_data freed), or -1 when one or more descriptors
 * could not be destroyed and were retained (shared_data kept alive). */
int hl_wasm_free_shared_data(HlWasmModule *mod);

/* Attach shared data chain to a WASM instance.
 * chain_head must be snapshotted under mod->mutex. */
void hl_wasm_attach_shared_heap(void *inst, void *chain_head);

#endif /* HL_ENABLE_WASM */
#endif /* HL_CAP_WASM_DATA_H */
