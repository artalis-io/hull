#ifndef MEM_HISTOGRAM_NATIVE_H
#define MEM_HISTOGRAM_NATIVE_H

#include <stddef.h>
#include <stdint.h>

/* Identical algorithm to mem_histogram.wat: histogram + counting sort.
 * Returns bytes written (= in_len) or -2 if output too small. */
int32_t mem_histogram_native(const void *in, size_t in_len,
                             void *out, size_t out_max);

#endif
