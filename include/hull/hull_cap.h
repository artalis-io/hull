/*
 * hull_cap.h — Shared C Capability API
 *
 * All enforcement (path validation, host allowlists, SQL parameterization)
 * lives in hl_cap_* functions. Both Lua and JS bindings call these —
 * neither runtime touches SQLite, filesystem, or network directly.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HL_CAP_H
#define HL_CAP_H

#include <stddef.h>
#include <stdint.h>

/* Forward declarations for vendor types */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

/* ── Value types (runtime-agnostic) ─────────────────────────────────── */

typedef enum {
    HL_TYPE_NIL    = 0,
    HL_TYPE_INT    = 1,
    HL_TYPE_DOUBLE = 2,
    HL_TYPE_TEXT   = 3,
    HL_TYPE_BLOB   = 4,
    HL_TYPE_BOOL   = 5,
} HlValueType;

typedef struct {
    HlValueType type;
    union {
        int64_t     i;
        double      d;
        int         b;     /* bool */
        struct {
            const char *s;
            size_t      len;
        };
    };
} HlValue;

typedef struct {
    const char *name;
    HlValue   value;
} HlColumn;

/* ── Body reader capability ─────────────────────────────────────────── */

#include <keel/body_reader.h>

/*
 * Body reader factory — wraps kl_body_reader_buffer with 1 MB limit.
 * Pass as the body_reader argument to kl_server_route().
 * user_data is ignored (factory provides its own max_size).
 */
KlBodyReader *hl_cap_body_factory(KlAllocator *alloc, const KlRequest *req,
                                  void *user_data);

/*
 * Extract body data from a KlBufReader (as created by hl_cap_body_factory).
 * Returns the body length and sets *out_data to the buffer.
 * Returns 0 if reader is NULL or body is empty.
 */
size_t hl_cap_body_data(const KlBodyReader *reader, const char **out_data);

/* ── Database capability ────────────────────────────────────────────── */

/*
 * Callback invoked for each row returned by hl_cap_db_query().
 * Return 0 to continue, non-zero to stop iteration.
 */
typedef int (*HlRowCallback)(void *ctx, HlColumn *cols, int ncols);

/*
 * Execute a SELECT query with parameterized binding.
 * Calls cb(ctx, cols, ncols) for each result row.
 * Returns 0 on success, -1 on error.
 */
int hl_cap_db_query(sqlite3 *db, const char *sql,
                      const HlValue *params, int nparams,
                      HlRowCallback cb, void *ctx);

/*
 * Execute a non-SELECT statement (INSERT/UPDATE/DELETE) with parameterized
 * binding. Returns the number of rows affected, or -1 on error.
 */
int hl_cap_db_exec(sqlite3 *db, const char *sql,
                     const HlValue *params, int nparams);

/*
 * Return the last insert rowid. Wrapper around sqlite3_last_insert_rowid().
 */
int64_t hl_cap_db_last_id(sqlite3 *db);

/* ── Filesystem capability ──────────────────────────────────────────── */

typedef struct {
    const char *base_dir;   /* allowed root directory (e.g. "data/") */
    size_t      base_len;
} HlFsConfig;

/*
 * Validate that `path` is within `cfg->base_dir`. Rejects "..", absolute
 * paths, and symlink escapes. Returns 0 if valid, -1 if rejected.
 */
int hl_cap_fs_validate(const HlFsConfig *cfg, const char *path);

/*
 * Read file contents into caller-provided buffer.
 * Returns bytes read, or -1 on error.
 * If buf is NULL, returns the file size without reading.
 */
int64_t hl_cap_fs_read(const HlFsConfig *cfg, const char *path,
                         char *buf, size_t buf_size);

/*
 * Write data to a file. Creates parent directories if needed.
 * Returns 0 on success, -1 on error.
 */
int hl_cap_fs_write(const HlFsConfig *cfg, const char *path,
                      const char *data, size_t len);

/*
 * Check if a file exists within the allowed directory.
 * Returns 1 if exists, 0 if not, -1 on validation error.
 */
int hl_cap_fs_exists(const HlFsConfig *cfg, const char *path);

/*
 * Delete a file within the allowed directory.
 * Returns 0 on success, -1 on error.
 */
int hl_cap_fs_delete(const HlFsConfig *cfg, const char *path);

/* ── Time capability ────────────────────────────────────────────────── */

/*
 * Current Unix timestamp (seconds since epoch).
 */
int64_t hl_cap_time_now(void);

/*
 * Current time in milliseconds since epoch.
 */
int64_t hl_cap_time_now_ms(void);

/*
 * Monotonic clock in milliseconds (for elapsed time measurement).
 */
int64_t hl_cap_time_clock(void);

/*
 * Format current time as ISO 8601 date string (YYYY-MM-DD).
 * Writes to buf (must be >= 11 bytes). Returns 0 on success.
 */
int hl_cap_time_date(char *buf, size_t buf_size);

/*
 * Format current time as ISO 8601 datetime (YYYY-MM-DDTHH:MM:SSZ).
 * Writes to buf (must be >= 21 bytes). Returns 0 on success.
 */
int hl_cap_time_datetime(char *buf, size_t buf_size);

/* ── Environment capability ─────────────────────────────────────────── */

typedef struct {
    const char **allowed;   /* NULL-terminated list of allowed env var names */
    int          count;
} HlEnvConfig;

/*
 * Get an environment variable. Returns NULL if the variable is not in
 * the allowlist or is not set.
 */
const char *hl_cap_env_get(const HlEnvConfig *cfg, const char *name);

/* ── Crypto capability ──────────────────────────────────────────────── */

/*
 * SHA-256 hash. Writes 32 bytes to `out`. Returns 0 on success.
 */
int hl_cap_crypto_sha256(const void *data, size_t len, uint8_t out[32]);

/*
 * Generate cryptographically secure random bytes.
 * Returns 0 on success, -1 on error.
 */
int hl_cap_crypto_random(void *buf, size_t len);

/*
 * PBKDF2-HMAC-SHA256 key derivation.
 * Returns 0 on success, -1 on error.
 */
int hl_cap_crypto_pbkdf2(const char *password, size_t pw_len,
                           const uint8_t *salt, size_t salt_len,
                           int iterations,
                           uint8_t *out, size_t out_len);

/*
 * Ed25519 signature verification.
 * Returns 0 if valid, -1 if invalid or error.
 */
int hl_cap_crypto_ed25519_verify(const uint8_t *msg, size_t msg_len,
                                   const uint8_t sig[64],
                                   const uint8_t pubkey[32]);

/* ── HTTP client capability ─────────────────────────────────────────── */

typedef struct {
    const char **allowed_hosts; /* NULL-terminated list of allowed hostnames */
    int          count;
} HlHttpConfig;

typedef struct {
    int          status;
    const char  *body;
    size_t       body_len;
    const char  *content_type;
} HlHttpResponse;

/*
 * Make an HTTPS request. Host must be in the allowlist.
 * Caller must free response body with hl_cap_http_free().
 * Returns 0 on success, -1 on error.
 */
int hl_cap_http_request(const HlHttpConfig *cfg,
                          const char *method, const char *url,
                          const char *body, size_t body_len,
                          const char *content_type,
                          HlHttpResponse *resp);

void hl_cap_http_free(HlHttpResponse *resp);

/* ── SMTP capability ────────────────────────────────────────────────── */

typedef struct {
    const char *host;
    int         port;
    const char *username;
    const char *password;
    int         use_tls;    /* 0 = plain, 1 = STARTTLS, 2 = implicit TLS */
} HlSmtpConfig;

typedef struct {
    const char *from;
    const char *to;         /* comma-separated recipients */
    const char *subject;
    const char *body;
    const char *content_type; /* "text/plain" or "text/html" */
} HlSmtpMessage;

/*
 * Send an email via SMTP. Returns 0 on success, -1 on error.
 */
int hl_cap_smtp_send(const HlSmtpConfig *cfg,
                       const HlSmtpMessage *msg);

#endif /* HL_CAP_H */
