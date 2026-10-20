#include <setjmp.h>
#include <stddef.h>
#include <stdlib.h>

/* Hull: the most stb_image may hold at once - the SUM of its live
 * allocations - for the decode running on this thread (0 = no cap);
 * image_stb.c sets it from the image header. stb inflates a PNG's zlib
 * stream into a buffer it keeps doubling, with no bound of its own, so a
 * few kilobytes of IDAT could ask for gigabytes before any dimension check
 * applied. A cap on each allocation alone (audit 10) still let a decode hold
 * several cap-sized buffers at once - a progressive JPEG keeps coefficient,
 * component and output buffers together, ~3x the cap (audit 12) - so every
 * block carries its size and the live total is what is capped. */
_Thread_local size_t hl_stb_alloc_cap;
/* Bytes stb_image holds live on this thread (headers included). */
_Thread_local size_t hl_stb_live;

typedef union {
    size_t      n;
    max_align_t align;   /* keep the block malloc-aligned */
} HlStbHdr;

static void *hl_stb_malloc(size_t n)
{
    if (n > (size_t)-1 - sizeof(HlStbHdr)) return NULL;
    size_t total = n + sizeof(HlStbHdr);
    if (hl_stb_alloc_cap &&
        (total > hl_stb_alloc_cap || hl_stb_live > hl_stb_alloc_cap - total))
        return NULL;
    HlStbHdr *h = (HlStbHdr *)malloc(total);
    if (!h) return NULL;
    h->n = total;
    hl_stb_live += total;
    return h + 1;
}

static void hl_stb_free(void *p)
{
    if (!p) return;
    HlStbHdr *h = (HlStbHdr *)p - 1;
    hl_stb_live = hl_stb_live >= h->n ? hl_stb_live - h->n : 0;
    free(h);
}

static void *hl_stb_realloc(void *p, size_t n)
{
    if (!p) return hl_stb_malloc(n);
    if (n > (size_t)-1 - sizeof(HlStbHdr)) return NULL;
    HlStbHdr *h = (HlStbHdr *)p - 1;
    size_t old = h->n;
    size_t total = n + sizeof(HlStbHdr);
    /* realloc may move the block: old and new are live together, so the
     * check counts both (what the process really holds at the peak). */
    if (hl_stb_alloc_cap &&
        (total > hl_stb_alloc_cap || hl_stb_live > hl_stb_alloc_cap - total))
        return NULL;
    HlStbHdr *nh = (HlStbHdr *)realloc(h, total);
    if (!nh) return NULL;
    nh->n = total;
    hl_stb_live = (hl_stb_live >= old ? hl_stb_live - old : 0) + total;
    return nh + 1;
}

#define STBI_MALLOC(sz)           hl_stb_malloc(sz)
#define STBI_REALLOC(p, newsz)    hl_stb_realloc(p, newsz)
#define STBI_FREE(p)              hl_stb_free(p)

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
/* Hull: only the formats Hull offers (PNG, JPEG, BMP). stb otherwise also
 * parses GIF, PSD, PIC, PNM and TGA - TGA has no magic, so an explicit
 * format reached parsers Hull never meant to expose. */
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
/* Hull: a backstop for image_stb.c's header check - stb refuses a side
 * past HL_IMAGE_MAX_DIM itself (stb's default is 2^24). */
#define STBI_MAX_DIMENSIONS 65536
#include "stb_image.h"

/* Hull: stb_image_write through the same capped allocator, so an encode's
 * working set is bounded too (image_stb.c sets the cap; audit 18).
 *
 * stb_image_write does not survive a failed allocation: its growable
 * buffers (stbiw__sbgrowf, the PNG zlib stream) STBIW_ASSERT the new block
 * and push into the old one regardless, so a refused growth was an abort()
 * - or, built with NDEBUG, a write past the block (audit 19). So every
 * STBIW block is also linked into a per-thread list, a failed STBIW_ASSERT
 * longjmps back to the hl_stbiw_*_to_func wrapper below instead of
 * aborting, and the wrapper frees what the encode still held. */
typedef union HlStbwLink {
    struct { union HlStbwLink *prev, *next; } l;
    max_align_t align;   /* keep the block malloc-aligned */
} HlStbwLink;

static _Thread_local HlStbwLink *hl_stbw_head;
static _Thread_local jmp_buf    *hl_stbw_jmp;

static void hl_stbw_link(HlStbwLink *b)
{
    b->l.prev = NULL;
    b->l.next = hl_stbw_head;
    if (hl_stbw_head) hl_stbw_head->l.prev = b;
    hl_stbw_head = b;
}

static void hl_stbw_unlink(HlStbwLink *b)
{
    if (b->l.prev) b->l.prev->l.next = b->l.next;
    else hl_stbw_head = b->l.next;
    if (b->l.next) b->l.next->l.prev = b->l.prev;
}

static void *hl_stbw_malloc(size_t n)
{
    if (n > (size_t)-1 - sizeof(HlStbwLink)) return NULL;
    HlStbwLink *b = (HlStbwLink *)hl_stb_malloc(n + sizeof *b);
    if (!b) return NULL;
    hl_stbw_link(b);
    return b + 1;
}

static void hl_stbw_free(void *p)
{
    if (!p) return;
    HlStbwLink *b = (HlStbwLink *)p - 1;
    hl_stbw_unlink(b);
    hl_stb_free(b);
}

static void *hl_stbw_realloc(void *p, size_t n)
{
    if (!p) return hl_stbw_malloc(n);
    if (n > (size_t)-1 - sizeof(HlStbwLink)) return NULL;
    HlStbwLink *b = (HlStbwLink *)p - 1;
    hl_stbw_unlink(b);
    HlStbwLink *nb = (HlStbwLink *)hl_stb_realloc(b, n + sizeof *b);
    if (!nb) {
        hl_stbw_link(b);   /* still live; freed by the caller or the wrapper */
        return NULL;
    }
    hl_stbw_link(nb);
    return nb + 1;
}

/* A failed STBIW_ASSERT: back to the wrapper, which reports the encode
 * failed. Outside one (nothing else calls stb_image_write) it aborts, as
 * stb's own assert did. */
static void hl_stbw_fail(void)
{
    if (hl_stbw_jmp) longjmp(*hl_stbw_jmp, 1);
    abort();
}

#define STBIW_MALLOC(sz)          hl_stbw_malloc(sz)
#define STBIW_REALLOC(p, newsz)   hl_stbw_realloc(p, newsz)
#define STBIW_FREE(p)             hl_stbw_free(p)
#define STBIW_ASSERT(x)           ((x) ? (void)0 : hl_stbw_fail())

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

/* Free every STBIW block still linked: what an encode held when it failed. */
static void hl_stbw_release_all(void)
{
    while (hl_stbw_head) {
        HlStbwLink *b = hl_stbw_head;
        hl_stbw_head = b->l.next;
        hl_stb_free(b);
    }
}

/* stbi_write_png_to_func / stbi_write_jpg_to_func that return 0 instead of
 * aborting when stb_image_write cannot get the memory it needs (the cap is
 * hl_stb_alloc_cap), with everything it allocated freed. */
int hl_stbiw_png_to_func(stbi_write_func *func, void *context, int w, int h,
                         int comp, const void *data, int stride_bytes)
{
    jmp_buf jb;
    jmp_buf *volatile saved = hl_stbw_jmp;
    volatile int rc = 0;
    if (setjmp(jb) == 0) {
        hl_stbw_jmp = &jb;
        rc = stbi_write_png_to_func(func, context, w, h, comp, data, stride_bytes);
    } else {
        rc = 0;
        hl_stbw_release_all();
    }
    hl_stbw_jmp = saved;
    return rc;
}

int hl_stbiw_jpg_to_func(stbi_write_func *func, void *context, int w, int h,
                         int comp, const void *data, int quality)
{
    jmp_buf jb;
    jmp_buf *volatile saved = hl_stbw_jmp;
    volatile int rc = 0;
    if (setjmp(jb) == 0) {
        hl_stbw_jmp = &jb;
        rc = stbi_write_jpg_to_func(func, context, w, h, comp, data, quality);
    } else {
        rc = 0;
        hl_stbw_release_all();
    }
    hl_stbw_jmp = saved;
    return rc;
}
