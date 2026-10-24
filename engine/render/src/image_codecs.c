/* stb_image's JPEG decoder and PNG decoder, for decode_image and decode_gray16 (image.cpp), and
   a 16-bit gray PNG encoder on stb_image_write's zlib compressor, for encode_gray16_png. They
   work with integer math and no SIMD, so a file decodes and encodes to the same bytes on every
   machine. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_SIMD
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#include "image_codecs.h"

#include <stdlib.h>
#include <string.h>

unsigned char* slash_decode_jpeg(const unsigned char* bytes, int size, int* width, int* height) {
    int channels = 0;
    return stbi_load_from_memory(bytes, size, width, height, &channels, 4);
}

unsigned short* slash_decode_gray16(const unsigned char* bytes, int size, int* width,
                                    int* height) {
    int channels = 0;
    return stbi_load_16_from_memory(bytes, size, width, height, &channels, 1);
}

const char* slash_image_error(void) {
    return stbi_failure_reason();
}

void slash_free_image(void* pixels) {
    stbi_image_free(pixels);
}

/* PNG's CRC-32 of the bytes, continued from crc (0 to start). */
static unsigned int png_crc(unsigned int crc, const unsigned char* bytes, size_t size) {
    static unsigned int table[256];
    static int made = 0;
    if (!made) {
        for (unsigned int n = 0; n < 256; ++n) {
            unsigned int c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        made = 1;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ bytes[i]) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}

static void put_u32(unsigned char* at, unsigned int value) {
    at[0] = (unsigned char)(value >> 24);
    at[1] = (unsigned char)(value >> 16);
    at[2] = (unsigned char)(value >> 8);
    at[3] = (unsigned char)value;
}

/* Appends a chunk of the type and data at out, returning where it ends. */
static unsigned char* put_chunk(unsigned char* out, const char* type, const unsigned char* data,
                                unsigned int size) {
    put_u32(out, size);
    memcpy(out + 4, type, 4);
    if (size > 0) {
        memcpy(out + 8, data, size);
    }
    put_u32(out + 8 + size, png_crc(0, out + 4, (size_t)size + 4));
    return out + 12 + size;
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a);
    int pb = abs(p - b);
    int pc = abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

unsigned char* slash_encode_gray16_png(const unsigned short* pixels, int width, int height,
                                       int* size) {
    const size_t row = (size_t)width * 2;
    unsigned char* raw = (unsigned char*)malloc(row * (size_t)height);
    unsigned char* filtered = (unsigned char*)malloc((row + 1) * (size_t)height);
    unsigned char* line = (unsigned char*)malloc(row);
    if (raw == NULL || filtered == NULL || line == NULL) {
        free(raw);
        free(filtered);
        free(line);
        return NULL;
    }
    for (size_t i = 0; i < (size_t)width * (size_t)height; ++i) {
        raw[i * 2] = (unsigned char)(pixels[i] >> 8);
        raw[(i * 2) + 1] = (unsigned char)(pixels[i] & 0xFFU);
    }
    /* Each row takes the filter whose bytes add up least as signed values, the heuristic PNG's
       specification suggests; two bytes a pixel. */
    for (int y = 0; y < height; ++y) {
        const unsigned char* now = raw + (row * (size_t)y);
        const unsigned char* up = y > 0 ? now - row : NULL;
        unsigned char* out = filtered + ((row + 1) * (size_t)y);
        long best = -1;
        for (int filter = 0; filter < 5; ++filter) {
            long sum = 0;
            for (size_t x = 0; x < row; ++x) {
                const int a = x >= 2 ? now[x - 2] : 0;
                const int b = up != NULL ? up[x] : 0;
                const int c = x >= 2 && up != NULL ? up[x - 2] : 0;
                int predicted = 0;
                switch (filter) {
                case 1: predicted = a; break;
                case 2: predicted = b; break;
                case 3: predicted = (a + b) / 2; break;
                case 4: predicted = paeth(a, b, c); break;
                default: break;
                }
                line[x] = (unsigned char)(now[x] - predicted);
                sum += labs((long)(signed char)line[x]);
            }
            if (best < 0 || sum < best) {
                best = sum;
                out[0] = (unsigned char)filter;
                memcpy(out + 1, line, row);
            }
        }
    }
    free(raw);
    free(line);
    int compressed_size = 0;
    unsigned char* compressed =
        stbi_zlib_compress(filtered, (int)((row + 1) * (size_t)height), &compressed_size, 8);
    free(filtered);
    if (compressed == NULL) {
        return NULL;
    }
    unsigned char* png = (unsigned char*)malloc(8 + 25 + 12 + (size_t)compressed_size + 12);
    if (png == NULL) {
        STBIW_FREE(compressed);
        return NULL;
    }
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    memcpy(png, signature, 8);
    unsigned char header[13];
    put_u32(header, (unsigned int)width);
    put_u32(header + 4, (unsigned int)height);
    header[8] = 16; /* bits a sample */
    header[9] = 0;  /* gray */
    header[10] = 0; /* deflate */
    header[11] = 0; /* adaptive filters */
    header[12] = 0; /* not interlaced */
    unsigned char* end = put_chunk(png + 8, "IHDR", header, 13);
    end = put_chunk(end, "IDAT", compressed, (unsigned int)compressed_size);
    end = put_chunk(end, "IEND", NULL, 0);
    STBIW_FREE(compressed);
    *size = (int)(end - png);
    return png;
}
