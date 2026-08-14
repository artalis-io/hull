/*
 * Freestanding libc (#327) C-level test. The canonical compute SDK header
 * (hull_compute.h, embedded in stdlib/cli/lua/hull/compute.lua) supplies bare
 * memcpy / memset / memmove so that clang's *implicit* lowering of struct
 * copies, block initializers, and runtime-length byte loops resolves inside the
 * module instead of becoming an undefined `env.memcpy` import that traps at the
 * first call ("failed to call unlinked import function (env, memcpy)").
 *
 * The memops fixture (tests/fixtures/compute/memops.c, built from the canonical
 * header by build_memops.sh) exercises all three ops via BOTH compiler-generated
 * lowering AND direct calls, and lays its result out so a caller can verify each
 * path. This drives it through Hull's cap layer (interpreter) at representative
 * sizes; a successful call is itself proof there is no unresolved libc import
 * (WAMR would fail instantiation otherwise), and the byte checks prove the ops
 * are correct and non-recursive (a self-lowered memcpy would blow the stack).
 * The AOT leg + an explicit objdump import scan on the real `hull compute build`
 * output live in tests/e2e_compute_memops.sh.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"

#include "hull/cap/wasm.h"
#include "hull/vfs.h"
#include "hull/entry.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* memops_wasm[] + memops_wasm_len — xxd of tests/fixtures/compute/memops.wasm,
 * generated at build time so the embedded bytes never drift from the fixture. */
#include "gen_memops_wasm.h"

#define BLK 64
#define OUT_LEN (39 + BLK)

static const HlEntry memops_entries[] = {
    { "compute/memops.wasm", memops_wasm, memops_wasm_len },
    { 0, 0, 0 }
};

/* Expected bytes [0..38] — the input-independent part of the fixture's output.
 * Each region is one memmove/memcpy/memset case (see tests/fixtures/compute/
 * memops.c). Kept as literals so the test independently pins the semantics:
 *   fwd overlap  memmove(a+2,a,6) on {0..7}      -> {0,1,0,1,2,3,4,5}
 *   bwd overlap  memmove(b,b+2,6) on {0..7}      -> {2,3,4,5,6,7,6,7}
 *   identical    memmove(c,c,4)   on {9,8,7,6}   -> {9,8,7,6}
 *   zero length  memmove(d+1,d,0) on {5,6}       -> {5,6}   (no write)
 *   memcpy       memcpy(dst+1,src+2,5), src[i]=i*11+1, dst=0 -> {0,23,34,45,56,67,0,0,0}
 *   memset       memset(e+1,0xAB,5) on e=0x11    -> {0x11,0xAB*5,0x11,0x11} */
static const unsigned char expected_fixed[39] = {
    0, 1, 0, 1, 2, 3, 4, 5,             /* [0..7]   memmove fwd overlap  */
    2, 3, 4, 5, 6, 7, 6, 7,             /* [8..15]  memmove bwd overlap  */
    9, 8, 7, 6,                         /* [16..19] memmove identical    */
    5, 6,                               /* [20..21] memmove zero length  */
    0, 23, 34, 45, 56, 67, 0, 0, 0,     /* [22..30] memcpy misaligned    */
    0x11, 0xAB, 0xAB, 0xAB, 0xAB, 0xAB, 0x11, 0x11 /* [31..38] memset misaligned */
};

/* Run the fixture once for an input of length L and assert the whole contract.
 * Takes utest_result so the ASSERT_* macros (which write through it) work from
 * this helper, not just a UTEST body. */
static void run_one(int *utest_result, HlWasmCache *cache, HlVfs *vfs, int L)
{
    unsigned char input[BLK * 2];
    int in_len = L;
    if (in_len > (int)sizeof(input)) in_len = (int)sizeof(input);
    for (int i = 0; i < in_len; i++)
        input[i] = (unsigned char)((L * 7 + i * 13 + 1) & 0xff);

    void *output = NULL;
    size_t output_len = 0;
    const char *err = NULL;
    int rc = hl_cap_wasm_call(cache, "memops",
                              input, (size_t)in_len,
                              &output, &output_len,
                              NULL, NULL, NULL,
                              vfs, NULL, NULL, &err);
    ASSERT_EQ_MSG(rc, 0, err ? err : "call failed");
    ASSERT_EQ(output_len, (size_t)OUT_LEN);
    ASSERT_NE(output, NULL);

    const unsigned char *o = (const unsigned char *)output;

    /* Input-independent memmove/memcpy/memset cases. */
    for (int i = 0; i < 39; i++)
        ASSERT_EQ(o[i], expected_fixed[i]);

    /* Compiler-generated copy(prefix)+fill(pad) of the input. */
    int n = in_len < BLK ? in_len : BLK;
    for (int i = 0; i < BLK; i++) {
        unsigned char want = (i < n) ? input[i] : 0;
        ASSERT_EQ(o[39 + i], want);
    }

    free(output);
}

UTEST(hl_cap_wasm_memops, direct_and_compiler_generated)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);

    HlVfs vfs;
    hl_vfs_init(&vfs, memops_entries, NULL);

    /* Representative sizes for the compiler-generated block: empty, sub-word,
     * word, odd tail, exact block, and over-block (clamped). The fixed memmove/
     * memcpy/memset cases (both overlap directions, identical pointers, zero
     * length, misaligned src/dst) run on every call. */
    static const int sizes[] = { 0, 1, 3, 7, 8, 15, 32, 63, 64, 100 };
    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++)
        run_one(utest_result, &cache, &vfs, sizes[k]);

    hl_cap_wasm_destroy(&cache);
}

UTEST_MAIN();
