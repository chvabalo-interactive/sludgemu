#pragma once

#include <windows.h>

#include "common/types.h"

// Creation parameters passed by the title to D3D12XboxCreateDevice (layout is the console ABI).
struct ConsoleDeviceParams {
    u32 version;
    u32 process_debug_flags;
    u32 graphics_queue_ring_bytes;
    u32 pad0;
    u64 offchip_tessellation_buffer;
    u32 graphics_scratch_bytes;
    u32 compute_scratch_bytes;
    BOOL disable_geometry_shader_allocations;
    BOOL disable_tessellation_shader_allocations;
    BOOL disable_raytracing;
    BOOL disable_automatic_dpbb_break_batch_events;
    u64 raytracing_stack_buffer;
    u32 raytracing_stack_bytes;
    u32 create_flags;
    u32 auto_hdr_paper_white_nits;
    BOOL disable_automatic_command_segment_chaining;
    u32 amplification_indirect_args_bytes;
    u32 amplification_payload_bytes;
};
static_assert(sizeof(ConsoleDeviceParams) == 80, "console device parameters layout");

HRESULT console_create_device(void* adapter, const ConsoleDeviceParams* params, REFIID iid, void** out);

// Device entries beyond creating the core objects (device_x.cpp): resource and pipeline variants,
// residency, features, footprints, and the console's debugging and tuning entries.
struct ConsoleEntry;
extern const ConsoleEntry k_device_x_entries[];
extern const u32 k_device_x_entry_count;
