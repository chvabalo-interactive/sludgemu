// Shader translation on the GPU: console wave64 compute programs (assembled from the RDNA 2 source in
// the comments) are translated, compiled and run on the desktop device, and what they store is checked
// against what the console computes. Covers the lane mapping of wave64 programs onto desktop waves
// (see shader/translate.h): cross-lane reads and writes across the halves, whole-mask queries, masks
// written as data, DPP row masks, LDS append, console wave numbering in groups of several waves.
// Skipped without a D3D12 device that runs waves of 32 lanes.

#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <d3d12.h>
#include <ole2.h> // dxcapi.h needs COM types the lean Windows headers leave out
#include <dxcapi.h>

#include "shader/rdna.h"
#include "shader/translate.h"
#include "test.h"

// Program A: one console wave of 64 lanes. Stores 16 regions of 64 dwords (one per lane, at lane * 4
// plus 256 * region) through a root UAV whose address is in s[0:1].
static const u32 k_lanes_program[] = {
    0xbe8203c1,             // s_mov_b32 s2, -1
    0xbe830380,             // s_mov_b32 s3, 0
    0xd7650001, 0x020100c1, // v_mbcnt_lo_u32_b32 v1, -1, 0
    0xd7660001, 0x020202c1, // v_mbcnt_hi_u32_b32 v1, -1, v1
    0x34040282,             // v_lshlrev_b32_e32 v2, 2, v1
    0x16060283,             // v_mul_u32_u24_e32 v3, 3, v1
    0xe0701000, 0x80000102, // buffer_store_dword v1, v2, s[0:3], 0 offen
    0xd7600004, 0x02017f03, // v_readlane_b32 s4, v3, 63
    0xd7600005, 0x02014103, // v_readlane_b32 s5, v3, 32
    0x7e080204,             // v_mov_b32_e32 v4, s4
    0xe0701100, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:256
    0x7e080205,             // v_mov_b32_e32 v4, s5
    0xe0701200, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:512
    0xbe880481,             // s_mov_b64 s[8:9], 1
    0xbe8a2408,             // s_and_saveexec_b64 s[10:11], s[8:9]
    0xd7600007, 0x02017f03, // v_readlane_b32 s7, v3, 63
    0xbefe040a,             // s_mov_b64 exec, s[10:11]
    0x7e080207,             // v_mov_b32_e32 v4, s7
    0xe0701300, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:768
    0x7d8802a8,             // v_cmp_gt_u32_e32 vcc, 40, v1
    0xbe8c106a,             // s_bcnt1_i32_b64 s12, vcc
    0x7d8202a4,             // v_cmp_lt_u32_e32 vcc, 36, v1
    0xbe8d146a,             // s_ff1_i32_b64 s13, vcc
    0x7e08020c,             // v_mov_b32_e32 v4, s12
    0xe0701400, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:1024
    0x7e08020d,             // v_mov_b32_e32 v4, s13
    0xe0701500, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:1280
    0x4a0802ff, 0x000003e8, // v_add_nc_u32_e32 v4, 0x3e8, v1
    0x92fe80a0,             // s_bfm_b64 exec, 32, 0
    0xe0701600, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:1536
    0xbefe04c1,             // s_mov_b64 exec, -1
    0xbefe0391,             // s_mov_b32 exec_lo, 17
    0xbeff0391,             // s_mov_b32 exec_hi, 17
    0xe0701700, 0x80000102, // buffer_store_dword v1, v2, s[0:3], 0 offen offset:1792
    0xbefe04c1,             // s_mov_b64 exec, -1
    0x7e0a0280,             // v_mov_b32_e32 v5, 0
    0xbefe0480,             // s_mov_b64 exec, 0
    0xd7610005, 0x02015004, // v_writelane_b32 v5, s4, 40
    0xbefe04c1,             // s_mov_b64 exec, -1
    0xe0701800, 0x80000502, // buffer_store_dword v5, v2, s[0:3], 0 offen offset:2048
    0x92fea188,             // s_bfm_b64 exec, 8, 33
    0x7e1c0503,             // v_readfirstlane_b32 s14, v3
    0xbefe04c1,             // s_mov_b64 exec, -1
    0x7e08020e,             // v_mov_b32_e32 v4, s14
    0xe0701900, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:2304
    0x7e0c0301,             // v_mov_b32_e32 v6, v1
    0x4a0c02fa, 0xcf011101, // v_add_nc_u32_dpp v6, v1, v1 row_shr:1 row_mask:0xc bank_mask:0xf
    0xe0701a00, 0x80000602, // buffer_store_dword v6, v2, s[0:3], 0 offen offset:2560
    0x4c1002bf,             // v_sub_nc_u32_e32 v8, 63, v1
    0x34101082,             // v_lshlrev_b32_e32 v8, 2, v8
    0xdacc0000, 0x09000308, // ds_bpermute_b32 v9, v8, v3
    0xe0701b00, 0x80000902, // buffer_store_dword v9, v2, s[0:3], 0 offen offset:2816
    0xbefe03ff, 0x0000ffff, // s_mov_b32 exec_lo, 0xffff
    0xbeff03ff, 0x0000ff00, // s_mov_b32 exec_hi, 0xff00
    0xd765000a, 0x0201007e, // v_mbcnt_lo_u32_b32 v10, exec_lo, 0
    0xd766000a, 0x0202147f, // v_mbcnt_hi_u32_b32 v10, exec_hi, v10
    0xe0701c00, 0x80000a02, // buffer_store_dword v10, v2, s[0:3], 0 offen offset:3072
    0xbefe04c1,             // s_mov_b64 exec, -1
    0xb06a0060,             // s_movk_i32 vcc_lo, 0x60
    0xbeeb0387,             // s_mov_b32 vcc_hi, 7
    0x800f6b6a,             // s_add_u32 s15, vcc_lo, vcc_hi
    0x7e08020f,             // v_mov_b32_e32 v4, s15
    0xe0701d00, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:3328
    0xbefc0380,             // s_mov_b32 m0, 0
    0x7e1602ff, 0x00000064, // v_mov_b32_e32 v11, 0x64
    0x7e1a0280,             // v_mov_b32_e32 v13, 0
    0xd8340000, 0x00000b0d, // ds_write_b32 v13, v11
    0xbefe03ff, 0x000003ff, // s_mov_b32 exec_lo, 0x3ff
    0xbeff03ff, 0x0ffc0000, // s_mov_b32 exec_hi, 0xffc0000
    0xd8f80000, 0x0c000000, // ds_append v12
    0xe0701e00, 0x80000c02, // buffer_store_dword v12, v2, s[0:3], 0 offen offset:3584
    0xbefe04c1,             // s_mov_b64 exec, -1
    0xb010002d,             // s_movk_i32 s16, 0x2d
    0xd7600011, 0x02002103, // v_readlane_b32 s17, v3, s16
    0x7e080211,             // v_mov_b32_e32 v4, s17
    0xe0701f00, 0x80000402, // buffer_store_dword v4, v2, s[0:3], 0 offen offset:3840
    0xbf810000,             // s_endpgm
};
// Program B: stores thread ID + 1000 * (lane 0's thread ID) at thread ID * 4. Run in groups that hold
// more than one console wave.
static const u32 k_waves_program[] = {
    0xbe8203c1,                         // s_mov_b32 s2, -1
    0xbe830380,                         // s_mov_b32 s3, 0
    0x34040082,                         // v_lshlrev_b32_e32 v2, 2, v0
    0xd7600004, 0x02010100,             // v_readlane_b32 s4, v0, 0
    0xd50b0003, 0x0201fe04, 0x000003e8, // v_mul_u32_u24_e64 v3, s4, 0x3e8
    0x4a060103,                         // v_add_nc_u32_e32 v3, v3, v0
    0xe0701000, 0x80000302,             // buffer_store_dword v3, v2, s[0:3], 0 offen
    0xbf810000,                         // s_endpgm
};

#define UNWRITTEN    0xDEADBEEFu // what the output buffer starts with
#define OUTPUT_BYTES 4096

// What program A stores in region `r` for lane `i`.
static u32 lanes_expected(u32 r, u32 i) {
    switch (r) {
    case 0: // v_mbcnt_lo/hi with -1: the lane
        return i;
    case 1: // v_readlane 63
        return 63 * 3;
    case 2: // v_readlane 32
        return 32 * 3;
    case 3: // v_readlane 63 while EXEC holds lane 0 only
        return 63 * 3;
    case 4: // s_bcnt1_i32_b64 of a compare true in lanes 0-39
        return 40;
    case 5: // s_ff1_i32_b64 of a compare true in lanes 37-63
        return 37;
    case 6: // EXEC = s_bfm_b64 32, 0: lanes 0-31
        return i < 32 ? 1000 + i : UNWRITTEN;
    case 7: // exec_lo = exec_hi = 17: lanes 0, 4, 32, 36
        return (i & 31) == 0 || (i & 31) == 4 ? i : UNWRITTEN;
    case 8: // v_writelane lane 40 with EXEC empty
        return i == 40 ? 63 * 3 : 0;
    case 9: // v_readfirstlane with EXEC = lanes 33-40
        return 33 * 3;
    case 10: // DPP row_shr:1 with row_mask 0xc: rows 2 and 3 only, the first lane of a row unwritten
        return i < 32 || (i & 15) == 0 ? i : i + (i - 1);
    case 11: // ds_bpermute from lane 63 - i
        return (63 - i) * 3;
    case 12: // v_mbcnt_lo/hi of EXEC = lanes 0-15 and 40-47
        return i < 16 ? i : i >= 40 && i < 48 ? 16 + (i - 40) : UNWRITTEN;
    case 13: // VCC as data: vcc_lo = 0x60, vcc_hi = 7, added
        return 0x67;
    case 14: // ds_append in lanes 0-9 and 50-59 on a counter at 100: the old value, in every lane
        return i < 10 || (i >= 50 && i < 60) ? 100 : UNWRITTEN;
    case 15: // v_readlane with the lane in an SGPR (45)
        return 45 * 3;
    default:
        return UNWRITTEN;
    }
}

// Program B, in groups of 100 threads (two console waves, the second with 36 lanes): thread t stores
// t + 1000 * (thread ID of lane 0 of its wave).
static u32 waves_expected(u32 t) {
    return t < 100 ? t + 1000 * (t & ~63u) : UNWRITTEN;
}

struct GpuBench {
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    ID3D12CommandAllocator* allocator;
    ID3D12GraphicsCommandList* list;
    ID3D12Fence* fence;
    u64 fence_value;
    ID3D12RootSignature* root;
    ID3D12Resource* output;   // UAV
    ID3D12Resource* upload;   // the initial contents
    ID3D12Resource* readback; // what the program stored
    IDxcCompiler3* compiler;
    ShaderTarget target;
};

static void release(IUnknown* object) {
    if (object)
        object->Release();
}

static ID3D12Resource* make_buffer(ID3D12Device* device, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                                   D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = OUTPUT_BYTES;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ID3D12Resource* r = nullptr;
    if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

// The device and what every run needs. False (and a reason) to skip.
static bool bench_init(GpuBench& b, const char** why) {
    memset(&b, 0, sizeof(b));
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&b.device)))) {
        *why = "no D3D12 device";
        return false;
    }
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_6};
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1 = {};
    if (FAILED(b.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
        sm.HighestShaderModel < D3D_SHADER_MODEL_6_6 ||
        FAILED(b.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1))) || !o1.WaveOps) {
        *why = "no shader model 6.6 with wave operations";
        return false;
    }
    b.target.wave_min = (u8)(o1.WaveLaneCountMin < 255 ? o1.WaveLaneCountMin : 255);
    b.target.wave_max = (u8)(o1.WaveLaneCountMax < 255 ? o1.WaveLaneCountMax : 255);
    if (b.target.wave_min > 32 || b.target.wave_max < 32) {
        *why = "no waves of 32 lanes";
        return false;
    }
    HMODULE dxc = LoadLibraryW(L"dxcompiler.dll");
    DxcCreateInstanceProc create =
        dxc ? (DxcCreateInstanceProc)GetProcAddress(dxc, "DxcCreateInstance") : nullptr;
    if (!create || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&b.compiler)))) {
        *why = "no dxcompiler.dll";
        return false;
    }
    // The translated programs' root signature: user data (root constants b0) and the root UAV u0, both
    // in space 100 (see shader/translate.h).
    D3D12_ROOT_PARAMETER1 params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.RegisterSpace = 100;
    params[0].Constants.Num32BitValues = 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].Descriptor.RegisterSpace = 100;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs = {};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rs.Desc_1_1.NumParameters = 2;
    rs.Desc_1_1.pParameters = params;
    ID3DBlob* blob = nullptr;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rs, &blob, nullptr)) ||
        FAILED(b.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&b.root)))) {
        release(blob);
        *why = "cannot create the root signature";
        return false;
    }
    release(blob);
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    b.output = make_buffer(b.device, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_COMMON);
    b.upload = make_buffer(b.device, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                           D3D12_RESOURCE_STATE_GENERIC_READ);
    b.readback = make_buffer(b.device, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                             D3D12_RESOURCE_STATE_COPY_DEST);
    if (!b.output || !b.upload || !b.readback ||
        FAILED(b.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&b.queue))) ||
        FAILED(
            b.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&b.allocator))) ||
        FAILED(b.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, b.allocator, nullptr,
                                           IID_PPV_ARGS(&b.list))) ||
        FAILED(b.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&b.fence)))) {
        *why = "cannot create D3D12 objects";
        return false;
    }
    b.list->Close();
    u32* p = nullptr;
    if (FAILED(b.upload->Map(0, nullptr, (void**)&p))) {
        *why = "cannot map the upload buffer";
        return false;
    }
    for (u32 i = 0; i < OUTPUT_BYTES / 4; i++)
        p[i] = UNWRITTEN;
    b.upload->Unmap(0, nullptr);
    return true;
}

static void bench_release(GpuBench& b) {
    release(b.compiler);
    release(b.readback);
    release(b.upload);
    release(b.output);
    release(b.fence);
    release(b.list);
    release(b.allocator);
    release(b.queue);
    release(b.root);
    release(b.device);
}

static void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER rb = {};
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = r;
    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    rb.Transition.StateBefore = before;
    rb.Transition.StateAfter = after;
    list->ResourceBarrier(1, &rb);
}

// Translates `code` with `info` and runs one group of it; `out` gets the output buffer. False if any
// step failed (reported).
static bool run_program(GpuBench& b, const u32* code, u32 size, const ShaderInfo& info, u32* out) {
    TranslatedShader t = {};
    if (!shader_translate(code, size, info, b.target, &t)) {
        printf("FAIL shader lanes: translation failed: %s\n", t.error);
        t.hlsl.release();
        return false;
    }
    DxcBuffer source = {t.hlsl.data, t.hlsl.length, DXC_CP_UTF8};
    const wchar_t* args[] = {L"-E", L"main", L"-T", L"cs_6_6", L"-O3", L"-HV", L"2021"};
    IDxcResult* result = nullptr;
    HRESULT status = E_FAIL;
    if (SUCCEEDED(b.compiler->Compile(&source, args, ARRAY_COUNT(args), nullptr, IID_PPV_ARGS(&result))))
        result->GetStatus(&status);
    IDxcBlob* object = nullptr;
    if (SUCCEEDED(status))
        result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr);
    if (!object) {
        IDxcBlobUtf8* errors = nullptr;
        if (result)
            result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
        printf("FAIL shader lanes: compiling failed: %.400s\n%s\n", errors ? errors->GetStringPointer() : "?",
               t.hlsl.data);
        release(errors);
        release(result);
        t.hlsl.release();
        return false;
    }
    t.hlsl.release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = b.root;
    pd.CS.pShaderBytecode = object->GetBufferPointer();
    pd.CS.BytecodeLength = object->GetBufferSize();
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = b.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso));
    release(object);
    release(result);
    if (FAILED(hr)) {
        printf("FAIL shader lanes: pipeline creation failed (0x%08lx)\n", (unsigned long)hr);
        return false;
    }
    b.allocator->Reset();
    b.list->Reset(b.allocator, pso);
    barrier(b.list, b.output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    b.list->CopyResource(b.output, b.upload);
    barrier(b.list, b.output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    b.list->SetComputeRootSignature(b.root);
    u32 user[4] = {}; // s[0:1]: the root UAV, at offset 0 without a stride
    b.list->SetComputeRoot32BitConstants(0, 4, user, 0);
    b.list->SetComputeRootUnorderedAccessView(1, b.output->GetGPUVirtualAddress());
    b.list->Dispatch(1, 1, 1);
    barrier(b.list, b.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    b.list->CopyResource(b.readback, b.output);
    barrier(b.list, b.output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    b.list->Close();
    ID3D12CommandList* lists[] = {b.list};
    b.queue->ExecuteCommandLists(1, lists);
    b.queue->Signal(b.fence, ++b.fence_value);
    HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    b.fence->SetEventOnCompletion(b.fence_value, done);
    WaitForSingleObject(done, 10000);
    CloseHandle(done);
    release(pso);
    if (b.fence->GetCompletedValue() < b.fence_value) {
        printf("FAIL shader lanes: the GPU did not finish\n");
        return false;
    }
    u32* p = nullptr;
    D3D12_RANGE range = {0, OUTPUT_BYTES};
    if (FAILED(b.readback->Map(0, &range, (void**)&p)))
        return false;
    memcpy(out, p, OUTPUT_BYTES);
    D3D12_RANGE none = {0, 0};
    b.readback->Unmap(0, &none);
    return true;
}

static ShaderInfo compute_info(u16 threads, u32 lds_bytes) {
    ShaderInfo info;
    memset(&info, 0, sizeof(info));
    info.stage = SHADER_COMPUTE;
    info.user_sgprs = 2;
    info.parameter_count = 1;
    info.parameters[0].kind = USER_DATA_ROOT_UAV;
    info.parameters[0].sgpr = 0;
    info.parameters[0].dwords = 2;
    info.parameters[0].root_register = 0;
    info.resource_heap_sgpr = NO_HEAP_SGPR;
    info.sampler_heap_sgpr = NO_HEAP_SGPR;
    info.threads[0] = threads;
    info.threads[1] = 1;
    info.threads[2] = 1;
    info.thread_id_count = 1;
    info.lds_bytes = lds_bytes;
    info.wave_size = 64;
    return info;
}

void test_shader_lanes() {
    rdna_init();
    GpuBench b;
    const char* why = nullptr;
    if (!bench_init(b, &why)) {
        printf("skipped shader lane tests: %s\n", why);
        bench_release(b);
        return;
    }
    static u32 out[OUTPUT_BYTES / 4];
    if (run_program(b, k_lanes_program, sizeof(k_lanes_program), compute_info(64, 1024), out)) {
        for (u32 r = 0; r < 16; r++)
            for (u32 i = 0; i < 64; i++) {
                u32 want = lanes_expected(r, i);
                if (out[r * 64 + i] != want) {
                    printf("FAIL shader lanes: region %u lane %u: 0x%x, expected 0x%x\n", r, i,
                           out[r * 64 + i], want);
                    g_failures++;
                    break; // one report per region
                }
            }
    } else {
        g_failures++;
    }
    if (run_program(b, k_waves_program, sizeof(k_waves_program), compute_info(100, 0), out)) {
        for (u32 t = 0; t < 128; t++)
            if (out[t] != waves_expected(t)) {
                printf("FAIL shader lanes: group thread %u: 0x%x, expected 0x%x\n", t, out[t],
                       waves_expected(t));
                g_failures++;
                break;
            }
    } else {
        g_failures++;
    }
    printf("shader lanes: 2 programs run on the GPU (waves of %u-%u lanes)\n", b.target.wave_min,
           b.target.wave_max);
    bench_release(b);
}
