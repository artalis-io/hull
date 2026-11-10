/*
 * WAMR ems allocator bounds (WAMR patch 0009, audit 44 rating).
 *
 * An app heap lives inside guest linear memory, which the guest can write, and
 * Hull builds WAMR with no guard region (WASM_DISABLE_HW_BOUND_CHECK=1). So the
 * allocator's free-list / tree metadata is guest-controlled, and its corruption
 * checks must cover every byte the allocator then touches. Before patch 0009 a
 * check covered only a node's START address: a node starting just inside the
 * heap but extending past its end passed, and the allocator read the node's
 * fields past the end of the mapping.
 *
 * The test drives WAMR's ems allocator directly. The pool ends 4 bytes before
 * a PROT_NONE page; a free-tree node is pointed at an address 8 bytes before
 * the heap end, so its header is in the heap and its child / size fields are
 * past it. The allocation runs in a child process: a fault there (the pre-fix
 * behaviour) is a test failure, not a crash of the binary.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "utest.h"

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef HL_ENABLE_WASM

/* WAMR's mem_alloc.h needs WAMR's platform include paths; these are the
 * declarations it carries (core/shared/mem-alloc/mem_alloc.h). */
typedef void *mem_allocator_t;
mem_allocator_t mem_allocator_create_with_struct_and_pool(void *struct_buf,
                                                          uint32_t struct_buf_size,
                                                          void *pool_buf,
                                                          uint32_t pool_buf_size);
int mem_allocator_destroy(mem_allocator_t allocator);
uint32_t mem_allocator_get_heap_struct_size(void);
void *mem_allocator_malloc(mem_allocator_t allocator, uint32_t size);
bool mem_allocator_is_heap_corrupted(mem_allocator_t allocator);

/* ems_gc_internal.h / ems_gc.h layout this test depends on. */
#define EMS_HEAD_PADDING 4u  /* GC_HEAD_PADDING: heap base = pool + 4 */
#define EMS_TREE_LEFT_OFF 4u /* offsetof(hmu_tree_node_t, left) */

typedef struct {
    unsigned char *map; /* one pool page + one PROT_NONE page */
    size_t map_len;
    unsigned char *pool;
    uint32_t pool_size;
    unsigned char *heap_end;
    void *heap_struct;
    mem_allocator_t alloc;
} EmsFixture;

static int
ems_fixture_init(EmsFixture *f)
{
    memset(f, 0, sizeof *f);
    long pg = sysconf(_SC_PAGESIZE);
    if (pg <= 0)
        return -1;
    f->map_len = 2 * (size_t)pg;
    void *m = mmap(NULL, f->map_len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED)
        return -1;
    f->map = m;
    unsigned char *guard = f->map + pg;
    if (mprotect(guard, (size_t)pg, PROT_NONE) != 0)
        return -1;
    /* An 8-aligned pool that ends at the guard page: the heap is
     * (pool_size - 4) & ~7 bytes from pool + 4, so it ends at guard - 4. */
    f->pool_size = 1024;
    f->pool = guard - f->pool_size;
    f->heap_end = f->pool + EMS_HEAD_PADDING
                  + ((f->pool_size - EMS_HEAD_PADDING) & ~7u);
    uint32_t ssz = mem_allocator_get_heap_struct_size();
    f->heap_struct = aligned_alloc(8, (ssz + 7) & ~7u);
    if (!f->heap_struct)
        return -1;
    f->alloc = mem_allocator_create_with_struct_and_pool(
        f->heap_struct, ssz, f->pool, f->pool_size);
    return f->alloc ? 0 : -1;
}

static void
ems_fixture_free(EmsFixture *f)
{
    if (f->alloc)
        mem_allocator_destroy(f->alloc);
    free(f->heap_struct);
    if (f->map)
        munmap(f->map, f->map_len);
}

/* The control: an untouched heap allocates. */
UTEST(wamr_audit44, ems_untouched_heap_allocates)
{
    EmsFixture f;
    ASSERT_EQ(0, ems_fixture_init(&f));
    ASSERT_TRUE(f.heap_end + 4 == f.pool + f.pool_size);
    ASSERT_TRUE(mem_allocator_malloc(f.alloc, 64) != NULL);
    ASSERT_FALSE(mem_allocator_is_heap_corrupted(f.alloc));
    ems_fixture_free(&f);
}

/* After init the whole heap is one free-tree node at the heap base (the tree
 * root's right child). Its `left` child is set to a node whose header is the
 * last 8 bytes of the heap: the start-only check passed it, and reading its
 * `size` (offset 28) touched the PROT_NONE page. Patched, the node is heap
 * corruption: the allocation returns NULL and the heap is marked corrupted. */
UTEST(wamr_audit44, ems_node_past_heap_end_is_corruption)
{
    EmsFixture f;
    ASSERT_EQ(0, ems_fixture_init(&f));

    unsigned char *base = f.pool + EMS_HEAD_PADDING;
    unsigned char *node = f.heap_end - 8;
    ASSERT_TRUE(node >= base && node < f.heap_end);
    memcpy(base + EMS_TREE_LEFT_OFF, &node, sizeof node);

    pid_t pid = fork();
    ASSERT_TRUE(pid >= 0);
    if (pid == 0) {
        void *p = mem_allocator_malloc(f.alloc, 64);
        bool corrupted = mem_allocator_is_heap_corrupted(f.alloc);
        _exit(p == NULL && corrupted ? 0 : 3);
    }
    int status = 0;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    /* Pre-fix: the child dies on SIGSEGV / SIGBUS (or an ASan report). */
    EXPECT_FALSE(WIFSIGNALED(status));
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(0, WEXITSTATUS(status));

    ems_fixture_free(&f);
}

#else /* !HL_ENABLE_WASM */

UTEST(wamr_audit44, disabled_placeholder)
{
    ASSERT_TRUE(1);
}

#endif /* HL_ENABLE_WASM */

UTEST_MAIN()
