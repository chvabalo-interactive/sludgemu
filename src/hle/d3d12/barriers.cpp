// Console barriers on desktop enhanced barriers.

#include "hle/d3d12/barriers.h"
#include "hle/d3d12/texture_sync.h"

#include "common/log.h"

// Console resource states that desktop D3D12 doesn't have.
#define CONSOLE_STATE_PRESENT       0x00100000u // desktop: COMMON
#define CONSOLE_STATE_PRESERVE_MASK 0x7e000000u // keep console compression metadata as is

// Barriers recorded per desktop Barrier call; longer lists are split.
#define BARRIER_BATCH 64

// Accesses that apply to textures and to buffers.
static const u32 k_texture_access =
    D3D12_BARRIER_ACCESS_RENDER_TARGET | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS |
    D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ |
    D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_COPY_DEST | D3D12_BARRIER_ACCESS_COPY_SOURCE |
    D3D12_BARRIER_ACCESS_RESOLVE_DEST | D3D12_BARRIER_ACCESS_RESOLVE_SOURCE |
    D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE;
static const u32 k_buffer_access = D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER |
                                   D3D12_BARRIER_ACCESS_INDEX_BUFFER | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS |
                                   D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_STREAM_OUTPUT |
                                   D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT | D3D12_BARRIER_ACCESS_COPY_DEST |
                                   D3D12_BARRIER_ACCESS_COPY_SOURCE |
                                   D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ |
                                   D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;

// The desktop part of a console state; the console's present state is desktop's common state.
static u32 desktop_state(u32 state) {
    state &= ~CONSOLE_STATE_PRESERVE_MASK;
    if (state & CONSOLE_STATE_PRESENT)
        return D3D12_RESOURCE_STATE_COMMON;
    return state;
}

#define CONSOLE_LAYOUT_PRESENT                                            31
#define CONSOLE_LAYOUT_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE 32

D3D12_BARRIER_LAYOUT console_desktop_layout(u32 layout) {
    if (layout == CONSOLE_LAYOUT_PRESENT)
        return D3D12_BARRIER_LAYOUT_PRESENT;
    if (layout == CONSOLE_LAYOUT_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE)
        return D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE;
    return (D3D12_BARRIER_LAYOUT)layout;
}

D3D12_BARRIER_LAYOUT console_texture_layout(const ConsoleResource* r, D3D12_BARRIER_LAYOUT layout) {
    if (texture_sync_tracks(r->desc))
        return D3D12_BARRIER_LAYOUT_COMMON; // contents come from title memory (texture_sync.h)
    if (!console_texture_loses_uav(r->desc))
        return layout;
    switch (layout) {
    case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
        return D3D12_BARRIER_LAYOUT_COMMON;
    case D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS:
        return D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON;
    case D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS:
        return D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COMMON;
    default:
        return layout;
    }
}

// The accesses a texture's desktop counterpart can have.
static D3D12_BARRIER_ACCESS texture_access(const ConsoleResource* r, u32 access) {
    if (console_texture_loses_uav(r->desc))
        access &= ~(u32)D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    return (D3D12_BARRIER_ACCESS)access;
}

D3D12_BARRIER_LAYOUT console_layout_from_state(u32 state) {
    const u32 shader_read =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    state = desktop_state(state);
    if (state == D3D12_RESOURCE_STATE_COMMON)
        return D3D12_BARRIER_LAYOUT_COMMON;
    // Writable states stand alone.
    if (state & D3D12_RESOURCE_STATE_RENDER_TARGET)
        return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    if (state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
    if (state & D3D12_RESOURCE_STATE_DEPTH_WRITE)
        return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
    if (state & D3D12_RESOURCE_STATE_COPY_DEST)
        return D3D12_BARRIER_LAYOUT_COPY_DEST;
    if (state & D3D12_RESOURCE_STATE_RESOLVE_DEST)
        return D3D12_BARRIER_LAYOUT_RESOLVE_DEST;
    // Read states can combine.
    if (state & D3D12_RESOURCE_STATE_DEPTH_READ)
        return D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ;
    if (state == D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE)
        return D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE;
    if ((state & ~shader_read) == 0)
        return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    if (state == D3D12_RESOURCE_STATE_COPY_SOURCE)
        return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
    if (state == D3D12_RESOURCE_STATE_RESOLVE_SOURCE)
        return D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE;
    return D3D12_BARRIER_LAYOUT_GENERIC_READ;
}

// Every access a state allows, before narrowing to what a resource type can do.
static u32 access_from_state(u32 state) {
    static const struct {
        u32 state;
        u32 access;
    } k_map[] = {
        {D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
         D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER},
        {D3D12_RESOURCE_STATE_INDEX_BUFFER, D3D12_BARRIER_ACCESS_INDEX_BUFFER},
        {D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET},
        {D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS},
        {D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE},
        {D3D12_RESOURCE_STATE_DEPTH_READ, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ},
        {D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_BARRIER_ACCESS_SHADER_RESOURCE},
        {D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_BARRIER_ACCESS_SHADER_RESOURCE},
        {D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_BARRIER_ACCESS_STREAM_OUTPUT},
        {D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT},
        {D3D12_RESOURCE_STATE_COPY_DEST, D3D12_BARRIER_ACCESS_COPY_DEST},
        {D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_BARRIER_ACCESS_COPY_SOURCE},
        {D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_BARRIER_ACCESS_RESOLVE_DEST},
        {D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_BARRIER_ACCESS_RESOLVE_SOURCE},
        {D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
         D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ |
             D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE},
        {D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE, D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE},
    };
    u32 access = 0;
    for (u32 i = 0; i < ARRAY_COUNT(k_map); i++)
        if (state & k_map[i].state)
            access |= k_map[i].access;
    return access;
}

// Common state (and anything with no access this resource type has) allows every compatible access.
static D3D12_BARRIER_ACCESS narrowed_access(u32 state, u32 allowed) {
    u32 access = access_from_state(desktop_state(state)) & allowed;
    return access ? (D3D12_BARRIER_ACCESS)access : D3D12_BARRIER_ACCESS_COMMON;
}

// --- legacy barriers ---------------------------------------------------------------------------------

struct BarrierBatch {
    D3D12_BUFFER_BARRIER buffers[BARRIER_BATCH];
    D3D12_TEXTURE_BARRIER textures[BARRIER_BATCH];
    u32 buffer_count;
    u32 texture_count;
    bool global; // a UAV or aliasing barrier: wait for and flush everything
};

static void flush(ID3D12GraphicsCommandList7* list, BarrierBatch* b) {
    D3D12_GLOBAL_BARRIER global = {D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_ALL,
                                   D3D12_BARRIER_ACCESS_COMMON, D3D12_BARRIER_ACCESS_COMMON};
    D3D12_BARRIER_GROUP groups[3];
    u32 n = 0;
    if (b->buffer_count) {
        groups[n].Type = D3D12_BARRIER_TYPE_BUFFER;
        groups[n].NumBarriers = b->buffer_count;
        groups[n++].pBufferBarriers = b->buffers;
    }
    if (b->texture_count) {
        groups[n].Type = D3D12_BARRIER_TYPE_TEXTURE;
        groups[n].NumBarriers = b->texture_count;
        groups[n++].pTextureBarriers = b->textures;
    }
    if (b->global) {
        groups[n].Type = D3D12_BARRIER_TYPE_GLOBAL;
        groups[n].NumBarriers = 1;
        groups[n++].pGlobalBarriers = &global;
    }
    if (n)
        list->Barrier(n, groups);
    b->buffer_count = 0;
    b->texture_count = 0;
    b->global = false;
}

static void add_transition(ID3D12GraphicsCommandList7* list, BarrierBatch* b,
                           const D3D12_RESOURCE_TRANSITION_BARRIER& t) {
    ConsoleResource* r = (ConsoleResource*)t.pResource;
    u64 offset;
    ID3D12Resource* desktop = r ? console_resource_desktop(r, &offset, GPU_READ) : nullptr;
    if (!desktop) {
        LOG_W_ONCE("d3d12", "ResourceBarrier: transition of a resource without desktop backing skipped");
        return;
    }
    if (r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        if (b->buffer_count == BARRIER_BATCH)
            flush(list, b);
        D3D12_BUFFER_BARRIER& o = b->buffers[b->buffer_count++];
        o.SyncBefore = D3D12_BARRIER_SYNC_ALL;
        o.SyncAfter = D3D12_BARRIER_SYNC_ALL;
        o.AccessBefore = narrowed_access(t.StateBefore, k_buffer_access);
        o.AccessAfter = narrowed_access(t.StateAfter, k_buffer_access);
        o.pResource = desktop;
        o.Offset = 0; // buffer barriers always cover the whole buffer
        o.Size = UINT64_MAX;
        return;
    }
    if (b->texture_count == BARRIER_BATCH)
        flush(list, b);
    D3D12_TEXTURE_BARRIER& o = b->textures[b->texture_count++];
    o.SyncBefore = D3D12_BARRIER_SYNC_ALL;
    o.SyncAfter = D3D12_BARRIER_SYNC_ALL;
    o.LayoutBefore = console_texture_layout(r, console_layout_from_state(t.StateBefore));
    o.LayoutAfter = console_texture_layout(r, console_layout_from_state(t.StateAfter));
    o.AccessBefore = texture_access(r, narrowed_access(t.StateBefore, k_texture_access));
    o.AccessAfter = texture_access(r, narrowed_access(t.StateAfter, k_texture_access));
    o.pResource = desktop;
    // A subresource index, or all of them (0xffffffff), as legacy barriers name them.
    o.Subresources = {t.Subresource, 0, 0, 0, 0, 0};
    o.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
}

void console_record_resource_barriers(ID3D12GraphicsCommandList7* list, u32 count,
                                      const D3D12_RESOURCE_BARRIER* barriers) {
    BarrierBatch b;
    b.buffer_count = 0;
    b.texture_count = 0;
    b.global = false;
    for (u32 i = 0; i < count; i++) {
        const D3D12_RESOURCE_BARRIER& barrier = barriers[i];
        // Split barriers happen whole at their end.
        if (barrier.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY)
            continue;
        switch (barrier.Type) {
        case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
            add_transition(list, &b, barrier.Transition);
            break;
        case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
        case D3D12_RESOURCE_BARRIER_TYPE_UAV:
            b.global = true;
            break;
        default:
            LOG_W_ONCE("d3d12", "ResourceBarrier: unknown barrier type %d skipped", barrier.Type);
            break;
        }
    }
    flush(list, &b);
}

// --- enhanced barriers -------------------------------------------------------------------------------

static void record_buffer_group(ID3D12GraphicsCommandList7* list, const D3D12_BARRIER_GROUP& g) {
    D3D12_BUFFER_BARRIER out[BARRIER_BATCH];
    u32 n = 0;
    for (u32 i = 0; i < g.NumBarriers; i++) {
        D3D12_BUFFER_BARRIER b = g.pBufferBarriers[i];
        // One desktop buffer (a page table segment or a memory window) stands for every title buffer in
        // its range. A title barrier syncing nothing before or after promises that its own buffer isn't
        // accessed on that side within the submission, which the other title buffers sharing the desktop
        // buffer break. Nothing before: nothing to wait for or write back, and buffers have no layout,
        // so the barrier does nothing. Nothing after: its writes are waited for and made visible to all
        // later work instead, which keeps memory handed from one title buffer to the next in order.
        if (b.SyncBefore == D3D12_BARRIER_SYNC_NONE)
            continue;
        if (b.SyncAfter == D3D12_BARRIER_SYNC_NONE) {
            b.SyncAfter = D3D12_BARRIER_SYNC_ALL;
            b.AccessAfter = D3D12_BARRIER_ACCESS_COMMON;
        }
        u64 offset;
        b.pResource =
            b.pResource ? console_resource_desktop((ConsoleResource*)b.pResource, &offset, GPU_READ) : nullptr;
        if (!b.pResource) {
            LOG_W_ONCE("d3d12", "Barrier: buffer without desktop backing skipped");
            continue;
        }
        b.Offset = 0;
        b.Size = UINT64_MAX;
        out[n++] = b;
        if (n == BARRIER_BATCH) {
            D3D12_BARRIER_GROUP group = {D3D12_BARRIER_TYPE_BUFFER, n};
            group.pBufferBarriers = out;
            list->Barrier(1, &group);
            n = 0;
        }
    }
    if (n) {
        D3D12_BARRIER_GROUP group = {D3D12_BARRIER_TYPE_BUFFER, n};
        group.pBufferBarriers = out;
        list->Barrier(1, &group);
    }
}

static void record_texture_group(ID3D12GraphicsCommandList7* list, const D3D12_BARRIER_GROUP& g) {
    D3D12_TEXTURE_BARRIER out[BARRIER_BATCH];
    u32 n = 0;
    for (u32 i = 0; i < g.NumBarriers; i++) {
        D3D12_TEXTURE_BARRIER b = g.pTextureBarriers[i];
        const ConsoleResource* r = (const ConsoleResource*)b.pResource;
        u64 offset;
        b.pResource = r ? console_resource_desktop((ConsoleResource*)r, &offset, GPU_READ) : nullptr;
        if (!b.pResource) {
            LOG_W_ONCE("d3d12", "Barrier: texture without desktop backing skipped");
            continue;
        }
        b.LayoutBefore = console_texture_layout(r, console_desktop_layout(b.LayoutBefore));
        b.LayoutAfter = console_texture_layout(r, console_desktop_layout(b.LayoutAfter));
        b.AccessBefore = texture_access(r, b.AccessBefore);
        b.AccessAfter = texture_access(r, b.AccessAfter);
        if (texture_sync_tracks(r->desc))
            b.Flags &= ~D3D12_TEXTURE_BARRIER_FLAG_DISCARD; // their contents are kept
        out[n++] = b;
        if (n == BARRIER_BATCH) {
            D3D12_BARRIER_GROUP group = {D3D12_BARRIER_TYPE_TEXTURE, n};
            group.pTextureBarriers = out;
            list->Barrier(1, &group);
            n = 0;
        }
    }
    if (n) {
        D3D12_BARRIER_GROUP group = {D3D12_BARRIER_TYPE_TEXTURE, n};
        group.pTextureBarriers = out;
        list->Barrier(1, &group);
    }
}

void console_record_barrier_groups(ID3D12GraphicsCommandList7* list, u32 count,
                                   const D3D12_BARRIER_GROUP* groups) {
    for (u32 i = 0; i < count; i++) {
        const D3D12_BARRIER_GROUP& g = groups[i];
        switch (g.Type) {
        case D3D12_BARRIER_TYPE_GLOBAL:
            list->Barrier(1, &g); // no resources to translate
            break;
        case D3D12_BARRIER_TYPE_BUFFER:
            record_buffer_group(list, g);
            break;
        case D3D12_BARRIER_TYPE_TEXTURE:
            record_texture_group(list, g);
            break;
        default:
            LOG_W_ONCE("d3d12", "Barrier: unknown barrier group type %d skipped", g.Type);
            break;
        }
    }
}

void console_record_full_barrier(ID3D12GraphicsCommandList7* list) {
    D3D12_GLOBAL_BARRIER global = {D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_ALL,
                                   D3D12_BARRIER_ACCESS_COMMON, D3D12_BARRIER_ACCESS_COMMON};
    D3D12_BARRIER_GROUP group = {D3D12_BARRIER_TYPE_GLOBAL, 1};
    group.pGlobalBarriers = &global;
    list->Barrier(1, &group);
}
