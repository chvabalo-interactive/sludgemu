#pragma once

#include <dxgiformat.h>

#include "common/types.h"

// Console GPU image formats.
//
// How the console GPU reads a format through an image descriptor: one of its image formats (the format
// field of an RDNA 2 image descriptor; 0 when the format has none here) and, per channel a view returns
// (red, green, blue, alpha), the image format channel it takes or a constant. Image formats name their
// fields from the most significant bits down and number channels from the least significant up (X is
// the lowest), so formats keeping blue in their low bits take their channels swizzled.
enum ImageSelect : u8 {
    IMAGE_SELECT_ZERO = 0,
    IMAGE_SELECT_ONE = 1,
    IMAGE_SELECT_X = 4,
    IMAGE_SELECT_Y = 5,
    IMAGE_SELECT_Z = 6,
    IMAGE_SELECT_W = 7,
};

struct ImageFormat {
    u32 format;
    u8 select[4]; // ImageSelect per channel: red, green, blue, alpha
};

ImageFormat format_image(DXGI_FORMAT format);
