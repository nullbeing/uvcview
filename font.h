#ifndef UVCVIEW_FONT_H
#define UVCVIEW_FONT_H

#include <cstdint>

// ASCII 0x20-0x7E, 8x16, bit 7 is the leftmost pixel.
extern const uint8_t font8x16[95][16];

#define FONT_8X16_WIDTH 8
#define FONT_8X16_HEIGHT 16

#endif
