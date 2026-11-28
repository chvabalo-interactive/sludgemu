#pragma once

#include "common/types.h"

// The desktop format and sample count of the render target or depth view in a console slot.
bool view_target_format(u64 console_handle, u32* format, u32* samples);

#include "hle/d3d12/d3d12_objects.h"

// Device entries that write descriptors (views.cpp). Resources arrive as console objects.

// Writes the desktop view of a console constant buffer at a desktop CPU handle.
void write_constant_buffer_view(u64 address, u64 size, D3D12_CPU_DESCRIPTOR_HANDLE dest);

void WINAPI device_create_constant_buffer_view(ConsoleDevice* self,
                                               const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                               D3D12_CPU_DESCRIPTOR_HANDLE dest);
void WINAPI device_create_shader_resource_view(ConsoleDevice* self, void* resource,
                                               const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                               D3D12_CPU_DESCRIPTOR_HANDLE dest);
// `desc` is a console UAV description, which is shorter than the desktop one.
void WINAPI device_create_unordered_access_view(ConsoleDevice* self, void* resource, void* counter,
                                                const void* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest);
void WINAPI device_create_render_target_view(ConsoleDevice* self, void* resource,
                                             const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                                             D3D12_CPU_DESCRIPTOR_HANDLE dest);
void WINAPI device_create_depth_stencil_view(ConsoleDevice* self, void* resource,
                                             const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                                             D3D12_CPU_DESCRIPTOR_HANDLE dest);
void WINAPI device_create_sampler(ConsoleDevice* self, const D3D12_SAMPLER_DESC* desc,
                                  D3D12_CPU_DESCRIPTOR_HANDLE dest);
void WINAPI device_copy_descriptors(ConsoleDevice* self, UINT dest_range_count,
                                    const D3D12_CPU_DESCRIPTOR_HANDLE* dest_starts, const UINT* dest_sizes,
                                    UINT src_range_count, const D3D12_CPU_DESCRIPTOR_HANDLE* src_starts,
                                    const UINT* src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type);
void WINAPI device_copy_descriptors_simple(ConsoleDevice* self, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dest,
                                           D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type);

struct ConsoleSrvDescX;
struct ConsoleUavDescX;
void WINAPI device_create_placed_raw_srv_x(ConsoleDevice* self, const D3D12_RESOURCE_DESC* resource_desc,
                                           const ConsoleSrvDescX* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest);
void WINAPI device_create_placed_raw_uav_x(ConsoleDevice* self, const D3D12_RESOURCE_DESC* resource_desc,
                                           const ConsoleUavDescX* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest);

// The memory location a console placed view description (the SRV or UAV kind) names.
u64 console_view_location_x(const void* desc, bool unordered_access);

struct ConsoleSamplerDescX;
void WINAPI device_create_sampler_x(ConsoleDevice* self, const ConsoleSamplerDescX* desc,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dest);
