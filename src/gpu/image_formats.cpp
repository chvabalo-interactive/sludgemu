#include "gpu/image_formats.h"

#include "gpu/formats.h"

// RDNA 2 image formats (the values of the image descriptor's format field) that console formats read
// through. Buffer formats share the values up to 77.
enum : u32 {
    IMG_8_UNORM = 1,
    IMG_8_SNORM = 2,
    IMG_8_UINT = 5,
    IMG_8_SINT = 6,
    IMG_16_UNORM = 7,
    IMG_16_SNORM = 8,
    IMG_16_UINT = 11,
    IMG_16_SINT = 12,
    IMG_16_FLOAT = 13,
    IMG_8_8_UNORM = 14,
    IMG_8_8_SNORM = 15,
    IMG_8_8_UINT = 18,
    IMG_8_8_SINT = 19,
    IMG_32_UINT = 20,
    IMG_32_SINT = 21,
    IMG_32_FLOAT = 22,
    IMG_16_16_UNORM = 23,
    IMG_16_16_SNORM = 24,
    IMG_16_16_UINT = 27,
    IMG_16_16_SINT = 28,
    IMG_16_16_FLOAT = 29,
    IMG_10_11_11_FLOAT = 36,
    IMG_2_10_10_10_UNORM = 50,
    IMG_2_10_10_10_UINT = 54,
    IMG_8_8_8_8_UNORM = 56,
    IMG_8_8_8_8_SNORM = 57,
    IMG_8_8_8_8_UINT = 60,
    IMG_8_8_8_8_SINT = 61,
    IMG_32_32_UINT = 62,
    IMG_32_32_SINT = 63,
    IMG_32_32_FLOAT = 64,
    IMG_16_16_16_16_UNORM = 65,
    IMG_16_16_16_16_SNORM = 66,
    IMG_16_16_16_16_UINT = 69,
    IMG_16_16_16_16_SINT = 70,
    IMG_16_16_16_16_FLOAT = 71,
    IMG_32_32_32_UINT = 72,
    IMG_32_32_32_SINT = 73,
    IMG_32_32_32_FLOAT = 74,
    IMG_32_32_32_32_UINT = 75,
    IMG_32_32_32_32_SINT = 76,
    IMG_32_32_32_32_FLOAT = 77,
    IMG_8_8_8_8_SRGB = 0x82,
    IMG_5_9_9_9_FLOAT = 0x84,
    IMG_5_6_5_UNORM = 0x85,
    IMG_1_5_5_5_UNORM = 0x86,
    IMG_4_4_4_4_UNORM = 0x88,
    IMG_4_4_UNORM = 0x89,
    IMG_BC1_UNORM = 0xA9,
    IMG_BC1_SRGB = 0xAA,
    IMG_BC2_UNORM = 0xAB,
    IMG_BC2_SRGB = 0xAC,
    IMG_BC3_UNORM = 0xAD,
    IMG_BC3_SRGB = 0xAE,
    IMG_BC4_UNORM = 0xAF,
    IMG_BC4_SNORM = 0xB0,
    IMG_BC5_UNORM = 0xB1,
    IMG_BC5_SNORM = 0xB2,
    IMG_BC6_UFLOAT = 0xB3,
    IMG_BC6_SFLOAT = 0xB4,
    IMG_BC7_UNORM = 0xB5,
    IMG_BC7_SRGB = 0xB6,
};

// Which image format channel each view channel takes.
enum ChannelOrder {
    ORDER_RGBA,    // red in the lowest bits; channels the format lacks read 0, alpha 1
    ORDER_BGRA,    // blue in the lowest bits
    ORDER_BGRX,    // blue in the lowest bits, no alpha
    ORDER_ALPHA,   // alpha alone
    ORDER_STENCIL, // a stencil plane, which views read in green
};

static ImageFormat image(u32 format, u32 channels, ChannelOrder order = ORDER_RGBA) {
    ImageFormat f = {format, {IMAGE_SELECT_ZERO, IMAGE_SELECT_ZERO, IMAGE_SELECT_ZERO, IMAGE_SELECT_ONE}};
    switch (order) {
    case ORDER_RGBA:
        for (u32 c = 0; c < channels; c++)
            f.select[c] = (u8)(IMAGE_SELECT_X + c);
        break;
    case ORDER_BGRA:
    case ORDER_BGRX:
        f.select[0] = IMAGE_SELECT_Z;
        f.select[1] = IMAGE_SELECT_Y;
        f.select[2] = IMAGE_SELECT_X;
        if (order == ORDER_BGRA)
            f.select[3] = IMAGE_SELECT_W;
        break;
    case ORDER_ALPHA:
        f.select[3] = IMAGE_SELECT_X;
        break;
    case ORDER_STENCIL:
        f.select[1] = IMAGE_SELECT_X;
        break;
    }
    return f;
}

ImageFormat format_image(DXGI_FORMAT f) {
    switch ((u32)f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return image(IMG_32_32_32_32_FLOAT, 4);
    case DXGI_FORMAT_R32G32B32A32_UINT:
        return image(IMG_32_32_32_32_UINT, 4);
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return image(IMG_32_32_32_32_SINT, 4);
    case DXGI_FORMAT_R32G32B32_FLOAT:
        return image(IMG_32_32_32_FLOAT, 3);
    case DXGI_FORMAT_R32G32B32_UINT:
        return image(IMG_32_32_32_UINT, 3);
    case DXGI_FORMAT_R32G32B32_SINT:
        return image(IMG_32_32_32_SINT, 3);
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return image(IMG_16_16_16_16_FLOAT, 4);
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        return image(IMG_16_16_16_16_UNORM, 4);
    case DXGI_FORMAT_R16G16B16A16_UINT:
        return image(IMG_16_16_16_16_UINT, 4);
    case DXGI_FORMAT_R16G16B16A16_SNORM:
        return image(IMG_16_16_16_16_SNORM, 4);
    case DXGI_FORMAT_R16G16B16A16_SINT:
        return image(IMG_16_16_16_16_SINT, 4);
    case DXGI_FORMAT_R32G32_FLOAT:
        return image(IMG_32_32_FLOAT, 2);
    case DXGI_FORMAT_R32G32_UINT:
        return image(IMG_32_32_UINT, 2);
    case DXGI_FORMAT_R32G32_SINT:
        return image(IMG_32_32_SINT, 2);
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return image(IMG_2_10_10_10_UNORM, 4);
    case DXGI_FORMAT_R10G10B10A2_UINT:
        return image(IMG_2_10_10_10_UINT, 4);
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return image(IMG_10_11_11_FLOAT, 3);
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return image(IMG_8_8_8_8_UNORM, 4);
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return image(IMG_8_8_8_8_SRGB, 4);
    case DXGI_FORMAT_R8G8B8A8_UINT:
        return image(IMG_8_8_8_8_UINT, 4);
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        return image(IMG_8_8_8_8_SNORM, 4);
    case DXGI_FORMAT_R8G8B8A8_SINT:
        return image(IMG_8_8_8_8_SINT, 4);
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return image(IMG_8_8_8_8_UNORM, 4, ORDER_BGRA);
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return image(IMG_8_8_8_8_SRGB, 4, ORDER_BGRA);
    case DXGI_FORMAT_B8G8R8X8_UNORM:
        return image(IMG_8_8_8_8_UNORM, 4, ORDER_BGRX);
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return image(IMG_8_8_8_8_SRGB, 4, ORDER_BGRX);
    case DXGI_FORMAT_R16G16_FLOAT:
        return image(IMG_16_16_FLOAT, 2);
    case DXGI_FORMAT_R16G16_UNORM:
        return image(IMG_16_16_UNORM, 2);
    case DXGI_FORMAT_R16G16_UINT:
        return image(IMG_16_16_UINT, 2);
    case DXGI_FORMAT_R16G16_SNORM:
        return image(IMG_16_16_SNORM, 2);
    case DXGI_FORMAT_R16G16_SINT:
        return image(IMG_16_16_SINT, 2);
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
        return image(IMG_32_FLOAT, 1);
    case DXGI_FORMAT_R32_UINT:
        return image(IMG_32_UINT, 1);
    case DXGI_FORMAT_R32_SINT:
        return image(IMG_32_SINT, 1);
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
        return image(IMG_5_9_9_9_FLOAT, 3);
    case DXGI_FORMAT_R8G8_UNORM:
        return image(IMG_8_8_UNORM, 2);
    case DXGI_FORMAT_R8G8_UINT:
        return image(IMG_8_8_UINT, 2);
    case DXGI_FORMAT_R8G8_SNORM:
        return image(IMG_8_8_SNORM, 2);
    case DXGI_FORMAT_R8G8_SINT:
        return image(IMG_8_8_SINT, 2);
    case DXGI_FORMAT_R16_FLOAT:
        return image(IMG_16_FLOAT, 1);
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case CONSOLE_FORMAT_D16_UNORM_S8_UINT:
    case CONSOLE_FORMAT_R16_UNORM_X8_TYPELESS:
        return image(IMG_16_UNORM, 1);
    case DXGI_FORMAT_R16_UINT:
        return image(IMG_16_UINT, 1);
    case DXGI_FORMAT_R16_SNORM:
        return image(IMG_16_SNORM, 1);
    case DXGI_FORMAT_R16_SINT:
        return image(IMG_16_SINT, 1);
    case DXGI_FORMAT_B5G6R5_UNORM:
        return image(IMG_5_6_5_UNORM, 3, ORDER_BGRX);
    case DXGI_FORMAT_B5G5R5A1_UNORM:
        return image(IMG_1_5_5_5_UNORM, 4, ORDER_BGRA);
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        return image(IMG_4_4_4_4_UNORM, 4, ORDER_BGRA);
    case DXGI_FORMAT_R8_UNORM:
        return image(IMG_8_UNORM, 1);
    case DXGI_FORMAT_R8_UINT:
        return image(IMG_8_UINT, 1);
    case DXGI_FORMAT_R8_SNORM:
        return image(IMG_8_SNORM, 1);
    case DXGI_FORMAT_R8_SINT:
        return image(IMG_8_SINT, 1);
    case DXGI_FORMAT_A8_UNORM:
        return image(IMG_8_UNORM, 1, ORDER_ALPHA);
    case CONSOLE_FORMAT_R4G4_UNORM:
        return image(IMG_4_4_UNORM, 2);
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case CONSOLE_FORMAT_X16_TYPELESS_G8_UINT:
        return image(IMG_8_UINT, 1, ORDER_STENCIL);
    case DXGI_FORMAT_BC1_UNORM:
        return image(IMG_BC1_UNORM, 4);
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        return image(IMG_BC1_SRGB, 4);
    case DXGI_FORMAT_BC2_UNORM:
        return image(IMG_BC2_UNORM, 4);
    case DXGI_FORMAT_BC2_UNORM_SRGB:
        return image(IMG_BC2_SRGB, 4);
    case DXGI_FORMAT_BC3_UNORM:
        return image(IMG_BC3_UNORM, 4);
    case DXGI_FORMAT_BC3_UNORM_SRGB:
        return image(IMG_BC3_SRGB, 4);
    case DXGI_FORMAT_BC4_UNORM:
        return image(IMG_BC4_UNORM, 1);
    case DXGI_FORMAT_BC4_SNORM:
        return image(IMG_BC4_SNORM, 1);
    case DXGI_FORMAT_BC5_UNORM:
        return image(IMG_BC5_UNORM, 2);
    case DXGI_FORMAT_BC5_SNORM:
        return image(IMG_BC5_SNORM, 2);
    case DXGI_FORMAT_BC6H_UF16:
        return image(IMG_BC6_UFLOAT, 3);
    case DXGI_FORMAT_BC6H_SF16:
        return image(IMG_BC6_SFLOAT, 3);
    case DXGI_FORMAT_BC7_UNORM:
        return image(IMG_BC7_UNORM, 4);
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return image(IMG_BC7_SRGB, 4);
    default:
        // Typeless formats (no view reads them), 24-bit depth, packed and planar video, and the
        // console's own float formats.
        return image(0, 4);
    }
}
