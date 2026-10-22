#pragma once

// stb_image's JPEG decoder (jpeg.c), kept to one C file so its header is compiled once.
#ifdef __cplusplus
extern "C" {
#endif

// The file's pixels as RGBA, width by height, or null if it isn't a JPEG file it can read
// (slash_jpeg_error says why). Free them with slash_free_jpeg.
unsigned char* slash_decode_jpeg(const unsigned char* bytes, int size, int* width, int* height);
const char* slash_jpeg_error(void);
void slash_free_jpeg(unsigned char* pixels);

#ifdef __cplusplus
}
#endif
