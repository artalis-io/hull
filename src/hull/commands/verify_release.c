/*
 * commands/verify_release.c — `hull verify-release` subcommand
 *
 * Verify an Ed25519 signature over a release manifest using the embedded
 * release public key (HL_RELEASE_PUBKEY_HEX) or an explicit --pubkey
 * override. Mainly useful for offline auditing of releases.
 *
 *   Usage: hull verify-release <manifest_path> <signature_path>
 *                              [--pubkey <hex>]
 *
 * Exit 0 = signature valid, 1 = invalid / error.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/verify_release.h"
#include "hull/release.h"
#include "hull/cap/crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr,
        "Usage: hull verify-release <manifest_path> <signature_path> [--pubkey <hex>]\n"
        "\n"
        "  Verifies that <signature_path> is a valid Ed25519 signature over\n"
        "  <manifest_path> using the embedded release public key (or the\n"
        "  --pubkey <hex> override, 64 hex chars).\n"
        "\n"
        "  Exit code: 0 = valid, 1 = invalid / error.\n");
}

static int read_file(const char *path, char **out_buf, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "hull verify-release: cannot open %s\n", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return -1; }
    buf[got] = '\0';

    *out_buf = buf;
    *out_len = got;
    return 0;
}

int hl_cmd_verify_release(int argc, char **argv, const HlCommandEnv *env)
{
    (void)env;

    const char *manifest_path = NULL;
    const char *sig_path      = NULL;
    const char *pubkey_hex    = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pubkey") == 0 && i + 1 < argc) {
            pubkey_hex = argv[++i];
        } else if (strncmp(argv[i], "--pubkey=", 9) == 0) {
            pubkey_hex = argv[i] + 9;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "hull verify-release: unknown flag %s\n", argv[i]);
            usage();
            return 1;
        } else if (!manifest_path) {
            manifest_path = argv[i];
        } else if (!sig_path) {
            sig_path = argv[i];
        } else {
            fprintf(stderr, "hull verify-release: extra positional argument %s\n", argv[i]);
            usage();
            return 1;
        }
    }

    if (!manifest_path || !sig_path) {
        usage();
        return 1;
    }

    /* Resolve pubkey: either --pubkey override or the embedded one. */
    uint8_t pubkey_buf[32];
    const uint8_t *pubkey = NULL;
    if (pubkey_hex) {
        if (hl_cap_crypto_hex_decode(pubkey_hex, strlen(pubkey_hex), pubkey_buf, 32) != 32) {
            fprintf(stderr, "hull verify-release: --pubkey must be 64 hex chars\n");
            return 1;
        }
        pubkey = pubkey_buf;
    } else {
        if (!hl_release_pubkey_configured()) {
            fprintf(stderr,
                "hull verify-release: this build has no embedded release public key\n"
                "                     (HL_RELEASE_PUBKEY_HEX is the placeholder).\n"
                "                     Pass --pubkey <hex> to verify against a specific key.\n");
            return 1;
        }
        /* leave pubkey = NULL — release.c resolves the embedded key */
    }

    /* Read manifest + signature */
    char *manifest = NULL, *sig = NULL;
    size_t manifest_len = 0, sig_len = 0;
    if (read_file(manifest_path, &manifest, &manifest_len) != 0)
        return 1;
    if (read_file(sig_path, &sig, &sig_len) != 0) {
        free(manifest);
        return 1;
    }

    int rc = hl_release_verify_manifest_sig(manifest, manifest_len,
                                            sig, sig_len, pubkey);
    free(manifest);
    free(sig);

    if (rc != 0) {
        fprintf(stderr, "hull verify-release: INVALID signature\n");
        return 1;
    }
    printf("hull verify-release: OK\n");
    return 0;
}
