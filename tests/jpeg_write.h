#pragma once

// stb_image_write's JPEG encoder (jpeg_write.c), for tests that read JPEG files.
#ifdef __cplusplus
extern "C" {
#endif

// Encodes width by height RGBA pixels as a JPEG file of quality 1 to 100, handing its bytes to
// write in pieces. Returns 0 on failure.
int slash_test_write_jpeg(const unsigned char* rgba, int width, int height, int quality,
                          void (*write)(void* context, void* data, int size), void* context);

#ifdef __cplusplus
}
#endif
