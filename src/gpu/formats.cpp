#include "gpu/formats.h"

#include <d3d12.h>

static FormatInfo color(u8 bytes) {
    return {FORMAT_KIND_COLOR, bytes, 1, 1};
}

FormatInfo format_info(DXGI_FORMAT f) {
    switch ((u32)f) { // console-only formats are outside the DXGI enum
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return color(16);
    case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return color(12);
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
        return color(8);
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return {FORMAT_KIND_DEPTH_STENCIL, 4, 1, 1};
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        return {FORMAT_KIND_DEPTH, 4, 1, 1};
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
        return {FORMAT_KIND_STENCIL, 1, 1, 1};
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case CONSOLE_FORMAT_R10G10B10_7E3_A2_FLOAT:
    case CONSOLE_FORMAT_R10G10B10_6E4_A2_FLOAT:
    case CONSOLE_FORMAT_R10G10B10_SNORM_A2_UNORM:
        return color(4);
    case DXGI_FORMAT_D32_FLOAT:
        return {FORMAT_KIND_DEPTH, 4, 1, 1};
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case CONSOLE_FORMAT_R24G8_UNORM:
        return {FORMAT_KIND_DEPTH_STENCIL, 4, 1, 1};
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        return {FORMAT_KIND_DEPTH, 4, 1, 1};
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
        return {FORMAT_KIND_STENCIL, 1, 1, 1};
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        return color(2);
    case DXGI_FORMAT_D16_UNORM:
        return {FORMAT_KIND_DEPTH, 2, 1, 1};
    case CONSOLE_FORMAT_D16_UNORM_S8_UINT:
        return {FORMAT_KIND_DEPTH_STENCIL, 2, 1, 1};
    case CONSOLE_FORMAT_R16_UNORM_X8_TYPELESS:
        return {FORMAT_KIND_DEPTH, 2, 1, 1};
    case CONSOLE_FORMAT_X16_TYPELESS_G8_UINT:
        return {FORMAT_KIND_STENCIL, 1, 1, 1};
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_A8_UNORM:
    case CONSOLE_FORMAT_R4G4_UNORM:
        return color(1);
    case DXGI_FORMAT_R8G8_B8G8_UNORM:
    case DXGI_FORMAT_G8R8_G8B8_UNORM:
    case DXGI_FORMAT_YUY2:
        return {FORMAT_KIND_PACKED_422, 4, 2, 1};
    case DXGI_FORMAT_Y210:
    case DXGI_FORMAT_Y216:
        return {FORMAT_KIND_PACKED_422, 8, 2, 1};
    case DXGI_FORMAT_AYUV:
    case DXGI_FORMAT_Y410:
        return color(4);
    case DXGI_FORMAT_Y416:
        return color(8);
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC4_UNORM:
    case DXGI_FORMAT_BC4_SNORM:
        return {FORMAT_KIND_BLOCK_COMPRESSED, 8, 4, 4};
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC5_TYPELESS:
    case DXGI_FORMAT_BC5_UNORM:
    case DXGI_FORMAT_BC5_SNORM:
    case DXGI_FORMAT_BC6H_TYPELESS:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return {FORMAT_KIND_BLOCK_COMPRESSED, 16, 4, 4};
    case DXGI_FORMAT_NV12:
    case DXGI_FORMAT_420_OPAQUE:
    case DXGI_FORMAT_NV11:
        return {FORMAT_KIND_VIDEO, 1, 1, 1};
    case DXGI_FORMAT_P010:
    case DXGI_FORMAT_P016:
        return {FORMAT_KIND_VIDEO, 2, 1, 1};
    default:
        return {FORMAT_KIND_UNKNOWN, 0, 1, 1};
    }
}

// --- desktop equivalents ---------------------------------------------------------------------------

DXGI_FORMAT format_desktop(DXGI_FORMAT f) {
    switch ((u32)f) {
    case CONSOLE_FORMAT_R10G10B10_7E3_A2_FLOAT:
    case CONSOLE_FORMAT_R10G10B10_6E4_A2_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case CONSOLE_FORMAT_R10G10B10_SNORM_A2_UNORM:
        return DXGI_FORMAT_R16G16B16A16_SNORM; // alpha's 0..1 fits the signed range
    case CONSOLE_FORMAT_R4G4_UNORM:
        return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_B5G6R5_UNORM: // optional on desktop, and never UAVs there
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case CONSOLE_FORMAT_D16_UNORM_S8_UINT:
    case CONSOLE_FORMAT_R24G8_UNORM:
        return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case CONSOLE_FORMAT_R16_UNORM_X8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case CONSOLE_FORMAT_X16_TYPELESS_G8_UINT:
        return DXGI_FORMAT_X24_TYPELESS_G8_UINT;
    case CONSOLE_FORMAT_SAMPLER_FEEDBACK_MIN_MIP:
        return DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE;
    case CONSOLE_FORMAT_SAMPLER_FEEDBACK_REGIONS:
        return DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE;
    default:
        // The rest share their values; past the last shared one, desktop's meanings differ.
        if ((u32)f <= DXGI_FORMAT_B4G4R4A4_UNORM || (f >= DXGI_FORMAT_P208 && f <= DXGI_FORMAT_V408))
            return f;
        return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT format_desktop_resource(DXGI_FORMAT f) {
    DXGI_FORMAT d = format_desktop(f);
    switch (d) {
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    default:
        return d;
    }
}

DXGI_FORMAT format_desktop_target(DXGI_FORMAT f, u32 resource_flags) {
    // Console titles render/UAV-write R9G9B9E5_SHAREDEXP (e.g. FSR output); desktop only allows
    // sampled reads, so stand in R16G16B16A16_FLOAT when RT or UAV access is requested.
    const u32 writable = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (f == DXGI_FORMAT_R9G9B9E5_SHAREDEXP && (resource_flags & writable))
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    return format_desktop_resource(f);
}

DXGI_FORMAT format_desktop_shader_read(DXGI_FORMAT f) {
    DXGI_FORMAT d = format_desktop(f);
    switch (d) {
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default:
        return d;
    }
}

bool format_has_stand_in(DXGI_FORMAT f) {
    switch ((u32)f) {
    case CONSOLE_FORMAT_R10G10B10_7E3_A2_FLOAT:
    case CONSOLE_FORMAT_R10G10B10_6E4_A2_FLOAT:
    case CONSOLE_FORMAT_R10G10B10_SNORM_A2_UNORM:
    case CONSOLE_FORMAT_R4G4_UNORM:
    case DXGI_FORMAT_B5G6R5_UNORM:
    case DXGI_FORMAT_B5G5R5A1_UNORM:
    case DXGI_FORMAT_B4G4R4A4_UNORM:
    case CONSOLE_FORMAT_D16_UNORM_S8_UINT:
    case CONSOLE_FORMAT_R16_UNORM_X8_TYPELESS:
    case CONSOLE_FORMAT_X16_TYPELESS_G8_UINT:
        return true;
    default:
        return false;
    }
}

static bool is_d16_s8_family(DXGI_FORMAT f) {
    return f == CONSOLE_FORMAT_D16_UNORM_S8_UINT || f == CONSOLE_FORMAT_R16_UNORM_X8_TYPELESS ||
           f == CONSOLE_FORMAT_X16_TYPELESS_G8_UINT;
}

bool format_view_compatible(DXGI_FORMAT resource, DXGI_FORMAT view) {
    if (view == DXGI_FORMAT_UNKNOWN || view == resource)
        return true;
    if (!format_has_stand_in(resource) && !format_has_stand_in(view))
        return true; // desktop checks the rest itself
    return is_d16_s8_family(resource) && is_d16_s8_family(view);
}
