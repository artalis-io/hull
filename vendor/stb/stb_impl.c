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

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
