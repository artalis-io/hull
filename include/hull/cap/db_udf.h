/*
 * cap/db_udf.h - SQLite user-defined function integration
 *
 * Orthogonal bridge: connects SQLite and WASM without coupling either.
 * Script UDFs (Lua/JS callbacks) are handled in runtime bindings (mod_db.c).
 *
 * Gated by HL_ENABLE_WASM for WASM-backed UDFs.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_DB_UDF_H
#define HL_CAP_DB_UDF_H

#ifdef HL_ENABLE_WASM

#include <stddef.h>
#include <stdint.h>

struct HlDbHandle;
struct HlWasmCache;
struct HlWasmInstance;
struct HlVfs;
typedef struct HlAllocator HlAllocator;

/* Wire format: opcodes */
#define HL_UDF_OP_SCALAR     0x01
#define HL_UDF_OP_STEP       0x01
#define HL_UDF_OP_FINALIZE   0x02

/* Wire format: type tags */
#define HL_UDF_TYPE_INTEGER  0x01
#define HL_UDF_TYPE_REAL     0x02
#define HL_UDF_TYPE_TEXT     0x03
#define HL_UDF_TYPE_BLOB     0x04
#define HL_UDF_TYPE_NULL     0x05

/* Wire format: result types */
#define HL_UDF_RESULT_VOID    0x00
#define HL_UDF_RESULT_INTEGER 0x01
#define HL_UDF_RESULT_REAL    0x02
#define HL_UDF_RESULT_TEXT    0x03
#define HL_UDF_RESULT_BLOB    0x04
#define HL_UDF_RESULT_NULL    0x05

/* Defaults */
#define HL_UDF_DEFAULT_GAS    (100 * 1000)
#define HL_UDF_MAX_INPUT_SIZE (8 * 1024)   /* 8 KB - sufficient for typical SQL values */

/* Registration options (for WASM UDFs) */
typedef struct {
    const char *sql_name;       /* must start with "hull_" */
    const char *module_name;    /* WASM module name */
    int         nargs;          /* -1 = variadic, default 1 */
    int64_t     gas_per_call;   /* 0 = HL_UDF_DEFAULT_GAS */
    int         deterministic;  /* 1 = SQLITE_DETERMINISTIC */
    int         is_aggregate;   /* 1 = step + finalize */
    uint32_t    heap_size;      /* 0 = default */
    uint32_t    stack_size;     /* 0 = default */
} HlDbUdfOpts;

/**
 * Register a WASM-backed UDF.
 *
 * For scalar UDFs: creates a persistent WASM instance at registration time,
 * reused across all rows. Module must be stateless between rows.
 *
 * For aggregate UDFs: creates per-group instances via per-row aggregate
 * contexts. Each group gets its own WASM linear memory for accumulation.
 *
 * UDFs are currently SQLite-specific. If the backend behind `handle` is
 * not the SQLite backend, the call returns -1 with err_msg set.
 *
 * @param handle    Database handle (vtable-based; must be SQLite-backed)
 * @param cache     WASM module cache
 * @param opts      Registration options
 * @param app_vfs   App VFS for module loading
 * @param app_dir   App directory for filesystem fallback
 * @param alloc     Allocator (NULL = raw malloc)
 * @param err_msg   Output: error description on failure
 * @return 0 on success, -1 on failure
 */
int hl_cap_db_udf_register_wasm(struct HlDbHandle *handle,
                                 struct HlWasmCache *cache,
                                 const HlDbUdfOpts *opts,
                                 const struct HlVfs *app_vfs,
                                 const char *app_dir,
                                 HlAllocator *alloc,
                                 const char **err_msg);

/**
 * Unregister any UDF by name.
 *
 * @param handle    Database handle (vtable-based; must be SQLite-backed)
 * @param sql_name  Function name to remove
 * @return 0 on success, -1 on failure (or unsupported backend)
 */
int hl_cap_db_udf_unregister(struct HlDbHandle *handle, const char *sql_name);

#endif /* HL_ENABLE_WASM */

#endif /* HL_CAP_DB_UDF_H */
