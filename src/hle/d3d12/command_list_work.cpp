// Console command list entries that do work: draws, dispatches, copies, clears, queries and memory
// writes.

#include "common/log.h"
#include "gpu/formats.h"
#include "hle/d3d12/barriers.h"
#include "hle/d3d12/command_args.h"
#include "hle/d3d12/command_list.h"

static D3D12_GPU_VIRTUAL_ADDRESS translate(u64 address, u64 size, GpuAccess access, GpuAddress* out) {
    if (!gpu_translate(address, size ? size : 1, access, out)) {
        LOG_W_ONCE("d3d12", "GPU address 0x%llx (+%llu) is outside GPU-visible memory", address, size);
        return 0;
    }
    return out->desktop_address;
}

// --- draws and dispatches ----------------------------------------------------------------------------

// The desktop pipeline and user data for a draw or dispatch, waiting for the pipeline to be made if
// it is not yet. Draws are skipped when it can't be made, or isn't made within the wait budget.
static bool prepare(ConsoleCommandList* self, bool graphics) {
    ListState& s = self->state;
    ID3D12RootSignature* signature = nullptr;
    PipelineOutputs outputs = s.outputs;
    outputs.rect_list = s.topology == CONSOLE_TOPOLOGY_RECT_LIST;
    // Only samples the targets have: a mask leaving out samples of more samples than that is the full
    // mask for these targets, and must not make a pipeline variant of its own.
    u32 samples = outputs.sample_count ? outputs.sample_count : 1;
    outputs.dropped_samples = (u16)(s.dropped_samples & ((1u << samples) - 1));
    bool building = false;
    ID3D12PipelineState* desktop =
        console_pipeline_desktop(s.pipeline, graphics ? &outputs : nullptr, &signature, &building);
    if (!desktop) {
        if (!building)
            console_pipeline_report_skip(s.pipeline, graphics);
        self->skipped_draws++;
        return false;
    }
    // Pipelines with inline samplers have a signature of their own; changing signatures drops the
    // root arguments, which then go again.
    u32 g = graphics ? 0 : 1;
    if (signature != s.bound_signature[g]) {
        graphics ? self->desktop->SetGraphicsRootSignature(signature)
                 : self->desktop->SetComputeRootSignature(signature);
        s.bound_signature[g] = signature;
        s.user_data_dirty[g] = true;
    }
    if (desktop != s.bound) {
        self->desktop->SetPipelineState(desktop);
        s.bound = desktop;
    }
    list_flush_user_data(self, graphics);
    return true;
}

static void WINAPI list_draw_instanced(ConsoleCommandList* self, UINT vertices, UINT instances,
                                       UINT first_vertex, UINT first_instance) {
    if (prepare(self, true))
        self->desktop->DrawInstanced(vertices, instances, first_vertex, first_instance);
}

static void WINAPI list_draw_indexed_instanced(ConsoleCommandList* self, UINT indices, UINT instances,
                                               UINT first_index, INT base_vertex, UINT first_instance) {
    if (prepare(self, true))
        self->desktop->DrawIndexedInstanced(indices, instances, first_index, base_vertex, first_instance);
}

// An indexed draw with its own index buffer.
static void WINAPI list_draw_indexed_x(ConsoleCommandList* self, UINT64 index_buffer, DXGI_FORMAT format,
                                       UINT indices, UINT first_index) {
    if (!prepare(self, true))
        return;
    u32 index_size = format == DXGI_FORMAT_R32_UINT ? 4 : 2;
    GpuAddress a;
    D3D12_INDEX_BUFFER_VIEW view;
    view.SizeInBytes = (first_index + indices) * index_size;
    view.BufferLocation = translate(index_buffer, view.SizeInBytes, GPU_READ, &a);
    view.Format = format;
    self->desktop->IASetIndexBuffer(&view);
    self->desktop->DrawIndexedInstanced(indices, 1, first_index, 0, 0);
}

static void WINAPI list_dispatch(ConsoleCommandList* self, UINT x, UINT y, UINT z) {
    if (prepare(self, false))
        self->desktop->Dispatch(x, y, z);
}

// The flags tune console wave launch.
static void WINAPI list_dispatch_x(ConsoleCommandList* self, UINT x, UINT y, UINT z, UINT) {
    list_dispatch(self, x, y, z);
}

static void WINAPI list_execute_indirect(ConsoleCommandList*, void*, UINT, void*, UINT64, void*, UINT64) {
    LOG_W_ONCE("d3d12", "ExecuteIndirect is not translated yet; skipped");
}

// --- copies ------------------------------------------------------------------------------------------

static void WINAPI list_copy_buffer_region(ConsoleCommandList* self, void* dst, UINT64 dst_offset, void* src,
                                           UINT64 src_offset, UINT64 size) {
    u64 dst_base, src_base;
    ID3D12Resource* d = list_resource(dst, &dst_base, GPU_WRITE);
    ID3D12Resource* s = list_resource(src, &src_base, GPU_READ);
    if (!d || !s) {
        LOG_W_ONCE("d3d12", "CopyBufferRegion with a buffer outside GPU-visible memory skipped");
        return;
    }
    self->desktop->CopyBufferRegion(d, dst_base + dst_offset, s, src_base + src_offset, size);
}

// Buffer footprints move by where the console buffer starts in its desktop buffer.
static bool copy_location(const D3D12_TEXTURE_COPY_LOCATION* in, D3D12_TEXTURE_COPY_LOCATION* out,
                          GpuAccess access) {
    *out = *in;
    u64 base;
    out->pResource = list_resource(in->pResource, &base, access);
    if (!out->pResource)
        return false;
    if (in->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        // Bytes in memory keep the console format's layout, which a stand-in format can't read.
        DXGI_FORMAT f = in->PlacedFootprint.Footprint.Format;
        if (format_has_stand_in(f) || format_desktop(f) == DXGI_FORMAT_UNKNOWN) {
            LOG_W_ONCE("d3d12",
                       "CopyTextureRegion through memory in format %d has no desktop equivalent; skipped", f);
            return false;
        }
        out->PlacedFootprint.Footprint.Format = format_desktop(f);
        out->PlacedFootprint.Offset += base;
    }
    return true;
}

static void WINAPI list_copy_texture_region(ConsoleCommandList* self, const D3D12_TEXTURE_COPY_LOCATION* dst,
                                            UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src,
                                            const D3D12_BOX* box) {
    D3D12_TEXTURE_COPY_LOCATION d, s;
    if (!copy_location(dst, &d, GPU_WRITE) || !copy_location(src, &s, GPU_READ)) {
        LOG_W_ONCE("d3d12", "CopyTextureRegion with a resource without desktop backing skipped");
        return;
    }
    self->desktop->CopyTextureRegion(&d, x, y, z, &s, box);
}

static void WINAPI list_copy_resource(ConsoleCommandList* self, void* dst, void* src) {
    ConsoleResource* dr = (ConsoleResource*)dst;
    u64 dst_base, src_base;
    ID3D12Resource* d = list_resource(dst, &dst_base, GPU_WRITE);
    ID3D12Resource* s = list_resource(src, &src_base, GPU_READ);
    if (!d || !s) {
        LOG_W_ONCE("d3d12", "CopyResource with a resource without desktop backing skipped");
        return;
    }
    // Buffers share desktop buffers, so only their own bytes are copied.
    if (dr->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        self->desktop->CopyBufferRegion(d, dst_base, s, src_base, dr->desc.Width);
    else
        self->desktop->CopyResource(d, s);
}

// The X copies add console copy flags. Most manage console compression metadata around the copy, which
// desktop drivers do on their own (CONSOLE_COPY_FLAGS_METADATA); copies of depth or stencil samples
// into color channels are copies between a depth and a color format, which desktop copies refuse
// (recording one would invalidate the list), so those are skipped.
static bool copy_flags_supported(u32 flags, const char* entry) {
    u32 unsupported = console_copy_flags_unsupported(flags);
    if (unsupported)
        LOG_W_ONCE("d3d12", "%s with copy flags 0x%x (0x%x have no desktop equivalent) skipped", entry, flags,
                   unsupported);
    return !unsupported;
}

static void WINAPI list_copy_texture_region_x(ConsoleCommandList* self,
                                              const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
                                              const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box,
                                              UINT flags) {
    if (copy_flags_supported(flags, "CopyTextureRegionX"))
        list_copy_texture_region(self, dst, x, y, z, src, box);
}

static void WINAPI list_copy_resource_x(ConsoleCommandList* self, void* dst, void* src, UINT flags) {
    if (copy_flags_supported(flags, "CopyResourceX"))
        list_copy_resource(self, dst, src);
}

static void WINAPI list_resolve_subresource(ConsoleCommandList* self, void* dst, UINT dst_subresource,
                                            void* src, UINT src_subresource, DXGI_FORMAT format) {
    u64 base;
    ID3D12Resource* d = list_resource(dst, &base, GPU_WRITE);
    ID3D12Resource* s = list_resource(src, &base, GPU_READ);
    if (d && s && format_desktop(format) != DXGI_FORMAT_UNKNOWN)
        self->desktop->ResolveSubresource(d, dst_subresource, s, src_subresource, format_desktop(format));
}

static void WINAPI list_resolve_subresource_region(ConsoleCommandList* self, void* dst, UINT dst_subresource,
                                                   UINT x, UINT y, void* src, UINT src_subresource,
                                                   D3D12_RECT* rect, DXGI_FORMAT format,
                                                   D3D12_RESOLVE_MODE mode) {
    u64 base;
    ID3D12Resource* d = list_resource(dst, &base, GPU_WRITE);
    ID3D12Resource* s = list_resource(src, &base, GPU_READ);
    if (!d || !s || format_desktop(format) == DXGI_FORMAT_UNKNOWN) {
        LOG_W_ONCE("d3d12", "ResolveSubresourceRegion without desktop textures or format %d skipped", format);
        return;
    }
    self->desktop->ResolveSubresourceRegion(d, dst_subresource, x, y, s, src_subresource, rect,
                                            format_desktop(format), mode);
}

// Atomic copies name the resources whose writes they publish; buffers are ranges of desktop buffers,
// so their byte ranges move by where each buffer starts.
static void atomic_copy(ConsoleCommandList* self, bool wide, void* dst, u64 dst_offset, void* src,
                        u64 src_offset, UINT count, void* const* dependents,
                        const D3D12_SUBRESOURCE_RANGE_UINT64* ranges) {
    ID3D12Resource* resources[16];
    D3D12_SUBRESOURCE_RANGE_UINT64 moved[16];
    u64 dst_base, src_base;
    ID3D12Resource* d = list_resource(dst, &dst_base, GPU_WRITE);
    ID3D12Resource* s = list_resource(src, &src_base, GPU_READ);
    if (!d || !s || count > ARRAY_COUNT(resources)) {
        LOG_W_ONCE("d3d12", "AtomicCopyBuffer with %u dependent resources or unbacked buffers skipped",
                   count);
        return;
    }
    for (UINT i = 0; i < count; i++) {
        u64 base;
        resources[i] = list_resource(dependents[i], &base, GPU_READ);
        if (!resources[i]) {
            LOG_W_ONCE("d3d12", "AtomicCopyBuffer dependent resource without desktop backing; skipped");
            return;
        }
        moved[i] = ranges[i];
        moved[i].Range.Begin += base;
        moved[i].Range.End += base;
    }
    if (wide)
        self->desktop->AtomicCopyBufferUINT64(d, dst_base + dst_offset, s, src_base + src_offset, count,
                                              resources, moved);
    else
        self->desktop->AtomicCopyBufferUINT(d, dst_base + dst_offset, s, src_base + src_offset, count,
                                            resources, moved);
}

static void WINAPI list_atomic_copy_buffer_uint(ConsoleCommandList* self, void* dst, UINT64 dst_offset,
                                                void* src, UINT64 src_offset, UINT count,
                                                void* const* dependents,
                                                const D3D12_SUBRESOURCE_RANGE_UINT64* ranges) {
    atomic_copy(self, false, dst, dst_offset, src, src_offset, count, dependents, ranges);
}

static void WINAPI list_atomic_copy_buffer_uint64(ConsoleCommandList* self, void* dst, UINT64 dst_offset,
                                                  void* src, UINT64 src_offset, UINT count,
                                                  void* const* dependents,
                                                  const D3D12_SUBRESOURCE_RANGE_UINT64* ranges) {
    atomic_copy(self, true, dst, dst_offset, src, src_offset, count, dependents, ranges);
}

static void WINAPI list_copy_memory_to_memory_x(ConsoleCommandList* self, UINT64 dst, UINT64 src, UINT64 size,
                                                UINT) {
    GpuAddress d, s;
    if (translate(dst, size, GPU_WRITE, &d) && translate(src, size, GPU_READ, &s))
        self->desktop->CopyBufferRegion(d.buffer, d.offset, s.buffer, s.offset, size);
}

// --- clears ------------------------------------------------------------------------------------------

static void WINAPI list_clear_depth_stencil_view(ConsoleCommandList* self, D3D12_CPU_DESCRIPTOR_HANDLE view,
                                                 D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil,
                                                 UINT count, const D3D12_RECT* rects) {
    D3D12_CPU_DESCRIPTOR_HANDLE d;
    if (list_cpu_descriptor(view.ptr, 1, &d))
        self->desktop->ClearDepthStencilView(d, flags, depth, stencil, count, rects);
}

static void WINAPI list_clear_render_target_view(ConsoleCommandList* self, D3D12_CPU_DESCRIPTOR_HANDLE view,
                                                 const FLOAT color[4], UINT count, const D3D12_RECT* rects) {
    D3D12_CPU_DESCRIPTOR_HANDLE d;
    if (list_cpu_descriptor(view.ptr, 1, &d))
        self->desktop->ClearRenderTargetView(d, color, count, rects);
}

// UAV clears name the view twice: in a bound shader-visible heap and in a CPU-only one.
static bool uav_clear_args(ConsoleCommandList* self, D3D12_GPU_DESCRIPTOR_HANDLE gpu,
                           D3D12_CPU_DESCRIPTOR_HANDLE cpu, void* resource,
                           D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
                           ID3D12Resource** out_resource) {
    u64 base;
    *out_resource = list_resource(resource, &base, GPU_WRITE);
    return *out_resource && list_gpu_descriptor(self, gpu.ptr, out_gpu) &&
           list_cpu_descriptor(cpu.ptr, 1, out_cpu);
}

static void WINAPI list_clear_uav_uint(ConsoleCommandList* self, D3D12_GPU_DESCRIPTOR_HANDLE gpu,
                                       D3D12_CPU_DESCRIPTOR_HANDLE cpu, void* resource, const UINT values[4],
                                       UINT count, const D3D12_RECT* rects) {
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    D3D12_CPU_DESCRIPTOR_HANDLE c;
    ID3D12Resource* r;
    if (uav_clear_args(self, gpu, cpu, resource, &g, &c, &r))
        self->desktop->ClearUnorderedAccessViewUint(g, c, r, values, count, rects);
}

static void WINAPI list_clear_uav_float(ConsoleCommandList* self, D3D12_GPU_DESCRIPTOR_HANDLE gpu,
                                        D3D12_CPU_DESCRIPTOR_HANDLE cpu, void* resource,
                                        const FLOAT values[4], UINT count, const D3D12_RECT* rects) {
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    D3D12_CPU_DESCRIPTOR_HANDLE c;
    ID3D12Resource* r;
    if (uav_clear_args(self, gpu, cpu, resource, &g, &c, &r))
        self->desktop->ClearUnorderedAccessViewFloat(g, c, r, values, count, rects);
}

// Discarding is a hint. Buffers share desktop buffers, so only textures pass it on.
static void WINAPI list_discard_resource(ConsoleCommandList* self, void* resource,
                                         const D3D12_DISCARD_REGION* region) {
    ConsoleResource* r = (ConsoleResource*)resource;
    if (!r || r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
        return;
    u64 base;
    ID3D12Resource* d = list_resource(resource, &base, GPU_READ);
    if (d)
        self->desktop->DiscardResource(d, region);
}

// --- memory writes -----------------------------------------------------------------------------------

// Small immediate writes: 32-bit values at title GPU addresses, ordered against earlier work. Title
// code polls such values (fences, counters) while the GPU writes them, so they go to title memory
// through a window as well as to the page tables, for later GPU work reading them.
static void write_values(ConsoleCommandList* self, u64 address, const u32* values, u32 count,
                         D3D12_WRITEBUFFERIMMEDIATE_MODE mode) {
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[128];
    D3D12_WRITEBUFFERIMMEDIATE_MODE modes[128];
    GpuAddress a;
    u64 coherent;
    if (count > ARRAY_COUNT(params) / 2 || !translate(address, (u64)count * 4, GPU_READ, &a) ||
        !console_translate_coherent(address, (u64)count * 4, &coherent))
        return;
    for (u32 i = 0; i < count; i++) {
        params[i] = {a.desktop_address + i * 4, values[i]};
        params[count + i] = {coherent + i * 4, values[i]};
        modes[i] = modes[count + i] = mode;
    }
    self->desktop->WriteBufferImmediate(count * 2, params, modes);
}

// A write once earlier work is done: how titles signal fences and counters from the GPU. The value
// goes to memory for title code polling it, and 64-bit writes also signal the desktop fence
// following the counter, for GPU waits on it: the list splits there and the queue signals in
// between. The low half is written first, so a reader never sees a counter ahead of its value.
static void write_bottom_of_pipe(ConsoleCommandList* self, u64 dst, u64 value, u32 words) {
    u32 halves[2] = {(u32)value, (u32)(value >> 32)};
    write_values(self, dst, halves, words, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT);
    ID3D12Fence* fence = words == 2 ? console_sync_fence(dst) : nullptr;
    if (fence)
        list_split(self, LIST_SYNC_SIGNAL, fence, value, 0);
}

static void WINAPI list_write_32bit_bottom_of_pipe_x(ConsoleCommandList* self, UINT64 dst, UINT value, UINT,
                                                     UINT) {
    write_bottom_of_pipe(self, dst, value, 1);
}

static void WINAPI list_write_64bit_bottom_of_pipe_x(ConsoleCommandList* self, UINT64 dst, UINT64 value, UINT,
                                                     UINT) {
    write_bottom_of_pipe(self, dst, value, 2);
}

// The sync flags say which caches and stages to wait for: all of them, on desktop.
static void WINAPI list_write_32bit_bottom_of_pipe_2x(ConsoleCommandList* self, UINT64 dst, UINT value, UINT,
                                                      UINT, UINT) {
    write_bottom_of_pipe(self, dst, value, 1);
}

static void WINAPI list_write_64bit_bottom_of_pipe_2x(ConsoleCommandList* self, UINT64 dst, UINT64 value,
                                                      UINT, UINT, UINT) {
    write_bottom_of_pipe(self, dst, value, 2);
}

// A write once one shader stage of earlier work is done: once all of it is, a little later than the
// console but never early.
static void WINAPI list_write_32bit_bottom_of_shader_x(ConsoleCommandList* self, UINT64 dst, UINT value,
                                                       UINT) {
    write_bottom_of_pipe(self, dst, value, 1);
}

static void WINAPI list_write_32bit_top_of_pipe_x(ConsoleCommandList* self, UINT64 dst, UINT value, UINT) {
    write_values(self, dst, &value, 1, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN);
}

static void WINAPI list_write_64bit_top_of_pipe_x(ConsoleCommandList* self, UINT64 dst, UINT64 value, UINT) {
    u32 halves[2] = {(u32)value, (u32)(value >> 32)};
    write_values(self, dst, halves, 2, D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN);
}

static void WINAPI list_write_buffer_immediate(ConsoleCommandList* self, UINT count,
                                               const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER* params,
                                               const D3D12_WRITEBUFFERIMMEDIATE_MODE* modes) {
    for (UINT i = 0; i < count; i++) {
        D3D12_WRITEBUFFERIMMEDIATE_MODE mode = modes ? modes[i] : D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
        write_values(self, params[i].Dest, &params[i].Value, 1, mode);
    }
}

// Fills of up to FILL_IMMEDIATE_BYTES are immediate writes, like other value writes. Larger ones write
// the pattern once and then double the filled part with copies inside the destination (each copy
// waits for the one before), so a fill of n bytes takes log2(n / FILL_IMMEDIATE_BYTES) copies.
#define FILL_IMMEDIATE_BYTES 256

static void fill(ConsoleCommandList* self, u64 dst, u64 size, const u32* pattern, u32 pattern_words) {
    u32 values[FILL_IMMEDIATE_BYTES / 4];
    if (size % 4 || dst % 4) {
        LOG_W_ONCE("d3d12", "memory fill of %llu bytes at 0x%llx isn't dword aligned; skipped", size, dst);
        return;
    }
    u64 words = size / 4;
    if (words <= ARRAY_COUNT(values)) {
        for (u64 i = 0; i < words; i++)
            values[i] = pattern[i % pattern_words];
        write_values(self, dst, values, (u32)words, D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT);
        return;
    }
    GpuAddress d;
    if (!translate(dst, size, GPU_WRITE, &d))
        return;
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER params[ARRAY_COUNT(values)];
    for (u32 i = 0; i < ARRAY_COUNT(values); i++)
        params[i] = {d.desktop_address + i * 4, pattern[i % pattern_words]};
    self->desktop->WriteBufferImmediate(ARRAY_COUNT(params), params, nullptr);
    // The filled part is always a whole number of patterns (FILL_IMMEDIATE_BYTES and its doublings are
    // multiples of 16 bytes), so each copy continues the pattern in phase.
    for (u64 filled = FILL_IMMEDIATE_BYTES; filled < size; filled *= 2) {
        console_record_full_barrier(self->desktop);
        u64 bytes = filled < size - filled ? filled : size - filled;
        self->desktop->CopyBufferRegion(d.buffer, d.offset + filled, d.buffer, d.offset, bytes);
    }
}

static void WINAPI list_fill_32bit_x(ConsoleCommandList* self, UINT64 dst, UINT64 size, UINT value, UINT) {
    fill(self, dst, size, &value, 1);
}

static void WINAPI list_fill_64bit_x(ConsoleCommandList* self, UINT64 dst, UINT64 size, UINT64 value, UINT) {
    u32 halves[2] = {(u32)value, (u32)(value >> 32)};
    fill(self, dst, size, halves, 2);
}

static void WINAPI list_fill_128bit_x(ConsoleCommandList* self, UINT64 dst, UINT64 size, UINT* value, UINT) {
    fill(self, dst, size, value, 4);
}

// --- queries and predication -------------------------------------------------------------------------

static void WINAPI list_begin_query(ConsoleCommandList* self, ConsoleQueryHeap* heap, D3D12_QUERY_TYPE type,
                                    UINT index) {
    if (heap)
        self->desktop->BeginQuery(heap->desktop, type, index);
}

static void WINAPI list_end_query(ConsoleCommandList* self, ConsoleQueryHeap* heap, D3D12_QUERY_TYPE type,
                                  UINT index) {
    if (heap)
        self->desktop->EndQuery(heap->desktop, type, index);
}

static void WINAPI list_resolve_query_data(ConsoleCommandList* self, ConsoleQueryHeap* heap,
                                           D3D12_QUERY_TYPE type, UINT start, UINT count, void* dst,
                                           UINT64 dst_offset) {
    u64 base;
    ID3D12Resource* d = list_resource(dst, &base, GPU_WRITE); // the CPU reads it back through a download
    if (heap && d)
        self->desktop->ResolveQueryData(heap->desktop, type, start, count, d, base + dst_offset);
}

static void WINAPI list_set_predication(ConsoleCommandList* self, void* buffer, UINT64 offset,
                                        D3D12_PREDICATION_OP op) {
    ListState& st = self->state;
    u64 base;
    st.predication = list_resource(buffer, &base, GPU_READ);
    st.predication_offset = st.predication ? base + offset : 0;
    st.predication_op = op;
    self->desktop->SetPredication(st.predication, st.predication_offset, op);
}

const ConsoleEntry k_list_work_entries[] = {
    CONSOLE_ENTRY(12, list_draw_instanced),
    CONSOLE_ENTRY(13, list_draw_indexed_instanced),
    CONSOLE_ENTRY(14, list_dispatch),
    CONSOLE_ENTRY(15, list_copy_buffer_region),
    CONSOLE_ENTRY(16, list_copy_texture_region),
    CONSOLE_ENTRY(17, list_copy_resource),
    CONSOLE_ENTRY(19, list_resolve_subresource),
    CONSOLE_ENTRY(28, list_begin_query),
    CONSOLE_ENTRY(29, list_end_query),
    CONSOLE_ENTRY(30, list_resolve_query_data),
    CONSOLE_ENTRY(31, list_set_predication),
    CONSOLE_ENTRY(51, list_clear_depth_stencil_view),
    CONSOLE_ENTRY(52, list_clear_render_target_view),
    CONSOLE_ENTRY(53, list_clear_uav_uint),
    CONSOLE_ENTRY(54, list_clear_uav_float),
    CONSOLE_ENTRY(55, list_discard_resource),
    CONSOLE_ENTRY(61, list_execute_indirect),
    CONSOLE_ENTRY(62, list_copy_memory_to_memory_x),
    CONSOLE_ENTRY(63, list_fill_32bit_x),
    CONSOLE_ENTRY(64, list_fill_64bit_x),
    CONSOLE_ENTRY(65, list_fill_128bit_x),
    CONSOLE_ENTRY(71, list_write_32bit_bottom_of_pipe_x),
    CONSOLE_ENTRY(72, list_write_64bit_bottom_of_pipe_x),
    CONSOLE_ENTRY(80, list_dispatch_x),
    CONSOLE_ENTRY(83, list_draw_indexed_x),
    CONSOLE_ENTRY(85, list_copy_texture_region_x),
    CONSOLE_ENTRY(86, list_copy_resource_x),
    CONSOLE_ENTRY(90, list_write_32bit_top_of_pipe_x),
    CONSOLE_ENTRY(91, list_write_64bit_top_of_pipe_x),
    CONSOLE_ENTRY(92, list_write_32bit_bottom_of_shader_x),
    CONSOLE_ENTRY(107, list_atomic_copy_buffer_uint),
    CONSOLE_ENTRY(108, list_atomic_copy_buffer_uint64),
    CONSOLE_ENTRY(111, list_resolve_subresource_region),
    CONSOLE_ENTRY(113, list_write_buffer_immediate),
    CONSOLE_ENTRY(137, list_write_32bit_bottom_of_pipe_2x),
    CONSOLE_ENTRY(138, list_write_64bit_bottom_of_pipe_2x),
};

const u32 k_list_work_entry_count = ARRAY_COUNT(k_list_work_entries);
