// CPU access to console texture memory: ID3D12Resource::WriteToSubresource and ReadFromSubresource.
//
// A console texture's memory is title memory holding it in the console layout, so the CPU reads and
// writes texels there, element by element where the console layout puts them, as the console runtime
// does. Writes reach the desktop texture through texture sync, like any CPU write to texture memory.
// Desktop textures the GPU writes (render targets, depth, UAVs) don't go back to title memory, so
// reads of those see what title memory held.

#include <string.h>

#include "common/log.h"
#include "gpu/console_layout.h"
#include "gpu/formats.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/texture_sync.h"

// Where a subresource's elements are and which of them a box covers, in elements.
struct SubresourceAccess {
    ConsoleSubresourceAddressing addressing;
    u64 texture_size;
    u32 begin[3];
    u32 end[3];
};

static bool subresource_access(const ConsoleResource* r, UINT subresource, const D3D12_BOX* box,
                               SubresourceAccess* out) {
    const D3D12_RESOURCE_DESC1& d = r->desc;
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || d.SampleDesc.Count > 1)
        return false;
    ConsoleTextureDesc t = {};
    t.dimension = d.Dimension;
    t.width = d.Width;
    t.height = d.Height;
    t.depth_or_array_size = d.DepthOrArraySize;
    t.mip_levels = d.MipLevels;
    t.format = d.Format;
    t.sample_count = d.SampleDesc.Count;
    t.layout = d.Layout;
    t.flags = d.Flags;
    ConsoleTextureLayout layout;
    if (!console_texture_layout(t, &layout))
        return false;
    // Subresources count mips, then array slices, then planes.
    u32 mips = layout.mip_levels;
    u32 slices = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : d.DepthOrArraySize;
    u32 plane = subresource / (mips * slices);
    u32 slice = subresource / mips % slices;
    u32 mip = subresource % mips;
    if (plane >= layout.plane_count)
        return false;
    ConsoleSubresourceAddressing& a = out->addressing;
    if (!console_subresource_addressing(t, layout, plane, mip, slice, &a, false))
        return false;
    out->texture_size = layout.size;
    // Boxes are in texels; elements of block compressed formats are blocks.
    FormatInfo f = format_info(d.Format);
    u32 bw = plane == 0 && f.block_width ? f.block_width : 1;
    u32 bh = plane == 0 && f.block_height ? f.block_height : 1;
    u32 limit[3] = {a.width, a.height, a.depth};
    if (box) {
        u32 b[3] = {box->left / bw, box->top / bh, box->front};
        u32 e[3] = {(box->right + bw - 1) / bw, (box->bottom + bh - 1) / bh, box->back};
        for (u32 i = 0; i < 3; i++) {
            out->begin[i] = b[i];
            out->end[i] = e[i] < limit[i] ? e[i] : limit[i];
            if (out->begin[i] >= out->end[i])
                return false;
        }
    } else {
        for (u32 i = 0; i < 3; i++) {
            out->begin[i] = 0;
            out->end[i] = limit[i];
        }
    }
    return true;
}

// Copies the box between texture memory at `texture` and linear memory: rows at once where the layout
// is linear, else element by element.
static void copy_box(const SubresourceAccess& s, u8* texture, u8* linear, u64 row_pitch, u64 depth_pitch,
                     bool into_texture) {
    const ConsoleSubresourceAddressing& a = s.addressing;
    u32 bpe = a.bytes_per_element;
    u32 row_elements = s.end[0] - s.begin[0];
    for (u32 z = s.begin[2]; z < s.end[2]; z++) {
        for (u32 y = s.begin[1]; y < s.end[1]; y++) {
            u8* row = linear + (u64)(z - s.begin[2]) * depth_pitch + (u64)(y - s.begin[1]) * row_pitch;
            if (a.linear) {
                u8* t = texture + console_subresource_offset(a, s.begin[0], y, z);
                into_texture ? memcpy(t, row, (u64)row_elements * bpe)
                             : memcpy(row, t, (u64)row_elements * bpe);
                continue;
            }
            for (u32 x = s.begin[0]; x < s.end[0]; x++) {
                u8* t = texture + console_subresource_offset(a, x, y, z);
                u8* l = row + (u64)(x - s.begin[0]) * bpe;
                into_texture ? memcpy(t, l, bpe) : memcpy(l, t, bpe);
            }
        }
    }
}

HRESULT WINAPI console_resource_write_to_subresource(ConsoleResource* self, UINT subresource,
                                                     const D3D12_BOX* box, const void* data, UINT row_pitch,
                                                     UINT depth_pitch) {
    SubresourceAccess s;
    if (!data || !subresource_access(self, subresource, box, &s)) {
        LOG_W_ONCE("d3d12", "WriteToSubresource on a buffer, multisampled texture or invalid box: refused");
        return E_INVALIDARG;
    }
    copy_box(s, (u8*)self->gpu_address, (u8*)data, row_pitch, depth_pitch, true);
    texture_sync_mark(self->gpu_address, s.texture_size);
    return S_OK;
}

HRESULT WINAPI console_resource_read_from_subresource(ConsoleResource* self, void* data, UINT row_pitch,
                                                      UINT depth_pitch, UINT subresource,
                                                      const D3D12_BOX* box) {
    SubresourceAccess s;
    if (!data || !subresource_access(self, subresource, box, &s)) {
        LOG_W_ONCE("d3d12", "ReadFromSubresource on a buffer, multisampled texture or invalid box: refused");
        return E_INVALIDARG;
    }
    const u32 written = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (self->desc.Flags & written)
        LOG_W_ONCE("d3d12", "ReadFromSubresource of a texture the GPU writes reads title memory, which GPU "
                            "writes don't reach");
    copy_box(s, (u8*)self->gpu_address, (u8*)data, row_pitch, depth_pitch, false);
    return S_OK;
}
