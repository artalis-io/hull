/* test_tls_mbedtls_multi_ctx.c - regression for v2.5.1 audit finding F4.
 *
 * The deep mbedTLS allocator seal (Keel v2.5.0) installs a process-
 * wide calloc/free hook the first time any client TLS ctx is created.
 * After that, every mbedtls_free call routes through
 * kl_mbed_arena_free which walks a registry of live arena ranges.
 * The audit flagged that the multi-context create/destroy ordering
 * deserves a regression test under ASan - this is it.
 *
 * Three scenarios:
 *   1. Two no-CA-verify client contexts created in order, destroyed
 *      in reverse (LIFO) order.
 *   2. Same, destroyed in forward (FIFO) order - exercises mid-list
 *      unregister in the registry walk.
 *   3. Interleaved create / destroy / create - confirms the hook
 *      stays installed and the registry handles add-after-remove.
 *
 * Lives in Hull's test tree because mbedTLS objects are linked into
 * Hull builds; Keel's own test rule links libkeel only.  Run under
 * `make debug && make test` to exercise ASan + UBSan over the
 * registry walk and the destroy paths. */

#include "utest.h"
#include <keel/tls.h>
#include <keel/tls_mbedtls.h>
#include <keel/allocator.h>

UTEST(tls_multi_ctx, two_clients_destroyed_lifo)
{
    KlAllocator a = kl_allocator_default();

    KlTlsCtx *ctx1 = kl_tls_mbedtls_client_ctx_create(NULL, &a);
    ASSERT_TRUE(ctx1 != NULL);

    KlTlsCtx *ctx2 = kl_tls_mbedtls_client_ctx_create(NULL, &a);
    ASSERT_TRUE(ctx2 != NULL);

    kl_tls_mbedtls_ctx_destroy(ctx2);
    kl_tls_mbedtls_ctx_destroy(ctx1);
}

UTEST(tls_multi_ctx, two_clients_destroyed_fifo)
{
    KlAllocator a = kl_allocator_default();

    KlTlsCtx *ctx1 = kl_tls_mbedtls_client_ctx_create(NULL, &a);
    ASSERT_TRUE(ctx1 != NULL);

    KlTlsCtx *ctx2 = kl_tls_mbedtls_client_ctx_create(NULL, &a);
    ASSERT_TRUE(ctx2 != NULL);

    /* ctx1 was registered first → its node sits deeper in the
     * arena registry's linked list when ctx2's head node is removed
     * next.  Tests mid-list unregister. */
    kl_tls_mbedtls_ctx_destroy(ctx1);
    kl_tls_mbedtls_ctx_destroy(ctx2);
}

UTEST(tls_multi_ctx, interleaved_create_destroy_create)
{
    KlAllocator a = kl_allocator_default();

    /* Create A, destroy A, create B.  The global hook stays
     * installed forever (kl_mbed_hook_install_once is one-way);
     * B's create must still work - registry add after remove. */
    KlTlsCtx *ctx_a = kl_tls_mbedtls_client_ctx_create(NULL, &a);
    ASSERT_TRUE(ctx_a != NULL);
    kl_tls_mbedtls_ctx_destroy(ctx_a);

    KlTlsCtx *ctx_b = kl_tls_mbedtls_client_ctx_create(NULL, &a);
    ASSERT_TRUE(ctx_b != NULL);
    kl_tls_mbedtls_ctx_destroy(ctx_b);
}

UTEST(tls_multi_ctx, single_client_create_destroy)
{
    /* Sanity baseline - guards against the case where the basic
     * single-ctx path silently breaks and the multi-ctx tests pass
     * for the wrong reason. */
    KlAllocator a = kl_allocator_default();
    KlTlsCtx *ctx = kl_tls_mbedtls_client_ctx_create(NULL, &a);
    ASSERT_TRUE(ctx != NULL);
    kl_tls_mbedtls_ctx_destroy(ctx);
}

UTEST_MAIN();
