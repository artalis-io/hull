/*
 * cap/db_udf.c — SQLite UDF integration with WASM compute
 *
 * Orthogonal bridge module: connects SQLite and WASM without coupling either.
 * Registers user-defined SQL functions backed by WASM modules using the
 * hull_process ABI with a typed wire format for arguments and results.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifdef HL_ENABLE_WASM

#include "hull/cap/db_udf.h"
#include "hull/cap/wasm.h"
#include "hull/alloc.h"
#include "hull/vfs.h"
#include "log.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

/* ── Internal context (stored as sqlite3 user_data) ────────────────── */

typedef struct {
    HlWasmCache  *cache;
    const HlVfs  *app_vfs;
    const char   *app_dir;
    HlAllocator  *alloc;
    char          module_name[256];
    int64_t       gas_per_call;
    uint32_t      heap_size;
    uint32_t      stack_size;
    int           is_aggregate;
    HlWasmInstance *scalar_inst;  /* non-NULL for scalar UDFs */
} HlDbUdfCtx;

/* Per-group state for aggregate UDFs (via sqlite3_aggregate_context) */
typedef struct {
    HlWasmInstance *inst;  /* NULL = not yet created */
} HlDbUdfAggState;

/* ── Wire format marshalling ───────────────────────────────────────── */

/*
 * Marshal sqlite3_value[] into the UDF wire format:
 *   [opcode: u8] [argc: u8] [type_1: u8] [len_1: u32 LE] [data_1...] ...
 *
 * Returns bytes written, or -1 if buffer too small.
 */
static int marshal_udf_input(uint8_t *buf, size_t buf_size,
                              uint8_t opcode, int argc,
                              sqlite3_value **argv)
{
    if (buf_size < 2)
        return -1;

    buf[0] = opcode;
    buf[1] = (uint8_t)(argc > 255 ? 255 : argc);
    size_t pos = 2;

    for (int i = 0; i < argc; i++) {
        int vtype = sqlite3_value_type(argv[i]);
        uint8_t tag;
        const void *data = NULL;
        uint32_t data_len = 0;

        switch (vtype) {
        case SQLITE_INTEGER: {
            tag = HL_UDF_TYPE_INTEGER;
            data_len = 8;
            break;
        }
        case SQLITE_FLOAT: {
            tag = HL_UDF_TYPE_REAL;
            data_len = 8;
            break;
        }
        case SQLITE_TEXT: {
            tag = HL_UDF_TYPE_TEXT;
            data = sqlite3_value_text(argv[i]);
            data_len = (uint32_t)sqlite3_value_bytes(argv[i]);
            break;
        }
        case SQLITE_BLOB: {
            tag = HL_UDF_TYPE_BLOB;
            data = sqlite3_value_blob(argv[i]);
            data_len = (uint32_t)sqlite3_value_bytes(argv[i]);
            break;
        }
        default: /* SQLITE_NULL */
            tag = HL_UDF_TYPE_NULL;
            data_len = 0;
            break;
        }

        /* Need: 1 (tag) + 4 (len) + data_len */
        if (pos + 5 + data_len > buf_size)
            return -1;

        buf[pos++] = tag;
        buf[pos++] = (uint8_t)(data_len & 0xFF);
        buf[pos++] = (uint8_t)((data_len >> 8) & 0xFF);
        buf[pos++] = (uint8_t)((data_len >> 16) & 0xFF);
        buf[pos++] = (uint8_t)((data_len >> 24) & 0xFF);

        if (tag == HL_UDF_TYPE_INTEGER) {
            int64_t v = sqlite3_value_int64(argv[i]);
            memcpy(&buf[pos], &v, 8);
            pos += 8;
        } else if (tag == HL_UDF_TYPE_REAL) {
            double v = sqlite3_value_double(argv[i]);
            memcpy(&buf[pos], &v, 8);
            pos += 8;
        } else if (data && data_len > 0) {
            memcpy(&buf[pos], data, data_len);
            pos += data_len;
        }
    }

    return (int)pos;
}

/*
 * Unmarshal WASM output into sqlite3_result_*:
 *   [type: u8] [data...]
 *
 * Returns 0 on success, -1 on error.
 */
static int unmarshal_udf_output(sqlite3_context *ctx,
                                 const uint8_t *output, size_t output_len)
{
    if (!output || output_len == 0) {
        sqlite3_result_null(ctx);
        return 0;
    }

    uint8_t rtype = output[0];
    const uint8_t *data = output + 1;
    size_t data_len = output_len - 1;

    switch (rtype) {
    case HL_UDF_RESULT_VOID:
        /* Aggregate step — no result yet */
        return 0;

    case HL_UDF_RESULT_INTEGER:
        if (data_len < 8) return -1;
        {
            int64_t v;
            memcpy(&v, data, 8);
            sqlite3_result_int64(ctx, v);
        }
        return 0;

    case HL_UDF_RESULT_REAL:
        if (data_len < 8) return -1;
        {
            double v;
            memcpy(&v, data, 8);
            sqlite3_result_double(ctx, v);
        }
        return 0;

    case HL_UDF_RESULT_TEXT:
        sqlite3_result_text(ctx, (const char *)data, (int)data_len,
                            SQLITE_TRANSIENT);
        return 0;

    case HL_UDF_RESULT_BLOB:
        sqlite3_result_blob(ctx, data, (int)data_len, SQLITE_TRANSIENT);
        return 0;

    case HL_UDF_RESULT_NULL:
        sqlite3_result_null(ctx);
        return 0;

    default:
        return -1;
    }
}

/* ── Scalar WASM UDF callback ──────────────────────────────────────── */

static void wasm_scalar_func(sqlite3_context *ctx, int argc,
                              sqlite3_value **argv)
{
    HlDbUdfCtx *udf = (HlDbUdfCtx *)sqlite3_user_data(ctx);

    /* Marshal input */
    uint8_t input_buf[HL_UDF_MAX_INPUT_SIZE];
    int input_len = marshal_udf_input(input_buf, sizeof(input_buf),
                                       HL_UDF_OP_SCALAR, argc, argv);
    if (input_len < 0) {
        sqlite3_result_error(ctx, "UDF input exceeds 64KB limit", -1);
        return;
    }

    /* Call WASM instance */
    HlWasmCallOpts call_opts = {
        .gas = udf->gas_per_call,
        .max_input  = HL_UDF_MAX_INPUT_SIZE,
        .max_output = HL_UDF_MAX_INPUT_SIZE,
    };

    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;

    int rc = hl_cap_wasm_instance_call(udf->scalar_inst,
                                        input_buf, (size_t)input_len,
                                        &output, &output_len,
                                        &call_opts, NULL, NULL,
                                        udf->alloc, &err_msg);
    if (rc != 0) {
        char err[256];
        snprintf(err, sizeof(err), "WASM UDF error: %s",
                 err_msg ? err_msg : "call_failed");
        sqlite3_result_error(ctx, err, -1);
        return;
    }

    /* Unmarshal output */
    if (unmarshal_udf_output(ctx, (const uint8_t *)output, output_len) != 0)
        sqlite3_result_error(ctx, "WASM UDF: invalid output format", -1);

    free(output);
}

/* ── Aggregate WASM UDF callbacks ──────────────────────────────────── */

static void wasm_step_func(sqlite3_context *ctx, int argc,
                            sqlite3_value **argv)
{
    HlDbUdfCtx *udf = (HlDbUdfCtx *)sqlite3_user_data(ctx);
    HlDbUdfAggState *gs = (HlDbUdfAggState *)sqlite3_aggregate_context(
        ctx, (int)sizeof(HlDbUdfAggState));
    if (!gs) {
        sqlite3_result_error_nomem(ctx);
        return;
    }

    /* Create instance on first step for this group */
    if (!gs->inst) {
        HlWasmCallOpts create_opts = {
            .heap_size  = udf->heap_size,
            .stack_size = udf->stack_size,
            .gas        = udf->gas_per_call,
            .max_input  = HL_UDF_MAX_INPUT_SIZE,
            .max_output = HL_UDF_MAX_INPUT_SIZE,
        };
        const char *err_msg = NULL;
        gs->inst = hl_cap_wasm_instance_create(
            udf->cache, udf->module_name, &create_opts,
            udf->app_vfs, udf->app_dir, udf->alloc, &err_msg);
        if (!gs->inst) {
            char err[256];
            snprintf(err, sizeof(err), "WASM UDF: failed to create instance: %s",
                     err_msg ? err_msg : "unknown");
            sqlite3_result_error(ctx, err, -1);
            return;
        }
    }

    /* Marshal input with STEP opcode */
    uint8_t input_buf[HL_UDF_MAX_INPUT_SIZE];
    int input_len = marshal_udf_input(input_buf, sizeof(input_buf),
                                       HL_UDF_OP_STEP, argc, argv);
    if (input_len < 0) {
        sqlite3_result_error(ctx, "UDF step input exceeds 64KB limit", -1);
        return;
    }

    /* Call instance — step result should be VOID */
    HlWasmCallOpts call_opts = {
        .gas = udf->gas_per_call,
        .max_input  = HL_UDF_MAX_INPUT_SIZE,
        .max_output = HL_UDF_MAX_INPUT_SIZE,
    };

    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;

    int rc = hl_cap_wasm_instance_call(gs->inst,
                                        input_buf, (size_t)input_len,
                                        &output, &output_len,
                                        &call_opts, NULL, NULL,
                                        udf->alloc, &err_msg);
    free(output); /* step output is ignored (VOID) */

    if (rc != 0) {
        char err[256];
        snprintf(err, sizeof(err), "WASM UDF step error: %s",
                 err_msg ? err_msg : "call_failed");
        sqlite3_result_error(ctx, err, -1);
    }
}

static void wasm_finalize_func(sqlite3_context *ctx)
{
    HlDbUdfCtx *udf = (HlDbUdfCtx *)sqlite3_user_data(ctx);
    HlDbUdfAggState *gs = (HlDbUdfAggState *)sqlite3_aggregate_context(ctx, 0);

    if (!gs || !gs->inst) {
        sqlite3_result_null(ctx);
        return;
    }

    /* Marshal FINALIZE opcode (no arguments) */
    uint8_t input_buf[2] = { HL_UDF_OP_FINALIZE, 0 };

    HlWasmCallOpts call_opts = {
        .gas = udf->gas_per_call,
        .max_input  = HL_UDF_MAX_INPUT_SIZE,
        .max_output = HL_UDF_MAX_INPUT_SIZE,
    };

    void *output = NULL;
    size_t output_len = 0;
    const char *err_msg = NULL;

    int rc = hl_cap_wasm_instance_call(gs->inst,
                                        input_buf, sizeof(input_buf),
                                        &output, &output_len,
                                        &call_opts, NULL, NULL,
                                        udf->alloc, &err_msg);
    if (rc != 0) {
        char err[256];
        snprintf(err, sizeof(err), "WASM UDF finalize error: %s",
                 err_msg ? err_msg : "call_failed");
        sqlite3_result_error(ctx, err, -1);
    } else {
        if (unmarshal_udf_output(ctx, (const uint8_t *)output, output_len) != 0)
            sqlite3_result_error(ctx, "WASM UDF: invalid finalize output", -1);
    }

    free(output);

    /* Destroy per-group instance */
    hl_cap_wasm_instance_destroy(gs->inst);
    gs->inst = NULL;
}

/* ── Destroy callback (called by SQLite on unregister/close) ───────── */

static void wasm_udf_destroy(void *data)
{
    HlDbUdfCtx *udf = (HlDbUdfCtx *)data;
    if (!udf)
        return;

    /* Destroy scalar persistent instance */
    if (udf->scalar_inst) {
        hl_cap_wasm_instance_destroy(udf->scalar_inst);
        udf->scalar_inst = NULL;
    }

    if (udf->alloc)
        hl_alloc_free(udf->alloc, udf, sizeof(*udf));
    else
        free(udf);
}

/* ── Public API ────────────────────────────────────────────────────── */

int hl_cap_db_udf_register_wasm(sqlite3 *db,
                                 HlWasmCache *cache,
                                 const HlDbUdfOpts *opts,
                                 const HlVfs *app_vfs,
                                 const char *app_dir,
                                 HlAllocator *alloc,
                                 const char **err_msg)
{
    static const char *err_invalid  = "invalid arguments";
    static const char *err_prefix   = "UDF name must start with 'hull_'";
    static const char *err_oom      = "out of memory";
    static const char *err_inst     = "failed to create WASM instance";
    static const char *err_sqlite   = "sqlite3_create_function_v2 failed";

    if (!db || !cache || !opts || !opts->sql_name || !opts->module_name) {
        if (err_msg) *err_msg = err_invalid;
        return -1;
    }

    /* Validate hull_ prefix */
    if (strncmp(opts->sql_name, "hull_", 5) != 0) {
        if (err_msg) *err_msg = err_prefix;
        return -1;
    }

    /* Auto-load module into cache */
    int load_rc = hl_cap_wasm_load(cache, opts->module_name, app_vfs, app_dir);
    if (load_rc != 0) {
        if (err_msg) *err_msg = "failed to load WASM module";
        return -1;
    }

    /* Allocate context */
    HlDbUdfCtx *udf_ctx = alloc ? hl_alloc_calloc(alloc, 1, sizeof(*udf_ctx))
                                : calloc(1, sizeof(*udf_ctx));
    if (!udf_ctx) {
        if (err_msg) *err_msg = err_oom;
        return -1;
    }

    udf_ctx->cache    = cache;
    udf_ctx->app_vfs  = app_vfs;
    udf_ctx->app_dir  = app_dir;
    udf_ctx->alloc    = alloc;
    snprintf(udf_ctx->module_name, sizeof(udf_ctx->module_name),
             "%s", opts->module_name);
    udf_ctx->gas_per_call = opts->gas_per_call > 0 ? opts->gas_per_call
                                                     : HL_UDF_DEFAULT_GAS;
    udf_ctx->heap_size    = opts->heap_size;
    udf_ctx->stack_size   = opts->stack_size;
    udf_ctx->is_aggregate = opts->is_aggregate;

    int nargs = opts->nargs != 0 ? opts->nargs : 1;

    /* For scalar UDFs, create persistent instance at registration time */
    if (!opts->is_aggregate) {
        HlWasmCallOpts create_opts = {
            .heap_size  = udf_ctx->heap_size,
            .stack_size = udf_ctx->stack_size,
            .gas        = udf_ctx->gas_per_call,
            .max_input  = HL_UDF_MAX_INPUT_SIZE,
            .max_output = HL_UDF_MAX_INPUT_SIZE,
        };
        const char *create_err = NULL;
        udf_ctx->scalar_inst = hl_cap_wasm_instance_create(
            cache, opts->module_name, &create_opts,
            app_vfs, app_dir, alloc, &create_err);
        if (!udf_ctx->scalar_inst) {
            log_error("[db_udf] failed to create scalar instance for '%s': %s",
                      opts->sql_name, create_err ? create_err : "unknown");
            wasm_udf_destroy(udf_ctx);
            if (err_msg) *err_msg = err_inst;
            return -1;
        }
    }

    /* Register with SQLite */
    int encoding = SQLITE_UTF8;
    if (opts->deterministic)
        encoding |= SQLITE_DETERMINISTIC;

    int rc;
    if (opts->is_aggregate) {
        rc = sqlite3_create_function_v2(
            db, opts->sql_name, nargs, encoding, udf_ctx,
            NULL,                 /* xFunc = NULL for aggregates */
            wasm_step_func,       /* xStep */
            wasm_finalize_func,   /* xFinal */
            wasm_udf_destroy);    /* xDestroy */
    } else {
        rc = sqlite3_create_function_v2(
            db, opts->sql_name, nargs, encoding, udf_ctx,
            wasm_scalar_func,     /* xFunc */
            NULL,                 /* xStep = NULL for scalars */
            NULL,                 /* xFinal = NULL for scalars */
            wasm_udf_destroy);    /* xDestroy */
    }

    if (rc != SQLITE_OK) {
        log_error("[db_udf] sqlite3_create_function_v2 failed for '%s': %s",
                  opts->sql_name, sqlite3_errmsg(db));
        /* SQLite won't call xDestroy if registration fails, so clean up */
        wasm_udf_destroy(udf_ctx);
        if (err_msg) *err_msg = err_sqlite;
        return -1;
    }

    log_info("[db_udf] registered WASM UDF '%s' (module=%s, %s, nargs=%d)",
             opts->sql_name, opts->module_name,
             opts->is_aggregate ? "aggregate" : "scalar", nargs);
    return 0;
}

int hl_cap_db_udf_unregister(sqlite3 *db, const char *sql_name)
{
    if (!db || !sql_name)
        return -1;

    /* Passing NULL function pointers removes the function.
     * SQLite calls xDestroy on the old registration. */
    int rc = sqlite3_create_function_v2(
        db, sql_name, -1, SQLITE_UTF8,
        NULL, NULL, NULL, NULL, NULL);

    if (rc != SQLITE_OK) {
        log_error("[db_udf] unregister failed for '%s': %s",
                  sql_name, sqlite3_errmsg(db));
        return -1;
    }

    log_info("[db_udf] unregistered UDF '%s'", sql_name);
    return 0;
}

#endif /* HL_ENABLE_WASM */
