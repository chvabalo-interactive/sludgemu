// Console ID3D12Fence / ID3D12Fence1.
//
// Console fences keep their value in GPU-visible memory, whose address titles hand to command lists
// to signal and wait from the GPU. Each fence gets an 8-byte slot in blocks of title memory; the
// slot's address finds the fence again when a command list names it.

#include <stdlib.h>

#include "common/log.h"
#include "gpu/gpu.h"
#include "core/memory.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"
#include "hle/xmem/xmem.h"

#define FENCE_BLOCK_SIZE  MEM_PAGE_SIZE
#define FENCE_BLOCK_SLOTS (FENCE_BLOCK_SIZE / sizeof(u64))
#define FENCE_MAX_BLOCKS  8

struct FenceBlock {
    u64* memory;
    ConsoleFence* owners[FENCE_BLOCK_SLOTS]; // nullptr: free slot
};

static SRWLOCK g_fence_lock = SRWLOCK_INIT;
static FenceBlock* g_blocks[FENCE_MAX_BLOCKS];

static u64 fence_memory_attributes() {
    return xmem_d3d_attributes(XMEM_D3D_OBJECT_RESOURCE, XMEM_TYPE_GRAPHICS_CACHEABLE, FENCE_BLOCK_SIZE,
                               MEM_PAGE_SIZE, false);
}

// A free value slot, claimed for `fence`; nullptr if there's no memory.
static volatile u64* claim_slot(ConsoleFence* fence) {
    volatile u64* slot = nullptr;
    AcquireSRWLockExclusive(&g_fence_lock);
    for (u32 b = 0; b < FENCE_MAX_BLOCKS && !slot; b++) {
        if (!g_blocks[b]) {
            FenceBlock* block = (FenceBlock*)calloc(1, sizeof(FenceBlock));
            u64* memory = block ? (u64*)xmem_alloc(FENCE_BLOCK_SIZE, fence_memory_attributes()) : nullptr;
            if (!memory) {
                free(block);
                break;
            }
            block->memory = memory;
            g_blocks[b] = block;
        }
        FenceBlock* block = g_blocks[b];
        for (u32 i = 0; i < FENCE_BLOCK_SLOTS; i++) {
            if (!block->owners[i]) {
                block->owners[i] = fence;
                slot = &block->memory[i];
                break;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_fence_lock);
    return slot;
}

static void release_slot(volatile u64* slot) {
    AcquireSRWLockExclusive(&g_fence_lock);
    for (u32 b = 0; b < FENCE_MAX_BLOCKS && g_blocks[b]; b++) {
        u64 base = (u64)g_blocks[b]->memory;
        if ((u64)slot >= base && (u64)slot < base + FENCE_BLOCK_SIZE)
            g_blocks[b]->owners[((u64)slot - base) / sizeof(u64)] = nullptr;
    }
    ReleaseSRWLockExclusive(&g_fence_lock);
}

ConsoleFence* console_fence_at(u64 address) {
    ConsoleFence* fence = nullptr;
    AcquireSRWLockShared(&g_fence_lock);
    for (u32 b = 0; b < FENCE_MAX_BLOCKS && g_blocks[b]; b++) {
        u64 base = (u64)g_blocks[b]->memory;
        if (address >= base && address < base + FENCE_BLOCK_SIZE && address % sizeof(u64) == 0) {
            fence = g_blocks[b]->owners[(address - base) / sizeof(u64)];
            break;
        }
    }
    ReleaseSRWLockShared(&g_fence_lock);
    return fence;
}

// --- counters in title memory ------------------------------------------------------------------------
//
// Titles also keep their own 64-bit counters in GPU memory, which command lists write once work is
// done and wait on. Each gets a desktop fence on first use, kept for the process lifetime (titles
// use a fixed set of them).

#define COUNTER_CAPACITY 1024 // power of two

struct CounterFence {
    u64 address; // 0: free
    ID3D12Fence* fence;
    u64 waited;   // the highest value queued GPU work waits for
    bool trapped; // CPU stores to the counter are trapped (see console_counter_wait)
};

static CounterFence g_counters[COUNTER_CAPACITY];
static u32 g_counter_count = 0;

static u32 counter_hash(u64 address) {
    return (u32)((address >> 3) * 0x9E3779B97F4A7C15ull >> 54) & (COUNTER_CAPACITY - 1);
}

// Caller holds g_fence_lock (shared or exclusive).
static CounterFence* find_counter(u64 address) {
    for (u32 i = counter_hash(address), n = 0; n < COUNTER_CAPACITY;
         i = (i + 1) & (COUNTER_CAPACITY - 1), n++) {
        if (g_counters[i].address == address || g_counters[i].address == 0)
            return &g_counters[i];
    }
    return nullptr;
}

ID3D12Fence* console_sync_fence(u64 address) {
    if (ConsoleFence* fence = console_fence_at(address))
        return fence->desktop;
    if (!address || address % sizeof(u64))
        return nullptr;
    AcquireSRWLockShared(&g_fence_lock);
    CounterFence* c = find_counter(address);
    ID3D12Fence* fence = c && c->address == address ? c->fence : nullptr;
    ReleaseSRWLockShared(&g_fence_lock);
    if (fence)
        return fence;

    // The counter's current value starts the fence; GPU-visible memory is mapped and readable. CPU
    // stores to it signal the fence while GPU work waits for it (console_counter_wait).
    u64 desktop;
    if (!console_translate_coherent(address, sizeof(u64), &desktop))
        return nullptr;
    AcquireSRWLockExclusive(&g_fence_lock);
    c = find_counter(address);
    if (c && c->address == address) {
        fence = c->fence;
    } else if (c && g_counter_count < COUNTER_CAPACITY / 2) {
        if (SUCCEEDED(gpu_device()->CreateFence(*(volatile u64*)address, D3D12_FENCE_FLAG_NONE,
                                                IID_PPV_ARGS(&fence)))) {
            c->address = address;
            c->fence = fence;
            c->waited = 0;
            c->trapped = false;
            g_counter_count++;
        }
    } else {
        LOG_W_ONCE("d3d12", "more than %u GPU counters; waits on 0x%llx are skipped", COUNTER_CAPACITY / 2,
                   address);
    }
    ReleaseSRWLockExclusive(&g_fence_lock);
    return fence;
}

// --- object ------------------------------------------------------------------------------------------

static const GUID* const k_fence_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                           &__uuidof(ID3D12Pageable), &__uuidof(ID3D12Fence),
                                           &__uuidof(ID3D12Fence1)};

static void fence_destroy(void* object);

static const ConsoleClass k_fence_class = {"ID3D12Fence", k_fence_iids, ARRAY_COUNT(k_fence_iids),
                                           fence_destroy};

static void* const* g_fence_table = nullptr;

static void fence_destroy(void* object) {
    ConsoleFence* fence = (ConsoleFence*)object;
    if (fence->value)
        release_slot(fence->value);
}

static UINT64 WINAPI fence_get_completed_value(ConsoleFence* self) {
    return self->desktop->GetCompletedValue();
}

static HRESULT WINAPI fence_set_event_on_completion(ConsoleFence* self, UINT64 value, HANDLE event) {
    return self->desktop->SetEventOnCompletion(value, event);
}

static HRESULT WINAPI fence_signal(ConsoleFence* self, UINT64 value) {
    *self->value = value;
    gpu_page_tables_release(); // GPU work waiting for the value reads what the CPU wrote before
    return self->desktop->Signal(value);
}

HRESULT console_fence_signal_cpu(ConsoleFence* fence, u64 value) {
    return fence_signal(fence, value);
}

static D3D12_FENCE_FLAGS WINAPI fence_get_creation_flags(ConsoleFence* self) {
    return self->desktop->GetCreationFlags();
}

static UINT64 WINAPI fence_get_gpu_address_x(ConsoleFence* self) {
    return (u64)self->value;
}

static const ConsoleEntry k_fence_impl[] = {
    CONSOLE_ENTRY(8, fence_get_completed_value),
    CONSOLE_ENTRY(9, fence_set_event_on_completion),
    CONSOLE_ENTRY(10, fence_signal),
    CONSOLE_ENTRY(11, fence_get_creation_flags),
    CONSOLE_ENTRY(12, fence_get_gpu_address_x),
};

// The counter reached `value`: the trap goes once nothing waits for more. Every store to a trapped
// page costs two exceptions, and the title's memory around a counter may be anything later, so traps
// last only while GPU work waits. g_fence_lock held exclusively.
static void release_trap_locked(CounterFence* c, u64 value) {
    if (c->trapped && value >= c->waited) {
        mem_untrap_stores((const void*)c->address, sizeof(u64));
        c->trapped = false;
    }
}

// GPU work on a queue waits for the counter at `address` to reach `value`. Titles release such work
// by storing the value from the CPU, which the trap reports (on_counter_store); trapped before the
// counter is read, so no store is missed.
void console_counter_wait(u64 address, u64 value) {
    AcquireSRWLockExclusive(&g_fence_lock);
    CounterFence* c = find_counter(address);
    ID3D12Fence* fence = c && c->address == address ? c->fence : nullptr;
    if (fence) {
        c->waited = value > c->waited ? value : c->waited;
        if (!c->trapped) {
            mem_trap_stores((const void*)address, sizeof(u64));
            c->trapped = true;
        }
        u64 now = *(volatile u64*)address;
        if (now > fence->GetCompletedValue()) {
            gpu_page_tables_release();
            fence->Signal(now);
        }
        release_trap_locked(c, now);
    }
    ReleaseSRWLockExclusive(&g_fence_lock);
}

// A CPU store into a counter GPU work waits on: the counter's fence follows it.
static void on_counter_store(u64 base, u64) {
    AcquireSRWLockExclusive(&g_fence_lock);
    CounterFence* c = find_counter(base);
    if (c && c->address == base) {
        u64 value = *(volatile u64*)base;
        if (value > c->fence->GetCompletedValue()) {
            gpu_page_tables_release(); // the GPU work it releases reads what the CPU wrote before
            c->fence->Signal(value);
        }
        release_trap_locked(c, value);
    }
    ReleaseSRWLockExclusive(&g_fence_lock);
}

void console_fence_init() {
    mem_set_store_listener(on_counter_store);
    g_fence_table = console_build_table("ID3D12Fence", k_fence_entries, ARRAY_COUNT(k_fence_entries),
                                        k_fence_impl, ARRAY_COUNT(k_fence_impl));
}

HRESULT console_create_fence(ConsoleDevice* device, UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID iid,
                             void** out) {
    const D3D12_FENCE_FLAGS desktop_flags =
        D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER | D3D12_FENCE_FLAG_NON_MONITORED;
    if (flags & ~desktop_flags)
        LOG_W("d3d12", "CreateFence: ignoring console fence flags 0x%x", flags & ~desktop_flags);

    ID3D12Fence1* desktop = nullptr;
    HRESULT hr = device->desktop->CreateFence(initial_value, flags & desktop_flags, IID_PPV_ARGS(&desktop));
    if (FAILED(hr)) {
        LOG_E("d3d12", "CreateFence(%llu, 0x%x) failed (0x%08lx)", initial_value, flags, (unsigned long)hr);
        return hr;
    }
    ConsoleFence* fence =
        (ConsoleFence*)console_alloc(sizeof(ConsoleFence), &k_fence_class, g_fence_table, desktop);
    fence->desktop = desktop;
    fence->value = claim_slot(fence);
    if (!fence->value) {
        LOG_E("d3d12", "CreateFence: no memory for the fence value");
        console_release(fence);
        return E_OUTOFMEMORY;
    }
    *fence->value = initial_value;
    return console_return(fence, iid, out);
}
