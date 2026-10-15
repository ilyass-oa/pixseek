#include "bmp.h"
#include "verbose.h"
#include <stdio.h>
#include <stdlib.h>

#define MAX_BMP_BYTES (4u * 1024u * 1024u)
#define MAX_PIXELS (1024u * 1024u)

static uint16_t u16(const unsigned char *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t u32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

/* Decode bounded, uncompressed 24/32-bit BMPs into top-down RGB pixels. */
bool InitBmpFromRaw(RgbImage *image, const char raw[], unsigned int size) {
  if (image == NULL)
    return false;
  *image = (RgbImage){0};
  if (raw == NULL || size < 54 || size > MAX_BMP_BYTES)
    return false;
  const unsigned char *p = (const unsigned char *)raw;
  if (p[0] != 'B' || p[1] != 'M')
    return false;
  uint32_t declared = u32(p + 2), offset = u32(p + 10), dib = u32(p + 14);
  uint32_t width = u32(p + 18), height_bits = u32(p + 22);
  int64_t signed_height = height_bits < 0x80000000u
                              ? (int64_t)height_bits
                              : (int64_t)height_bits - 0x100000000LL;
  uint64_t height =
      signed_height < 0 ? (uint64_t)-signed_height : (uint64_t)signed_height;
  uint16_t bpp = u16(p + 28);
  if (dib < 40 || dib > size - 14 || offset < 14u + dib || offset > size ||
      width == 0 || width > 4096 || height == 0 || height > 4096 ||
      (uint64_t)width * height > MAX_PIXELS || u16(p + 26) != 1 ||
      (bpp != 24 && bpp != 32) || u32(p + 30) != 0)
    return false;
  uint64_t stride = (((uint64_t)width * bpp + 31) / 32) * 4;
  uint64_t end = offset + stride * height;
  if (end > size || declared > size || (declared != 0 && declared < end))
    return false;
  uint32_t *pixels = calloc((size_t)width * (size_t)height, sizeof(*pixels));
  if (pixels == NULL)
    return false;
  for (uint32_t y = 0; y < height; ++y) {
    uint32_t source_y = signed_height < 0 ? y : (uint32_t)height - 1 - y;
    const unsigned char *row = p + offset + (size_t)source_y * (size_t)stride;
    for (uint32_t x = 0; x < width; ++x) {
      const unsigned char *pixel = row + x * (bpp / 8);
      pixels[(size_t)y * width + x] = (uint32_t)pixel[0] |
                                      ((uint32_t)pixel[1] << 8) |
                                      ((uint32_t)pixel[2] << 16);
    }
  }
  image->width = (uint16_t)width;
  image->height = (uint16_t)height;
  image->data = pixels;
  return true;
}

bool LoadBmp(RgbImage *image, const char path[]) {
  if (image == NULL)
    return false;
  *image = (RgbImage){0};
  if (path == NULL)
    return false;
  FILE *file = fopen(path, "rb");
  if (file == NULL)
    return false;
  bool ok = false;
  if (fseek(file, 0, SEEK_END) == 0) {
    long length = ftell(file);
    if (length >= 54 && length <= MAX_BMP_BYTES &&
        fseek(file, 0, SEEK_SET) == 0) {
      char *bytes = malloc((size_t)length);
      if (bytes != NULL) {
        if (fread(bytes, 1, (size_t)length, file) == (size_t)length)
          ok = InitBmpFromRaw(image, bytes, (unsigned int)length);
        free(bytes);
      }
    }
  }
  fclose(file);
  return ok;
}

void FreeImage(RgbImage *image) {
  if (image != NULL) {
    free(image->data);
    *image = (RgbImage){0};
  }
}

void DisplayBMP(RgbImage *image) {
  if (!MODE_VERBOSE_ON || image == NULL || image->data == NULL)
    return;
  for (uint32_t y = 0; y < image->height; ++y) {
    for (uint32_t x = 0; x < image->width; ++x) {
      uint32_t c = image->data[(size_t)y * image->width + x];
      printf("\x1b[38;2;%u;%u;%um X", (c >> 16) & 255u, (c >> 8) & 255u,
             c & 255u);
    }
    putchar('\n');
  }
  printf("\x1b[0m\n");
}
