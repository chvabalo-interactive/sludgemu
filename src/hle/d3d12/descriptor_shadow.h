// Console resource descriptor slots as we write them, and the descriptor shadow records made from them
// (the shadow is described in shader/translate.h). Plain data, no device: unit tested.

#pragma once

#include <windows.h>
#include <d3d12.h>

#include "common/types.h"
#include "shader/translate.h"

// Constant buffer views in the console's own descriptor format: the buffer address with a 16-byte
// stride in the top bits of the first quadword, the size in 16-byte records with a fixed format word
// on top of the second. Titles write these themselves (the console runtime inlines their creation),
// and we write them too, so copies of descriptor memory carry the view itself.
#define NATIVE_CBV_STRIDE_BITS  0x0010ull     // top 16 bits of the first quadword
#define NATIVE_CBV_FORMAT_BITS  0x0002403Cull // top 32 bits of the second quadword
#define NATIVE_CBV_ADDRESS_MASK 0x0000FFFFFFFFFFFFull

// Console descriptor slots written by us start with this marker, then the slot's descriptor type
// (the desktop heap type, with the view's ShadowViewKind in bits 8-15 and DESCRIPTOR_MARKER_IMAGE for
// image descriptors) and its desktop CPU-only descriptor. Resource slots then hold the console
// descriptor's dwords 0-3 for the descriptor shadow, so that title copies of descriptor memory carry
// them along. An image descriptor's dwords 4-7 don't fit: they stay with the slot the view was made in
// (see ConsoleDescriptorHeap::image_words), which the marker names through its desktop descriptor.
#define DESCRIPTOR_MARKER_MAGIC 0x47444C53u // "SLDG"
#define DESCRIPTOR_MARKER_BITS  16          // where the dwords start in a resource slot
#define DESCRIPTOR_MARKER_IMAGE 0x10000u    // in the type word

// A console buffer descriptor's dwords 0-3 in the hardware layout: the base address (48 bits) with the
// stride in the high half of dword 1, the record count, then the format word. Untyped buffers carry
// the word console constant buffers do.
void shadow_buffer_descriptor(u64 base, u32 stride, u32 records, u32 out[4]);

// A console image descriptor (8 dwords, the hardware layout) of a view of a texture, as the console
// runtime makes it from the texture's console description, its address and the view's description (in
// console format numbering): format, size, mip and slice range, type, component selects, swizzle mode.
// `image_format`: a hardware image format the title named itself (0: the view's format). Left zero: the
// compression metadata fields (which take the texture's metadata plane) and the bank rotation of
// swizzled layouts.
void shadow_texture_srv(const D3D12_RESOURCE_DESC1& texture, u64 address,
                        const D3D12_SHADER_RESOURCE_VIEW_DESC& view, u32 image_format, u32 out[8]);
void shadow_texture_uav(const D3D12_RESOURCE_DESC1& texture, u64 address,
                        const D3D12_UNORDERED_ACCESS_VIEW_DESC& view, u32 out[8]);

// The shadow record of the descriptor in a console resource slot (SHADER_SHADOW_RECORD_BYTES): a
// title-written constant buffer view is a hardware descriptor already, our views carry dwords 0-3 after
// the marker, image views their dwords 4-7 in `image_words` (the slot the view was made in keeps them;
// nullptr reads zeros), and anything else (memory never written as a descriptor) has a record of zeros,
// a null view's.
void shadow_record(const u8* slot, const u32* image_words, u32 record[SHADER_SHADOW_RECORD_BYTES / 4]);
