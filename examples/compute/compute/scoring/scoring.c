/*
 * scoring.c - Feature normalization + weighted scoring
 *
 * Input format:
 *   4 bytes           - uint32 LE: number of features N
 *   N×4 bytes         - float32 LE: raw feature values
 *   N×4 bytes         - float32 LE: weights
 *
 * Output: 4 bytes float32 LE (weighted sum of min-max normalized features)
 *
 * Min-max normalization maps each feature to [0,1] range:
 *   normalized = (value - min) / (max - min)
 * If all values are equal, normalized = 0.5.
 *
 * Build:  hull compute build scoring
 * Test:   hull compute test scoring
 */

#include "hull_compute.h"

HULL_VERSION_EXPORT

HULL_EXPORT
int32_t hull_process(const void *in_ptr, int32_t in_len,
                     void *out_ptr, int32_t out_max)
{
    if (out_max < 4) return HULL_ERR_OUTPUT;
    if (in_len < 4) return HULL_ERR_INPUT;

    const unsigned char *in = (const unsigned char *)in_ptr;

    /* Read feature count */
    uint32_t n = (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
                 ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);

    if (n == 0) return HULL_ERR_INPUT;

    int32_t expected = 4 + (int32_t)(n * 4 * 2);
    if (in_len < expected) return HULL_ERR_INPUT;

    const float *values  = (const float *)(in + 4);
    const float *weights = (const float *)(in + 4 + n * 4);

    /* Find min/max of feature values */
    float min_val = values[0];
    float max_val = values[0];
    for (uint32_t i = 1; i < n; i++) {
        if (values[i] < min_val) min_val = values[i];
        if (values[i] > max_val) max_val = values[i];
    }

    float range = max_val - min_val;

    /* Weighted sum of normalized features */
    float score = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        float norm;
        if (range <= 0.0f)
            norm = 0.5f;  /* all values equal */
        else
            norm = (values[i] - min_val) / range;
        score += norm * weights[i];
    }

    /* Write float32 LE output */
    unsigned char *out = (unsigned char *)out_ptr;
    hull_memcpy(out, &score, 4);
    return 4;
}
