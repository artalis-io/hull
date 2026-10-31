/*
 * cap/wasm_stream.h - WASM streaming I/O
 *
 * Processes data in chunks through a WASM module using a persistent
 * instance. Input/output can be buffers, files, or callbacks.
 * The WASM module uses hull_process for each chunk; stream metadata
 * (first/last flags, chunk index) is queryable via host_call(0x03).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_WASM_STREAM_H
#define HL_CAP_WASM_STREAM_H

#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm.h"
#include "hull/utils/buffer.h"
#include "hull/cap/fs_resolve.h"   /* HlFsChargeFn */
#include <stddef.h>

/* Forward declarations */
typedef struct HlFsConfig HlFsConfig;
struct HlVfs;

/* ── Input source ─────────────────────────────────────────────────── */

typedef enum {
    HL_STREAM_IN_BUFFER,
    HL_STREAM_IN_FILE,
} HlStreamInKind;

typedef struct {
    HlStreamInKind kind;
    union {
        HlBufferView buffer;
        const char  *path;      /* relative path under app_dir */
    };
} HlStreamInput;

/* ── Output sink ──────────────────────────────────────────────────── */

typedef int (*HlStreamChunkFn)(const void *data, size_t len,
                                uint32_t index, int is_last,
                                void *user_data);

typedef enum {
    HL_STREAM_OUT_BUFFER,
    HL_STREAM_OUT_FILE,
    HL_STREAM_OUT_CALLBACK,
} HlStreamOutKind;

typedef struct {
    HlStreamOutKind kind;
    union {
        struct { void **data; size_t *len; } buffer;
        const char *path;
        struct { HlStreamChunkFn fn; void *user_data; } callback;
    };
} HlStreamOutput;

/* ── Options ──────────────────────────────────────────────────────── */

/* Called before each chunk with that chunk's input length. Nonzero stops
 * the stream (it fails "stopped") before the chunk runs. The bindings charge
 * the run's instruction budget here and check its wall-clock deadline: each
 * chunk has its own gas and timeout_ms, and a stream runs up to UINT32_MAX
 * chunks inside one binding call, where no script instruction is counted
 * (audit 15). */
typedef int (*HlStreamBeforeChunkFn)(size_t chunk_len, void *user_data);

/* Called after a chunk ran with the length of the output the guest produced,
 * BEFORE that output is copied into the buffer, written to the file or handed
 * to the output callback. Nonzero stops the stream (it fails "stopped") and
 * the output is dropped unwritten. The bindings charge the run's budget for
 * the copies and the write here (1 unit per 8 bytes, as fs.write): a guest's
 * output per chunk is up to max_output whatever its input, so the input-sized
 * before_chunk charge let a 512 KiB input write 2 GiB (audit 41). */
typedef int (*HlStreamAfterChunkFn)(size_t out_len, void *user_data);

typedef struct {
    size_t          chunk_size;  /* 0 = default (64 KB) */
    HlWasmCallOpts  call_opts;   /* per-chunk gas, heap, stack */
    HlStreamBeforeChunkFn before_chunk;   /* NULL = none */
    void           *before_chunk_ud;
    HlStreamAfterChunkFn after_chunk;     /* NULL = none (audit 41) */
    void           *after_chunk_ud;
    /* Charge scope for opening a file input / output (cap/fs_resolve.h): the
     * resolution of a deep or mkdir-p output path is charged as fs.write's is
     * (audit 38). NULL = none; a trip fails the stream "budget_exhausted". */
    HlFsChargeFn    fs_charge;
    void           *fs_charge_ud;
    size_t          max_output;  /* buffer-mode output cap (bytes); 0 = none.
                                  * Past it the stream fails "output_too_large". */
} HlStreamOpts;

/* ── Result ───────────────────────────────────────────────────────── */

typedef struct {
    size_t   total_input;
    size_t   total_output;
    uint32_t chunks;
} HlStreamResult;

/* ── API ──────────────────────────────────────────────────────────── */

/**
 * Stream data through a WASM module in chunks.
 *
 * Creates a persistent instance internally, feeds input in chunk_size
 * pieces through hull_process, and routes output to buffer/file/callback.
 * The WASM module can query stream metadata via host_call(0x03, 0, sub).
 *
 * @param cache     Module cache (must be initialized)
 * @param name      Module name (e.g. "compress" -> compute/compress.wasm)
 * @param input     Input source (buffer or file)
 * @param output    Output sink (buffer, file, or callback). NULL = buffer.
 * @param opts      Stream options (chunk_size, per-chunk call opts). NULL for defaults.
 * @param fs_cfg    Filesystem config for file I/O validation (may be NULL if no files)
 * @param app_vfs   App VFS for module loading
 * @param app_dir   App directory for filesystem fallback
 * @param alloc     Allocator (NULL = raw malloc)
 * @param result    Output: stream statistics. May be NULL.
 * @param err_msg   Output: static error string on failure
 * @return 0 on success, negative error code on failure
 */
int hl_cap_wasm_stream(HlWasmCache *cache, const char *name,
                       const HlStreamInput *input,
                       const HlStreamOutput *output,
                       const HlStreamOpts *opts,
                       const HlFsConfig *fs_cfg,
                       const struct HlVfs *app_vfs, const char *app_dir,
                       HlAllocator *alloc,
                       HlStreamResult *result,
                       const char **err_msg);

#endif /* HL_ENABLE_WASM */
#endif /* HL_CAP_WASM_STREAM_H */
