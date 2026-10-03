#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_HDR
/* Hull: a backstop for image_stb.c's header check - stb refuses a side
 * past HL_IMAGE_MAX_DIM itself (stb's default is 2^24). */
#define STBI_MAX_DIMENSIONS 65536
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
