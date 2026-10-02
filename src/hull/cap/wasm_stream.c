/*
 * hull_cap_wasm_stream.c - WASM streaming I/O
 *
 * Processes data in chunks through a WASM persistent instance.
 * See cap/wasm_stream.h for the public API.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_WASM

#include "hull/cap/wasm_stream.h"
#include "hull/cap/wasm.h"
#include "hull/cap/fs.h"
#include "hull/utils/alloc.h"
#include "hull/limits/wasm.h"
#include "hull/vfs.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Internal helpers ──────────────────────────────────────────────── */

static size_t resolve_chunk_size(const HlStreamOpts *opts)
{
    size_t cs = (opts && opts->chunk_size > 0)
                ? opts->chunk_size
                : HL_WASM_STREAM_DEFAULT_CHUNK;
    if (cs < HL_WASM_STREAM_MIN_CHUNK) cs = HL_WASM_STREAM_MIN_CHUNK;
    if (cs > HL_WASM_STREAM_MAX_CHUNK) cs = HL_WASM_STREAM_MAX_CHUNK;
    return cs;
}

static int grow_output_buf(uint8_t **buf, size_t *cap, size_t needed,
                            HlAllocator *alloc)
{
    size_t new_cap = *cap;
    while (new_cap < needed) {
        size_t doubled = new_cap * 2;
        if (doubled < new_cap) return -1; /* overflow */
        new_cap = doubled;
    }
    if (new_cap == *cap) return 0;

    uint8_t *nb = hl_alloc_realloc(alloc, *buf, *cap, new_cap);
    if (!nb) return -1;
    *buf = nb;
    *cap = new_cap;
    return 0;
}

/* ── Public API ────────────────────────────────────────────────────── */

int hl_cap_wasm_stream(HlWasmCache *cache, const char *name,
                       const HlStreamInput *input,
                       const HlStreamOutput *output,
                       const HlStreamOpts *opts,
                       const HlFsConfig *fs_cfg,
                       const struct HlVfs *app_vfs, const char *app_dir,
                       HlAllocator *alloc,
                       HlStreamResult *result,
                       const char **err_msg)
{
    if (!cache || !name || !input) {
        if (err_msg) *err_msg = "invalid_args";
        return HL_WASM_ERR_INTERNAL;
    }

    size_t chunk_size = resolve_chunk_size(opts);

    /* ── Resolve input ──────────────────────────────────────────── */
    const uint8_t *in_data = NULL;
    size_t in_total = 0;
    FILE *in_file = NULL;
    uint8_t *in_buf = NULL; /* heap buffer for file reads */

    if (input->kind == HL_STREAM_IN_BUFFER) {
        in_data = (const uint8_t *)input->buffer.data;
        in_total = input->buffer.len;
    } else if (input->kind == HL_STREAM_IN_FILE) {
        if (!fs_cfg || !input->path) {
            if (err_msg) *err_msg = "missing_fs_config";
            return HL_WASM_ERR_INTERNAL;
        }
        /* Opened as fs.read opens: through the fs.read grants and the
         * descriptor-relative resolver. A check that the path lay under the
         * app directory, then a plain fopen, ignored the grants (an app with
         * no fs.read could stream any file in its directory), left the path
         * open to a swap between the check and the open, and followed a FIFO. */
        const char *ferr = NULL;
        int in_fd = hl_cap_fs_open_read_fd(fs_cfg, input->path, &ferr);
        if (in_fd < 0) {
            if (err_msg) *err_msg = ferr ? ferr : "permission";
            return HL_WASM_ERR_INTERNAL;
        }
        in_file = fdopen(in_fd, "rb");
        if (!in_file) {
            close(in_fd);
            if (err_msg) *err_msg = "open_input_failed";
            return HL_WASM_ERR_INTERNAL;
        }
        if (fseek(in_file, 0, SEEK_END) != 0) {
            fclose(in_file);
            if (err_msg) *err_msg = "open_input_failed";
            return HL_WASM_ERR_INTERNAL;
        }
        long ftell_res = ftell(in_file);
        if (ftell_res < 0) {
            fclose(in_file);
            if (err_msg) *err_msg = "open_input_failed";
            return HL_WASM_ERR_INTERNAL;
        }
        in_total = (size_t)ftell_res;
        if (fseek(in_file, 0, SEEK_SET) != 0) {
            fclose(in_file);
            if (err_msg) *err_msg = "open_input_failed";
            return HL_WASM_ERR_INTERNAL;
        }
        in_buf = hl_alloc_malloc(alloc, chunk_size);
        if (!in_buf) {
            fclose(in_file);
            if (err_msg) *err_msg = "alloc_failed";
            return HL_WASM_ERR_INTERNAL;
        }
    }

    /* ── Open output file if needed ────────────────────────────── */
    FILE *out_file = NULL;
    if (output && output->kind == HL_STREAM_OUT_FILE) {
        if (!fs_cfg || !output->path) {
            if (in_file) fclose(in_file);
            if (in_buf) hl_alloc_free(alloc, in_buf, chunk_size);
            if (err_msg) *err_msg = "missing_fs_config";
            return HL_WASM_ERR_INTERNAL;
        }
        /* Opened as fs.write opens (see the input above): the fs.write
         * grants decide, not "somewhere under the app directory". */
        const char *ferr = NULL;
        int out_fd = hl_cap_fs_open_write_fd(fs_cfg, output->path, &ferr);
        if (out_fd < 0) {
            if (in_file) fclose(in_file);
            if (in_buf) hl_alloc_free(alloc, in_buf, chunk_size);
            if (err_msg) *err_msg = ferr ? ferr : "permission";
            return HL_WASM_ERR_INTERNAL;
        }
        out_file = fdopen(out_fd, "wb");
        if (!out_file) {
            close(out_fd);
            if (in_file) fclose(in_file);
            if (in_buf) hl_alloc_free(alloc, in_buf, chunk_size);
            if (err_msg) *err_msg = "open_output_failed";
            return HL_WASM_ERR_INTERNAL;
        }
    }

    /* ── Allocate output buffer for BUFFER mode ────────────────── */
    uint8_t *out_buf = NULL;
    size_t out_buf_len = 0;
    size_t out_buf_cap = 0;
    if (!output || output->kind == HL_STREAM_OUT_BUFFER) {
        out_buf_cap = chunk_size;
        out_buf = hl_alloc_malloc(alloc, out_buf_cap);
        if (!out_buf) {
            if (in_file) fclose(in_file);
            if (in_buf) hl_alloc_free(alloc, in_buf, chunk_size);
            if (out_file) fclose(out_file);
            if (err_msg) *err_msg = "alloc_failed";
            return HL_WASM_ERR_INTERNAL;
        }
    }

    /* ── Create persistent instance ────────────────────────────── */
    HlWasmCallOpts inst_opts = opts ? opts->call_opts : (HlWasmCallOpts){0};
    HlWasmInstance *inst = hl_cap_wasm_instance_create(
        cache, name, &inst_opts, app_vfs, app_dir, alloc, err_msg);
    if (!inst) {
        if (in_file) fclose(in_file);
        if (in_buf) hl_alloc_free(alloc, in_buf, chunk_size);
        if (out_file) fclose(out_file);
        if (out_buf) hl_alloc_free(alloc, out_buf, out_buf_cap);
        return HL_WASM_ERR_INTERNAL;
    }

    /* ── Stream loop ───────────────────────────────────────────── */
    size_t offset = 0;
    uint32_t chunk_index = 0;
    int rc = HL_WASM_OK;
    size_t total_out = 0;

    /* At least one iteration even for empty input (first + last) */
    size_t num_chunks = (in_total == 0)
                        ? 1
                        : (in_total + chunk_size - 1) / chunk_size;

    if (num_chunks > UINT32_MAX) {
        if (err_msg) *err_msg = "input_too_large";
        rc = HL_WASM_ERR_INPUT;
        goto cleanup;
    }

    for (size_t ci = 0; ci < num_chunks; ci++) {
        size_t this_len = 0;
        const void *this_data = NULL;

        if (in_total > 0) {
            this_len = (offset + chunk_size <= in_total)
                       ? chunk_size
                       : (in_total - offset);
            if (input->kind == HL_STREAM_IN_BUFFER) {
                this_data = in_data + offset;
            } else {
                size_t rd = fread(in_buf, 1, this_len, in_file);
                if (rd != this_len) {
                    rc = HL_WASM_ERR_INTERNAL;
                    if (err_msg) *err_msg = "read_failed";
                    break;
                }
                this_data = in_buf;
            }
        }

        int is_first = (ci == 0);
        int is_last  = (in_total == 0) || (offset + this_len >= in_total);

        uint32_t flags = 0;
        if (is_first) flags |= HL_WASM_STREAM_FLAG_FIRST;
        if (is_last)  flags |= HL_WASM_STREAM_FLAG_LAST;

        /* Set stream metadata - survives through instance_call because
         * it only overwrites fn/ctx/shared_data, not the stream fields. */
        hl_cap_wasm_set_stream_ctx(flags, (uint32_t)ci);

        void *chunk_out = NULL;
        size_t chunk_out_len = 0;
        HlWasmCallOpts per_chunk = opts ? opts->call_opts : (HlWasmCallOpts){0};
        rc = hl_cap_wasm_instance_call(inst, this_data, this_len,
                                        &chunk_out, &chunk_out_len,
                                        &per_chunk, NULL, NULL,
                                        alloc, err_msg);

        hl_cap_wasm_clear_stream_ctx();

        if (rc != HL_WASM_OK) break;

        /* Route output chunk */
        if (chunk_out && chunk_out_len > 0) {
            if (!output || output->kind == HL_STREAM_OUT_BUFFER) {
                if (grow_output_buf(&out_buf, &out_buf_cap,
                                     out_buf_len + chunk_out_len, alloc) != 0) {
                    rc = HL_WASM_ERR_INTERNAL;
                    if (err_msg) *err_msg = "alloc_failed";
                    hl_alloc_free(alloc, chunk_out, chunk_out_len);
                    break;
                }
                memcpy(out_buf + out_buf_len, chunk_out, chunk_out_len);
                out_buf_len += chunk_out_len;
            } else if (output->kind == HL_STREAM_OUT_FILE) {
                size_t wr = fwrite(chunk_out, 1, chunk_out_len, out_file);
                if (wr != chunk_out_len) {
                    rc = HL_WASM_ERR_INTERNAL;
                    if (err_msg) *err_msg = "write_failed";
                    hl_alloc_free(alloc, chunk_out, chunk_out_len);
                    break;
                }
            } else if (output->kind == HL_STREAM_OUT_CALLBACK) {
                int cb_rc = output->callback.fn(
                    chunk_out, chunk_out_len, (uint32_t)ci, is_last,
                    output->callback.user_data);
                if (cb_rc != 0) {
                    rc = HL_WASM_ERR_INTERNAL;
                    if (err_msg) *err_msg = "callback_failed";
                    hl_alloc_free(alloc, chunk_out, chunk_out_len);
                    break;
                }
            }
            total_out += chunk_out_len;
            hl_alloc_free(alloc, chunk_out, chunk_out_len);
        }

        offset += this_len;
        chunk_index = ci + 1;
    }

    /* ── Cleanup ───────────────────────────────────────────────── */
cleanup:
    hl_cap_wasm_instance_destroy(inst);
    if (in_file) fclose(in_file);
    if (in_buf) hl_alloc_free(alloc, in_buf, chunk_size);
    if (out_file) fclose(out_file);

    /* Set output for BUFFER mode */
    if (rc == HL_WASM_OK && out_buf &&
        output && output->kind == HL_STREAM_OUT_BUFFER &&
        output->buffer.data && output->buffer.len) {
        *output->buffer.data = out_buf;
        *output->buffer.len = out_buf_len;
        out_buf = NULL; /* ownership transferred */
    }
    /* Always free if not transferred */
    if (out_buf)
        hl_alloc_free(alloc, out_buf, out_buf_cap);

    /* Fill result */
    if (result) {
        result->total_input = in_total;
        result->total_output = total_out;
        result->chunks = chunk_index;
    }

    return rc;
}

#endif /* HL_ENABLE_WASM */
