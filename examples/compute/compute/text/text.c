/*
 * text.c - Levenshtein edit distance between two strings
 *
 * Input format:
 *   2 bytes   - uint16 LE: length of string A
 *   len_a bytes - string A
 *   2 bytes   - uint16 LE: length of string B
 *   len_b bytes - string B
 *
 * Output: 4 bytes int32 LE (edit distance)
 *
 * Uses the output buffer as scratch space for the two-row DP matrix.
 * Maximum string length is limited by the output buffer size.
 *
 * Build:  hull compute build text
 * Test:   hull compute test text
 */

#include "hull_compute.h"

/* Suppress unused-variable warnings for the hull arena we don't use */
#define HULL_TEXT_SCRATCH 1

HULL_VERSION_EXPORT

HULL_EXPORT
int32_t hull_process(const void *in_ptr, int32_t in_len,
                     void *out_ptr, int32_t out_max)
{
    if (out_max < 4) return HULL_ERR_OUTPUT;
    if (in_len < 4) return HULL_ERR_INPUT;

    const unsigned char *in = (const unsigned char *)in_ptr;

    /* Read string A length */
    uint32_t len_a = (uint32_t)in[0] | ((uint32_t)in[1] << 8);
    if (in_len < (int32_t)(2 + len_a + 2)) return HULL_ERR_INPUT;

    const char *str_a = (const char *)(in + 2);

    /* Read string B length */
    uint32_t off_b = 2 + len_a;
    uint32_t len_b = (uint32_t)in[off_b] | ((uint32_t)in[off_b + 1] << 8);
    if (in_len < (int32_t)(off_b + 2 + len_b)) return HULL_ERR_INPUT;

    const char *str_b = (const char *)(in + off_b + 2);

    /* Edge cases */
    int32_t distance;
    if (len_a == 0) {
        distance = (int32_t)len_b;
        goto write_output;
    }
    if (len_b == 0) {
        distance = (int32_t)len_a;
        goto write_output;
    }

    /*
     * Two-row DP using the output buffer as scratch space.
     * We need 2 × (len_b + 1) × 4 bytes of scratch.
     * The final 4 bytes of output overwrite the scratch at the end.
     */
    {
        uint32_t cols = len_b + 1;
        int32_t scratch_needed = (int32_t)(cols * 2 * sizeof(int32_t));
        if (out_max < scratch_needed) return HULL_ERR_OUTPUT;

        int32_t *prev = (int32_t *)out_ptr;
        int32_t *curr = prev + cols;

        /* Initialize first row */
        for (uint32_t j = 0; j <= len_b; j++)
            prev[j] = (int32_t)j;

        /* Fill matrix */
        for (uint32_t i = 1; i <= len_a; i++) {
            curr[0] = (int32_t)i;
            for (uint32_t j = 1; j <= len_b; j++) {
                int32_t cost = (str_a[i - 1] == str_b[j - 1]) ? 0 : 1;
                int32_t del = prev[j] + 1;
                int32_t ins = curr[j - 1] + 1;
                int32_t sub = prev[j - 1] + cost;

                int32_t m = del;
                if (ins < m) m = ins;
                if (sub < m) m = sub;
                curr[j] = m;
            }
            /* Swap rows */
            int32_t *tmp = prev;
            prev = curr;
            curr = tmp;
        }

        distance = prev[len_b];
    }

write_output:;
    unsigned char *out = (unsigned char *)out_ptr;
    out[0] = (unsigned char)(distance & 0xFF);
    out[1] = (unsigned char)((distance >> 8) & 0xFF);
    out[2] = (unsigned char)((distance >> 16) & 0xFF);
    out[3] = (unsigned char)((distance >> 24) & 0xFF);
    return 4;
}
