// Console ID3D12CommandAllocator.

#include <stdlib.h>

#include "common/log.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"
#include "hle/xmem/xmem.h"

static const GUID* const k_allocator_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                               &__uuidof(ID3D12Pageable), &__uuidof(ID3D12CommandAllocator)};

static void allocator_destroy(void* object);

static const ConsoleClass k_allocator_class = {"ID3D12CommandAllocator", k_allocator_iids,
                                               ARRAY_COUNT(k_allocator_iids), allocator_destroy};

static void* const* g_allocator_table = nullptr;

// --- memory for AllocateFromCommandAllocatorX ------------------------------------------------------
//
// Titles write constants and small buffers there with the CPU for the GPU to read, as into command
// memory: write-combined GPU memory, chunks of CHUNK_BYTES (or one allocation's size, if larger).

#define CHUNK_BYTES     MiB(1)
#define CHUNK_ALIGNMENT KiB(64)

static u64 chunk_attributes(u32 size) {
    return xmem_d3d_attributes(XMEM_D3D_OBJECT_RESOURCE, XMEM_TYPE_GRAPHICS_WRITECOMBINE, size,
                               CHUNK_ALIGNMENT, false);
}

static void allocator_destroy(void* object) {
    ConsoleCommandAllocator* self = (ConsoleCommandAllocator*)object;
    // Titles release allocators once the GPU is done with their recordings, as with the command memory.
    for (u32 i = 0; i < self->chunk_count; i++)
        xmem_free(self->chunks[i].memory, chunk_attributes(self->chunks[i].size));
    free(self->chunks);
}

void* console_allocator_allocate(ConsoleCommandAllocator* self, u32 size, u32 alignment) {
    if (!alignment)
        alignment = 1;
    if ((alignment & (alignment - 1)) || alignment > CHUNK_ALIGNMENT || size > UINT32_MAX - CHUNK_ALIGNMENT) {
        LOG_W("d3d12", "AllocateFromCommandAllocatorX(%u bytes, alignment %u): invalid request", size,
              alignment);
        return nullptr;
    }
    // The current chunk, then the next ones (kept from earlier recordings), then a new one.
    for (;;) {
        if (self->current_chunk < self->chunk_count) {
            AllocatorChunk& c = self->chunks[self->current_chunk];
            u32 at = (u32)ALIGN_UP((u64)self->chunk_used, alignment);
            if ((u64)at + size <= c.size) {
                self->chunk_used = at + size;
                return c.memory + at;
            }
            self->current_chunk++;
            self->chunk_used = 0;
            continue;
        }
        u32 chunk_size = size > CHUNK_BYTES ? (u32)ALIGN_UP((u64)size, CHUNK_ALIGNMENT) : CHUNK_BYTES;
        AllocatorChunk* grown =
            (AllocatorChunk*)realloc(self->chunks, (self->chunk_count + 1) * sizeof(AllocatorChunk));
        if (!grown)
            return nullptr;
        self->chunks = grown;
        u8* memory = (u8*)xmem_alloc(chunk_size, chunk_attributes(chunk_size));
        if (!memory) {
            LOG_E("d3d12", "AllocateFromCommandAllocatorX: no memory for a chunk of %u KiB",
                  chunk_size >> 10);
            return nullptr;
        }
        self->chunks[self->chunk_count++] = {memory, chunk_size};
        // current_chunk == the new chunk's index: the loop allocates from it
    }
}

// --- entries -----------------------------------------------------------------------------------------

static HRESULT WINAPI allocator_reset(ConsoleCommandAllocator* self) {
    self->current_chunk = 0;
    self->chunk_used = 0;
    return self->desktop->Reset();
}

static void WINAPI allocator_set_callback_x(ConsoleCommandAllocator* self, ConsoleAllocatorCallback callback,
                                            UINT64 context) {
    self->callback = callback;
    self->callback_context = context;
}

static const ConsoleEntry k_allocator_impl[] = {
    CONSOLE_ENTRY(8, allocator_reset),
    CONSOLE_ENTRY(9, allocator_set_callback_x),
};

void console_command_allocator_init() {
    g_allocator_table = console_build_table("ID3D12CommandAllocator", k_command_allocator_entries,
                                            ARRAY_COUNT(k_command_allocator_entries), k_allocator_impl,
                                            ARRAY_COUNT(k_allocator_impl));
}

HRESULT console_create_command_allocator(ConsoleDevice* device, D3D12_COMMAND_LIST_TYPE type, REFIID iid,
                                         void** out) {
    ID3D12CommandAllocator* desktop = nullptr;
    HRESULT hr = device->desktop->CreateCommandAllocator(type, IID_PPV_ARGS(&desktop));
    if (FAILED(hr)) {
        LOG_E("d3d12", "CreateCommandAllocator(type %d) failed (0x%08lx)", type, (unsigned long)hr);
        return hr;
    }
    ConsoleCommandAllocator* allocator = (ConsoleCommandAllocator*)console_alloc(
        sizeof(ConsoleCommandAllocator), &k_allocator_class, g_allocator_table, desktop);
    allocator->type = type;
    allocator->desktop = desktop;
    return console_return(allocator, iid, out);
}
