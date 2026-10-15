#ifndef PIXSEEK_BMP_H
#define PIXSEEK_BMP_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint16_t width;
  uint16_t height;
  uint32_t *data;
} RgbImage;

/* On success the caller owns data; release it with FreeImage.
 * Pass an empty image, or free its previous contents before loading again.
 * On failure image is empty. Pixels are top-down 0xRRGGBB.
 */
bool LoadBmp(RgbImage *image, const char file[]);
bool InitBmpFromRaw(RgbImage *image, const char rawImageBytes[],
                    unsigned int size);
void FreeImage(RgbImage *image);
void DisplayBMP(RgbImage *image);
#endif
