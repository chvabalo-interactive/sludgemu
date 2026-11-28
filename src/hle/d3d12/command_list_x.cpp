// Console command list entries for state and hints beyond the core ones: variable rate shading,
// sample masks and positions, view instancing, stream output, and the console's hardware tuning
// entries.
//
// Many console entries program GPU features desktop drivers manage themselves (compression
// metadata, hierarchical stencil, primitive binning, wave launch limits, prefetching, perf
// counters). They change how fast the console GPU renders, not what it renders, so their desktop
// behaviour is to do nothing, deliberately. Entries that would change results but have no desktop
// equivalent warn once.

#include "common/log.h"
#include "gpu/gpu.h"
#include "hle/d3d12/command_args.h"
#include "hle/d3d12/command_list.h"

// What the desktop GPU supports, asked once.
static struct {
    D3D12_VARIABLE_SHADING_RATE_TIER vrs_tier;
    bool additional_shading_rates;
    D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER sample_positions_tier;
    D3D12_VIEW_INSTANCING_TIER view_instancing_tier;
} g_caps;

void console_command_list_x_init() {
    ID3D12Device13* device = gpu_device();
    D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6)))) {
        g_caps.vrs_tier = o6.VariableShadingRateTier;
        g_caps.additional_shading_rates = o6.AdditionalShadingRatesSupported;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &o2, sizeof(o2))))
        g_caps.sample_positions_tier = o2.ProgrammableSamplePositionsTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &o3, sizeof(o3))))
        g_caps.view_instancing_tier = o3.ViewInstancingTier;
    LOG_D("d3d12",
          "desktop GPU: shading rate tier %d (additional rates %d), sample positions tier %d, view "
          "instancing tier %d",
          g_caps.vrs_tier, g_caps.additional_shading_rates, g_caps.sample_positions_tier,
          g_caps.view_instancing_tier);
}

// --- variable rate shading ---------------------------------------------------------------------------

// Tier 1 has a base rate only: combiners must stay passthrough (shader and image rates need tier 2).
static void set_shading_rate(ConsoleCommandList* self) {
    const ListState& s = self->state;
    if (g_caps.vrs_tier == D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED)
        return;
    bool combined = s.shading_rate_combiners[0] != D3D12_SHADING_RATE_COMBINER_PASSTHROUGH ||
                    s.shading_rate_combiners[1] != D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
    bool tier2 = g_caps.vrs_tier >= D3D12_VARIABLE_SHADING_RATE_TIER_2;
    if (combined && !tier2)
        LOG_W_ONCE("d3d12", "shading rate combiners need shading rate tier 2; only the base rate applies");
    self->desktop->RSSetShadingRate(s.shading_rate, tier2 ? s.shading_rate_combiners : nullptr);
}

static void record_shading_rate(ConsoleCommandList* self, D3D12_SHADING_RATE rate,
                                const D3D12_SHADING_RATE_COMBINER combiners[2]) {
    ListState& s = self->state;
    if (g_caps.vrs_tier == D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED &&
        (rate != D3D12_SHADING_RATE_1X1 || combiners[0] || combiners[1]))
        LOG_W_ONCE("d3d12", "the GPU has no variable rate shading: draws shade every pixel");
    s.shading_rate = rate;
    s.shading_rate_combiners[0] = combiners[0];
    s.shading_rate_combiners[1] = combiners[1];
    s.has_shading_rate = true;
    if (self->type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        set_shading_rate(self);
}

static void WINAPI list_rs_set_shading_rate(ConsoleCommandList* self, D3D12_SHADING_RATE rate,
                                            const D3D12_SHADING_RATE_COMBINER* combiners) {
    D3D12_SHADING_RATE_COMBINER c[2] = {D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                                        D3D12_SHADING_RATE_COMBINER_PASSTHROUGH};
    if (combiners) {
        c[0] = console_shading_rate_combiner((u32)combiners[0]);
        c[1] = console_shading_rate_combiner((u32)combiners[1]);
    }
    record_shading_rate(self, console_shading_rate((u32)rate, g_caps.additional_shading_rates), c);
}

// The console's image is the bound depth buffer's metadata or a resource like desktop's; the desktop
// image is the resource's desktop texture (8-bit rates per tile, like the console's).
static void WINAPI list_rs_set_shading_rate_image(ConsoleCommandList* self, void* image) {
    u64 offset;
    ID3D12Resource* desktop = list_resource(image, &offset, GPU_READ);
    if (image && !desktop)
        LOG_W_ONCE("d3d12", "shading rate image without a desktop texture; draws use no image");
    self->state.shading_rate_image = desktop;
    if (self->type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    if (g_caps.vrs_tier < D3D12_VARIABLE_SHADING_RATE_TIER_2) {
        if (desktop)
            LOG_W_ONCE("d3d12", "shading rate images need shading rate tier 2; ignored");
        return;
    }
    self->desktop->RSSetShadingRateImage(desktop);
}

// Console VRS register settings (the settings' first member is a 32-bit type, the union follows).
struct ConsoleShadingRateSettingsArgs {
    u32 type;
    u32 bits[3];
};

#define CONSOLE_SHADING_RATE_SETTINGS_COMBINERS      0
#define CONSOLE_SHADING_RATE_SETTINGS_CENTER_OFFSETS 1
#define CONSOLE_SHADING_RATE_SETTINGS_HTILE_MODE     2
#define CONSOLE_SHADING_RATE_SETTINGS_CMASK_HINT     3

static void WINAPI list_rs_set_shading_rate_x(ConsoleCommandList* self,
                                              const ConsoleShadingRateSettingsArgs* settings) {
    if (!settings)
        return;
    switch (settings->type) {
    case CONSOLE_SHADING_RATE_SETTINGS_COMBINERS: {
        ConsoleShadingRateSettings s =
            console_shading_rate_settings(settings->bits[0], g_caps.additional_shading_rates);
        if (s.supersampling)
            LOG_W_ONCE("d3d12", "supersampling shading rates have no desktop equivalent; shaded at 1x1");
        record_shading_rate(self, s.base, s.combiners);
        break;
    }
    case CONSOLE_SHADING_RATE_SETTINGS_CENTER_OFFSETS:
        // Where coarse pixels are evaluated: desktop drivers pick the center.
        if (settings->bits[0])
            LOG_W_ONCE("d3d12", "coarse pixel center offsets have no desktop equivalent; centers are used");
        break;
    default:
        // How the console encodes rates in depth and color metadata: no meaning without them.
        break;
    }
}

// --- sample mask, sample positions -------------------------------------------------------------------

static void WINAPI list_om_set_sample_mask_x(ConsoleCommandList* self, UINT64 quad_mask) {
    bool uniform;
    u16 mask = console_quad_sample_mask(quad_mask, &uniform);
    if (!uniform)
        LOG_W_ONCE("d3d12",
                   "sample mask 0x%016llx differs between the pixels of a quad, which desktop sample masks "
                   "can't; every pixel keeps the samples any of them keeps",
                   (unsigned long long)quad_mask);
    // Pipelines are made per mask; the next draw picks the variant (see PipelineOutputs).
    self->state.dropped_samples = (u16)~mask;
}

static void set_sample_positions(ConsoleCommandList* self) {
    const ListState& s = self->state;
    self->desktop->SetSamplePositions(s.sample_positions_per_pixel, s.sample_position_pixels,
                                      (D3D12_SAMPLE_POSITION*)s.sample_positions);
}

static void record_sample_positions(ConsoleCommandList* self, u32 per_pixel, u32 pixels,
                                    const D3D12_SAMPLE_POSITION* positions) {
    ListState& s = self->state;
    bool standard = per_pixel == 0;
    if (!standard) {
        D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER needed = pixels > 1
                                                              ? D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_2
                                                              : D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_1;
        if (g_caps.sample_positions_tier < needed || per_pixel * pixels > ARRAY_COUNT(s.sample_positions)) {
            LOG_W_ONCE("d3d12",
                       "programmable sample positions (%u per pixel, %u pixels) aren't supported by "
                       "the GPU; standard positions are used",
                       per_pixel, pixels);
            standard = true;
        }
    }
    if (standard) {
        s.sample_positions_per_pixel = 0;
        s.sample_position_pixels = 0;
    } else {
        s.sample_positions_per_pixel = (u8)per_pixel;
        s.sample_position_pixels = (u8)pixels;
        memcpy(s.sample_positions, positions, per_pixel * pixels * sizeof(D3D12_SAMPLE_POSITION));
    }
    if (self->type == D3D12_COMMAND_LIST_TYPE_DIRECT &&
        g_caps.sample_positions_tier != D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED)
        set_sample_positions(self);
}

static void WINAPI list_set_sample_positions(ConsoleCommandList* self, UINT per_pixel, UINT pixels,
                                             D3D12_SAMPLE_POSITION* positions) {
    record_sample_positions(self, positions ? per_pixel : 0, pixels, positions);
}

// The low bits of the first word of the console MSAA parameters: log2 of the sample count.
#define CONSOLE_MSAA_SAMPLES_LOG2_MASK 7u

static void WINAPI list_rs_set_msaa_parameters_x(ConsoleCommandList* self,
                                                 const ConsoleSampleLocations* locations,
                                                 const void* centroid_priorities, const u32* parameters) {
    if (centroid_priorities || parameters)
        LOG_W_ONCE("d3d12", "centroid priorities and console MSAA parameters have no desktop equivalent; the "
                            "desktop defaults apply");
    if (!locations) {
        record_sample_positions(self, 0, 0, nullptr);
        return;
    }
    u32 samples = parameters ? 1u << (parameters[0] & CONSOLE_MSAA_SAMPLES_LOG2_MASK)
                             : self->state.outputs.sample_count;
    if (samples < 2 || samples > 16)
        return; // positions of single-sampled targets: the pixel center either way
    D3D12_SAMPLE_POSITION positions[64];
    u32 pixels = console_sample_positions(*locations, samples, positions);
    record_sample_positions(self, samples, pixels, positions);
}

// --- view instancing, stream output ------------------------------------------------------------------

static void WINAPI list_set_view_instance_mask(ConsoleCommandList* self, UINT mask) {
    self->state.view_instance_mask = mask;
    self->state.has_view_instance_mask = true;
    if (g_caps.view_instancing_tier == D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED) {
        LOG_W_ONCE("d3d12", "the GPU has no view instancing; SetViewInstanceMask ignored");
        return;
    }
    if (self->type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        self->desktop->SetViewInstanceMask(mask);
}

// Stream output buffers and their filled-size counters are title memory the GPU writes.
static void WINAPI list_so_set_targets(ConsoleCommandList* self, UINT start, UINT count,
                                       const D3D12_STREAM_OUTPUT_BUFFER_VIEW* views) {
    ListState& s = self->state;
    if (start >= D3D12_SO_BUFFER_SLOT_COUNT)
        return;
    if (count > D3D12_SO_BUFFER_SLOT_COUNT - start)
        count = D3D12_SO_BUFFER_SLOT_COUNT - start;
    LOG_W_ONCE("d3d12", "stream output targets are set, but translated pipelines have no stream output yet");
    for (UINT i = 0; i < count; i++) {
        D3D12_STREAM_OUTPUT_BUFFER_VIEW& v = s.stream_output[start + i];
        v = {};
        s.stream_output_mask &= ~(1u << (start + i));
        if (!views || !views[i].BufferLocation)
            continue;
        GpuAddress buffer, filled;
        if (!gpu_translate(views[i].BufferLocation, views[i].SizeInBytes ? views[i].SizeInBytes : 1,
                           GPU_WRITE, &buffer) ||
            !gpu_translate(views[i].BufferFilledSizeLocation, sizeof(u64), GPU_WRITE, &filled)) {
            LOG_W_ONCE("d3d12", "stream output target 0x%llx is outside GPU-visible memory; unbound",
                       views[i].BufferLocation);
            continue;
        }
        v.BufferLocation = buffer.desktop_address;
        v.SizeInBytes = views[i].SizeInBytes;
        v.BufferFilledSizeLocation = filled.desktop_address;
        s.stream_output_mask |= 1u << (start + i);
    }
    if (self->type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        self->desktop->SOSetTargets(start, count, &s.stream_output[start]);
}

void list_apply_x_state(ConsoleCommandList* self) {
    const ListState& s = self->state;
    if (self->type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    if (s.has_shading_rate)
        set_shading_rate(self);
    if (s.shading_rate_image && g_caps.vrs_tier >= D3D12_VARIABLE_SHADING_RATE_TIER_2)
        self->desktop->RSSetShadingRateImage(s.shading_rate_image);
    if (s.sample_positions_per_pixel)
        set_sample_positions(self);
    if (s.has_view_instance_mask && g_caps.view_instancing_tier != D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED)
        self->desktop->SetViewInstanceMask(s.view_instance_mask);
    if (s.stream_output_mask)
        self->desktop->SOSetTargets(0, D3D12_SO_BUFFER_SLOT_COUNT, s.stream_output);
}

// --- console hardware tuning: no desktop meaning -----------------------------------------------------

// Whether pixel shaders that write depth run their depth test late, in draw order: desktop drivers
// order such pixels themselves (results are the same; only early-depth efficiency differs).
static void WINAPI list_set_pixel_shader_depth_force_z_order_x(ConsoleCommandList*, BOOL) {}

// Occlusion query precision and hierarchical stencil: desktop occlusion queries count exactly, and
// drivers manage their own hierarchical depth and stencil.
static void WINAPI list_set_occlusion_query_control_x(ConsoleCommandList*, const void*) {}
static void WINAPI list_set_perfect_occlusion_queries_x(ConsoleCommandList*, BOOL) {}
static void WINAPI list_set_hi_stencil_control_x(ConsoleCommandList*, const void*) {}
static void WINAPI list_set_hi_stencil_state_x(ConsoleCommandList*, void*, const void*) {}

// Padding in the console command buffer.
static void WINAPI list_nop_x(ConsoleCommandList*, UINT) {}

// Commands reach the GPU when the title executes the list; there is no early kickoff on desktop.
static void WINAPI list_kickoff_x(ConsoleCommandList*) {}

// Wave launch limits, scratch memory and launch parameters of the console's shader units: translated
// programs run where the desktop scheduler puts them and spill to their own memory.
static void WINAPI list_set_graphics_shader_limits_x(ConsoleCommandList*, BOOL, BOOL, BOOL, BOOL, BOOL,
                                                     const void*) {}
static void WINAPI list_set_compute_shader_limits_x(ConsoleCommandList*, const void*) {}
static void WINAPI list_set_shader_scratch_space_x(ConsoleCommandList*, UINT, UINT64, UINT64, UINT) {}
static void WINAPI list_set_compute_shader_launch_parameters_x(ConsoleCommandList*, const void*) {}

// Compression metadata of a resource: desktop drivers keep their own compression lossless and consistent.
static void WINAPI list_set_resource_compression_state_x(ConsoleCommandList*, void*, UINT, UINT) {}

// Deferred primitive batch binning: a rasterizer efficiency setting.
static void WINAPI list_set_deferred_primitive_batch_binning_x(ConsoleCommandList*, const void*) {}
static void WINAPI list_deferred_primitive_break_batch_x(ConsoleCommandList*) {}

// Cache prefetches.
static void WINAPI list_prefetch_memory_x(ConsoleCommandList*, UINT64, UINT64, UINT) {}
static void WINAPI list_prefetch_pipeline_state_x(ConsoleCommandList*, void*, UINT64, UINT, UINT) {}

// Driver hints for one list (memory statistics, context roll tuning, ...). Nothing on desktop reads
// them; the previous value is reported as 0 (off), as on a console where it was never set.
static UINT64 WINAPI list_driver_hint_x(ConsoleCommandList*, UINT64, UINT64) {
    return 0;
}

// --- debugging and profiling -------------------------------------------------------------------------

// Console GPU markers for its own capture tools; desktop debuggers see the PIX events instead.
static void WINAPI list_insert_gpu_marker(ConsoleCommandList*, UINT8, const void*, UINT) {}
static void WINAPI list_insert_timing_marker(ConsoleCommandList*, UINT8) {}

// Hardware performance counters: desktop has none to sample. The destination keeps what it held.
static void WINAPI list_start_counters_x(ConsoleCommandList*, void*) {}
static void WINAPI list_stop_counters_x(ConsoleCommandList*) {}
static void WINAPI list_sample_counters_x(ConsoleCommandList*, void*, UINT64) {
    LOG_W_ONCE("d3d12", "SampleCountersX: no performance counters on desktop; nothing written");
}

// Validates a built acceleration structure on the CPU (a debugging aid).
static void WINAPI list_cpu_validate_result_x(ConsoleCommandList*, const void*) {}

static HRESULT WINAPI list_enable_live_debugging_x(ConsoleCommandList*, void*) {
    return S_OK;
}

// Statistics on which mips of partially resident textures were sampled.
static void WINAPI list_write_prt_lod_statistics_x(ConsoleCommandList*, const void*) {
    LOG_W_ONCE("d3d12", "WritePRTLODStatisticsX: sampled mip statistics aren't collected; nothing written");
}

// Content protection doesn't exist for console titles on desktop; null is the only session there is.
static void WINAPI list_set_protected_resource_session(ConsoleCommandList*, void* session) {
    if (session)
        LOG_W_ONCE("d3d12", "SetProtectedResourceSession: protected sessions aren't supported; ignored");
}

// Removed from the console API; nothing calls them in titles that link the current runtime.
static void WINAPI list_deprecated_x(ConsoleCommandList*) {
    LOG_W_ONCE("d3d12", "deprecated predication buffer entry called; ignored");
}

// --- no desktop equivalent ---------------------------------------------------------------------------

// The global data share: on-chip memory shaders reach with GDS instructions, which translated programs
// don't have (their translation fails). Writes go nowhere and reads leave memory as it was.
static void WINAPI list_write_gds_x(ConsoleCommandList*, UINT, UINT, const UINT*, UINT) {
    LOG_W_ONCE("d3d12", "WriteGDSX: the global data share has no desktop equivalent; ignored");
}
static void WINAPI list_read_gds_x(ConsoleCommandList*, UINT, UINT, UINT64, UINT) {
    LOG_W_ONCE("d3d12", "ReadGDSX: the global data share has no desktop equivalent; nothing written");
}
static void WINAPI list_setup_ordered_append_counter_x(ConsoleCommandList*, UINT, UINT, UINT, UINT, UINT,
                                                       UINT) {
    LOG_W_ONCE("d3d12", "SetupOrderedAppendCounterX: ordered append has no desktop equivalent; ignored");
}

// Console bundles as raw command buffers in title memory: desktop has no command buffers to copy.
static UINT WINAPI list_get_execution_command_size_x(ConsoleCommandList*) {
    LOG_W_ONCE("d3d12", "GetExecutionCommandSizeX: raw bundles aren't supported");
    return 0;
}
static UINT WINAPI list_close_bundle_x(ConsoleCommandList*, void**, UINT) {
    LOG_W_ONCE("d3d12", "CloseBundleX: raw bundles aren't supported; nothing written");
    return 0;
}
static void WINAPI list_execute_indirect_bundle_x(ConsoleCommandList*, void*, UINT, UINT, void*, UINT, UINT) {
    LOG_W_ONCE("d3d12", "ExecuteIndirectBundleX: raw bundles aren't supported; skipped");
}

// Lists run from inside a list: desktop lists can't call other lists (only bundles).
static void WINAPI list_execute_command_lists_x(ConsoleCommandList*, UINT, void* const*, UINT* const*) {
    LOG_W_ONCE("d3d12", "ExecuteCommandListsX: lists executed from a list aren't supported; skipped");
}

// Depth bias set apart from the pipeline: desktop has it only for pipelines made for it (and
// depth clipping only in pipelines). The pipeline's own bias applies.
static void WINAPI list_rs_set_depth_bias_x(ConsoleCommandList*, INT, FLOAT, FLOAT, BOOL) {
    LOG_W_ONCE("d3d12",
               "RSSetDepthBiasX: dynamic depth bias isn't supported yet; the pipeline's bias applies");
}

// Meta commands are desktop driver extensions; consoles have none to offer.
static void WINAPI list_meta_command(ConsoleCommandList*, void*, const void*, SIZE_T) {
    LOG_W_ONCE("d3d12", "meta commands aren't supported; skipped");
}

// Reserved resources' tile copies: titles can't create reserved resources here (CreateReservedResource
// fails), so there are none to copy between.
static void WINAPI list_copy_tiles(ConsoleCommandList*, void*, const void*, const void*, void*, UINT64,
                                   UINT) {
    LOG_W_ONCE("d3d12", "CopyTiles: reserved resources aren't supported; skipped");
}

const ConsoleEntry k_list_x_entries[] = {
    CONSOLE_ENTRY(18, list_copy_tiles),
    CONSOLE_ENTRY(49, list_so_set_targets),
    CONSOLE_ENTRY(59, list_insert_gpu_marker),
    CONSOLE_ENTRY(60, list_insert_timing_marker),
    CONSOLE_ENTRY(66, list_set_occlusion_query_control_x),
    CONSOLE_ENTRY(67, list_set_perfect_occlusion_queries_x),
    CONSOLE_ENTRY(68, list_set_hi_stencil_control_x),
    CONSOLE_ENTRY(75, list_set_pixel_shader_depth_force_z_order_x),
    CONSOLE_ENTRY(76, list_execute_indirect_bundle_x),
    CONSOLE_ENTRY(77, list_setup_ordered_append_counter_x),
    CONSOLE_ENTRY(78, list_write_gds_x),
    CONSOLE_ENTRY(79, list_read_gds_x),
    CONSOLE_ENTRY(81, list_set_hi_stencil_state_x),
    CONSOLE_ENTRY(82, list_rs_set_msaa_parameters_x),
    CONSOLE_ENTRY(84, list_nop_x),
    CONSOLE_ENTRY(87, list_deprecated_x),
    CONSOLE_ENTRY(88, list_deprecated_x),
    CONSOLE_ENTRY(89, list_execute_command_lists_x),
    CONSOLE_ENTRY(93, list_get_execution_command_size_x),
    CONSOLE_ENTRY(94, list_close_bundle_x),
    CONSOLE_ENTRY(95, list_kickoff_x),
    CONSOLE_ENTRY(96, list_rs_set_depth_bias_x),
    CONSOLE_ENTRY(97, list_set_graphics_shader_limits_x),
    CONSOLE_ENTRY(98, list_set_compute_shader_limits_x),
    CONSOLE_ENTRY(99, list_set_resource_compression_state_x),
    CONSOLE_ENTRY(101, list_driver_hint_x),
    CONSOLE_ENTRY(102, list_start_counters_x),
    CONSOLE_ENTRY(103, list_sample_counters_x),
    CONSOLE_ENTRY(104, list_stop_counters_x),
    CONSOLE_ENTRY(105, list_set_deferred_primitive_batch_binning_x),
    CONSOLE_ENTRY(106, list_deferred_primitive_break_batch_x),
    CONSOLE_ENTRY(110, list_set_sample_positions),
    CONSOLE_ENTRY(112, list_set_view_instance_mask),
    CONSOLE_ENTRY(114, list_set_protected_resource_session),
    CONSOLE_ENTRY(117, list_meta_command),
    CONSOLE_ENTRY(118, list_meta_command),
    CONSOLE_ENTRY(124, list_cpu_validate_result_x),
    CONSOLE_ENTRY(126, list_rs_set_shading_rate),
    CONSOLE_ENTRY(127, list_rs_set_shading_rate_image),
    CONSOLE_ENTRY(128, list_rs_set_shading_rate_x),
    CONSOLE_ENTRY(135, list_prefetch_memory_x),
    CONSOLE_ENTRY(136, list_prefetch_pipeline_state_x),
    CONSOLE_ENTRY(141, list_set_shader_scratch_space_x),
    CONSOLE_ENTRY(142, list_om_set_sample_mask_x),
    CONSOLE_ENTRY(143, list_write_prt_lod_statistics_x),
    CONSOLE_ENTRY(144, list_enable_live_debugging_x),
    CONSOLE_ENTRY(145, list_set_compute_shader_launch_parameters_x),
};

const u32 k_list_x_entry_count = ARRAY_COUNT(k_list_x_entries);
