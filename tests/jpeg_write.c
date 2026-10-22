/* stb_image_write's JPEG encoder, so tests make the JPEG files they read rather than commit
   them. */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#include "jpeg_write.h"

int slash_test_write_jpeg(const unsigned char* rgba, int width, int height, int quality,
                          void (*write)(void* context, void* data, int size), void* context) {
    return stbi_write_jpg_to_func(write, context, width, height, 4, rgba, quality);
}
