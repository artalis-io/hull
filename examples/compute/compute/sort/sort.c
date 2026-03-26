/*
 * sort.c — In-place quicksort of int32 array
 *
 * Input:  N×4 bytes — array of int32 LE values
 * Output: N×4 bytes — sorted array of int32 LE values
 *
 * Build:  hull compute build sort
 * Test:   hull compute test sort
 */

#include "hull_compute.h"

static void swap(int32_t *a, int32_t *b)
{
    int32_t t = *a;
    *a = *b;
    *b = t;
}

static void quicksort(int32_t *arr, int32_t lo, int32_t hi)
{
    if (lo >= hi) return;

    int32_t pivot = arr[hi];
    int32_t i = lo;
    for (int32_t j = lo; j < hi; j++) {
        if (arr[j] <= pivot) {
            swap(&arr[i], &arr[j]);
            i++;
        }
    }
    swap(&arr[i], &arr[hi]);
    quicksort(arr, lo, i - 1);
    quicksort(arr, i + 1, hi);
}

HULL_VERSION_EXPORT

HULL_EXPORT
int32_t hull_process(const void *in_ptr, int32_t in_len,
                     void *out_ptr, int32_t out_max)
{
    if (in_len == 0) return 0;
    if (in_len % 4 != 0) return HULL_ERR_INPUT;
    if (out_max < in_len) return HULL_ERR_OUTPUT;

    int32_t count = in_len / 4;
    hull_memcpy(out_ptr, in_ptr, (size_t)in_len);
    quicksort((int32_t *)out_ptr, 0, count - 1);
    return in_len;
}
