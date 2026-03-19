#ifndef COMPUTE_HASH_NATIVE_H
#define COMPUTE_HASH_NATIVE_H

#include <stddef.h>
#include <stdint.h>

/* Identical algorithm to compute_hash.wat: iterative hash compression.
 * Returns bytes written (32) or -2 if output too small. */
int32_t compute_hash_native(const void *in, size_t in_len,
                            void *out, size_t out_max);

#endif
