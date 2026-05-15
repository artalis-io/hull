/*
 * commands/sign_release.c — `hull sign-release` subcommand
 *
 * Sign the `hull.sha256` release manifest with an Ed25519 secret key.
 * Used by the GitHub Actions release workflow; not typically invoked by
 * end users. See `docs/release_signing.md`.
 *
 *   Usage: hull sign-release <manifest_path> --key <secret_key_path>
 *
 * Output: writes <manifest_path>.sig containing 128 hex chars + newline.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "hull/commands/sign_release.h"
#include "hull/release.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr,
        "Usage: hull sign-release <manifest_path> --key <secret_key_path> [--output <path>]\n"
        "\n"
        "  Signs the contents of <manifest_path> with the Ed25519 key in\n"
        "  <secret_key_path> (the .key file from `hull keygen`). Writes the\n"
        "  signature to <manifest_path>.sig as 128 hex chars + newline\n"
        "  (override with --output).\n");
}

static void secure_zero(volatile void *p, size_t n)
{
    volatile unsigned char *q = (volatile unsigned char *)p;
    while (n--) *q++ = 0;
}

static int read_file(const char *path, char **out_buf, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "hull sign-release: cannot open %s\n", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return -1; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }

    char *buf = (char *)malloc((size_t)n);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)n, f);
    /* L3: capture read error before fclose. */
    int read_err = ferror(f);
    fclose(f);
    if (read_err || got != (size_t)n) { free(buf); return -1; }

    *out_buf = buf;
    *out_len = (size_t)n;
    return 0;
}

int hl_cmd_sign_release(int argc, char **argv, const HlCommandEnv *env)
{
    (void)env;

    const char *manifest_path = NULL;
    const char *key_path      = NULL;
    const char *output_path   = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--key") == 0 && i + 1 < argc) {
            key_path = argv[++i];
        } else if (strncmp(argv[i], "--key=", 6) == 0) {
            key_path = argv[i] + 6;
        } else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strncmp(argv[i], "--output=", 9) == 0) {
            output_path = argv[i] + 9;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "hull sign-release: unknown flag %s\n", argv[i]);
            usage();
            return 1;
        } else if (!manifest_path) {
            manifest_path = argv[i];
        } else {
            fprintf(stderr, "hull sign-release: extra positional argument %s\n", argv[i]);
            usage();
            return 1;
        }
    }

    if (!manifest_path || !key_path) {
        usage();
        return 1;
    }

    /* Read manifest */
    char *manifest = NULL;
    size_t manifest_len = 0;
    if (read_file(manifest_path, &manifest, &manifest_len) != 0)
        return 1;

    /* Load secret key (hex file) */
    uint8_t sk[64];
    if (hl_release_load_secret_key(key_path, sk) != 0) {
        fprintf(stderr, "hull sign-release: invalid secret key file %s\n", key_path);
        free(manifest);
        return 1;
    }

    /* Sign */
    char sig_hex[129];
    int rc = hl_release_sign_manifest(manifest, manifest_len, sk,
                                      sig_hex, sizeof(sig_hex));
    secure_zero(sk, sizeof(sk));
    free(manifest);
    if (rc != 0) {
        fprintf(stderr, "hull sign-release: signing failed\n");
        return 1;
    }

    /* Write output: <manifest>.sig or --output */
    char default_out[1024];
    if (!output_path) {
        int n = snprintf(default_out, sizeof(default_out), "%s.sig", manifest_path);
        if (n < 0 || (size_t)n >= sizeof(default_out)) {
            fprintf(stderr, "hull sign-release: manifest path too long\n");
            return 1;
        }
        output_path = default_out;
    }

    FILE *f = fopen(output_path, "w");
    if (!f) {
        fprintf(stderr, "hull sign-release: cannot write %s\n", output_path);
        return 1;
    }
    if (fprintf(f, "%s\n", sig_hex) < 0) {
        fclose(f);
        fprintf(stderr, "hull sign-release: write failed: %s\n", output_path);
        return 1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "hull sign-release: close failed: %s\n", output_path);
        return 1;
    }

    printf("wrote %s (signature for %s)\n", output_path, manifest_path);
    return 0;
}
