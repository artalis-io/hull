/*
 * hull_cap.h — Shared C Capability API
 *
 * All enforcement (path validation, host allowlists, SQL parameterization)
 * lives in hull_cap_* functions. Both Lua and JS bindings call these —
 * neither runtime touches SQLite, filesystem, or network directly.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef HULL_CAP_H
#define HULL_CAP_H

#include <stddef.h>
#include <stdint.h>

/* Forward declarations for vendor types */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;

/* ── Value types (runtime-agnostic) ─────────────────────────────────── */

typedef enum {
    HULL_TYPE_NIL    = 0,
    HULL_TYPE_INT    = 1,
    HULL_TYPE_DOUBLE = 2,
    HULL_TYPE_TEXT   = 3,
    HULL_TYPE_BLOB   = 4,
    HULL_TYPE_BOOL   = 5,
} HullValueType;

typedef struct {
    HullValueType type;
    union {
        int64_t     i;
        double      d;
        int         b;     /* bool */
        struct {
            const char *s;
            size_t      len;
        };
    };
} HullValue;

typedef struct {
    const char *name;
    HullValue   value;
} HullColumn;

/* ── Database capability ────────────────────────────────────────────── */

/*
 * Callback invoked for each row returned by hull_cap_db_query().
 * Return 0 to continue, non-zero to stop iteration.
 */
typedef int (*HullRowCallback)(void *ctx, HullColumn *cols, int ncols);

/*
 * Execute a SELECT query with parameterized binding.
 * Calls cb(ctx, cols, ncols) for each result row.
 * Returns 0 on success, -1 on error.
 */
int hull_cap_db_query(sqlite3 *db, const char *sql,
                      const HullValue *params, int nparams,
                      HullRowCallback cb, void *ctx);

/*
 * Execute a non-SELECT statement (INSERT/UPDATE/DELETE) with parameterized
 * binding. Returns the number of rows affected, or -1 on error.
 */
int hull_cap_db_exec(sqlite3 *db, const char *sql,
                     const HullValue *params, int nparams);

/*
 * Return the last insert rowid. Wrapper around sqlite3_last_insert_rowid().
 */
int64_t hull_cap_db_last_id(sqlite3 *db);

/* ── Filesystem capability ──────────────────────────────────────────── */

typedef struct {
    const char *base_dir;   /* allowed root directory (e.g. "data/") */
    size_t      base_len;
} HullFsConfig;

/*
 * Validate that `path` is within `cfg->base_dir`. Rejects "..", absolute
 * paths, and symlink escapes. Returns 0 if valid, -1 if rejected.
 */
int hull_cap_fs_validate(const HullFsConfig *cfg, const char *path);

/*
 * Read file contents into caller-provided buffer.
 * Returns bytes read, or -1 on error.
 * If buf is NULL, returns the file size without reading.
 */
int64_t hull_cap_fs_read(const HullFsConfig *cfg, const char *path,
                         char *buf, size_t buf_size);

/*
 * Write data to a file. Creates parent directories if needed.
 * Returns 0 on success, -1 on error.
 */
int hull_cap_fs_write(const HullFsConfig *cfg, const char *path,
                      const char *data, size_t len);

/*
 * Check if a file exists within the allowed directory.
 * Returns 1 if exists, 0 if not, -1 on validation error.
 */
int hull_cap_fs_exists(const HullFsConfig *cfg, const char *path);

/*
 * Delete a file within the allowed directory.
 * Returns 0 on success, -1 on error.
 */
int hull_cap_fs_delete(const HullFsConfig *cfg, const char *path);

/* ── Time capability ────────────────────────────────────────────────── */

/*
 * Current Unix timestamp (seconds since epoch).
 */
int64_t hull_cap_time_now(void);

/*
 * Current time in milliseconds since epoch.
 */
int64_t hull_cap_time_now_ms(void);

/*
 * Monotonic clock in milliseconds (for elapsed time measurement).
 */
int64_t hull_cap_time_clock(void);

/*
 * Format current time as ISO 8601 date string (YYYY-MM-DD).
 * Writes to buf (must be >= 11 bytes). Returns 0 on success.
 */
int hull_cap_time_date(char *buf, size_t buf_size);

/*
 * Format current time as ISO 8601 datetime (YYYY-MM-DDTHH:MM:SSZ).
 * Writes to buf (must be >= 21 bytes). Returns 0 on success.
 */
int hull_cap_time_datetime(char *buf, size_t buf_size);

/* ── Environment capability ─────────────────────────────────────────── */

typedef struct {
    const char **allowed;   /* NULL-terminated list of allowed env var names */
    int          count;
} HullEnvConfig;

/*
 * Get an environment variable. Returns NULL if the variable is not in
 * the allowlist or is not set.
 */
const char *hull_cap_env_get(const HullEnvConfig *cfg, const char *name);

/* ── Crypto capability ──────────────────────────────────────────────── */

/*
 * SHA-256 hash. Writes 32 bytes to `out`. Returns 0 on success.
 */
int hull_cap_crypto_sha256(const void *data, size_t len, uint8_t out[32]);

/*
 * Generate cryptographically secure random bytes.
 * Returns 0 on success, -1 on error.
 */
int hull_cap_crypto_random(void *buf, size_t len);

/*
 * PBKDF2-HMAC-SHA256 key derivation.
 * Returns 0 on success, -1 on error.
 */
int hull_cap_crypto_pbkdf2(const char *password, size_t pw_len,
                           const uint8_t *salt, size_t salt_len,
                           int iterations,
                           uint8_t *out, size_t out_len);

/*
 * Ed25519 signature verification.
 * Returns 0 if valid, -1 if invalid or error.
 */
int hull_cap_crypto_ed25519_verify(const uint8_t *msg, size_t msg_len,
                                   const uint8_t sig[64],
                                   const uint8_t pubkey[32]);

/* ── HTTP client capability ─────────────────────────────────────────── */

typedef struct {
    const char **allowed_hosts; /* NULL-terminated list of allowed hostnames */
    int          count;
} HullHttpConfig;

typedef struct {
    int          status;
    const char  *body;
    size_t       body_len;
    const char  *content_type;
} HullHttpResponse;

/*
 * Make an HTTPS request. Host must be in the allowlist.
 * Caller must free response body with hull_cap_http_free().
 * Returns 0 on success, -1 on error.
 */
int hull_cap_http_request(const HullHttpConfig *cfg,
                          const char *method, const char *url,
                          const char *body, size_t body_len,
                          const char *content_type,
                          HullHttpResponse *resp);

void hull_cap_http_free(HullHttpResponse *resp);

/* ── SMTP capability ────────────────────────────────────────────────── */

typedef struct {
    const char *host;
    int         port;
    const char *username;
    const char *password;
    int         use_tls;    /* 0 = plain, 1 = STARTTLS, 2 = implicit TLS */
} HullSmtpConfig;

typedef struct {
    const char *from;
    const char *to;         /* comma-separated recipients */
    const char *subject;
    const char *body;
    const char *content_type; /* "text/plain" or "text/html" */
} HullSmtpMessage;

/*
 * Send an email via SMTP. Returns 0 on success, -1 on error.
 */
int hull_cap_smtp_send(const HullSmtpConfig *cfg,
                       const HullSmtpMessage *msg);

#endif /* HULL_CAP_H */
