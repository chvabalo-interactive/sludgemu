// Descriptor shadow records (see descriptor_shadow.h).

#include "hle/d3d12/descriptor_shadow.h"

#include <string.h>

void shadow_buffer_descriptor(u64 base, u32 stride, u32 records, u32 out[4]) {
    out[0] = (u32)base;
    out[1] = ((u32)(base >> 32) & 0xFFFFu) | (stride & 0x3FFFu) << 16;
    out[2] = records;
    out[3] = (u32)NATIVE_CBV_FORMAT_BITS;
}

void shadow_record(const u8* slot, u32 record[SHADER_SHADOW_RECORD_BYTES / 4]) {
    memset(record, 0, SHADER_SHADOW_RECORD_BYTES);
    u64 q0, q1;
    memcpy(&q0, slot, 8);
    memcpy(&q1, slot + 8, 8);
    if ((q0 >> 48) == NATIVE_CBV_STRIDE_BITS && (q1 >> 32) == NATIVE_CBV_FORMAT_BITS) {
        memcpy(record, slot, 16);
        record[SHADER_SHADOW_KIND_OFFSET / 4] = SHADOW_VIEW_CBV;
    } else if ((u32)q0 == DESCRIPTOR_MARKER_MAGIC) {
        memcpy(record, slot + DESCRIPTOR_MARKER_BITS, 16);
        record[SHADER_SHADOW_KIND_OFFSET / 4] = (u32)(q0 >> 40) & 0xFF; // the marker's type word, bits 8-15
    }
}
