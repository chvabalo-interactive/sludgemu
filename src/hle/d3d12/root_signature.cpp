// Console root signatures: serialization (d3d12_xs exports) and ID3D12RootSignature.
//
// Console root signature descriptions are the desktop ones (versions 1.0 and 1.1) plus a few
// console flags in the high bits that select console shader ABIs. Our serializer writes the
// desktop format, since CreateRootSignature, also ours, is its only consumer.

#include <string.h>

#include "common/log.h"
#include "gpu/gpu.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"
#include "hle/d3d12/root_signature.h"

#define CONSOLE_ROOT_SIGNATURE_FLAGS 0xFF000000u

static const GUID* const k_root_signature_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                                    &__uuidof(ID3D12RootSignature)};

static void root_signature_destroy(void* object);

static const ConsoleClass k_root_signature_class = {
    "ID3D12RootSignature", k_root_signature_iids, ARRAY_COUNT(k_root_signature_iids), root_signature_destroy};

static void* const* g_root_signature_table = nullptr;

// --- serialization -----------------------------------------------------------------------------------

static D3D12_VERSIONED_ROOT_SIGNATURE_DESC
without_console_flags(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc) {
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d = desc;
    D3D12_ROOT_SIGNATURE_FLAGS* flags =
        d.Version == D3D_ROOT_SIGNATURE_VERSION_1_0 ? &d.Desc_1_0.Flags : &d.Desc_1_1.Flags;
    if (*flags & CONSOLE_ROOT_SIGNATURE_FLAGS)
        LOG_D("d3d12", "root signature: dropping console ABI flags 0x%x",
              *flags & CONSOLE_ROOT_SIGNATURE_FLAGS);
    *flags = (D3D12_ROOT_SIGNATURE_FLAGS)(*flags & ~CONSOLE_ROOT_SIGNATURE_FLAGS);
    return d;
}

HRESULT WINAPI console_serialize_versioned_root_signature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                                          ID3DBlob** blob, ID3DBlob** error) {
    if (!desc)
        return E_INVALIDARG;
    if (desc->Version != D3D_ROOT_SIGNATURE_VERSION_1_0 && desc->Version != D3D_ROOT_SIGNATURE_VERSION_1_1) {
        LOG_E("d3d12", "root signature version %d is not a console version", desc->Version);
        return E_INVALIDARG;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d = without_console_flags(*desc);
    HRESULT hr = D3D12SerializeVersionedRootSignature(&d, blob, error);
    if (FAILED(hr))
        LOG_E("d3d12", "serializing a root signature failed (0x%08lx)", (unsigned long)hr);
    return hr;
}

HRESULT WINAPI console_serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC* desc,
                                                D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob,
                                                ID3DBlob** error) {
    if (!desc)
        return E_INVALIDARG;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d = {};
    d.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    d.Desc_1_0 = *desc;
    if (version != D3D_ROOT_SIGNATURE_VERSION_1_0)
        LOG_W("d3d12", "D3D12SerializeRootSignature asked for version %d of a 1.0 description", version);
    return console_serialize_versioned_root_signature(&d, blob, error);
}

// --- object ------------------------------------------------------------------------------------------

static void root_signature_destroy(void* object) {
    ConsoleRootSignature* rs = (ConsoleRootSignature*)object;
    for (u32 i = 0; i < rs->variant_count; i++)
        gpu_retire(rs->variants[i].desktop);
    if (rs->deserializer)
        rs->deserializer->Release();
}

static HRESULT create_desktop_signature(ConsoleRootSignature* rs, const SamplerSet* sets, u32 set_count,
                                        ID3D12RootSignature** out);

ID3D12RootSignature* console_root_signature_desktop(ConsoleRootSignature* rs, const SamplerSet* sets,
                                                    u32 set_count) {
    u64 key = 0xcbf29ce484222325ull; // FNV-1a over the sets
    u32 samplers = 0;
    for (u32 i = 0; i < set_count; i++) {
        samplers += sets[i].count;
        const u8* bytes[2] = {(const u8*)sets[i].samplers, (const u8*)&sets[i].visibility};
        usize sizes[2] = {sets[i].count * sizeof(InlineSampler), sizeof(sets[i].visibility)};
        for (u32 part = 0; part < 2; part++)
            for (usize b = 0; b < sizes[part]; b++)
                key = (key ^ bytes[part][b]) * 0x100000001b3ull;
    }
    if (!samplers)
        return rs->desktop;
    AcquireSRWLockExclusive(&rs->variant_lock);
    ID3D12RootSignature* desktop = nullptr;
    for (u32 i = 0; i < rs->variant_count && !desktop; i++)
        if (rs->variants[i].key == key)
            desktop = rs->variants[i].desktop;
    if (!desktop) {
        if (rs->variant_count == ROOT_MAX_SIGNATURE_VARIANTS)
            LOG_W_ONCE("d3d12",
                       "more than %d sets of inline samplers with one root signature; pipelines skipped",
                       ROOT_MAX_SIGNATURE_VARIANTS);
        else if (SUCCEEDED(create_desktop_signature(rs, sets, set_count, &desktop)))
            rs->variants[rs->variant_count++] = {key, desktop};
    }
    ReleaseSRWLockExclusive(&rs->variant_lock);
    return desktop;
}

void console_root_signature_init() {
    g_root_signature_table = console_build_table("ID3D12RootSignature", k_root_signature_entries,
                                                 ARRAY_COUNT(k_root_signature_entries), nullptr, 0);
}

// The console's root parameters as user data for shader translation. Tables with more ranges than
// the translator keeps lose the last ones: shaders then guess their descriptor kinds.
static void console_layout(const D3D12_ROOT_SIGNATURE_DESC1& desc, ConsoleRootLayout* layout) {
    memset(layout, 0, sizeof(*layout));
    layout->flags = desc.Flags;
    layout->parameter_count = (u8)desc.NumParameters;
    for (u32 i = 0; i < desc.NumParameters; i++) {
        const D3D12_ROOT_PARAMETER1& p = desc.pParameters[i];
        ConsoleRootParameter& out = layout->parameters[i];
        switch (p.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE:
            out.type = ROOT_TABLE;
            if (p.DescriptorTable.NumDescriptorRanges > SHADER_MAX_TABLE_RANGES)
                LOG_W_ONCE("d3d12",
                           "root signature table with %u ranges: translated shaders see the first %d",
                           p.DescriptorTable.NumDescriptorRanges, SHADER_MAX_TABLE_RANGES);
            for (u32 r = 0; r < p.DescriptorTable.NumDescriptorRanges; r++) {
                const D3D12_DESCRIPTOR_RANGE1& range = p.DescriptorTable.pDescriptorRanges[r];
                if (r >= SHADER_MAX_TABLE_RANGES) {
                    // The rest still decides whether this is a sampler table.
                    out.sampler_table |= range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                    continue;
                }
                TableRange& t = out.ranges[out.range_count++];
                if (range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
                    out.sampler_table = true;
                t.kind = range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV   ? RANGE_UAV
                         : range.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV ? RANGE_CBV
                                                                              : RANGE_SRV;
                t.count = range.NumDescriptors;
                t.offset = range.OffsetInDescriptorsFromTableStart; // ~0u: appended
            }
            break;
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            out.type = ROOT_CONSTANTS;
            out.constants = (u8)p.Constants.Num32BitValues;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
            out.type = ROOT_CBV;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
            out.type = ROOT_SRV;
            break;
        default:
            out.type = ROOT_UAV;
            break;
        }
    }
}

// The desktop signature translated shaders use: user data as root constants, then the root
// descriptors, the console's static samplers, programs' inline samplers, and directly indexed heaps.
static HRESULT create_desktop_signature(ConsoleRootSignature* rs, const SamplerSet* sets, u32 set_count,
                                        ID3D12RootSignature** out) {
    const D3D12_ROOT_SIGNATURE_DESC1& desc = *rs->desc;
    // The user data image starts at s0; compute shaders put root parameters lower than graphics ones,
    // so the image covers the graphics layout, the longer one.
    ShaderInfo info = {};
    u32 user_sgprs = console_user_data_layout(rs->layout, CONSOLE_FIRST_ROOT_SGPR_GRAPHICS, &info);
    rs->user_dwords = (u8)((user_sgprs + 3) & ~3u); // shaders read the user data as uint4s
    D3D12_ROOT_PARAMETER1 params[1 + ROOT_MAX_PARAMETERS] = {};
    u32 count = 0;
    D3D12_ROOT_PARAMETER1& user = params[count++];
    user.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    user.Constants.Num32BitValues = rs->user_dwords;
    user.Constants.RegisterSpace = ROOT_USER_DATA_SPACE;
    user.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    u32 dwords = rs->user_dwords;
    for (u32 i = 0; i < desc.NumParameters && i < ROOT_MAX_PARAMETERS; i++) {
        rs->user_sgpr[i] = info.parameters[i].sgpr;
        rs->desktop_parameter[i] = -1;
        D3D12_ROOT_PARAMETER_TYPE type = desc.pParameters[i].ParameterType;
        if (type != D3D12_ROOT_PARAMETER_TYPE_CBV && type != D3D12_ROOT_PARAMETER_TYPE_SRV &&
            type != D3D12_ROOT_PARAMETER_TYPE_UAV)
            continue;
        // Constant buffers too are read as raw buffers: no 256-byte alignment for their addresses.
        D3D12_ROOT_PARAMETER1& p = params[count];
        p.ParameterType = type == D3D12_ROOT_PARAMETER_TYPE_UAV ? D3D12_ROOT_PARAMETER_TYPE_UAV
                                                                : D3D12_ROOT_PARAMETER_TYPE_SRV;
        p.Descriptor.ShaderRegister = i;
        p.Descriptor.RegisterSpace = ROOT_USER_DATA_SPACE;
        p.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rs->desktop_parameter[i] = (s8)count++;
        dwords += 2;
    }
    if (dwords > 64) {
        LOG_E("d3d12", "root signature: %u user data dwords and root descriptors exceed the desktop limit",
              dwords);
        return E_INVALIDARG;
    }
    D3D12_STATIC_SAMPLER_DESC1 samplers[64 + 3 * SHADER_MAX_INLINE_SAMPLERS];
    u32 sampler_count = desc.NumStaticSamplers < 64 ? desc.NumStaticSamplers : 64;
    for (u32 i = 0; i < sampler_count; i++) {
        const D3D12_STATIC_SAMPLER_DESC& s = desc.pStaticSamplers[i];
        D3D12_STATIC_SAMPLER_DESC1& d = samplers[i];
        memcpy(&d, &s, sizeof(s));
        d.ShaderRegister = i;
        d.RegisterSpace = ROOT_STATIC_SAMPLER_SPACE;
        d.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        d.Flags = D3D12_SAMPLER_FLAG_NONE;
    }
    u32 console_samplers = sampler_count;
    for (u32 i = 0; i < set_count; i++) {
        for (u32 k = 0; k < sets[i].count && sampler_count < ARRAY_COUNT(samplers); k++) {
            const InlineSampler& in = sets[i].samplers[k];
            D3D12_STATIC_SAMPLER_DESC1& d = samplers[sampler_count++];
            d = {};
            d.Filter = (D3D12_FILTER)in.filter;
            d.AddressU = (D3D12_TEXTURE_ADDRESS_MODE)in.address[0];
            d.AddressV = (D3D12_TEXTURE_ADDRESS_MODE)in.address[1];
            d.AddressW = (D3D12_TEXTURE_ADDRESS_MODE)in.address[2];
            d.MipLODBias = in.mip_bias;
            d.MaxAnisotropy = in.max_anisotropy;
            d.ComparisonFunc = (D3D12_COMPARISON_FUNC)in.comparison;
            d.BorderColor = (D3D12_STATIC_BORDER_COLOR)in.border_color;
            d.MinLOD = in.min_lod;
            d.MaxLOD = in.max_lod;
            d.ShaderRegister = k;
            d.RegisterSpace = SHADER_INLINE_SAMPLER_SPACE;
            d.ShaderVisibility = sets[i].visibility;
            d.Flags =
                in.unnormalized ? D3D12_SAMPLER_FLAG_NON_NORMALIZED_COORDINATES : D3D12_SAMPLER_FLAG_NONE;
        }
    }
    (void)console_samplers;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned = {};
    versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_2;
    versioned.Desc_1_2.NumParameters = count;
    versioned.Desc_1_2.pParameters = params;
    versioned.Desc_1_2.NumStaticSamplers = sampler_count;
    versioned.Desc_1_2.pStaticSamplers = samplers;
    versioned.Desc_1_2.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                               D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                               D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&versioned, &blob, &error);
    if (FAILED(hr)) {
        LOG_E("d3d12", "serializing a translated root signature failed: %s",
              error ? (const char*)error->GetBufferPointer() : "?");
        if (error)
            error->Release();
        return hr;
    }
    hr = gpu_device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(out));
    blob->Release();
    if (FAILED(hr))
        LOG_E("d3d12", "CreateRootSignature failed (0x%08lx)", (unsigned long)hr);
    return hr;
}

HRESULT console_create_root_signature(ConsoleDevice* device, const void* blob, usize size, REFIID iid,
                                      void** out) {
    if (!blob || size == 0)
        return E_INVALIDARG;
    ID3D12VersionedRootSignatureDeserializer* deserializer = nullptr;
    HRESULT hr = D3D12CreateVersionedRootSignatureDeserializer(blob, size, IID_PPV_ARGS(&deserializer));
    if (FAILED(hr)) {
        const u8* b = (const u8*)blob;
        LOG_E("d3d12",
              "root signature blob of %zu bytes is not in a known format (%02x %02x %02x %02x): 0x%08lx",
              size, b[0], size > 1 ? b[1] : 0, size > 2 ? b[2] : 0, size > 3 ? b[3] : 0, (unsigned long)hr);
        return hr;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
    hr = deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc);
    if (FAILED(hr)) {
        deserializer->Release();
        return hr;
    }
    if (desc->Desc_1_1.NumParameters > ROOT_MAX_PARAMETERS) {
        LOG_E("d3d12", "root signature with %u parameters", desc->Desc_1_1.NumParameters);
        deserializer->Release();
        return E_INVALIDARG;
    }
    ConsoleRootLayout layout;
    console_layout(desc->Desc_1_1, &layout);
    ConsoleRootSignature* rs = (ConsoleRootSignature*)console_alloc(
        sizeof(ConsoleRootSignature), &k_root_signature_class, g_root_signature_table, nullptr);
    rs->desc = &desc->Desc_1_1;
    rs->deserializer = deserializer;
    rs->layout = layout;
    // Without a desktop signature the title still gets its object; draws using it are skipped.
    InitializeSRWLock(&rs->variant_lock);
    if (SUCCEEDED(create_desktop_signature(rs, nullptr, 0, &rs->desktop)))
        console_private(rs)->desktop = rs->desktop; // the object owns it from here
    u32 constants = 0;
    for (u32 i = 0; i < rs->desc->NumParameters && i < ROOT_MAX_PARAMETERS; i++) {
        const D3D12_ROOT_PARAMETER1& p = rs->desc->pParameters[i];
        rs->constant_offset[i] = (u8)constants;
        if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
            constants += p.Constants.Num32BitValues;
    }
    LOG_D("d3d12", "root signature: %u parameters, %u static samplers, flags 0x%x", rs->desc->NumParameters,
          rs->desc->NumStaticSamplers, rs->desc->Flags);
    return console_return(rs, iid, out);
}
