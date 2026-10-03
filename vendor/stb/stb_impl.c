#include <stddef.h>
#include <stdlib.h>

/* Hull: the most one stb_image allocation may be, for the decode running on
 * this thread (0 = no cap); image_stb.c sets it from the image header.
 * stb inflates a PNG's zlib stream into a buffer it keeps doubling, with no
 * bound of its own, so a few kilobytes of IDAT could ask for gigabytes
 * before any dimension check applied. */
_Thread_local size_t hl_stb_alloc_cap;

static void *hl_stb_malloc(size_t n)
{
    if (hl_stb_alloc_cap && n > hl_stb_alloc_cap) return NULL;
    return malloc(n);
}

static void *hl_stb_realloc(void *p, size_t n)
{
    if (hl_stb_alloc_cap && n > hl_stb_alloc_cap) return NULL;
    return realloc(p, n);
}

#define STBI_MALLOC(sz)           hl_stb_malloc(sz)
#define STBI_REALLOC(p, newsz)    hl_stb_realloc(p, newsz)
#define STBI_FREE(p)              free(p)

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
