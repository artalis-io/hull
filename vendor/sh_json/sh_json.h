/*
 * sh_json.h - Arena-Allocated JSON Parser for OTTO Platform
 *
 * Minimal, zero-dependency JSON parser with arena allocation.
 * All parsed values are allocated from a single arena and freed together.
 *
 * Features:
 * - Recursive descent parser
 * - Full JSON escape sequence handling (\n, \uXXXX, etc.)
 * - NULL-safe value accessors with defaults
 * - Path-based access (e.g., "user.name", "items[0].id")
 * - WASM compatible (no external dependencies)
 *
 * Usage:
 *   SHArena *arena = sh_arena_create(4096);
 *   ShJsonValue *root;
 *   if (sh_json_parse(json_str, strlen(json_str), arena, &root) == SH_JSON_OK) {
 *       double price = sh_json_as_double(sh_json_get(root, "price"), 0.0);
 *       const char *name = sh_json_as_string(sh_json_get(root, "name"), "");
 *       // ...
 *   }
 *   sh_arena_free(arena);  // Frees all parsed values
 */

#ifndef SH_JSON_H
#define SH_JSON_H

#include <sh_arena.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration
 * ============================================================================ */

#ifndef SH_JSON_MAX_DEPTH
#define SH_JSON_MAX_DEPTH 64
#endif

/* ============================================================================
 * Status Codes
 * ============================================================================ */

typedef enum {
    SH_JSON_OK = 0,
    SH_JSON_ERR_NULL,              /* NULL input pointer */
    SH_JSON_ERR_EMPTY,             /* Empty or whitespace-only input */
    SH_JSON_ERR_SYNTAX,            /* Syntax error */
    SH_JSON_ERR_UNTERMINATED_STRING,
    SH_JSON_ERR_INVALID_ESCAPE,
    SH_JSON_ERR_INVALID_NUMBER,
    SH_JSON_ERR_DEPTH_EXCEEDED,
    SH_JSON_ERR_ARENA_FULL
} ShJsonStatus;

/* ============================================================================
 * Value Types
 * ============================================================================ */

typedef enum {
    SH_JSON_NULL,
    SH_JSON_BOOL,
    SH_JSON_NUMBER,
    SH_JSON_STRING,
    SH_JSON_ARRAY,
    SH_JSON_OBJECT
} ShJsonType;

/* ============================================================================
 * Data Structures
 * ============================================================================ */

/* Forward declaration */
typedef struct ShJsonValue ShJsonValue;

/* Object member (key-value pair) */
typedef struct {
    const char *key;      /* Key string (arena-allocated, null-terminated) */
    size_t key_len;       /* Key length (excludes null terminator) */
    ShJsonValue *value;   /* Value (arena-allocated) */
} ShJsonMember;

/* JSON value (arena-allocated) */
struct ShJsonValue {
    ShJsonType type;
    union {
        bool bool_val;
        double num_val;
        struct {
            const char *str;  /* Null-terminated string */
            size_t len;       /* Length (excludes null terminator) */
        } string_val;
        struct {
            ShJsonValue **items;
            size_t count;
        } array_val;
        struct {
            ShJsonMember *members;
            size_t count;
        } object_val;
    } u;
};

/* ============================================================================
 * Parsing
 * ============================================================================ */

/*
 * Parse JSON string into arena-allocated DOM.
 *
 * @param json  JSON string (not modified)
 * @param len   Length of JSON string
 * @param arena Arena for allocations (caller manages lifecycle)
 * @param out   Output value pointer (set on success)
 * @return SH_JSON_OK on success, error code otherwise
 */
ShJsonStatus sh_json_parse(const char *json, size_t len,
                           SHArena *arena, ShJsonValue **out);

/*
 * Get human-readable error message for status code.
 */
const char *sh_json_status_str(ShJsonStatus status);

/* ============================================================================
 * Type Checking
 * ============================================================================ */

/*
 * Get type of value (SH_JSON_NULL if v is NULL).
 */
ShJsonType sh_json_type(const ShJsonValue *v);

/*
 * Check if value is null (or pointer is NULL).
 */
bool sh_json_is_null(const ShJsonValue *v);

/* ============================================================================
 * Value Access (NULL-safe, returns default on type mismatch)
 * ============================================================================ */

/*
 * Get boolean value.
 * Returns def if v is NULL or not a boolean.
 */
bool sh_json_as_bool(const ShJsonValue *v, bool def);

/*
 * Get numeric value.
 * Returns def if v is NULL or not a number.
 */
double sh_json_as_double(const ShJsonValue *v, double def);

/*
 * Get integer value (truncates toward zero).
 * Returns def if v is NULL or not a number.
 */
int sh_json_as_int(const ShJsonValue *v, int def);

/*
 * Get string value.
 * Returns def if v is NULL or not a string.
 */
const char *sh_json_as_string(const ShJsonValue *v, const char *def);

/*
 * Get string length.
 * Returns 0 if v is NULL or not a string.
 */
size_t sh_json_string_len(const ShJsonValue *v);

/* ============================================================================
 * Array Access
 * ============================================================================ */

/*
 * Get array length.
 * Returns 0 if v is NULL or not an array.
 */
size_t sh_json_array_len(const ShJsonValue *v);

/*
 * Get array element by index.
 * Returns NULL if v is NULL, not an array, or index out of bounds.
 */
ShJsonValue *sh_json_array_get(const ShJsonValue *v, size_t index);

/* ============================================================================
 * Object Access
 * ============================================================================ */

/*
 * Get object member count.
 * Returns 0 if v is NULL or not an object.
 */
size_t sh_json_object_len(const ShJsonValue *v);

/*
 * Get object member by key.
 * Returns NULL if v is NULL, not an object, or key not found.
 * Uses linear search (O(n) for n members).
 */
ShJsonValue *sh_json_get(const ShJsonValue *v, const char *key);

/*
 * Get object member by key with length.
 * Useful when key is not null-terminated.
 */
ShJsonValue *sh_json_get_n(const ShJsonValue *v, const char *key, size_t key_len);

/* ============================================================================
 * Path Access
 * ============================================================================ */

/*
 * Access nested value by path.
 *
 * Path syntax:
 *   - "key" - object key
 *   - "key.subkey" - nested object keys
 *   - "array[0]" - array index
 *   - "obj.arr[2].name" - combined access
 *
 * Returns NULL if path is invalid, any segment not found, or type mismatch.
 *
 * Examples:
 *   sh_json_get_path(root, "name")           // root.name
 *   sh_json_get_path(root, "user.email")     // root.user.email
 *   sh_json_get_path(root, "items[0]")       // root.items[0]
 *   sh_json_get_path(root, "data.items[2].id") // root.data.items[2].id
 */
ShJsonValue *sh_json_get_path(const ShJsonValue *v, const char *path);

/* ============================================================================
 * JSON Writer (Streaming)
 * ============================================================================
 *
 * Streaming JSON writer that outputs directly to a callback function.
 * No intermediate buffering - writes go straight to the destination.
 *
 * Usage with mongoose:
 *   static int mg_json_write(void *ctx, const char *data, size_t len) {
 *       mg_send((struct mg_connection *)ctx, data, len);
 *       return 0;
 *   }
 *
 *   ShJsonWriter w;
 *   sh_json_writer_init(&w, mg_json_write, c);
 *   sh_json_write_object_start(&w);
 *   sh_json_write_kv_string(&w, "status", "ok");
 *   sh_json_write_kv_double(&w, "distance", 12345.6);
 *   sh_json_write_object_end(&w);
 */

/* Write callback: return 0 on success, non-zero on error */
typedef int (*ShJsonWriteFn)(void *ctx, const char *data, size_t len);

typedef struct {
    ShJsonWriteFn write_fn;  /* Output callback */
    void *ctx;               /* Callback context (e.g., socket, file) */
    int depth;               /* Nesting depth (for future pretty-print) */
    int needs_comma;         /* Need comma before next element */
    int error;               /* Sticky error flag */
    int first_in_container;  /* First element in current array/object */
} ShJsonWriter;

/*
 * Initialize writer with output callback.
 *
 * @param w        Writer to initialize
 * @param write_fn Callback to write data (e.g., mg_send wrapper)
 * @param ctx      Context passed to write_fn
 */
void sh_json_writer_init(ShJsonWriter *w, ShJsonWriteFn write_fn, void *ctx);

/*
 * Check if writer has encountered an error.
 */
int sh_json_writer_error(const ShJsonWriter *w);

/* ----------------------------------------------------------------------------
 * Primitive Values
 * ---------------------------------------------------------------------------- */

int sh_json_write_null(ShJsonWriter *w);
int sh_json_write_bool(ShJsonWriter *w, bool val);
int sh_json_write_int(ShJsonWriter *w, int64_t val);
int sh_json_write_double(ShJsonWriter *w, double val);
int sh_json_write_double_fmt(ShJsonWriter *w, double val, int precision);
int sh_json_write_string(ShJsonWriter *w, const char *str);
int sh_json_write_string_n(ShJsonWriter *w, const char *str, size_t len);

/* Write raw JSON (for pre-formatted content) */
int sh_json_write_raw(ShJsonWriter *w, const char *raw, size_t len);

/* ----------------------------------------------------------------------------
 * Objects
 * ---------------------------------------------------------------------------- */

int sh_json_write_object_start(ShJsonWriter *w);
int sh_json_write_object_end(ShJsonWriter *w);
int sh_json_write_key(ShJsonWriter *w, const char *key);

/* Convenience: key + value in one call */
int sh_json_write_kv_null(ShJsonWriter *w, const char *key);
int sh_json_write_kv_bool(ShJsonWriter *w, const char *key, bool val);
int sh_json_write_kv_int(ShJsonWriter *w, const char *key, int64_t val);
int sh_json_write_kv_double(ShJsonWriter *w, const char *key, double val);
int sh_json_write_kv_double_fmt(ShJsonWriter *w, const char *key, double val, int precision);
int sh_json_write_kv_string(ShJsonWriter *w, const char *key, const char *val);

/* ----------------------------------------------------------------------------
 * Arrays
 * ---------------------------------------------------------------------------- */

int sh_json_write_array_start(ShJsonWriter *w);
int sh_json_write_array_end(ShJsonWriter *w);

/* ----------------------------------------------------------------------------
 * Buffer Helper
 *
 * Growable buffer for building JSON in memory. Use this with ShJsonWriter
 * when you need to build a complete JSON string before sending (e.g., for
 * transport-agnostic APIs that return malloc'd strings).
 *
 * Usage:
 *   ShJsonBuf jb;
 *   sh_json_buf_init(&jb);
 *
 *   ShJsonWriter w;
 *   sh_json_writer_init(&w, sh_json_buf_write, &jb);
 *   sh_json_write_object_start(&w);
 *   sh_json_write_kv_string(&w, "status", "ok");
 *   sh_json_write_object_end(&w);
 *
 *   if (!sh_json_writer_error(&w) && jb.buf) {
 *       send_response(jb.buf);  // jb.buf is null-terminated
 *   }
 *   sh_json_buf_free(&jb);
 *
 * For mongoose specifically:
 *   send_json(c, 200, jb.buf);  // Works with existing helpers
 *   sh_json_buf_free(&jb);
 * ---------------------------------------------------------------------------- */

typedef struct {
    char *buf;    /* Growable buffer (null-terminated) */
    size_t len;   /* Current length (excludes null terminator) */
    size_t cap;   /* Capacity */
} ShJsonBuf;

/*
 * Initialize an empty JSON buffer.
 */
void sh_json_buf_init(ShJsonBuf *jb);

/*
 * Free the buffer. Safe to call on an uninitialized/zeroed struct.
 */
void sh_json_buf_free(ShJsonBuf *jb);

/*
 * Reset buffer for reuse (keeps allocated memory).
 */
void sh_json_buf_reset(ShJsonBuf *jb);

/*
 * Write callback for ShJsonWriter. Pass this to sh_json_writer_init()
 * with an ShJsonBuf* as the context.
 *
 * Returns 0 on success, -1 on allocation failure.
 */
int sh_json_buf_write(void *ctx, const char *data, size_t len);

/*
 * Take ownership of the buffer. After calling, jb is reset to empty.
 * Caller is responsible for freeing the returned pointer.
 * Returns NULL if buffer is empty.
 */
char *sh_json_buf_take(ShJsonBuf *jb);

#ifdef __cplusplus
}
#endif

#endif /* SH_JSON_H */
