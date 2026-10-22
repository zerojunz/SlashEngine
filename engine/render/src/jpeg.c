/* stb_image's JPEG decoder, for decode_image (image.cpp). It decodes with integer math and no
   SIMD, so a JPEG file gives the same pixels on every machine. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_SIMD
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"

#include "jpeg.h"

unsigned char* slash_decode_jpeg(const unsigned char* bytes, int size, int* width, int* height) {
    int channels = 0;
    return stbi_load_from_memory(bytes, size, width, height, &channels, 4);
}

const char* slash_jpeg_error(void) {
    return stbi_failure_reason();
}

void slash_free_jpeg(unsigned char* pixels) {
    stbi_image_free(pixels);
}
