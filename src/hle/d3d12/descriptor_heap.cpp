// Console ID3D12DescriptorHeap.

#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "core/memory.h"
#include "gpu/gpu.h"
#include "gpu/gpu_window.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"
#include "hle/d3d12/views.h"
#include "hle/xmem/xmem.h"

static const GUID* const k_descriptor_heap_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                                     &__uuidof(ID3D12Pageable),
                                                     &__uuidof(ID3D12DescriptorHeap)};

static void descriptor_heap_destroy(void* object);

static const ConsoleClass k_descriptor_heap_class = {"ID3D12DescriptorHeap", k_descriptor_heap_iids,
                                                     ARRAY_COUNT(k_descriptor_heap_iids),
                                                     descriptor_heap_destroy};

static void* const* g_descriptor_heap_table = nullptr;

// Desktop limits for shader-visible heaps: what the device reports, at least the tier 3 minimum.
// Heaps over the limit give 1/RELOCATION_SHARE of the desktop heap to relocated tables.
#define RELOCATION_SHARE 8
static u32 g_max_visible_resources = 1000000;
static u32 g_max_visible_samplers = 2048;

static u32 g_desktop_stride[D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES];

static u64 descriptor_memory_attributes(u64 size) {
    return xmem_d3d_attributes(XMEM_D3D_OBJECT_RESOURCE, XMEM_TYPE_GRAPHICS_CACHEABLE, size, MEM_PAGE_SIZE,
                               true);
}

// --- heap registry: console handle -> heap -----------------------------------------------------------

struct HeapEntry {
    u64 base;
    u64 end;
    ConsoleDescriptorHeap* heap;
};

static struct {
    HeapEntry* entries; // sorted by base, non-overlapping
    u32 count;
    u32 capacity;
    SRWLOCK lock;
} g_heaps = {nullptr, 0, 0, SRWLOCK_INIT};

static u32 heaps_at_or_below(u64 address) {
    u32 lo = 0, hi = g_heaps.count;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_heaps.entries[mid].base <= address)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void register_heap(ConsoleDescriptorHeap* heap) {
    AcquireSRWLockExclusive(&g_heaps.lock);
    if (g_heaps.count == g_heaps.capacity) {
        u32 capacity = g_heaps.capacity ? g_heaps.capacity * 2 : 64;
        HeapEntry* grown = (HeapEntry*)realloc(g_heaps.entries, capacity * sizeof(HeapEntry));
        if (!grown)
            FATAL("out of memory tracking descriptor heaps");
        g_heaps.entries = grown;
        g_heaps.capacity = capacity;
    }
    u64 base = (u64)heap->memory;
    u32 at = heaps_at_or_below(base);
    memmove(&g_heaps.entries[at + 1], &g_heaps.entries[at], (g_heaps.count - at) * sizeof(HeapEntry));
    g_heaps.entries[at] = {base, base + (u64)heap->desc.NumDescriptors * heap->stride, heap};
    g_heaps.count++;
    ReleaseSRWLockExclusive(&g_heaps.lock);
}

static void unregister_heap(ConsoleDescriptorHeap* heap) {
    AcquireSRWLockExclusive(&g_heaps.lock);
    u32 at = heaps_at_or_below((u64)heap->memory);
    if (at > 0 && g_heaps.entries[at - 1].heap == heap) {
        memmove(&g_heaps.entries[at - 1], &g_heaps.entries[at], (g_heaps.count - at) * sizeof(HeapEntry));
        g_heaps.count--;
    }
    ReleaseSRWLockExclusive(&g_heaps.lock);
}

bool console_descriptor_slots(u64 handle, u32 count, DescriptorSlot* out) {
    bool ok = false;
    AcquireSRWLockShared(&g_heaps.lock);
    u32 at = heaps_at_or_below(handle);
    if (at > 0 && handle < g_heaps.entries[at - 1].end) {
        ConsoleDescriptorHeap* heap = g_heaps.entries[at - 1].heap;
        u64 offset = handle - (u64)heap->memory;
        u64 index = offset / heap->stride;
        if (offset % heap->stride == 0 && index + count <= heap->desc.NumDescriptors) {
            u32 stride = g_desktop_stride[heap->desktop_type];
            u64 desktop_offset = index * stride;
            // Past a capped heap's same-index slots there is no shader-visible copy to keep: tables
            // over those slots are mirrored where they are relocated, when used.
            bool visible = heap->visible && index + count <= heap->identity_count;
            out->type = heap->desktop_type;
            out->desktop_stride = stride;
            out->cpu.ptr = heap->cpu_start + desktop_offset;
            out->visible_cpu.ptr = visible ? heap->visible_cpu_start + desktop_offset : 0;
            out->visible_gpu.ptr = visible ? heap->visible_gpu_start + desktop_offset : 0;
            out->console = heap->memory + offset;
            out->heap = heap;
            out->index = (u32)index;
            ok = true;
        }
    }
    ReleaseSRWLockShared(&g_heaps.lock);
    if (!ok)
        LOG_E("d3d12", "descriptor handle 0x%llx (+%u) is not inside a live descriptor heap", handle, count);
    return ok;
}

static void set_keeper(ConsoleDescriptorHeap* heap, u32 index, ConsoleResource* resource) {
    if (!heap->keepers) {
        if (!resource)
            return;
        ConsoleResource* volatile* keepers =
            (ConsoleResource* volatile*)calloc(heap->desc.NumDescriptors, sizeof(ConsoleResource*));
        if (!keepers)
            FATAL("out of memory tracking descriptors");
        if (InterlockedCompareExchangePointer((void* volatile*)&heap->keepers, (void*)keepers, nullptr))
            free((void*)keepers); // another thread was first
    }
    if (resource && !console_try_add_ref(resource))
        resource = nullptr; // being destroyed: no other slot keeps it any more
    ConsoleResource* old =
        (ConsoleResource*)InterlockedExchangePointer((void* volatile*)&heap->keepers[index], resource);
    if (old)
        console_release(old);
}

void console_descriptor_keep(const DescriptorSlot& slot, ConsoleResource* resource) {
    set_keeper(slot.heap, slot.index, resource);
}

void console_descriptor_copy_keepers(const DescriptorSlot& dest, const DescriptorSlot& src, u32 count) {
    if (!src.heap->keepers)
        return; // nothing kept there; the destination's keepers went when it was written
    for (u32 i = 0; i < count; i++)
        set_keeper(dest.heap, dest.index + i, src.heap->keepers[src.index + i]);
}

// --- descriptor shadow ---------------------------------------------------------------------------------

// The shadow of a resource heap with `visible_count` shader-visible slots, mapped for good (upload heap
// memory is write-combined: written, never read back). Created zeroed: no view in any slot.
static ID3D12Resource* create_shadow(u32 visible_count, u8** mapped) {
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC1 desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = (u64)visible_count * SHADER_SHADOW_RECORD_BYTES;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* shadow = nullptr;
    HRESULT hr = gpu_device()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0,
                                                        nullptr, IID_PPV_ARGS(&shadow));
    if (SUCCEEDED(hr)) {
        hr = shadow->Map(0, nullptr, (void**)mapped);
        if (FAILED(hr)) {
            shadow->Release();
            shadow = nullptr;
        }
    }
    if (FAILED(hr)) {
        LOG_E("d3d12", "cannot create the %llu KiB descriptor shadow of a heap of %u slots (0x%08lx)",
              desc.Width >> 10, visible_count, (unsigned long)hr);
        return nullptr;
    }
    gpu_set_name(shadow, "descriptor shadow (%u slots)", visible_count);
    return shadow;
}

// Writes the record of shader-visible slot `desktop_index` (with the descriptor copied there).
static void write_shadow(ConsoleDescriptorHeap* heap, u32 desktop_index, const u8* console) {
    if (!heap->shadow_cpu)
        return;
    u32 record[SHADER_SHADOW_RECORD_BYTES / 4];
    shadow_record(console, record);
    memcpy(heap->shadow_cpu + (u64)desktop_index * SHADER_SHADOW_RECORD_BYTES, record, sizeof(record));
}

void console_descriptor_publish(const DescriptorSlot& slot, u32 count) {
    if (slot.heap->keepers)
        for (u32 i = 0; i < count; i++)
            set_keeper(slot.heap, slot.index + i, nullptr);
    if (!slot.visible_cpu.ptr)
        return;
    gpu_device()->CopyDescriptorsSimple(count, slot.visible_cpu, slot.cpu, slot.type);
    for (u32 i = 0; i < count; i++)
        write_shadow(slot.heap, slot.index + i, slot.console + (u64)i * slot.heap->stride);
    if (slot.heap->native)
        for (u32 i = 0; i < count; i++)
            slot.heap->native[slot.index + i].generation = 0;
}

// Titles build descriptor tables by copying descriptor memory themselves. A slot copied that way
// carries the marker of the descriptor it came from (see views.cpp), naming that descriptor's
// desktop copy, which goes to the slot's shader-visible desktop descriptor. Copied on every call:
// the source may have been rewritten since.
//
// Constant buffer views are written by the title itself (the console runtime inlines their
// creation): a hardware buffer descriptor whose first quadword is the buffer address with a 16-byte
// stride in its top bits, and whose second is the size in 16-byte records with a fixed format word
// on top. Their desktop views are made here, and remade only when the descriptor or the memory
// windows change.

static void mirror_native_constant_buffer(ConsoleDescriptorHeap* heap, u32 desktop_index, const u8* console,
                                          u64 address, u64 records) {
    ConsoleDescriptorHeap::NativeView& seen = heap->native[desktop_index];
    u32 generation = gpu_window_generation();
    if (seen.generation == generation && seen.address == address && seen.records == records)
        return; // the slot and its shadow record hold this view (other writes of the slot reset `seen`)
    D3D12_CPU_DESCRIPTOR_HANDLE dst = {heap->visible_cpu_start + (u64)desktop_index * heap->desktop_stride};
    write_constant_buffer_view(address, records * 16, dst);
    write_shadow(heap, desktop_index, console);
    seen.address = address;
    seen.records = records;
    seen.generation = generation;
}

void console_descriptor_mirror(ConsoleDescriptorHeap* heap, u32 first, u32 count, u32 desktop_first) {
    if (!heap->visible || first >= heap->desc.NumDescriptors || desktop_first >= heap->visible_count)
        return;
    if (count > heap->desc.NumDescriptors - first)
        count = heap->desc.NumDescriptors - first;
    if (count > heap->visible_count - desktop_first)
        count = heap->visible_count - desktop_first;
    bool same_index = first == desktop_first;
    ID3D12Device* device = gpu_device();
    for (u32 k = 0; k < count; k++) {
        u32 i = first + k, d = desktop_first + k;
        const u8* console = heap->memory + (u64)i * heap->stride;
        u64 q0, q1;
        memcpy(&q0, console, 8);
        memcpy(&q1, console + 8, 8);
        if (heap->native && (q0 >> 48) == NATIVE_CBV_STRIDE_BITS && (q1 >> 32) == NATIVE_CBV_FORMAT_BITS) {
            mirror_native_constant_buffer(heap, d, console, q0 & NATIVE_CBV_ADDRESS_MASK, (u32)q1);
            continue;
        }
        u32 magic = (u32)q0, type = (u32)(q0 >> 32) & 0xFF;
        u64 source = q1;
        u64 own = heap->cpu_start + (u64)i * heap->desktop_stride;
        if (magic != DESCRIPTOR_MARKER_MAGIC || type != (u32)heap->desktop_type)
            continue; // not a descriptor of ours
        if (same_index && source == own)
            continue; // written in place, and published then
        D3D12_CPU_DESCRIPTOR_HANDLE dst = {heap->visible_cpu_start + (u64)d * heap->desktop_stride};
        device->CopyDescriptorsSimple(1, dst, {source}, heap->desktop_type);
        write_shadow(heap, d, console);
        if (heap->native)
            heap->native[d].generation = 0;
    }
}

u32 console_descriptor_place(ConsoleDescriptorHeap* heap, u32 first, u32 count) {
    if (!heap->visible || first >= heap->desc.NumDescriptors)
        return ~0u;
    if (count > heap->desc.NumDescriptors - first)
        count = heap->desc.NumDescriptors - first;
    if (first + count <= heap->identity_count)
        return first;
    if (count == 0 || count > heap->relocation_count)
        return ~0u;
    // Ring space is handed out in order and reused when the cursor comes round again: by then the
    // GPU is long done with the tables copied there (the ring holds many frames of them).
    for (;;) {
        u32 end = (u32)InterlockedAdd(&heap->relocation_cursor, (long)count);
        u32 start = end - count;
        if (end <= heap->relocation_count)
            return heap->relocation_first + start;
        // The range would run past the end: it is skipped, and whoever crossed the end rewinds.
        if (start <= heap->relocation_count)
            InterlockedExchange(&heap->relocation_cursor, 0);
    }
}

// --- object ------------------------------------------------------------------------------------------

static void descriptor_heap_destroy(void* object) {
    ConsoleDescriptorHeap* heap = (ConsoleDescriptorHeap*)object;
    unregister_heap(heap);
    gpu_retire(heap->cpu);
    gpu_retire(heap->visible);
    gpu_retire(heap->shadow); // unmapped with its release
    free(heap->native);
    if (heap->keepers) {
        for (u32 i = 0; i < heap->desc.NumDescriptors; i++)
            if (heap->keepers[i])
                console_release(heap->keepers[i]);
        free((void*)heap->keepers);
    }
    if (heap->memory)
        xmem_free(heap->memory, descriptor_memory_attributes((u64)heap->desc.NumDescriptors * heap->stride));
}

static void WINAPI descriptor_heap_get_desc(ConsoleDescriptorHeap* self, D3D12_DESCRIPTOR_HEAP_DESC* out) {
    *out = self->desc;
}

static D3D12_CPU_DESCRIPTOR_HANDLE WINAPI descriptor_heap_get_cpu_start(ConsoleDescriptorHeap* self) {
    return {(SIZE_T)self->memory};
}

static D3D12_GPU_DESCRIPTOR_HANDLE WINAPI descriptor_heap_get_gpu_start(ConsoleDescriptorHeap* self) {
    return {(u64)self->memory};
}

static const ConsoleEntry k_descriptor_heap_impl[] = {
    CONSOLE_ENTRY(8, descriptor_heap_get_desc),
    CONSOLE_ENTRY(9, descriptor_heap_get_cpu_start),
    CONSOLE_ENTRY(10, descriptor_heap_get_gpu_start),
};

void console_descriptor_heap_init() {
    g_descriptor_heap_table = console_build_table(
        "ID3D12DescriptorHeap", k_descriptor_heap_entries, ARRAY_COUNT(k_descriptor_heap_entries),
        k_descriptor_heap_impl, ARRAY_COUNT(k_descriptor_heap_impl));
    ID3D12Device13* device = gpu_device();
    for (u32 t = 0; t < D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES; t++)
        g_desktop_stride[t] = device->GetDescriptorHandleIncrementSize((D3D12_DESCRIPTOR_HEAP_TYPE)t);
    D3D12_FEATURE_DATA_D3D12_OPTIONS19 o19 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS19, &o19, sizeof(o19)))) {
        if (o19.MaxViewDescriptorHeapSize > g_max_visible_resources)
            g_max_visible_resources = o19.MaxViewDescriptorHeapSize;
        if (o19.MaxSamplerDescriptorHeapSize > g_max_visible_samplers)
            g_max_visible_samplers = o19.MaxSamplerDescriptorHeapSize;
    }
    LOG_I("d3d12", "shader-visible descriptor heaps: up to %u views, %u samplers", g_max_visible_resources,
          g_max_visible_samplers);
}

u32 console_descriptor_size(u32 type) {
    switch (type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
        return CONSOLE_DESCRIPTOR_SIZE_RESOURCE;
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
    case CONSOLE_DESCRIPTOR_HEAP_TYPE_SAMPLER_UNBOUNDED:
        return CONSOLE_DESCRIPTOR_SIZE_SAMPLER;
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
        return CONSOLE_DESCRIPTOR_SIZE_RTV;
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
        return CONSOLE_DESCRIPTOR_SIZE_DSV;
    default:
        return 0;
    }
}

HRESULT console_create_descriptor_heap(ConsoleDevice* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                       REFIID iid, void** out) {
    u32 stride = desc ? console_descriptor_size(desc->Type) : 0;
    if (!stride || desc->NumDescriptors == 0)
        return E_INVALIDARG;

    D3D12_DESCRIPTOR_HEAP_DESC d = *desc;
    if (d.Type == CONSOLE_DESCRIPTOR_HEAP_TYPE_SAMPLER_UNBOUNDED)
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    if (d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && d.Type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
        d.Flags &= ~D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    d.NodeMask = 0;
    // A shader-visible heap larger than desktop allows keeps most of its slots at their own index and
    // gives the rest of the desktop heap to the relocation ring.
    u32 visible_count = d.NumDescriptors, identity_count = d.NumDescriptors;
    if (d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
        u32 limit =
            d.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ? g_max_visible_samplers : g_max_visible_resources;
        if (d.NumDescriptors > limit) {
            visible_count = limit;
            identity_count = limit - limit / RELOCATION_SHARE;
            LOG_W("d3d12",
                  "descriptor heap of %u (type %d) exceeds the desktop limit of %u; slots from %u are "
                  "visible to shaders only through descriptor tables",
                  desc->NumDescriptors, desc->Type, limit, identity_count);
        }
    }
    ID3D12DescriptorHeap* visible = nullptr;
    ID3D12DescriptorHeap* cpu = nullptr;
    HRESULT hr = S_OK;
    if (d.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
        D3D12_DESCRIPTOR_HEAP_DESC visible_desc = d;
        visible_desc.NumDescriptors = visible_count;
        hr = device->desktop->CreateDescriptorHeap(&visible_desc, IID_PPV_ARGS(&visible));
    }
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC cpu_desc = d;
        cpu_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = device->desktop->CreateDescriptorHeap(&cpu_desc, IID_PPV_ARGS(&cpu));
    }
    if (FAILED(hr)) {
        LOG_E("d3d12", "CreateDescriptorHeap(type %d, %u, flags 0x%x) failed (0x%08lx)", d.Type,
              d.NumDescriptors, d.Flags, (unsigned long)hr);
        if (visible)
            visible->Release();
        return hr;
    }
    // Translated shaders bind the descriptor shadow of the resource heap they index: without one they
    // would read through an unbound root descriptor.
    ID3D12Resource* shadow = nullptr;
    u8* shadow_cpu = nullptr;
    if (visible && d.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        shadow = create_shadow(visible_count, &shadow_cpu);
        if (!shadow) {
            cpu->Release();
            visible->Release();
            return E_OUTOFMEMORY;
        }
    }

    // Like the console runtime, take the memory from XMemAlloc, where the title's allocation hooks
    // may supply memory set aside for this heap. Handles must fit in 32 bits: console hardware
    // passes descriptor table addresses that way.
    u64 bytes = (u64)desc->NumDescriptors * stride;
    u8* memory = (u8*)xmem_alloc(bytes, descriptor_memory_attributes(bytes));
    if (!memory || (u64)memory + bytes > GiB(4)) {
        LOG_E("d3d12", "no memory below 4 GiB for a %llu KiB descriptor heap (got %p)", bytes >> 10, memory);
        if (memory)
            xmem_free(memory, descriptor_memory_attributes(bytes));
        cpu->Release();
        if (visible)
            visible->Release();
        if (shadow)
            shadow->Release();
        return E_OUTOFMEMORY;
    }

    ConsoleDescriptorHeap* heap = (ConsoleDescriptorHeap*)console_alloc(
        sizeof(ConsoleDescriptorHeap), &k_descriptor_heap_class, g_descriptor_heap_table, nullptr);
    heap->desc = *desc;
    heap->memory = memory;
    heap->stride = stride;
    heap->visible_count = visible ? visible_count : 0;
    heap->identity_count = visible ? identity_count : 0;
    heap->relocation_first = heap->identity_count;
    heap->relocation_count = heap->visible_count - heap->identity_count;
    heap->desktop_type = d.Type;
    heap->desktop_stride = g_desktop_stride[d.Type];
    heap->cpu = cpu;
    heap->visible = visible;
    heap->shadow = shadow;
    heap->shadow_cpu = shadow_cpu;
    heap->shadow_gpu = shadow ? shadow->GetGPUVirtualAddress() : 0;
    heap->cpu_start = cpu->GetCPUDescriptorHandleForHeapStart().ptr;
    if (visible) {
        heap->visible_cpu_start = visible->GetCPUDescriptorHandleForHeapStart().ptr;
        heap->visible_gpu_start = visible->GetGPUDescriptorHandleForHeapStart().ptr;
        if (d.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
            heap->native = (ConsoleDescriptorHeap::NativeView*)calloc(visible_count, sizeof(*heap->native));
            if (!heap->native)
                LOG_E("d3d12", "out of memory for a descriptor heap's constant buffer table; title-written "
                               "constant buffer views will read as zeros");
        }
    }
    register_heap(heap);
    LOG_D("d3d12", "descriptor heap type %d, %u descriptors, flags 0x%x at %p", desc->Type,
          desc->NumDescriptors, desc->Flags, memory);
    return console_return(heap, iid, out);
}
