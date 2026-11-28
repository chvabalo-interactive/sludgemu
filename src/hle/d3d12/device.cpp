// Console ID3D12Device on top of the desktop device. One object and one function table serve
// every device version, as one vtable does on desktop.

#include "hle/d3d12/device.h"

#include <stddef.h>

#include "common/log.h"
#include "gpu/gpu.h"
#include "hle/d3d12/barriers.h"
#include "hle/d3d12/console_iids.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"
#include "hle/d3d12/frame_pacing.h"
#include "hle/d3d12/target_tiles.h"
#include "hle/d3d12/texture_sync.h"
#include "hle/d3d12/views.h"

static_assert(offsetof(ConsoleDevice, null_constant_buffer_address) == 0x10, "device layout");
static_assert(offsetof(ConsoleDevice, out_of_line_flags) == 0x18, "device layout");
static_assert(offsetof(ConsoleDevice, desktop) == 88, "device state follows the public members");

static const GUID* const k_device_iids[] = {
    &__uuidof(ID3D12Object),   &__uuidof(ID3D12Device),  &__uuidof(ID3D12Device1), &__uuidof(ID3D12Device2),
    &__uuidof(ID3D12Device3),  &__uuidof(ID3D12Device4), &__uuidof(ID3D12Device5), &__uuidof(ID3D12Device6),
    &__uuidof(ID3D12Device7),  &IID_ConsoleDevice8,      &__uuidof(ID3D12Device9), &__uuidof(ID3D12Device10),
    &__uuidof(ID3D12Device11),
};

static const ConsoleClass k_device_class = {"ID3D12Device", k_device_iids, ARRAY_COUNT(k_device_iids),
                                            nullptr};

// Created once; our own reference keeps it alive for the lifetime of the process.
static ConsoleDevice* g_device = nullptr;

static UINT WINAPI device_get_node_count(ConsoleDevice*) {
    return 1;
}

static HRESULT WINAPI device_create_command_queue(ConsoleDevice* self, const D3D12_COMMAND_QUEUE_DESC* desc,
                                                  REFIID iid, void** out) {
    return console_create_command_queue(self, desc, iid, out);
}

// Console queues address hardware engines and pipes directly and carry a priority level; the
// desktop scheduler decides those, so only the type and the desktop flags carry over.
static HRESULT WINAPI device_create_command_queue_x(ConsoleDevice* self, const ConsoleQueueDescX* desc,
                                                    REFIID iid, void** out) {
    if (!desc)
        return E_INVALIDARG;
    LOG_D("d3d12", "CreateCommandQueueX(type %d, flags 0x%x, engine/pipe %u, queue %u)", desc->type,
          desc->flags, desc->engine_or_pipe_index, desc->queue_index);
    D3D12_COMMAND_QUEUE_DESC d = {};
    d.Type = desc->type;
    d.Flags = (D3D12_COMMAND_QUEUE_FLAGS)(desc->flags & 0x00FFFFFF);
    return console_create_command_queue(self, &d, iid, out);
}

static HRESULT WINAPI device_create_command_allocator(ConsoleDevice* self, D3D12_COMMAND_LIST_TYPE type,
                                                      REFIID iid, void** out) {
    return console_create_command_allocator(self, type, iid, out);
}

static HRESULT WINAPI device_create_command_list_x(ConsoleDevice* self, const ConsoleCommandListDesc* desc,
                                                   ConsoleCommandAllocator* allocator, void* initial_state,
                                                   REFIID iid, void** out) {
    return console_create_command_list(self, desc, allocator, initial_state, iid, out);
}

static HRESULT WINAPI device_create_fence(ConsoleDevice* self, UINT64 initial_value, D3D12_FENCE_FLAGS flags,
                                          REFIID iid, void** out) {
    return console_create_fence(self, initial_value, flags, iid, out);
}

// Console descriptors are raw hardware descriptors with fixed sizes, and descriptor handles are
// plain pointers into heap memory that title code steps through with these strides.
static UINT WINAPI device_get_descriptor_handle_increment_size(ConsoleDevice*,
                                                               D3D12_DESCRIPTOR_HEAP_TYPE type) {
    return console_descriptor_size(type);
}

static HRESULT WINAPI device_create_descriptor_heap(ConsoleDevice* self,
                                                    const D3D12_DESCRIPTOR_HEAP_DESC* desc, REFIID iid,
                                                    void** out) {
    return console_create_descriptor_heap(self, desc, iid, out);
}

// --- frame pacing (see frame_pacing.h) --------------------------------------------------------------

static HRESULT WINAPI device_set_frame_interval_x(ConsoleDevice*, void* output, UINT length_us, UINT period,
                                                  UINT flags) {
    return frame_set_interval(length_us, period, flags);
}

static HRESULT WINAPI device_schedule_frame_event_x(ConsoleDevice*, UINT type, UINT offset_us,
                                                    const FrameSignalList* signals, UINT flags) {
    return frame_schedule_event((FrameEventType)type, offset_us, signals, flags);
}

static HRESULT WINAPI device_wait_frame_event_x(ConsoleDevice*, UINT type, UINT timeout_ms,
                                                const FrameWaitList* extra, UINT flags, FrameToken* token) {
    return frame_wait_event((FrameEventType)type, timeout_ms, extra, flags, token);
}

// No statistics are collected yet; S_FALSE means "nothing available for this frame".
static HRESULT WINAPI device_get_frame_statistics_x(ConsoleDevice*, FrameToken token, UINT types, UINT* count,
                                                    void* statistics) {
    if (count)
        *count = 0;
    return S_FALSE;
}

// Struct return through a hidden pointer: a C entry receives it before the object.
static D3D12_HEAP_PROPERTIES* WINAPI device_get_custom_heap_properties(D3D12_HEAP_PROPERTIES* ret,
                                                                       ConsoleDevice* self, UINT node_mask,
                                                                       D3D12_HEAP_TYPE type) {
    *ret = self->desktop->GetCustomHeapProperties(node_mask, type);
    return ret;
}

static HRESULT WINAPI device_create_placed_resource_x(ConsoleDevice* self, D3D12_GPU_VIRTUAL_ADDRESS address,
                                                      const D3D12_RESOURCE_DESC* desc,
                                                      D3D12_RESOURCE_STATES state,
                                                      const D3D12_CLEAR_VALUE* clear, REFIID iid,
                                                      void** out) {
    if (!desc)
        return E_INVALIDARG;
    D3D12_RESOURCE_DESC1 d1 = console_desc1_from_desc(*desc);
    return console_create_placed_resource(self, address, &d1, console_layout_from_state(state), clear, iid,
                                          out);
}

static HRESULT WINAPI device_create_placed_resource_x1(ConsoleDevice* self, D3D12_GPU_VIRTUAL_ADDRESS address,
                                                       const D3D12_RESOURCE_DESC1* desc,
                                                       D3D12_BARRIER_LAYOUT layout,
                                                       const D3D12_CLEAR_VALUE* clear, REFIID iid,
                                                       void** out) {
    return console_create_placed_resource(self, address, desc, console_desktop_layout(layout), clear, iid,
                                          out);
}

// Struct returns through a hidden pointer. Several resources are laid out one after another, each
// at its own alignment; the 1/2 versions also report where each one goes.
static D3D12_RESOURCE_ALLOCATION_INFO layout_resources(UINT count, const D3D12_RESOURCE_DESC1* descs1,
                                                       const D3D12_RESOURCE_DESC* descs,
                                                       D3D12_RESOURCE_ALLOCATION_INFO1* each) {
    u64 size = 0, alignment = 0;
    for (UINT i = 0; i < count; i++) {
        D3D12_RESOURCE_DESC1 d = descs1 ? descs1[i] : console_desc1_from_desc(descs[i]);
        D3D12_RESOURCE_ALLOCATION_INFO info = console_resource_allocation_info(d);
        if (info.SizeInBytes == UINT64_MAX)
            return info;
        size = ALIGN_UP(size, info.Alignment);
        if (each)
            each[i] = {size, info.Alignment, info.SizeInBytes};
        size += info.SizeInBytes;
        if (info.Alignment > alignment)
            alignment = info.Alignment;
    }
    return {size, alignment};
}

static D3D12_RESOURCE_ALLOCATION_INFO* WINAPI device_get_resource_allocation_info(
    D3D12_RESOURCE_ALLOCATION_INFO* ret, ConsoleDevice*, UINT, UINT count, const D3D12_RESOURCE_DESC* descs) {
    *ret = layout_resources(count, nullptr, descs, nullptr);
    return ret;
}

static D3D12_RESOURCE_ALLOCATION_INFO* WINAPI device_get_resource_allocation_info1(
    D3D12_RESOURCE_ALLOCATION_INFO* ret, ConsoleDevice*, UINT, UINT count, const D3D12_RESOURCE_DESC* descs,
    D3D12_RESOURCE_ALLOCATION_INFO1* each) {
    *ret = layout_resources(count, nullptr, descs, each);
    return ret;
}

static D3D12_RESOURCE_ALLOCATION_INFO* WINAPI device_get_resource_allocation_info2(
    D3D12_RESOURCE_ALLOCATION_INFO* ret, ConsoleDevice*, UINT, UINT count, const D3D12_RESOURCE_DESC1* descs,
    D3D12_RESOURCE_ALLOCATION_INFO1* each) {
    *ret = layout_resources(count, descs, nullptr, each);
    return ret;
}

static HRESULT WINAPI device_create_committed_resource(ConsoleDevice* self, const D3D12_HEAP_PROPERTIES* heap,
                                                       D3D12_HEAP_FLAGS heap_flags,
                                                       const D3D12_RESOURCE_DESC* desc,
                                                       D3D12_RESOURCE_STATES state,
                                                       const D3D12_CLEAR_VALUE* clear, REFIID iid,
                                                       void** out) {
    if (!desc)
        return E_INVALIDARG;
    D3D12_RESOURCE_DESC1 d1 = console_desc1_from_desc(*desc);
    return console_create_committed_resource(self, heap, heap_flags, &d1, console_layout_from_state(state),
                                             clear, iid, out);
}

// Console titles have no protected content, and desktop textures that need views in other formats
// are created typeless, so the session and castable formats don't carry over.
static HRESULT WINAPI device_create_committed_resource3(
    ConsoleDevice* self, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS heap_flags,
    const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* clear,
    void* protected_session, UINT32 castable_count, DXGI_FORMAT* castable_formats, REFIID iid, void** out) {
    return console_create_committed_resource(self, heap, heap_flags, desc, console_desktop_layout(layout),
                                             clear, iid, out);
}

// --- pipeline states ---------------------------------------------------------------------------------

static HRESULT WINAPI device_create_pipeline_state(ConsoleDevice* self,
                                                   const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID iid,
                                                   void** out) {
    return console_create_pipeline_state(self, desc, iid, out);
}

// The extended descriptions set console wave launch limits.
static HRESULT WINAPI device_create_compute_pipeline_state_x(ConsoleDevice* self,
                                                             const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                                             UINT, const void*, REFIID iid, void** out) {
    return console_create_compute_pipeline_state(self, desc, iid, out);
}

static HRESULT WINAPI device_create_derived_pipeline_state_x(ConsoleDevice* self, void* source, UINT count,
                                                             const void* descs, REFIID iid, void** out) {
    return console_derive_pipeline_state(self, source, count, descs, iid, out);
}

static HRESULT WINAPI device_deserialize_graphics_pipeline_state_x(ConsoleDevice* self,
                                                                   const ConsoleSerializedPipeline* state,
                                                                   REFIID iid, void** out) {
    return console_deserialize_pipeline_state(self, false, state, iid, out);
}

static HRESULT WINAPI device_deserialize_compute_pipeline_state_x(ConsoleDevice* self,
                                                                  const ConsoleSerializedPipeline* state,
                                                                  REFIID iid, void** out) {
    return console_deserialize_pipeline_state(self, true, state, iid, out);
}

static HRESULT WINAPI device_create_query_heap(ConsoleDevice* self, const D3D12_QUERY_HEAP_DESC* desc,
                                               REFIID iid, void** out) {
    return console_create_query_heap(self, desc, iid, out);
}

static HRESULT WINAPI device_create_command_signature(ConsoleDevice* self,
                                                      const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                                      void* root_signature, REFIID iid, void** out) {
    return console_create_command_signature(self, desc, root_signature, iid, out);
}

// Root signatures are per device on desktop too; the node mask has no meaning with one GPU.
static HRESULT WINAPI device_create_root_signature(ConsoleDevice* self, UINT, const void* blob, SIZE_T size,
                                                   REFIID iid, void** out) {
    return console_create_root_signature(self, blob, size, iid, out);
}

static HRESULT WINAPI device_register_page_pool_x(ConsoleDevice*, D3D12_GPU_VIRTUAL_ADDRESS address,
                                                  UINT page_count, HANDLE* handle) {
    return console_register_page_pool(address, page_count, 0, handle);
}

static HRESULT WINAPI device_register_page_pool2_x(ConsoleDevice*, D3D12_GPU_VIRTUAL_ADDRESS address,
                                                   UINT page_count, UINT flags, HANDLE* handle) {
    return console_register_page_pool(address, page_count, flags, handle);
}

static HRESULT WINAPI device_unregister_page_pool_x(ConsoleDevice*, HANDLE handle) {
    return console_unregister_page_pool(handle);
}

static const ConsoleEntry k_device_impl[] = {
    CONSOLE_ENTRY(7, device_get_node_count),
    CONSOLE_ENTRY(8, device_create_command_queue),
    CONSOLE_ENTRY(9, device_create_command_allocator),
    CONSOLE_ENTRY(14, device_create_descriptor_heap),
    CONSOLE_ENTRY(15, device_get_descriptor_handle_increment_size),
    CONSOLE_ENTRY(16, device_create_root_signature),
    CONSOLE_ENTRY(17, device_create_constant_buffer_view),
    CONSOLE_ENTRY(18, device_create_shader_resource_view),
    CONSOLE_ENTRY(19, device_create_unordered_access_view),
    CONSOLE_ENTRY(20, device_create_render_target_view),
    CONSOLE_ENTRY(21, device_create_depth_stencil_view),
    CONSOLE_ENTRY(22, device_create_sampler),
    CONSOLE_ENTRY(23, device_copy_descriptors),
    CONSOLE_ENTRY(24, device_copy_descriptors_simple),
    CONSOLE_ENTRY(25, device_get_resource_allocation_info),
    CONSOLE_ENTRY(26, device_get_custom_heap_properties),
    CONSOLE_ENTRY(27, device_create_committed_resource),
    CONSOLE_ENTRY(36, device_create_fence),
    CONSOLE_ENTRY(39, device_create_query_heap),
    CONSOLE_ENTRY(55, device_create_derived_pipeline_state_x),
    CONSOLE_ENTRY(57, device_deserialize_graphics_pipeline_state_x),
    CONSOLE_ENTRY(62, device_create_compute_pipeline_state_x),
    CONSOLE_ENTRY(63, device_create_derived_pipeline_state_x),
    CONSOLE_ENTRY(67, device_deserialize_compute_pipeline_state_x),
    CONSOLE_ENTRY(86, device_create_pipeline_state),
    CONSOLE_ENTRY(41, device_create_command_signature),
    CONSOLE_ENTRY(44, device_create_placed_resource_x),
    CONSOLE_ENTRY(48, device_create_command_queue_x),
    CONSOLE_ENTRY(70, device_create_placed_raw_srv_x),
    CONSOLE_ENTRY(71, device_create_placed_raw_uav_x),
    CONSOLE_ENTRY(61, device_create_sampler_x),
    CONSOLE_ENTRY(73, device_create_command_list_x),
    CONSOLE_ENTRY(74, device_register_page_pool_x),
    CONSOLE_ENTRY(75, device_unregister_page_pool_x),
    CONSOLE_ENTRY(78, device_set_frame_interval_x),
    CONSOLE_ENTRY(79, device_schedule_frame_event_x),
    CONSOLE_ENTRY(80, device_wait_frame_event_x),
    CONSOLE_ENTRY(81, device_get_frame_statistics_x),
    CONSOLE_ENTRY(95, device_get_resource_allocation_info1),
    CONSOLE_ENTRY(108, device_get_resource_allocation_info2),
    CONSOLE_ENTRY(120, device_register_page_pool2_x),
    CONSOLE_ENTRY(127, device_create_committed_resource3),
    CONSOLE_ENTRY(133, device_create_placed_resource_x1),
};

HRESULT console_create_device(void* adapter, const ConsoleDeviceParams* params, REFIID iid, void** out) {
    if (!params)
        return E_INVALIDARG;
    LOG_I("d3d12", "D3D12XboxCreateDevice(version 0x%x, debug flags 0x%x, create flags 0x%x, queue ring %u)",
          params->version, params->process_debug_flags, params->create_flags,
          params->graphics_queue_ring_bytes);

    if (!g_device) {
        ID3D12Device13* desktop = gpu_device();
        if (!desktop)
            return DXGI_ERROR_UNSUPPORTED;
        // This file's entries and device_x.cpp's in one table.
        static ConsoleEntry s_entries[ARRAY_COUNT(k_device_impl) + 128];
        u32 n = 0;
        for (u32 i = 0; i < ARRAY_COUNT(k_device_impl); i++)
            s_entries[n++] = k_device_impl[i];
        SLUDGE_ASSERT(n + k_device_x_entry_count <= ARRAY_COUNT(s_entries));
        for (u32 i = 0; i < k_device_x_entry_count; i++)
            s_entries[n++] = k_device_x_entries[i];
        void* const* table = console_build_table("ID3D12Device", k_device_entries,
                                                 ARRAY_COUNT(k_device_entries), s_entries, n);
        desktop->AddRef(); // one reference for the console object, one held by the gpu module
        g_device = (ConsoleDevice*)console_alloc(sizeof(ConsoleDevice), &k_device_class, table, desktop);
        g_device->out_of_line_flags[0] = 1;
        g_device->desktop = desktop;
        console_set_device(g_device);
        console_command_queue_init();
        console_fence_init();
        console_command_allocator_init();
        console_command_list_init();
        gpu_window_init();
        gpu_page_tables_init();
        texture_sync_init();
        target_tiles_init();
        console_resource_init();
        console_descriptor_heap_init();
        console_query_heap_init();
        console_command_signature_init();
        console_root_signature_init();
        console_pipeline_state_init();
        console_heap_init();
    }
    if (!out)
        return S_FALSE; // capability check only
    return console_query(g_device, iid, out);
}
