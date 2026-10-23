#pragma once

// stb_image's JPEG and PNG decoders and a 16-bit gray PNG encoder (image_codecs.c), kept to one C
// file so the headers are compiled once.
#ifdef __cplusplus
extern "C" {
#endif

// The file's pixels as RGBA, width by height, or null if it isn't a JPEG file it can read
// (slash_image_error says why). Free them with slash_free_image.
unsigned char* slash_decode_jpeg(const unsigned char* bytes, int size, int* width, int* height);
// A PNG file's pixels as one 16-bit gray value each, 8-bit samples widened (v * 257) and colors
// made gray by their luminance, or null if it isn't a PNG file it can read. Free them with
// slash_free_image.
unsigned short* slash_decode_gray16(const unsigned char* bytes, int size, int* width, int* height);
const char* slash_image_error(void);
void slash_free_image(void* pixels);
// A 16-bit gray PNG file of the pixels, *size bytes long, or null if memory runs out. Free it
// with free().
unsigned char* slash_encode_gray16_png(const unsigned short* pixels, int width, int height,
                                       int* size);

#ifdef __cplusplus
}
#endif
