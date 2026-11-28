#pragma once

#include <windows.h>
#include <dxgiformat.h>

#include "common/types.h"

// Storage properties of formats in the console's numbering: the DXGI formats plus console-only
// ones, some of which reuse values desktop DXGI later gave other formats.

// Console-only formats.
#define CONSOLE_FORMAT_R10G10B10_7E3_A2_FLOAT   ((DXGI_FORMAT)116)
#define CONSOLE_FORMAT_R10G10B10_6E4_A2_FLOAT   ((DXGI_FORMAT)117)
#define CONSOLE_FORMAT_D16_UNORM_S8_UINT        ((DXGI_FORMAT)118)
#define CONSOLE_FORMAT_R16_UNORM_X8_TYPELESS    ((DXGI_FORMAT)119)
#define CONSOLE_FORMAT_X16_TYPELESS_G8_UINT     ((DXGI_FORMAT)120)
#define CONSOLE_FORMAT_R10G10B10_SNORM_A2_UNORM ((DXGI_FORMAT)189)
#define CONSOLE_FORMAT_R4G4_UNORM               ((DXGI_FORMAT)190)
#define CONSOLE_FORMAT_R24G8_UNORM              ((DXGI_FORMAT)191)
#define CONSOLE_FORMAT_SAMPLER_FEEDBACK_MIN_MIP ((DXGI_FORMAT)187) // desktop: 189
#define CONSOLE_FORMAT_SAMPLER_FEEDBACK_REGIONS ((DXGI_FORMAT)188) // desktop: 190

enum FormatKind : u8 {
    FORMAT_KIND_UNKNOWN,
    FORMAT_KIND_COLOR,
    FORMAT_KIND_BLOCK_COMPRESSED, // 4x4 texel blocks
    FORMAT_KIND_DEPTH,            // depth only
    FORMAT_KIND_DEPTH_STENCIL,    // depth and stencil, stored as separate planes on the GPU
    FORMAT_KIND_STENCIL,          // stencil only (a view of a depth-stencil format)
    FORMAT_KIND_PACKED_422,       // two texels per element
    FORMAT_KIND_VIDEO,            // planar YUV
};

struct FormatInfo {
    FormatKind kind;
    u8 bytes_per_element; // of the first plane; a block for compressed formats
    u8 block_width;       // texels per element horizontally (4 for BC, 2 for packed 4:2:2)
    u8 block_height;
};

// Properties of `format`; kind is FORMAT_KIND_UNKNOWN for formats with no storage.
FormatInfo format_info(DXGI_FORMAT format);

// --- desktop equivalents ---------------------------------------------------------------------------
//
// Every console format goes through these before reaching desktop D3D12: some console values name
// different desktop formats, and console-only formats have none. Those get a stand-in that holds at
// least their precision, with its own storage size.

// The desktop format for a view, clear value or copy footprint; DXGI_FORMAT_UNKNOWN for a value
// with no desktop meaning.
DXGI_FORMAT format_desktop(DXGI_FORMAT format);

// The desktop format to create a texture in: as format_desktop, but depth formats are typeless, the
// only way desktop shaders may read a depth texture (console shaders can read any).
DXGI_FORMAT format_desktop_resource(DXGI_FORMAT format);

// Same as format_desktop_resource, but substitutes a renderable/UAV stand-in when `format` (such
// as R9G9B9E5_SHAREDEXP) cannot take RT or UAV flags on desktop.
DXGI_FORMAT format_desktop_target(DXGI_FORMAT format, u32 resource_flags);

// The desktop format a shader reads a texture through when its view names no format: the depth
// plane for depth formats, format_desktop otherwise.
DXGI_FORMAT format_desktop_shader_read(DXGI_FORMAT format);

// The console format is drawn through a stand-in whose storage differs, so views can't reinterpret
// it as another format, nor it as them, and copies can't move its bytes as they are.
bool format_has_stand_in(DXGI_FORMAT format);

// Whether desktop can show a texture created in console format `resource` through a view in console
// format `view` (UNKNOWN: the texture's own). Formats with stand-ins only alias their own family.
bool format_view_compatible(DXGI_FORMAT resource, DXGI_FORMAT view);
