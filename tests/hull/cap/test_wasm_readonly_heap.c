/*
 * Hull-tree C-API regression test for the read-only shared-heap enforcement,
 * exercised THROUGH HULL'S OWN WAMR BUILD/CONFIGURATION (not the WAMR unit-test
 * config). It uses the public WAMR C-API directly (Hull's cap layer does not
 * expose the read_only permission) after initialising the runtime via
 * hl_cap_wasm_init, so it proves, under normal `make test` and ASan/UBSan:
 *   - a READ from a read-only pre-allocated shared heap succeeds;
 *   - an interpreter WRITE traps recoverably (call fails, exception set);
 *   - an AOT WRITE traps recoverably (when a wamrc-built .aot fixture is present);
 *   - the backing bytes remain unchanged after a trap;
 *   - a SUBSEQUENT invocation in the SAME process succeeds (host survived);
 *   - a WRITABLE shared heap remains writable.
 *
 * The .wasm fixture (store_i32/load_i32) is embedded; the matching .aot is
 * generated at build time by the Hull-built wamrc into gen_ro_heap_aot.h (arch +
 * OS correct). When wamrc/LLVM is unavailable the header defines an empty
 * fixture and the AOT case is SKIPPED (interp + e2e-compute + the WAMR-unit AOT
 * matrix still cover AOT); the interpreter case always runs.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"

#include "hull/cap/wasm.h"
#include "wasm_export.h"
#include "gen_ro_heap_aot.h" /* build-generated: ro_heap_aot[] + ro_heap_aot_len */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

UTEST_MAIN();

/* store_i32(p): *p = 0x11223344 ;  load_i32(p) -> *p */
static const unsigned char ro_heap_wasm[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x02, 0x60,
    0x01, 0x7f, 0x00, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x03, 0x03, 0x02, 0x00,
    0x01, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x21, 0x03, 0x06, 0x6d, 0x65,
    0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x09, 0x73, 0x74, 0x6f, 0x72, 0x65,
    0x5f, 0x69, 0x33, 0x32, 0x00, 0x00, 0x08, 0x6c, 0x6f, 0x61, 0x64, 0x5f,
    0x69, 0x33, 0x32, 0x00, 0x01, 0x0a, 0x17, 0x02, 0x0d, 0x00, 0x20, 0x00,
    0x41, 0xc4, 0xe6, 0x88, 0x89, 0x01, 0x36, 0x02, 0x00, 0x0b, 0x07, 0x00,
    0x20, 0x00, 0x28, 0x02, 0x00, 0x0b
};
static const unsigned int ro_heap_wasm_len = 90;

enum { CALL_OK = 0, CALL_TRAP = 1, CALL_SETUP_FAIL = -1 };

/* Load `buf`, attach `heap`, call `func(argv[0])`, detach, tear down. Returns
 * CALL_OK on a clean run, CALL_TRAP on a WASM trap (exception copied into exc),
 * CALL_SETUP_FAIL on a harness/load error. */
static int
call_with_heap(const unsigned char *buf, unsigned int len, wasm_shared_heap_t heap,
               const char *func, uint32_t *argv, uint32_t argc, char *exc,
               size_t exc_sz)
{
    char err[128] = { 0 };
    int result = CALL_SETUP_FAIL;
    /* WAMR's fast interpreter rewrites module bytes in place, so the loader needs
     * a MUTABLE buffer that outlives the module -- the embedded fixtures are const
     * (.rodata). Copy to a heap buffer freed at teardown. */
    uint8_t *mbuf = (uint8_t *)malloc(len);
    if (!mbuf)
        return CALL_SETUP_FAIL;
    memcpy(mbuf, buf, len);
    wasm_module_t mod = wasm_runtime_load(mbuf, len, err, sizeof(err));
    if (!mod) {
        free(mbuf);
        return CALL_SETUP_FAIL;
    }
    wasm_module_inst_t inst =
        wasm_runtime_instantiate(mod, 16 * 1024, 64 * 1024, err, sizeof(err));
    if (!inst) {
        wasm_runtime_unload(mod);
        return CALL_SETUP_FAIL;
    }
    wasm_exec_env_t env = wasm_runtime_create_exec_env(inst, 16 * 1024);
    if (env) {
        if (!heap || wasm_runtime_attach_shared_heap(inst, heap)) {
            wasm_function_inst_t f = wasm_runtime_lookup_function(inst, func);
            if (f) {
                if (wasm_runtime_call_wasm(env, f, argc, argv)) {
                    result = CALL_OK;
                }
                else {
                    result = CALL_TRAP;
                    const char *s = wasm_runtime_get_exception(inst);
                    if (exc && exc_sz) {
                        exc[0] = '\0';
                        if (s)
                            strncpy(exc, s, exc_sz - 1);
                    }
                }
            }
            if (heap)
                wasm_runtime_detach_shared_heap(inst);
        }
        wasm_runtime_destroy_exec_env(env);
    }
    wasm_runtime_deinstantiate(inst);
    wasm_runtime_unload(mod);
    free(mbuf);
    return result;
}

/* One full matrix against a fixture (interp .wasm or AOT .aot). */
static void
run_readonly_matrix(int *utest_result,
                    const unsigned char *fx, unsigned int fx_len,
                    const char *label)
{
    long pg = sysconf(_SC_PAGESIZE);
    uint32_t size = (pg > 0 && pg <= 65536) ? (uint32_t)pg : 4096u;
    uint32_t start = 0xFFFFFFFFu - size + 1u;
    uint32_t mid = start + 64u;
    char exc[128];

    /* --- read-only heap over a caller-owned buffer --- */
    unsigned char *ro_buf = (unsigned char *)calloc(size, 1);
    ASSERT_TRUE(ro_buf != NULL);
    SharedHeapInitArgs ro_args;
    memset(&ro_args, 0, sizeof(ro_args));
    ro_args.pre_allocated_addr = ro_buf;
    ro_args.size = size;
    ro_args.read_only = true;
    wasm_shared_heap_t ro = wasm_runtime_create_shared_heap(&ro_args);
    ASSERT_TRUE(ro != NULL);

    uint32_t argv[1];

    /* read succeeds */
    argv[0] = mid;
    ASSERT_EQ(CALL_OK, call_with_heap(fx, fx_len, ro, "load_i32", argv, 1, exc,
                                      sizeof(exc)));

    /* write traps recoverably, with an out-of-bounds exception */
    argv[0] = mid;
    ASSERT_EQ(CALL_TRAP, call_with_heap(fx, fx_len, ro, "store_i32", argv, 1,
                                        exc, sizeof(exc)));
    ASSERT_TRUE(strstr(exc, "out of bounds") != NULL);

    /* backing bytes unchanged by the trapped write */
    for (uint32_t i = 0; i < size; i++)
        ASSERT_EQ(ro_buf[i], 0);

    /* a SUBSEQUENT invocation in the same process succeeds (host survived) */
    argv[0] = mid;
    ASSERT_EQ(CALL_OK, call_with_heap(fx, fx_len, ro, "load_i32", argv, 1, exc,
                                      sizeof(exc)));

    /* --- writable heap stays writable --- */
    unsigned char *rw_buf = (unsigned char *)calloc(size, 1);
    ASSERT_TRUE(rw_buf != NULL);
    SharedHeapInitArgs rw_args;
    memset(&rw_args, 0, sizeof(rw_args));
    rw_args.pre_allocated_addr = rw_buf;
    rw_args.size = size; /* read_only defaults false */
    wasm_shared_heap_t rw = wasm_runtime_create_shared_heap(&rw_args);
    ASSERT_TRUE(rw != NULL);

    argv[0] = mid;
    ASSERT_EQ(CALL_OK, call_with_heap(fx, fx_len, rw, "store_i32", argv, 1, exc,
                                      sizeof(exc)));
    ASSERT_EQ(rw_buf[64], 0x44); /* 0x11223344 little-endian */
    ASSERT_EQ(rw_buf[65], 0x33);

    free(ro_buf);
    free(rw_buf);
    (void)label;
}

UTEST(wasm_readonly_heap, interpreter)
{
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    run_readonly_matrix(utest_result, ro_heap_wasm, ro_heap_wasm_len, "interp");
    hl_cap_wasm_destroy(&cache);
}

UTEST(wasm_readonly_heap, aot)
{
    if (ro_heap_aot_len == 0) {
        /* wamrc/LLVM absent in this build leg: AOT covered by e2e-compute + the
         * WAMR-unit AOT matrix. The interpreter case above still runs here. */
        UTEST_SKIP("no wamrc-built .aot fixture in this build leg");
        return;
    }
    HlWasmCache cache;
    ASSERT_EQ(hl_cap_wasm_init(&cache), 0);
    run_readonly_matrix(utest_result, ro_heap_aot, ro_heap_aot_len, "aot");
    hl_cap_wasm_destroy(&cache);
}
