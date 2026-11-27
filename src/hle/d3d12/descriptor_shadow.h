// Console resource descriptor slots as we write them, and the descriptor shadow records made from them
// (the shadow is described in shader/translate.h). Plain data, no device: unit tested.

#pragma once

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
// (the desktop heap type, with the view's ShadowViewKind in bits 8-15) and its desktop CPU-only
// descriptor. Resource slots then hold the console descriptor's dwords 0-3 for the descriptor shadow
// (zero for views of images), so that title copies of descriptor memory carry them along.
#define DESCRIPTOR_MARKER_MAGIC 0x47444C53u // "SLDG"
#define DESCRIPTOR_MARKER_BITS  16          // where the dwords start in a resource slot

// A console buffer descriptor's dwords 0-3 in the hardware layout: the base address (48 bits) with the
// stride in the high half of dword 1, the record count, then the format word. Untyped buffers carry
// the word console constant buffers do.
void shadow_buffer_descriptor(u64 base, u32 stride, u32 records, u32 out[4]);

// The shadow record of the descriptor in a console resource slot (32 bytes): a title-written constant
// buffer view is a hardware descriptor already, our views carry their dwords after the marker, and
// anything else (memory never written as a descriptor) has a record of zeros, a null view's.
void shadow_record(const u8* slot, u32 record[SHADER_SHADOW_RECORD_BYTES / 4]);
