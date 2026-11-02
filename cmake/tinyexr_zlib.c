/* The zlib tinyexr reads and writes EXR files with: stb_image's inflate and stb_image_write's
   deflate, kept static here and given to tinyexr under names of their own, so they meet no other
   copy of stb. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

int slash_exr_zlib_decode(char* out, int out_length, const char* in, int in_length);
unsigned char* slash_exr_zlib_compress(unsigned char* data, int length, int* out_length,
                                       int quality);

int slash_exr_zlib_decode(char* out, int out_length, const char* in, int in_length) {
    return stbi_zlib_decode_buffer(out, out_length, in, in_length);
}

unsigned char* slash_exr_zlib_compress(unsigned char* data, int length, int* out_length,
                                       int quality) {
    return stbi_zlib_compress(data, length, out_length, quality);
}
