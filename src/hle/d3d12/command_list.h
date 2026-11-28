#pragma once

#include "hle/d3d12/d3d12_objects.h"

// Shared by the command list sources: command_list.cpp (lifetime and state) and
// command_list_work.cpp (draws, dispatches, copies, clears, queries).

// Console-only primitive topology: three corners per rectangle.
#define CONSOLE_TOPOLOGY_RECT_LIST 17

// The desktop handle for a console GPU descriptor handle in one of the list's bound heaps.
bool list_gpu_descriptor(const ConsoleCommandList* list, u64 handle, D3D12_GPU_DESCRIPTOR_HANDLE* out);

// The desktop CPU-only handle for `count` console descriptors starting at `handle`.
// A console shader-visible descriptor's index in the bound heaps (desktop and console heaps match).
bool list_descriptor_index(const ConsoleCommandList* list, u64 handle, u32* out);
// Root arguments to the desktop list as user data, if they changed.
void list_flush_user_data(ConsoleCommandList* self, bool graphics);
bool list_cpu_descriptor(u64 handle, u32 count, D3D12_CPU_DESCRIPTOR_HANDLE* out);

// The desktop resource behind a console resource and where the console resource starts in it.
ID3D12Resource* list_resource(void* console_resource, u64* offset, GpuAccess access);

// Ends the current desktop list and continues on a new one with the same state; `sync` happens
// between the two on the queue. False (and the sync is lost) if the list can't be split.
// `counter`: for waits on a title counter (see console_sync_fence), its address; otherwise 0.
bool list_split(ConsoleCommandList* list, ListSyncKind sync, ID3D12Fence* fence, u64 value, u64 counter);

// State of command_list_x.cpp (shading rates, sample positions, view instancing, stream output) on a
// fresh desktop list.
void list_apply_x_state(ConsoleCommandList* list);

// What the desktop GPU supports of command_list_x.cpp's state; once, with the list table.
void console_command_list_x_init();

extern const ConsoleEntry k_list_work_entries[];
extern const u32 k_list_work_entry_count;
extern const ConsoleEntry k_list_x_entries[];
extern const u32 k_list_x_entry_count;
