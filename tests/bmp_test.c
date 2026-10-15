#include "bmp.h"
#include "pHash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned int checks;
#define CHECK(expr)                                                            \
  do {                                                                         \
    ++checks;                                                                  \
    if (!(expr)) {                                                             \
      fprintf(stderr, "Failed: %s (line %d)\n", #expr, __LINE__);              \
      return 1;                                                                \
    }                                                                          \
  } while (0)
static void le32(unsigned char *p, unsigned int n) {
  p[0] = n;
  p[1] = n >> 8;
  p[2] = n >> 16;
  p[3] = n >> 24;
}
static void fixture(unsigned char data[70]) {
  memset(data, 0, 70);
  data[0] = 'B';
  data[1] = 'M';
  le32(data + 2, 70);
  le32(data + 10, 54);
  le32(data + 14, 40);
  le32(data + 18, 2);
  le32(data + 22, 2);
  data[26] = 1;
  data[28] = 24;
  /* Bottom row: blue, white. Top row: red, green. Two padding bytes per row. */
  unsigned char pixels[16] = {255, 0, 0,   255, 255, 255, 77, 88,
                              0,   0, 255, 0,   255, 0,   99, 11};
  memcpy(data + 54, pixels, 16);
}
int main(void) {
  unsigned char data[70];
  fixture(data);
  RgbImage image = {0};
  CHECK(InitBmpFromRaw(&image, (const char *)data, sizeof(data)));
  CHECK(image.width == 2 && image.height == 2);
  CHECK(image.data[0] == 0xff0000);
  CHECK(image.data[1] == 0x00ff00);
  CHECK(image.data[2] == 0x0000ff);
  CHECK(image.data[3] == 0xffffff);
  uint64_t hash;
  CHECK(PHashRaw((const char *)data, sizeof(data), &hash));
  FreeImage(&image);
  FreeImage(&image);
  CHECK(image.data == NULL);
  /* A top-down file containing the same pixels must yield the same hash. */
  le32(data + 22, (unsigned int)-2);
  unsigned char row[8];
  memcpy(row, data + 54, 8);
  memcpy(data + 54, data + 62, 8);
  memcpy(data + 62, row, 8);
  uint64_t top_hash;
  CHECK(PHashRaw((const char *)data, sizeof(data), &top_hash));
  CHECK(top_hash == hash);
  /* 32-bit BGRX, including nonzero alpha bytes, has the same RGB pixels. */
  fixture(data);
  data[28] = 32;
  unsigned char bgra[16] = {255, 0, 0,   123, 255, 255, 255, 7,
                            0,   0, 255, 8,   0,   255, 0,   255};
  memcpy(data + 54, bgra, sizeof(bgra));
  uint64_t rgba_hash;
  CHECK(PHashRaw((const char *)data, sizeof(data), &rgba_hash));
  CHECK(rgba_hash == hash);
  for (unsigned int n = 0; n < 70; ++n) {
    CHECK(!InitBmpFromRaw(&image, (const char *)data, n));
    CHECK(image.data == NULL);
  }
  /* Header validation before allocating or indexing pixels. */
  unsigned int offsets[] = {0, 2, 10, 14, 18, 22, 26, 28, 30};
  unsigned int invalid[] = {0,          69, 0,  0xffffffff, 0xffffffff,
                            0x80000000, 0,  16, 1};
  for (unsigned int i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i) {
    fixture(data);
    le32(data + offsets[i], invalid[i]);
    CHECK(!InitBmpFromRaw(&image, (const char *)data, sizeof(data)));
    CHECK(image.data == NULL);
  }
  fixture(data);
  le32(data + 18, 100000);
  CHECK(!InitBmpFromRaw(&image, (const char *)data, sizeof(data)));
  CHECK(DistancePHash(0, UINT64_MAX) == 64);
  CHECK(DistancePHash(hash, hash) == 0);
  CHECK(!PHashRaw(NULL, 0, &hash));
  CHECK(!PHashRaw((const char *)data, sizeof(data), NULL));
  CHECK(!InitBmpFromRaw(NULL, (const char *)data, sizeof(data)));
  CHECK(!LoadBmp(&image, NULL));
  CHECK(!PHash(NULL, &hash));
  /* Deterministic malformed inputs exercise short buffers and random headers.
   */
  unsigned int seed = 42;
  unsigned char fuzz[256];
  for (int i = 0; i < 2000; ++i) {
    for (unsigned int j = 0; j < sizeof(fuzz); ++j) {
      seed = seed * 1664525u + 1013904223u;
      fuzz[j] = seed >> 24;
    }
    CHECK(!InitBmpFromRaw(&image, (const char *)fuzz,
                          (unsigned int)i % sizeof(fuzz)));
    CHECK(image.data == NULL);
  }
  printf("BMP/hash checks passed: %u\n", checks);
  return 0;
}
