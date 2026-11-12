/*
 * fuzz_db_wire_stubs.c: a fake byte transport, budget and crypto for the
 * message-level wire fuzzers (fuzz_pg_msg, fuzz_mysql_rows).
 *
 * Those harnesses drive the real connection layers - cap/pg_conn.c and
 * cap/mysql_conn.c, with every frame handler, the startup / auth exchange
 * and the row decoders - against bytes the fuzzer chooses. The layers read
 * through cap/db_transport.h, which here serves the fuzz input instead of a
 * socket (in chunks whose sizes the input also chooses, so a frame arrives
 * split the way a network splits it) and swallows what the client sends.
 * Nothing links Keel, mbedTLS or the TLS client: TLS is never attached (the
 * handshake stub fails), and the crypto the SCRAM / scramble code calls is
 * a deterministic stand-in (the fuzzer can learn its outputs; the parsing
 * around it is what is fuzzed). The budget refuses a charge above a cap, so
 * a server-chosen SCRAM iteration count cannot make one input run for long.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "fuzz_db_wire_stubs.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* ── The fake transport ──────────────────────────────────────────── */

struct HlDbTransport { int open; };
static struct HlDbTransport g_tp;

static const uint8_t *g_in;
static size_t g_in_len, g_in_off;
static size_t g_chunk = 1;   /* bytes a recv hands back at most */

void fz_wire_feed(const uint8_t *data, size_t len, size_t chunk)
{
    g_in = data;
    g_in_len = len;
    g_in_off = 0;
    g_chunk = chunk ? chunk : 1;
    g_tp.open = 1;
}

struct HlDbTransport *fz_wire_transport(void) { return &g_tp; }

struct HlDbTransport *hl_db_transport_adopt(const char *tag, void *alloc, int fd,
                                            const void *sp, char *errbuf,
                                            size_t errlen);
struct HlDbTransport *hl_db_transport_adopt(const char *tag, void *alloc, int fd,
                                            const void *sp, char *errbuf,
                                            size_t errlen)
{
    (void)tag; (void)alloc; (void)fd; (void)sp; (void)errbuf; (void)errlen;
    g_tp.open = 1;
    return &g_tp;
}

struct HlDbTransport *hl_db_transport_connect_stop(const char *tag, void *alloc,
                                                   const char *host, const char *port,
                                                   int timeout_ms, const void *sp,
                                                   void *stop, void *ud,
                                                   char *errbuf, size_t errlen);
struct HlDbTransport *hl_db_transport_connect_stop(const char *tag, void *alloc,
                                                   const char *host, const char *port,
                                                   int timeout_ms, const void *sp,
                                                   void *stop, void *ud,
                                                   char *errbuf, size_t errlen)
{
    (void)tag; (void)alloc; (void)host; (void)port; (void)timeout_ms; (void)sp;
    (void)stop; (void)ud;
    if (errbuf && errlen) snprintf(errbuf, errlen, "no network in the fuzzer");
    return NULL;
}

static ssize_t fz_read(uint8_t *buf, size_t len)
{
    if (!g_tp.open || g_in_off >= g_in_len) return 0;   /* EOF */
    size_t n = g_in_len - g_in_off;
    if (n > g_chunk) n = g_chunk;
    if (n > len) n = len;
    memcpy(buf, g_in + g_in_off, n);
    g_in_off += n;
    return (ssize_t)n;
}

ssize_t hl_db_transport_recv(struct HlDbTransport *t, uint8_t *buf, size_t len);
ssize_t hl_db_transport_recv(struct HlDbTransport *t, uint8_t *buf, size_t len)
{
    (void)t;
    return fz_read(buf, len);
}

ssize_t hl_db_transport_recv_sliced(struct HlDbTransport *t, uint8_t *buf, size_t len,
                                    int timeout_ms, void *stop, void *ud);
ssize_t hl_db_transport_recv_sliced(struct HlDbTransport *t, uint8_t *buf, size_t len,
                                    int timeout_ms, void *stop, void *ud)
{
    (void)t; (void)timeout_ms; (void)stop; (void)ud;
    return fz_read(buf, len);
}

size_t hl_db_transport_pending(struct HlDbTransport *t);
size_t hl_db_transport_pending(struct HlDbTransport *t) { (void)t; return 0; }

int hl_db_transport_wait_readable(struct HlDbTransport *t, int timeout_ms,
                                  void *stop, void *ud);
int hl_db_transport_wait_readable(struct HlDbTransport *t, int timeout_ms,
                                  void *stop, void *ud)
{
    (void)t; (void)timeout_ms; (void)stop; (void)ud;
    return 1;   /* readable: the recv then reports the data, or EOF */
}

int hl_db_transport_send_sliced(struct HlDbTransport *t, const uint8_t *buf,
                                size_t len, int timeout_ms, void *stop, void *ud);
int hl_db_transport_send_sliced(struct HlDbTransport *t, const uint8_t *buf,
                                size_t len, int timeout_ms, void *stop, void *ud)
{
    (void)buf; (void)len; (void)timeout_ms; (void)stop; (void)ud;
    return t && t->open ? 0 : -1;
}

int hl_db_transport_time_left(uint64_t *start, int timeout_ms);
int hl_db_transport_time_left(uint64_t *start, int timeout_ms)
{
    (void)start; (void)timeout_ms;
    return 0;   /* no bound */
}

int hl_db_transport_fd(const struct HlDbTransport *t);
int hl_db_transport_fd(const struct HlDbTransport *t) { (void)t; return -1; }

void hl_db_transport_set_io_timeout(struct HlDbTransport *t, int ms);
void hl_db_transport_set_io_timeout(struct HlDbTransport *t, int ms) { (void)t; (void)ms; }

int hl_db_transport_attach_tls(struct HlDbTransport *t, void *tls);
int hl_db_transport_attach_tls(struct HlDbTransport *t, void *tls)
{
    (void)t; (void)tls;
    return -1;
}

int hl_db_transport_close(struct HlDbTransport *t);
int hl_db_transport_close(struct HlDbTransport *t) { if (t) t->open = 0; return 0; }

int hl_db_transport_abort(struct HlDbTransport *t);
int hl_db_transport_abort(struct HlDbTransport *t) { if (t) t->open = 0; return 0; }

/* ── TLS: never attached ─────────────────────────────────────────── */

void *hl_tls_client_handshake_stop(int fd, const char *host, int verify,
                                   int timeout_ms, int (*stop)(void *), void *ud);
void *hl_tls_client_handshake_stop(int fd, const char *host, int verify,
                                   int timeout_ms, int (*stop)(void *), void *ud)
{
    (void)fd; (void)host; (void)verify; (void)timeout_ms; (void)stop; (void)ud;
    return NULL;
}

void hl_tls_client_free(void *c);
void hl_tls_client_free(void *c) { (void)c; }

/* ── The run's budget ────────────────────────────────────────────── */

/* A charge above this is refused, as a tripped run's would be: a SCRAM
 * iteration count the input chose is charged before the derivation runs, so
 * a large one stops there instead of running PBKDF2 for seconds. */
#define FZ_BUDGET_CAP 200000

int hl_db_budget_charge(int64_t units);
int hl_db_budget_charge(int64_t units) { return units > FZ_BUDGET_CAP; }

/* ── Crypto stand-ins (deterministic, not cryptographic) ─────────── */

static void fz_mix(const void *a, size_t alen, const void *b, size_t blen,
                   uint8_t *out, size_t outlen)
{
    uint64_t h = 1469598103934665603ULL;
    const uint8_t *p = a;
    for (size_t i = 0; i < alen; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    p = b;
    for (size_t i = 0; i < blen; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    for (size_t i = 0; i < outlen; i++) {
        h ^= i; h *= 1099511628211ULL;
        out[i] = (uint8_t)(h >> 56);
    }
}

int hl_cap_crypto_sha256(const void *data, size_t len, uint8_t out[32]);
int hl_cap_crypto_sha256(const void *data, size_t len, uint8_t out[32])
{
    fz_mix(data, len, "", 0, out, 32);
    return 0;
}

int hl_cap_crypto_sha1(const void *data, size_t len, uint8_t out[20]);
int hl_cap_crypto_sha1(const void *data, size_t len, uint8_t out[20])
{
    fz_mix(data, len, "1", 1, out, 20);
    return 0;
}

int hl_cap_crypto_hmac_sha256(const void *key, size_t key_len,
                              const void *data, size_t data_len, uint8_t out[32]);
int hl_cap_crypto_hmac_sha256(const void *key, size_t key_len,
                              const void *data, size_t data_len, uint8_t out[32])
{
    fz_mix(key, key_len, data, data_len, out, 32);
    return 0;
}

int hl_cap_crypto_ed25519_password_sign(const void *password, size_t pw_len,
                                        const uint8_t *msg, size_t msg_len,
                                        uint8_t out_sig[64]);
int hl_cap_crypto_ed25519_password_sign(const void *password, size_t pw_len,
                                        const uint8_t *msg, size_t msg_len,
                                        uint8_t out_sig[64])
{
    if (msg_len > 64) return -1;
    fz_mix(password, pw_len, msg, msg_len, out_sig, 64);
    return 0;
}

int hl_cap_crypto_random(void *buf, size_t len);
int hl_cap_crypto_random(void *buf, size_t len)
{
    memset(buf, 'r', len);   /* a fixed client nonce the fuzzer can extend */
    return 0;
}

int hl_cap_crypto_rsa_oaep_sha1_encrypt(const void *pubkey_pem, size_t pubkey_len,
                                        const void *in, size_t in_len,
                                        uint8_t *out, size_t out_size,
                                        size_t *out_len);
int hl_cap_crypto_rsa_oaep_sha1_encrypt(const void *pubkey_pem, size_t pubkey_len,
                                        const void *in, size_t in_len,
                                        uint8_t *out, size_t out_size,
                                        size_t *out_len)
{
    /* A fixed-size "ciphertext": the fuzzer drives the server's replies,
     * not the RSA math. */
    if (out_size < 256 || in_len > 214) return -1;
    fz_mix(pubkey_pem, pubkey_len, in, in_len, out, 256);
    *out_len = 256;
    return 0;
}
