// Console ID3D12Heap: a block of title memory the runtime allocates, which placed resources take parts
// of. Console resources live at GPU addresses (= their CPU addresses), so a heap is its memory and a
// resource placed in it is a resource at the heap's address plus the offset.

#include "common/log.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"
#include "hle/xmem/xmem.h"

static const GUID* const k_heap_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                          &__uuidof(ID3D12Pageable), &__uuidof(ID3D12Heap),
                                          &__uuidof(ID3D12Heap1)};

static void heap_destroy(void* object);

static const ConsoleClass k_heap_class = {"ID3D12Heap", k_heap_iids, ARRAY_COUNT(k_heap_iids), heap_destroy};

static void* const* g_heap_table = nullptr;

// Resources placed in a heap don't keep it alive (as on desktop): the title releases the heap after them.
static void heap_destroy(void* object) {
    ConsoleHeap* self = (ConsoleHeap*)object;
    xmem_free(self->memory, self->attributes);
}

// The console entry takes the description's address after the object (its inline GetDesc returns the
// struct itself), unlike desktop's struct return.
static void WINAPI heap_get_desc(ConsoleHeap* self, D3D12_HEAP_DESC* desc) {
    if (desc)
        *desc = self->desc;
}

// Console titles have no protected content.
static HRESULT WINAPI heap_get_protected_resource_session(ConsoleHeap*, REFIID, void** out) {
    if (out)
        *out = nullptr;
    return DXGI_ERROR_NOT_FOUND;
}

static D3D12_GPU_VIRTUAL_ADDRESS WINAPI heap_get_gpu_address_x(ConsoleHeap* self) {
    return (u64)self->memory;
}

static const ConsoleEntry k_heap_impl[] = {
    CONSOLE_ENTRY(8, heap_get_desc),
    CONSOLE_ENTRY(9, heap_get_protected_resource_session),
    CONSOLE_ENTRY(10, heap_get_gpu_address_x),
};

void console_heap_init() {
    g_heap_table = console_build_table("ID3D12Heap", k_heap_entries, ARRAY_COUNT(k_heap_entries), k_heap_impl,
                                       ARRAY_COUNT(k_heap_impl));
}

HRESULT console_create_heap(ConsoleDevice*, const D3D12_HEAP_DESC* desc, REFIID iid, void** out) {
    if (!desc || !desc->SizeInBytes)
        return E_INVALIDARG;
    const D3D12_HEAP_PROPERTIES& p = desc->Properties;
    // CPU caching of the memory follows what the heap is for, as for committed resources.
    bool write_combined =
        p.Type == D3D12_HEAP_TYPE_UPLOAD ||
        (p.Type == D3D12_HEAP_TYPE_CUSTOM && p.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE);
    u64 alignment = desc->Alignment ? desc->Alignment : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    u64 size = ALIGN_UP(desc->SizeInBytes, alignment);
    u32 object = (desc->Flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY) ? XMEM_D3D_OBJECT_PRESENTABLE
                                                               : XMEM_D3D_OBJECT_RESOURCE;
    u64 attributes = xmem_d3d_attributes(
        object, write_combined ? XMEM_TYPE_GRAPHICS_WRITECOMBINE : XMEM_TYPE_GRAPHICS_CACHEABLE, size,
        alignment, false);
    void* memory = xmem_alloc(size, attributes);
    if (!memory) {
        LOG_E("d3d12", "CreateHeap: no memory for a heap of %llu KiB", size >> 10);
        return E_OUTOFMEMORY;
    }
    LOG_D("d3d12", "heap at %p: %llu KiB, type %d, flags 0x%x", memory, size >> 10, p.Type, desc->Flags);
    ConsoleHeap* heap =
        (ConsoleHeap*)console_alloc(sizeof(ConsoleHeap), &k_heap_class, g_heap_table, nullptr);
    heap->desc = *desc;
    heap->desc.SizeInBytes = size;
    heap->desc.Alignment = alignment;
    heap->memory = memory;
    heap->attributes = attributes;
    return console_return(heap, iid, out);
}

HRESULT console_create_heap_resource(ConsoleDevice* device, ConsoleHeap* heap, u64 offset,
                                     const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout,
                                     const D3D12_CLEAR_VALUE* clear, REFIID iid, void** out) {
    if (!heap || !desc)
        return E_INVALIDARG;
    D3D12_RESOURCE_ALLOCATION_INFO info = console_resource_allocation_info(*desc);
    if (info.SizeInBytes == UINT64_MAX || offset > heap->desc.SizeInBytes ||
        info.SizeInBytes > heap->desc.SizeInBytes - offset || (info.Alignment && offset % info.Alignment)) {
        LOG_W("d3d12", "CreatePlacedResource: %llu bytes at offset %llu don't fit heap of %llu bytes",
              info.SizeInBytes, offset, heap->desc.SizeInBytes);
        return E_INVALIDARG;
    }
    return console_create_placed_resource(device, (u64)heap->memory + offset, desc, layout, clear, iid, out);
}
