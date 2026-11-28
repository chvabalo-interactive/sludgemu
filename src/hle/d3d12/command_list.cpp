// Console ID3D12GraphicsCommandList (direct, compute, copy and bundle lists share the class):
// lifetime, splitting, barriers and state.
//
// The console class can write command packets inline, straight into its command buffer, when the
// put buffer's limit pointers leave room. We keep every limit null, so the title's inlined methods
// always take their fallback path through the function table, and the whole recording arrives
// here at API level, where it's recorded onto a desktop command list as it comes.
//
// Console command lists can signal and wait on fences in GPU memory mid-list. Desktop queues only
// signal and wait between command lists, so a console list is a series of desktop lists (segments)
// with a fence operation between each two, run in order at submission. Every segment after the
// first starts with the state the list had set (ListState).

#include "hle/d3d12/command_list.h"
#include "hle/d3d12/views.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "common/log.h"
#include "gpu/gpu.h"
#include "gpu/shader_programs.h"
#include "hle/d3d12/barriers.h"
#include "hle/d3d12/console_iids.h"
#include "hle/d3d12/d3d12_tables.h"

static_assert(offsetof(ConsoleCommandList, type) == 0x10, "command list layout");
static_assert(offsetof(ConsoleCommandList, putter) == 0x18, "command list layout");
static_assert(sizeof(ConsolePutBuffer) == 296, "put buffer layout");
static_assert(offsetof(ConsoleCommandList, cpu_state) == 0x140, "command list layout");
static_assert(offsetof(ConsoleCommandList, graphics_root_packet_header) == 0x144, "command list layout");
static_assert(offsetof(ConsoleCommandList, compute_root_packet_header) == 0x344, "command list layout");
static_assert(offsetof(ConsoleCommandList, graphics_pso_shadow) == 0x544, "command list layout");
static_assert(offsetof(ConsoleCommandList, desktop) == 0x568,
              "command list state follows the public members");

static const GUID* const k_list_iids[] = {
    &__uuidof(ID3D12Object),
    &__uuidof(ID3D12DeviceChild),
    &__uuidof(ID3D12CommandList),
    &__uuidof(ID3D12GraphicsCommandList),
    &__uuidof(ID3D12GraphicsCommandList1),
    &__uuidof(ID3D12GraphicsCommandList2),
    &__uuidof(ID3D12GraphicsCommandList3),
    &__uuidof(ID3D12GraphicsCommandList4),
    &IID_ConsoleGraphicsCommandList5,
    &IID_ConsoleGraphicsCommandList6,
    &__uuidof(ID3D12GraphicsCommandList7),
    &__uuidof(ID3D12GraphicsCommandList8),
};

static void list_destroy(void* object);

static const ConsoleClass k_list_class = {"ID3D12GraphicsCommandList", k_list_iids, ARRAY_COUNT(k_list_iids),
                                          list_destroy};

static void* const* g_list_table = nullptr;

// --- shared helpers ----------------------------------------------------------------------------------

bool list_gpu_descriptor(const ConsoleCommandList* list, u64 handle, D3D12_GPU_DESCRIPTOR_HANDLE* out) {
    for (u32 i = 0; i < ARRAY_COUNT(list->state.heaps); i++) {
        const ConsoleDescriptorHeap* heap = list->state.heaps[i];
        if (!heap || handle < (u64)heap->memory)
            continue;
        u64 index = (handle - (u64)heap->memory) / heap->stride;
        if (index < heap->identity_count) {
            out->ptr = heap->visible_gpu_start + index * heap->desktop_stride;
            return true;
        }
    }
    LOG_W_ONCE("d3d12", "descriptor table 0x%llx is not in a bound descriptor heap", handle);
    return false;
}

bool list_descriptor_index(const ConsoleCommandList* list, u64 handle, u32* out) {
    for (u32 i = 0; i < ARRAY_COUNT(list->state.heaps); i++) {
        const ConsoleDescriptorHeap* heap = list->state.heaps[i];
        if (!heap || handle < (u64)heap->memory)
            continue;
        u64 index = (handle - (u64)heap->memory) / heap->stride;
        if (index < heap->desc.NumDescriptors) {
            *out = (u32)index;
            return true;
        }
    }
    LOG_W_ONCE("d3d12", "descriptor table 0x%llx is not in a bound descriptor heap", handle);
    return false;
}

bool list_cpu_descriptor(u64 handle, u32 count, D3D12_CPU_DESCRIPTOR_HANDLE* out) {
    DescriptorSlot slot;
    if (!console_descriptor_slots(handle, count, &slot))
        return false;
    *out = slot.cpu;
    return true;
}

ID3D12Resource* list_resource(void* console_resource, u64* offset, GpuAccess access) {
    *offset = 0;
    if (!console_resource)
        return nullptr;
    return console_resource_desktop((ConsoleResource*)console_resource, offset, access);
}

// Root descriptors and buffer views: title GPU addresses become desktop ones. Address 0 stays 0.
static u64 desktop_address(u64 address, u64 size, GpuAccess access) {
    u64 out = 0;
    if (address && !console_translate_gpu_address(address, size ? size : 1, access, &out))
        LOG_W_ONCE("d3d12", "GPU address 0x%llx is outside GPU-visible memory", address);
    return out;
}

// Root descriptors carry no size: what the shader reads past the address must be mapped as well. A
// constant buffer reaches at most 64 KiB (4096 vectors); raw buffers as far as the title's data goes,
// which nothing states, so the same is assumed.
#define ROOT_DESCRIPTOR_REACH KiB(64)

static u64 root_address(u64 address, GpuAccess access) {
    GpuAddress a;
    if (!address)
        return 0;
    if (!gpu_translate_reach(address, ROOT_DESCRIPTOR_REACH, access, &a)) {
        LOG_W_ONCE("d3d12", "GPU address 0x%llx is outside GPU-visible memory", address);
        return 0;
    }
    return a.desktop_address;
}

// --- state -------------------------------------------------------------------------------------------

// Console topologies desktop D3D12 lacks (the triangle fan only on some GPUs).
#define CONSOLE_TOPOLOGY_TRIANGLE_FAN 6
#define CONSOLE_TOPOLOGY_LINE_LOOP    18
#define CONSOLE_TOPOLOGY_QUAD_LIST    19
#define CONSOLE_TOPOLOGY_QUAD_STRIP   20

static bool g_triangle_fans; // the desktop GPU draws triangle fans

static bool is_desktop_topology(u32 t) {
    return t <= D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
           (t >= D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ && t <= D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ) ||
           (t >= D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST &&
            t <= D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST);
}

// The topology the desktop list gets. Drivers program the value as given, and the GPU faults on one
// it doesn't know as soon as the list runs, so console-only values never reach them. A quad strip
// covers what a triangle strip does. Rectangle lists, line loops, quad lists and unsupported fans
// have no desktop form; they stand in as the list or strip of the same primitive type, and draws
// with them need expanding (the list state keeps the console topology for that).
static D3D12_PRIMITIVE_TOPOLOGY desktop_topology(D3D12_PRIMITIVE_TOPOLOGY topology) {
    u32 t = (u32)topology;
    if (is_desktop_topology(t) || (t == CONSOLE_TOPOLOGY_TRIANGLE_FAN && g_triangle_fans))
        return topology;
    if (t == CONSOLE_TOPOLOGY_QUAD_STRIP)
        return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    D3D12_PRIMITIVE_TOPOLOGY stand_in;
    switch (t) {
    case CONSOLE_TOPOLOGY_TRIANGLE_FAN:
    case CONSOLE_TOPOLOGY_RECT_LIST:
    case CONSOLE_TOPOLOGY_QUAD_LIST:
        stand_in = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
    case CONSOLE_TOPOLOGY_LINE_LOOP:
        stand_in = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        break;
    default:
        LOG_W_ONCE("d3d12", "IASetPrimitiveTopology: unknown topology %u; left undefined", t);
        return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
    LOG_W_ONCE("d3d12",
               "primitive topology %u has no desktop equivalent; recorded as %d until draws expand it", t,
               stand_in);
    return stand_in;
}

// Tables used by the recording, mirrored again at submission.
static void list_note_table(ConsoleCommandList* self, ConsoleDescriptorHeap* heap, u32 first, u32 count,
                            u32 desktop_first) {
    if (self->table_count) {
        const ConsoleCommandList::TableUse& last = self->tables[self->table_count - 1];
        if (last.heap == heap && last.first == first && last.desktop_first == desktop_first)
            return;
    }
    if (self->table_count == self->table_capacity) {
        u32 capacity = self->table_capacity ? self->table_capacity * 2 : 256;
        void* grown = realloc(self->tables, capacity * sizeof(*self->tables));
        if (!grown)
            return;
        self->tables = (ConsoleCommandList::TableUse*)grown;
        self->table_capacity = capacity;
    }
    self->tables[self->table_count++] = {heap, first, count, desktop_first};
}

// Where shaders find a table: its own index, or a relocation, which the list reuses while it binds
// the same table again (draws often rebind tables between other root changes).
static u32 list_place_table(ConsoleCommandList* self, ConsoleDescriptorHeap* heap, u32 first, u32 count) {
    if (first + count <= heap->identity_count)
        return first;
    ConsoleCommandList::Relocation& r = self->relocations[first % ARRAY_COUNT(self->relocations)];
    if (r.heap == heap && r.first == first && r.count == count)
        return r.desktop_first;
    u32 desktop_first = console_descriptor_place(heap, first, count);
    if (desktop_first != ~0u)
        r = {heap, first, count, desktop_first};
    return desktop_first;
}

// Descriptors a table covers by its ranges. Unbounded ranges are taken as reaching a little way:
// a bindless table can be huge, and the title copies what it uses near the start.
#define UNBOUNDED_TABLE_EXTENT 64

static u32 table_extent(const ConsoleRootParameter& p) {
    u32 extent = 0;
    u32 next = 0;
    for (u32 r = 0; r < p.range_count; r++) {
        u32 offset = p.ranges[r].offset == ~0u ? next : p.ranges[r].offset;
        u32 end = p.ranges[r].count == ~0u ? offset + UNBOUNDED_TABLE_EXTENT : offset + p.ranges[r].count;
        next = end;
        if (end > extent)
            extent = end;
    }
    return extent;
}

// The user data image translated shaders read: root arguments where the console puts them in user
// SGPRs, descriptor tables as byte offsets into the desktop heaps. Root descriptors go to their own
// desktop root parameters.
// The slots of table parameter `index` the pipeline's programs read: what they load from, when that
// reaches past the root signature's ranges (`declared`).
static u32 table_reach(const ConsolePipelineState* pso, u32 index, u32 declared) {
    u32 extent = declared;
    for (u32 i = 0; pso && i < 2 && index < SHADER_MAX_ROOT_PARAMETERS; i++) {
        const ShaderProgram* p = pso->programs[i];
        if (p && p->state == PROGRAM_READY && p->binary.table_reach[index] != TABLE_REACH_UNKNOWN &&
            p->binary.table_reach[index] > extent)
            extent = p->binary.table_reach[index];
    }
    return extent;
}

void list_flush_user_data(ConsoleCommandList* self, bool graphics) {
    const RootArgs& args = graphics ? self->state.graphics : self->state.compute;
    const ConsoleRootSignature* rs = args.signature;
    if (!rs || !rs->desktop || !self->state.user_data_dirty[graphics ? 0 : 1])
        return;
    self->state.user_data_dirty[graphics ? 0 : 1] = false;
    ID3D12GraphicsCommandList8* l = self->desktop;
    // Compute shaders find root parameters lower than graphics ones.
    u32 shift = graphics ? 0 : CONSOLE_FIRST_ROOT_SGPR_GRAPHICS - CONSOLE_FIRST_ROOT_SGPR_COMPUTE;
    u32 image[64] = {};
    const D3D12_ROOT_SIGNATURE_DESC1* desc = rs->desc;
    for (u32 i = 0; i < desc->NumParameters && i < ROOT_MAX_PARAMETERS; i++) {
        u32 sgpr = rs->user_sgpr[i] - shift;
        u64 v = args.value[i];
        switch (args.kind[i]) {
        case ROOT_ARG_NONE:
            break;
        case ROOT_ARG_TABLE: {
            const ConsoleRootParameter& p = rs->layout.parameters[i];
            ConsoleDescriptorHeap* heap = self->state.heaps[p.sampler_table ? 1 : 0];
            u32 extent = table_reach(self->state.pipeline, i, table_extent(p));
            u32 desktop_first = heap ? list_place_table(self, heap, (u32)v, extent) : ~0u;
            if (desktop_first != ~0u) {
                console_descriptor_mirror(heap, (u32)v, extent, desktop_first);
                list_note_table(self, heap, (u32)v, extent, desktop_first);
            } else {
                LOG_W_ONCE("d3d12", "descriptor table at slot %llu has no shader-visible place", v);
                desktop_first = 0;
            }
            if (sgpr < ARRAY_COUNT(image))
                image[sgpr] = desktop_first * (p.sampler_table ? CONSOLE_DESCRIPTOR_SIZE_SAMPLER
                                                               : CONSOLE_DESCRIPTOR_SIZE_RESOURCE);
            break;
        }
        case ROOT_ARG_CONSTANTS: {
            u32 count = desc->pParameters[i].Constants.Num32BitValues;
            if (sgpr + count <= ARRAY_COUNT(image))
                memcpy(&image[sgpr], &args.constants[rs->constant_offset[i]], count * sizeof(u32));
            break;
        }
        case ROOT_ARG_CBV:
        case ROOT_ARG_SRV:
            if (rs->desktop_parameter[i] >= 0)
                graphics ? l->SetGraphicsRootShaderResourceView(rs->desktop_parameter[i], v)
                         : l->SetComputeRootShaderResourceView(rs->desktop_parameter[i], v);
            break;
        case ROOT_ARG_UAV:
            if (rs->desktop_parameter[i] >= 0)
                graphics ? l->SetGraphicsRootUnorderedAccessView(rs->desktop_parameter[i], v)
                         : l->SetComputeRootUnorderedAccessView(rs->desktop_parameter[i], v);
            break;
        }
    }
    graphics ? l->SetGraphicsRoot32BitConstants(0, rs->user_dwords, image, 0)
             : l->SetComputeRoot32BitConstants(0, rs->user_dwords, image, 0);
    // The shadow of the bound resource heap, which translated programs index like the heap.
    ConsoleDescriptorHeap* resources = self->state.heaps[0];
    if (rs->shadow_parameter >= 0 && resources && resources->shadow) {
        graphics ? l->SetGraphicsRootShaderResourceView(rs->shadow_parameter, resources->shadow_gpu)
                 : l->SetComputeRootShaderResourceView(rs->shadow_parameter, resources->shadow_gpu);
    }
}

// Sets everything the list had set on a fresh desktop list.
static void apply_state(ConsoleCommandList* self) {
    ListState& s = self->state;
    ID3D12GraphicsCommandList8* l = self->desktop;
    if (self->type == D3D12_COMMAND_LIST_TYPE_COPY)
        return;
    ID3D12DescriptorHeap* heaps[2];
    u32 heap_count = 0;
    for (u32 i = 0; i < 2; i++)
        if (s.heaps[i])
            heaps[heap_count++] = s.heaps[i]->visible;
    if (heap_count)
        l->SetDescriptorHeaps(heap_count, heaps);
    if (s.bound)
        l->SetPipelineState(s.bound);
    // Signatures and root arguments reach the new list with the next draw or dispatch.
    s.bound_signature[0] = s.bound_signature[1] = nullptr;
    self->state.user_data_dirty[1] = true;
    if (s.predication)
        l->SetPredication(s.predication, s.predication_offset, s.predication_op);
    if (self->type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    self->state.user_data_dirty[0] = true;
    if (s.topology != D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)
        l->IASetPrimitiveTopology(desktop_topology(s.topology));
    for (u32 i = 0; i < D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; i++)
        if (s.vertex_buffer_mask & (1u << i))
            l->IASetVertexBuffers(i, 1, &s.vertex_buffers[i]);
    if (s.has_index_buffer)
        l->IASetIndexBuffer(&s.index_buffer);
    if (s.viewport_count)
        l->RSSetViewports(s.viewport_count, s.viewports);
    if (s.scissor_count)
        l->RSSetScissorRects(s.scissor_count, s.scissors);
    if (s.has_targets)
        l->OMSetRenderTargets(s.render_target_count, s.render_target_count ? s.render_targets : nullptr,
                              FALSE, s.has_depth ? &s.depth : nullptr);
    if (s.has_blend_factor)
        l->OMSetBlendFactor(s.blend_factor);
    if (s.has_stencil_ref)
        l->OMSetFrontAndBackStencilRef(s.stencil_front, s.stencil_back);
    if (s.has_depth_bounds)
        l->OMSetDepthBounds(s.depth_min, s.depth_max);
    list_apply_x_state(self);
}

// --- segments ----------------------------------------------------------------------------------------

// Starts recording segment `index` on the list's allocator: a new desktop list, or a closed one
// from an earlier recording reset onto the allocator.
static HRESULT begin_segment(ConsoleCommandList* self, u32 index) {
    if (index == self->segment_capacity) {
        u32 capacity = self->segment_capacity ? self->segment_capacity * 2 : 2;
        ListSegment* grown = (ListSegment*)realloc(self->segments, capacity * sizeof(ListSegment));
        if (!grown)
            return E_OUTOFMEMORY;
        memset(grown + self->segment_capacity, 0, (capacity - self->segment_capacity) * sizeof(ListSegment));
        self->segments = grown;
        self->segment_capacity = capacity;
    }
    ListSegment& seg = self->segments[index];
    HRESULT hr;
    if (seg.list)
        hr = seg.list->Reset(self->allocator->desktop, nullptr);
    else
        hr = self->device->CreateCommandList(0, self->type, self->allocator->desktop, nullptr,
                                             IID_PPV_ARGS(&seg.list));
    if (SUCCEEDED(hr))
        gpu_set_name(seg.list, "title list %p part %u", (void*)self, index);
    if (FAILED(hr)) {
        LOG_E("d3d12", "cannot start part %u of a command list (0x%08lx)", index, (unsigned long)hr);
        return hr;
    }
    seg.fence = nullptr;
    seg.closed = false;
    self->desktop = seg.list;
    self->segment_count = index + 1;
    return S_OK;
}

static void release_segment_fences(ConsoleCommandList* self) {
    for (u32 i = 0; i < self->segment_count; i++) {
        if (self->segments[i].fence)
            self->segments[i].fence->Release();
        self->segments[i].fence = nullptr;
    }
}

// Closes the part being recorded. A part that doesn't close holds an invalid command: it is left out of
// submissions (see ListSegment::closed), logged with what desktop D3D12 said.
static void close_segment(ConsoleCommandList* self) {
    ListSegment& seg = self->segments[self->segment_count - 1];
    HRESULT hr = self->desktop->Close();
    seg.closed = SUCCEEDED(hr);
    if (FAILED(hr) && !gpu_device_lost(hr)) {
        static volatile long logged = 0;
        if (log_once_flag(&logged))
            LOG_E("d3d12", "part %u of a command list doesn't close (0x%08lx): it recorded an invalid command; "
                  "parts like it are left out of submissions (run with --gpu-debug to see which command)",
                  self->segment_count - 1, (unsigned long)hr);
    }
}

bool list_split(ConsoleCommandList* self, ListSyncKind sync, ID3D12Fence* fence, u64 value, u64 counter) {
    if (!self->recording)
        return false;
    fence->AddRef();
    close_segment(self);
    ListSegment& seg = self->segments[self->segment_count - 1];
    seg.sync = sync;
    seg.fence = fence;
    seg.value = value;
    seg.counter = counter;
    if (FAILED(begin_segment(self, self->segment_count))) {
        // Keep recording somewhere: the rest of the list goes nowhere, as after a failed Reset.
        self->recording = false;
        return false;
    }
    apply_state(self);
    return true;
}

// --- lifetime ----------------------------------------------------------------------------------------

static void list_destroy(void* object) {
    ConsoleCommandList* self = (ConsoleCommandList*)object;
    release_segment_fences(self);
    for (u32 i = 0; i < self->segment_capacity; i++)
        gpu_retire(self->segments[i].list);
    free(self->segments);
    free(self->tables);
}

static ID3D12PipelineState* desktop_pso(void* console_pso) {
    if (console_pso)
        LOG_W_ONCE("d3d12", "command list: pipeline states are not translated yet, initial state ignored");
    return nullptr;
}

static D3D12_COMMAND_LIST_TYPE WINAPI list_get_type(ConsoleCommandList* self) {
    return self->type;
}

static HRESULT WINAPI list_close(ConsoleCommandList* self) {
    if (!self->recording)
        return E_FAIL;
    self->recording = false;
    close_segment(self);
    return S_OK; // a part that didn't close is left out of submissions; the title's list stays usable
}

static HRESULT WINAPI list_reset_x(ConsoleCommandList* self, ConsoleCommandAllocator* allocator,
                                   void* initial_state, UINT flags) {
    if (!allocator)
        return E_INVALIDARG;
    // Lists are created recording. Titles may reset one straight away, which desktop D3D12 only
    // allows once it's closed.
    if (self->recording)
        self->desktop->Close();
    release_segment_fences(self);
    memset(&self->state, 0, sizeof(self->state));
    self->table_count = 0;
    memset(self->relocations, 0, sizeof(self->relocations));
    self->allocator = allocator;
    HRESULT hr = begin_segment(self, 0);
    self->recording = SUCCEEDED(hr);
    desktop_pso(initial_state);
    (void)flags; // sync and state-minimizing hints for the console command processor
    return hr;
}

static HRESULT WINAPI list_reset(ConsoleCommandList* self, ConsoleCommandAllocator* allocator,
                                 void* initial_state) {
    return list_reset_x(self, allocator, initial_state, 0);
}

static void WINAPI list_clear_state(ConsoleCommandList* self, void* pso) {
    self->desktop->ClearState(desktop_pso(pso));
    memset(&self->state, 0, sizeof(self->state));
}

// --- barriers and synchronization --------------------------------------------------------------------

static void WINAPI list_resource_barrier(ConsoleCommandList* self, UINT count,
                                         const D3D12_RESOURCE_BARRIER* barriers) {
    console_record_resource_barriers(self->desktop, count, barriers);
}

// The flags only control how the console splits the barrier into cache and pipeline operations.
static void WINAPI list_resource_barrier_x(ConsoleCommandList* self, UINT count,
                                           const D3D12_RESOURCE_BARRIER* barriers, UINT, void* actions) {
    if (actions)
        LOG_W_ONCE("d3d12", "ResourceBarrierX: summary actions are not reported");
    console_record_resource_barriers(self->desktop, count, barriers);
}

static void WINAPI list_barrier(ConsoleCommandList* self, UINT count, const D3D12_BARRIER_GROUP* groups) {
    console_record_barrier_groups(self->desktop, count, groups);
}

// Console cache flushes and pipeline waits; on desktop, a barrier covering everything.
static void WINAPI list_sync_x(ConsoleCommandList* self, UINT, UINT, UINT64, SIZE_T) {
    console_record_full_barrier(self->desktop);
}

static void WINAPI list_flush_pipeline_x(ConsoleCommandList* self, UINT, UINT64, SIZE_T) {
    console_record_full_barrier(self->desktop);
}

// The GPU waits until a value in memory compares as asked. Titles wait for counters (fences
// included) to reach a value, which is a wait on the desktop fence following the counter; other
// waits have no desktop equivalent.
static void wait_value(ConsoleCommandList* self, u64 address, D3D12_COMPARISON_FUNC func, u64 value,
                       u64 mask) {
    ID3D12Fence* fence = mask == UINT64_MAX ? console_sync_fence(address) : nullptr;
    if (!fence || (func != D3D12_COMPARISON_FUNC_GREATER_EQUAL && func != D3D12_COMPARISON_FUNC_GREATER)) {
        LOG_W_ONCE("d3d12", "GPU wait on 0x%llx (comparison %d, mask 0x%llx) is not supported; skipped",
                   address, func, mask);
        return;
    }
    list_split(self, LIST_SYNC_WAIT, fence, func == D3D12_COMPARISON_FUNC_GREATER ? value + 1 : value,
               address);
}

static void WINAPI list_wait_32bit_value_x(ConsoleCommandList* self, UINT64 address,
                                           D3D12_COMPARISON_FUNC func, UINT value, UINT mask, UINT) {
    wait_value(self, address, func, value, mask == UINT32_MAX ? UINT64_MAX : mask);
}

static void WINAPI list_wait_64bit_value_x(ConsoleCommandList* self, UINT64 address,
                                           D3D12_COMPARISON_FUNC func, UINT64 value, UINT) {
    wait_value(self, address, func, value, UINT64_MAX);
}

// --- pipeline state and root arguments ---------------------------------------------------------------

static void WINAPI list_set_pipeline_state(ConsoleCommandList* self, void* pso) {
    self->state.pipeline = (ConsolePipelineState*)pso; // the desktop one is found at the next draw
    self->state.has_sample_mask_x = false;             // a new pipeline brings back its own sample mask
}

static void WINAPI list_set_descriptor_heaps(ConsoleCommandList* self, UINT count,
                                             ConsoleDescriptorHeap** heaps) {
    ID3D12DescriptorHeap* desktop[2];
    u32 n = 0;
    self->state.heaps[0] = nullptr;
    self->state.heaps[1] = nullptr;
    for (UINT i = 0; i < count && n < 2; i++) {
        ConsoleDescriptorHeap* heap = heaps[i];
        if (!heap || !heap->visible)
            continue;
        self->state.heaps[heap->desktop_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? 1 : 0] = heap;
        desktop[n++] = heap->visible;
    }
    self->desktop->SetDescriptorHeaps(n, desktop);
    // Tables are placed in the bound heaps and the shadow is the resource heap's: both go again.
    self->state.user_data_dirty[0] = self->state.user_data_dirty[1] = true;
}

// A new root signature drops the arguments set for the old one.
static void set_root_signature(RootArgs* args, ConsoleRootSignature* signature) {
    if (args->signature != signature)
        memset(args->kind, ROOT_ARG_NONE, sizeof(args->kind));
    args->signature = signature;
}

static void WINAPI list_set_compute_root_signature(ConsoleCommandList* self,
                                                   ConsoleRootSignature* signature) {
    // The desktop signature goes with the next dispatch: it depends on the pipeline too.
    set_root_signature(&self->state.compute, signature);
    self->state.user_data_dirty[1] = true;
}

static void WINAPI list_set_graphics_root_signature(ConsoleCommandList* self,
                                                    ConsoleRootSignature* signature) {
    // The desktop signature goes with the next draw: it depends on the pipeline too.
    set_root_signature(&self->state.graphics, signature);
    self->state.user_data_dirty[0] = true;
}

static bool record_root_arg(ConsoleCommandList* self, bool graphics, UINT index, RootArgKind kind,
                            u64 value) {
    RootArgs* args = graphics ? &self->state.graphics : &self->state.compute;
    if (index >= ROOT_MAX_PARAMETERS)
        return false;
    args->kind[index] = kind;
    args->value[index] = value;
    self->state.user_data_dirty[graphics ? 0 : 1] = true;
    return true;
}

// Root constants are kept at their parameter's place among all of the signature's constants.
static bool record_root_constants(ConsoleCommandList* self, bool graphics, UINT index, UINT count,
                                  const void* values, UINT offset) {
    RootArgs* args = graphics ? &self->state.graphics : &self->state.compute;
    if (!args->signature || index >= ROOT_MAX_PARAMETERS)
        return false;
    self->state.user_data_dirty[graphics ? 0 : 1] = true;
    u32 start = args->signature->constant_offset[index] + offset;
    if (start + count > ARRAY_COUNT(args->constants))
        return false;
    memcpy(&args->constants[start], values, count * sizeof(u32));
    args->kind[index] = ROOT_ARG_CONSTANTS;
    return true;
}

static void WINAPI list_set_compute_root_descriptor_table(ConsoleCommandList* self, UINT index,
                                                          D3D12_GPU_DESCRIPTOR_HANDLE table) {
    u32 slot;
    if (list_descriptor_index(self, table.ptr, &slot))
        record_root_arg(self, false, index, ROOT_ARG_TABLE, slot);
}

static void WINAPI list_set_graphics_root_descriptor_table(ConsoleCommandList* self, UINT index,
                                                           D3D12_GPU_DESCRIPTOR_HANDLE table) {
    u32 slot;
    if (list_descriptor_index(self, table.ptr, &slot))
        record_root_arg(self, true, index, ROOT_ARG_TABLE, slot);
}

static void WINAPI list_set_compute_root_32bit_constant(ConsoleCommandList* self, UINT index, UINT value,
                                                        UINT offset) {
    record_root_constants(self, false, index, 1, &value, offset);
}

static void WINAPI list_set_graphics_root_32bit_constant(ConsoleCommandList* self, UINT index, UINT value,
                                                         UINT offset) {
    record_root_constants(self, true, index, 1, &value, offset);
}

static void WINAPI list_set_compute_root_32bit_constants(ConsoleCommandList* self, UINT index, UINT count,
                                                         const void* values, UINT offset) {
    record_root_constants(self, false, index, count, values, offset);
}

static void WINAPI list_set_graphics_root_32bit_constants(ConsoleCommandList* self, UINT index, UINT count,
                                                          const void* values, UINT offset) {
    record_root_constants(self, true, index, count, values, offset);
}

static void WINAPI list_set_compute_root_cbv(ConsoleCommandList* self, UINT index, UINT64 address) {
    u64 a = root_address(address, GPU_READ);
    record_root_arg(self, false, index, ROOT_ARG_CBV, a);
}

static void WINAPI list_set_graphics_root_cbv(ConsoleCommandList* self, UINT index, UINT64 address) {
    u64 a = root_address(address, GPU_READ);
    record_root_arg(self, true, index, ROOT_ARG_CBV, a);
}

static void WINAPI list_set_compute_root_srv(ConsoleCommandList* self, UINT index, UINT64 address) {
    u64 a = root_address(address, GPU_READ);
    record_root_arg(self, false, index, ROOT_ARG_SRV, a);
}

static void WINAPI list_set_graphics_root_srv(ConsoleCommandList* self, UINT index, UINT64 address) {
    u64 a = root_address(address, GPU_READ);
    record_root_arg(self, true, index, ROOT_ARG_SRV, a);
}

static void WINAPI list_set_compute_root_uav(ConsoleCommandList* self, UINT index, UINT64 address) {
    u64 a = root_address(address, GPU_WRITE);
    record_root_arg(self, false, index, ROOT_ARG_UAV, a);
}

static void WINAPI list_set_graphics_root_uav(ConsoleCommandList* self, UINT index, UINT64 address) {
    u64 a = root_address(address, GPU_WRITE);
    record_root_arg(self, true, index, ROOT_ARG_UAV, a);
}

// --- input assembly, rasterizer and output merger ----------------------------------------------------

static void WINAPI list_ia_set_primitive_topology(ConsoleCommandList* self,
                                                  D3D12_PRIMITIVE_TOPOLOGY topology) {
    self->state.topology = topology;
    self->desktop->IASetPrimitiveTopology(desktop_topology(topology));
}

static void WINAPI list_ia_set_index_buffer(ConsoleCommandList* self, const D3D12_INDEX_BUFFER_VIEW* view) {
    ListState& s = self->state;
    s.has_index_buffer = view != nullptr;
    if (!view) {
        self->desktop->IASetIndexBuffer(nullptr);
        return;
    }
    s.index_buffer = *view;
    s.index_buffer.BufferLocation = desktop_address(view->BufferLocation, view->SizeInBytes, GPU_READ);
    self->desktop->IASetIndexBuffer(&s.index_buffer);
}

static void WINAPI list_ia_set_vertex_buffers(ConsoleCommandList* self, UINT start, UINT count,
                                              const D3D12_VERTEX_BUFFER_VIEW* views) {
    ListState& s = self->state;
    if (start >= ARRAY_COUNT(s.vertex_buffers))
        return;
    if (count > ARRAY_COUNT(s.vertex_buffers) - start)
        count = ARRAY_COUNT(s.vertex_buffers) - start;
    for (UINT i = 0; i < count; i++) {
        D3D12_VERTEX_BUFFER_VIEW& v = s.vertex_buffers[start + i];
        if (views) {
            v = views[i];
            v.BufferLocation = desktop_address(v.BufferLocation, v.SizeInBytes, GPU_READ);
            s.vertex_buffer_mask |= 1u << (start + i);
        } else {
            v = {};
            s.vertex_buffer_mask &= ~(1u << (start + i));
        }
    }
    self->desktop->IASetVertexBuffers(start, count, views ? &s.vertex_buffers[start] : nullptr);
}

static void WINAPI list_rs_set_viewports(ConsoleCommandList* self, UINT count,
                                         const D3D12_VIEWPORT* viewports) {
    ListState& s = self->state;
    s.viewport_count = count < ARRAY_COUNT(s.viewports) ? count : ARRAY_COUNT(s.viewports);
    memcpy(s.viewports, viewports, s.viewport_count * sizeof(D3D12_VIEWPORT));
    self->desktop->RSSetViewports(s.viewport_count, s.viewports);
}

static void WINAPI list_rs_set_scissor_rects(ConsoleCommandList* self, UINT count, const D3D12_RECT* rects) {
    ListState& s = self->state;
    s.scissor_count = count < ARRAY_COUNT(s.scissors) ? count : ARRAY_COUNT(s.scissors);
    memcpy(s.scissors, rects, s.scissor_count * sizeof(D3D12_RECT));
    self->desktop->RSSetScissorRects(s.scissor_count, s.scissors);
}

static void WINAPI list_om_set_blend_factor(ConsoleCommandList* self, const FLOAT factor[4]) {
    ListState& s = self->state;
    static const f32 k_default[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // what a null factor means
    memcpy(s.blend_factor, factor ? factor : k_default, sizeof(s.blend_factor));
    s.has_blend_factor = true;
    self->desktop->OMSetBlendFactor(s.blend_factor);
}

static void WINAPI list_om_set_front_and_back_stencil_ref(ConsoleCommandList* self, UINT front, UINT back) {
    ListState& s = self->state;
    s.stencil_front = front;
    s.stencil_back = back;
    s.has_stencil_ref = true;
    self->desktop->OMSetFrontAndBackStencilRef(front, back);
}

static void WINAPI list_om_set_stencil_ref(ConsoleCommandList* self, UINT ref) {
    list_om_set_front_and_back_stencil_ref(self, ref, ref);
}

static void WINAPI list_om_set_depth_bounds(ConsoleCommandList* self, FLOAT min, FLOAT max) {
    ListState& s = self->state;
    s.depth_min = min;
    s.depth_max = max;
    s.has_depth_bounds = true;
    self->desktop->OMSetDepthBounds(min, max);
}

static void WINAPI list_om_set_render_targets(ConsoleCommandList* self, UINT count,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE* targets, BOOL single_range,
                                              const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
    ListState& s = self->state;
    if (count > ARRAY_COUNT(s.render_targets))
        count = ARRAY_COUNT(s.render_targets);
    // Console descriptors are 1:1 with desktop ones, so a range stays a range; each target is
    // kept on its own for reapplying.
    D3D12_CPU_DESCRIPTOR_HANDLE first;
    if (count && single_range) {
        DescriptorSlot slot;
        if (!console_descriptor_slots(targets[0].ptr, count, &slot))
            return;
        first = slot.cpu;
        for (UINT i = 0; i < count; i++)
            s.render_targets[i].ptr = first.ptr + i * slot.desktop_stride;
    } else {
        for (UINT i = 0; i < count; i++)
            if (!list_cpu_descriptor(targets[i].ptr, 1, &s.render_targets[i]))
                return;
    }
    s.has_depth = depth && list_cpu_descriptor(depth->ptr, 1, &s.depth);
    s.render_target_count = count;
    s.has_targets = true;
    // Output formats for the desktop pipelines draws need.
    PipelineOutputs& o = s.outputs;
    memset(&o, 0, sizeof(o));
    o.render_target_count = (u8)count;
    o.sample_count = 1;
    for (UINT i = 0; i < count; i++) {
        u64 handle = single_range ? targets[0].ptr + i * CONSOLE_DESCRIPTOR_SIZE_RTV : targets[i].ptr;
        u32 format = 0, samples = 1;
        if (view_target_format(handle, &format, &samples)) {
            o.render_target_formats[i] = (u8)format;
            o.sample_count = (u8)samples;
        }
    }
    if (s.has_depth) {
        u32 format = 0, samples = 1;
        if (view_target_format(depth->ptr, &format, &samples)) {
            o.depth_format = (u8)format;
            o.sample_count = (u8)samples;
        }
    }
    self->desktop->OMSetRenderTargets(count, count ? s.render_targets : nullptr, FALSE,
                                      s.has_depth ? &s.depth : nullptr);
}

// --- render passes -----------------------------------------------------------------------------------
//
// A render pass is render targets bound with what happens to their contents at its start and end.
// Recorded as the targets bound and cleared where the pass begins with clears: the same results, as
// keeping contents is always a valid outcome of discarding them. Desktop render passes would bring
// nothing for desktop GPUs, and they'd need the same descriptor translation anyway.

static void WINAPI list_begin_render_pass(ConsoleCommandList* self, UINT count,
                                          const D3D12_RENDER_PASS_RENDER_TARGET_DESC* targets,
                                          const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* depth, UINT flags) {
    (void)flags; // UAV writes are always allowed outside passes; suspending and resuming splits nothing here
    D3D12_CPU_DESCRIPTOR_HANDLE handles[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
    if (count > ARRAY_COUNT(handles))
        count = ARRAY_COUNT(handles);
    for (UINT i = 0; i < count; i++)
        handles[i] = targets[i].cpuDescriptor;
    list_om_set_render_targets(self, count, handles, FALSE, depth ? &depth->cpuDescriptor : nullptr);
    self->state.in_render_pass = true;
    bool resolves = false;
    for (UINT i = 0; i < count; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE d;
        if (targets[i].BeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR &&
            list_cpu_descriptor(targets[i].cpuDescriptor.ptr, 1, &d))
            self->desktop->ClearRenderTargetView(d, targets[i].BeginningAccess.Clear.ClearValue.Color, 0,
                                                 nullptr);
        resolves |= targets[i].EndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
    }
    if (depth) {
        u32 clear = 0;
        if (depth->DepthBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
            clear |= D3D12_CLEAR_FLAG_DEPTH;
        if (depth->StencilBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
            clear |= D3D12_CLEAR_FLAG_STENCIL;
        D3D12_CPU_DESCRIPTOR_HANDLE d;
        if (clear && list_cpu_descriptor(depth->cpuDescriptor.ptr, 1, &d))
            self->desktop->ClearDepthStencilView(
                d, (D3D12_CLEAR_FLAGS)clear, depth->DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth,
                depth->StencilBeginningAccess.Clear.ClearValue.DepthStencil.Stencil, 0, nullptr);
        resolves |= depth->DepthEndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE ||
                    depth->StencilEndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE;
    }
    // A resolve needs its textures in resolve layouts, which the pass would change to and back; the
    // title's barriers keep them as render targets.
    if (resolves)
        LOG_W_ONCE("d3d12", "render passes ending in resolves aren't supported; the targets aren't resolved");
}

static void WINAPI list_end_render_pass(ConsoleCommandList* self) {
    self->state.in_render_pass = false;
}

// --- bundles -----------------------------------------------------------------------------------------

// A console bundle recorded onto a desktop bundle (one part: bundles neither signal nor wait). The
// desktop list leaves the bundle with the bundle's pipeline, root arguments and input state; the
// console list's own state goes on again so later commands see what the title set.
static void WINAPI list_execute_bundle(ConsoleCommandList* self, ConsoleCommandList* bundle) {
    if (!bundle || bundle->type != D3D12_COMMAND_LIST_TYPE_BUNDLE || bundle->segment_count != 1 ||
        !bundle->segments[0].closed || self->type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        LOG_W_ONCE("d3d12", "ExecuteBundle with an unclosed or unsplittable bundle skipped");
        return;
    }
    self->desktop->ExecuteBundle(bundle->segments[0].list);
    // Its descriptor tables are mirrored again when this list is submitted.
    for (u32 t = 0; t < bundle->table_count; t++) {
        const ConsoleCommandList::TableUse& use = bundle->tables[t];
        list_note_table(self, use.heap, use.first, use.count, use.desktop_first);
    }
    apply_state(self);
}

// --- debug markers -----------------------------------------------------------------------------------

// PIX event data passes through as is, so graphics debuggers show the title's own markers.
static void WINAPI list_set_marker(ConsoleCommandList* self, UINT metadata, const void* data, UINT size) {
    self->desktop->SetMarker(metadata, data, size);
}

static void WINAPI list_begin_event(ConsoleCommandList* self, UINT metadata, const void* data, UINT size) {
    self->desktop->BeginEvent(metadata, data, size);
}

static void WINAPI list_end_event(ConsoleCommandList* self) {
    self->desktop->EndEvent();
}

// --- memory from the allocator -------------------------------------------------------------------------

// GPU-visible memory living as long as the list's allocator recording (command_allocator.cpp).
static void* WINAPI list_allocate_from_command_allocator_x(ConsoleCommandList* self, UINT size,
                                                           UINT alignment) {
    return self->allocator ? console_allocator_allocate(self->allocator, size, alignment) : nullptr;
}

static const ConsoleEntry k_list_impl[] = {
    CONSOLE_ENTRY(8, list_get_type),
    CONSOLE_ENTRY(9, list_close),
    CONSOLE_ENTRY(10, list_reset),
    CONSOLE_ENTRY(11, list_clear_state),
    CONSOLE_ENTRY(20, list_ia_set_primitive_topology),
    CONSOLE_ENTRY(21, list_rs_set_viewports),
    CONSOLE_ENTRY(22, list_rs_set_scissor_rects),
    CONSOLE_ENTRY(23, list_om_set_blend_factor),
    CONSOLE_ENTRY(24, list_om_set_stencil_ref),
    CONSOLE_ENTRY(25, list_set_pipeline_state),
    CONSOLE_ENTRY(26, list_resource_barrier),
    CONSOLE_ENTRY(27, list_execute_bundle),
    CONSOLE_ENTRY(32, list_set_descriptor_heaps),
    CONSOLE_ENTRY(33, list_set_compute_root_signature),
    CONSOLE_ENTRY(34, list_set_graphics_root_signature),
    CONSOLE_ENTRY(35, list_set_compute_root_descriptor_table),
    CONSOLE_ENTRY(36, list_set_graphics_root_descriptor_table),
    CONSOLE_ENTRY(37, list_set_compute_root_32bit_constant),
    CONSOLE_ENTRY(38, list_set_graphics_root_32bit_constant),
    CONSOLE_ENTRY(39, list_set_compute_root_32bit_constants),
    CONSOLE_ENTRY(40, list_set_graphics_root_32bit_constants),
    CONSOLE_ENTRY(41, list_set_compute_root_cbv),
    CONSOLE_ENTRY(42, list_set_graphics_root_cbv),
    CONSOLE_ENTRY(43, list_set_compute_root_srv),
    CONSOLE_ENTRY(44, list_set_graphics_root_srv),
    CONSOLE_ENTRY(45, list_set_compute_root_uav),
    CONSOLE_ENTRY(46, list_set_graphics_root_uav),
    CONSOLE_ENTRY(47, list_ia_set_index_buffer),
    CONSOLE_ENTRY(48, list_ia_set_vertex_buffers),
    CONSOLE_ENTRY(50, list_om_set_render_targets),
    CONSOLE_ENTRY(56, list_set_marker),
    CONSOLE_ENTRY(57, list_begin_event),
    CONSOLE_ENTRY(58, list_end_event),
    CONSOLE_ENTRY(69, list_flush_pipeline_x),
    CONSOLE_ENTRY(70, list_om_set_depth_bounds),
    CONSOLE_ENTRY(73, list_wait_32bit_value_x),
    CONSOLE_ENTRY(74, list_wait_64bit_value_x),
    CONSOLE_ENTRY(109, list_om_set_depth_bounds),
    CONSOLE_ENTRY(115, list_begin_render_pass),
    CONSOLE_ENTRY(116, list_end_render_pass),
    CONSOLE_ENTRY(131, list_sync_x),
    CONSOLE_ENTRY(132, list_sync_x),
    CONSOLE_ENTRY(139, list_reset_x),
    CONSOLE_ENTRY(140, list_resource_barrier_x),
    CONSOLE_ENTRY(146, list_barrier),
    CONSOLE_ENTRY(147, list_om_set_front_and_back_stencil_ref),
    CONSOLE_ENTRY(148, list_allocate_from_command_allocator_x),
};

void console_command_list_init() {
    // All sources' entries in one table.
    static ConsoleEntry s_entries[ARRAY_COUNT(k_list_impl) + 128];
    u32 n = 0;
    for (u32 i = 0; i < ARRAY_COUNT(k_list_impl); i++)
        s_entries[n++] = k_list_impl[i];
    SLUDGE_ASSERT(n + k_list_work_entry_count + k_list_x_entry_count <= ARRAY_COUNT(s_entries));
    for (u32 i = 0; i < k_list_work_entry_count; i++)
        s_entries[n++] = k_list_work_entries[i];
    for (u32 i = 0; i < k_list_x_entry_count; i++)
        s_entries[n++] = k_list_x_entries[i];
    console_command_list_x_init();
    g_list_table = console_build_table("ID3D12GraphicsCommandList", k_command_list_entries,
                                       ARRAY_COUNT(k_command_list_entries), s_entries, n);
    D3D12_FEATURE_DATA_D3D12_OPTIONS15 o15 = {};
    g_triangle_fans =
        SUCCEEDED(gpu_device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS15, &o15, sizeof(o15))) &&
        o15.TriangleFanSupported;
}

HRESULT console_create_closed_command_list(ConsoleDevice* device, D3D12_COMMAND_LIST_TYPE type, REFIID iid,
                                           void** out) {
    if (type > D3D12_COMMAND_LIST_TYPE_COPY) {
        LOG_E("d3d12", "CreateCommandList1: list type %d not supported yet", type);
        return E_NOTIMPL;
    }
    // No segment until the first Reset brings an allocator to record on.
    ConsoleCommandList* list =
        (ConsoleCommandList*)console_alloc(sizeof(ConsoleCommandList), &k_list_class, g_list_table, nullptr);
    list->type = type;
    list->device = device->desktop;
    list->submitted_slot = GPU_NO_QUEUE;
    return console_return(list, iid, out);
}

HRESULT console_create_command_list(ConsoleDevice* device, const ConsoleCommandListDesc* desc,
                                    ConsoleCommandAllocator* allocator, void* initial_state, REFIID iid,
                                    void** out) {
    if (!desc || !allocator)
        return E_INVALIDARG;
    if (desc->type > D3D12_COMMAND_LIST_TYPE_COPY) {
        LOG_E("d3d12", "CreateCommandListX: list type %d not supported yet", desc->type);
        return E_NOTIMPL;
    }
    desktop_pso(initial_state);

    // The segments own the desktop lists, so the object itself has no desktop counterpart.
    ConsoleCommandList* list =
        (ConsoleCommandList*)console_alloc(sizeof(ConsoleCommandList), &k_list_class, g_list_table, nullptr);
    list->type = desc->type;
    list->device = device->desktop;
    list->allocator = allocator;
    list->submitted_slot = GPU_NO_QUEUE;
    HRESULT hr = begin_segment(list, 0);
    LOG_D("d3d12", "CreateCommandListX(type %d, flags 0x%x, barrier batch %u) -> 0x%08lx", desc->type,
          desc->flags, desc->resource_barrier_batch_size, (unsigned long)hr);
    if (FAILED(hr)) {
        console_release(list);
        return hr;
    }
    list->recording = true;
    return console_return(list, iid, out);
}
