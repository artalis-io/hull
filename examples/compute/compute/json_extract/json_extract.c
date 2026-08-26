/*
 * json_extract.c - Extract a value by key from JSON-like input
 *
 * Input format:
 *   1 byte    - uint8: key length
 *   N bytes   - key string (no null terminator)
 *   remaining - JSON object string (e.g. {"name":"alice","age":30})
 *
 * Output: the extracted value as a string (without quotes for strings,
 *         raw text for numbers/booleans). Empty output if key not found.
 *
 * Limitations: flat objects only (no nested objects/arrays). This is a
 * teaching example showing string processing in freestanding WASM.
 *
 * Build:  hull compute build json_extract
 * Test:   hull compute test json_extract
 */

#include "hull_compute.h"

HULL_VERSION_EXPORT

HULL_EXPORT
int32_t hull_process(const void *in_ptr, int32_t in_len,
                     void *out_ptr, int32_t out_max)
{
    const unsigned char *in = (const unsigned char *)in_ptr;
    unsigned char *out = (unsigned char *)out_ptr;

    if (in_len < 2) return HULL_ERR_INPUT;

    uint32_t key_len = in[0];
    if (key_len == 0) return HULL_ERR_INPUT;
    if ((int32_t)(1 + key_len) >= in_len) return HULL_ERR_INPUT;

    const char *key  = (const char *)(in + 1);
    const char *json = (const char *)(in + 1 + key_len);
    int32_t json_len = in_len - 1 - (int32_t)key_len;

    /* Search for "key": pattern */
    for (int32_t i = 0; i < json_len - (int32_t)key_len - 3; i++) {
        if (json[i] != '"') continue;
        if (hull_memcmp(json + i + 1, key, key_len) != 0) continue;
        if (json[i + 1 + (int32_t)key_len] != '"') continue;

        /* Found the key - look for colon */
        int32_t after_key = i + 2 + (int32_t)key_len;
        /* Skip whitespace before colon */
        while (after_key < json_len &&
               (json[after_key] == ' ' || json[after_key] == '\t'))
            after_key++;
        if (after_key >= json_len || json[after_key] != ':') continue;

        /* Skip colon and whitespace */
        int32_t val_start = after_key + 1;
        while (val_start < json_len &&
               (json[val_start] == ' ' || json[val_start] == '\t'))
            val_start++;

        if (val_start >= json_len) return HULL_ERR_INPUT;

        if (json[val_start] == '"') {
            /* String value - find closing quote */
            int32_t val_end = val_start + 1;
            while (val_end < json_len && json[val_end] != '"')
                val_end++;
            int32_t val_len = val_end - val_start - 1;
            if (val_len < 0) return HULL_ERR_INPUT;
            if (val_len > out_max) return HULL_ERR_OUTPUT;
            hull_memcpy(out, json + val_start + 1, (size_t)val_len);
            return val_len;
        } else {
            /* Numeric/bool - read until delimiter */
            int32_t val_end = val_start;
            while (val_end < json_len && json[val_end] != ',' &&
                   json[val_end] != '}' && json[val_end] != ' ' &&
                   json[val_end] != '\n')
                val_end++;
            int32_t val_len = val_end - val_start;
            if (val_len <= 0) return HULL_ERR_INPUT;
            if (val_len > out_max) return HULL_ERR_OUTPUT;
            hull_memcpy(out, json + val_start, (size_t)val_len);
            return val_len;
        }
    }

    /* Key not found - zero bytes output */
    return 0;
}
