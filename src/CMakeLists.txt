# Console shader decoding and translation: platform independent, shared by sludgemu, the unit tests
# and the shader tool.
set(SLUDGE_SHADER_SOURCES
    shader/rdna.h
    shader/rdna_ops.inc
    shader/rdna_decode.cpp
    shader/rdna_disasm.cpp
    shader/text.h
    shader/text.cpp
    shader/translate.h
    shader/translate_internal.h
    shader/translate.cpp
    shader/translate_emit.cpp
    shader/translate_flow.cpp
    shader/translate_lanes.cpp
    shader/translate_alu.cpp
    shader/translate_memory.cpp
    shader/console_pipeline.h
    shader/console_pipeline.cpp
    shader/console_container.h
    shader/console_container.cpp
)
add_library(sludge_shader STATIC ${SLUDGE_SHADER_SOURCES})
target_include_directories(sludge_shader PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sludge_shader PRIVATE sludge_options libdeflate::libdeflate_static)
target_compile_options(sludge_shader PRIVATE /Zc:threadSafeInit-) # linked into sludgemu.exe (see below)

add_executable(sludgemu WIN32
    main.cpp

    common/types.h
    common/log.h
    common/log.cpp
    common/str.h
    common/str.cpp
    common/thread_slot.h

    core/title.h
    core/title.cpp
    core/memory.h
    core/memory.cpp
    core/memory_tracking.cpp
    core/vfs.h
    core/vfs.cpp
    core/console_profile.h
    core/console_profile.cpp
    core/cpu_compat.h
    core/cpu_compat.cpp

    loader/pe_image.h
    loader/pe_image.cpp
    loader/loader.h
    loader/loader.cpp
    loader/tls_reserve.h
    loader/tls_reserve.cpp
    loader/stubs.h
    loader/stubs.cpp
    loader/crash.h
    loader/crash.cpp

    hle/hle.h
    hle/hle.cpp
    hle/xbox_modules.cpp
    hle/audio/apu.h
    hle/audio/acphal.cpp
    hle/audio/xma.cpp
    hle/audio/xma_decoder.h
    hle/audio/xma_decoder.cpp
    hle/audio/xma_tables.h
    hle/audio/xapu.cpp
    hle/audio/xdsp.cpp
    hle/kernel/overrides.h
    hle/kernel/module_overrides.cpp
    hle/kernel/memory_overrides.cpp
    hle/kernel/cpu_overrides.cpp
    hle/kernel/file_overrides.cpp
    hle/kernel/registry_overrides.cpp
    hle/xmem/xmem.h
    hle/xmem/xmem.cpp
    hle/xmem/xmem_defs.h
    hle/d3d12/d3d12_xs.cpp
    hle/d3d12/d3d12_tables.h
    hle/d3d12/d3d12_tables.cpp
    hle/d3d12/console_iids.h
    hle/d3d12/console_object.h
    hle/d3d12/console_object.cpp
    hle/d3d12/device.h
    hle/d3d12/device.cpp
    hle/d3d12/d3d12_objects.h
    hle/d3d12/command_queue.cpp
    hle/d3d12/fence.cpp
    hle/d3d12/resource.cpp
    hle/d3d12/descriptor_heap.cpp
    hle/d3d12/descriptor_shadow.h
    hle/d3d12/descriptor_shadow.cpp
    hle/d3d12/query_heap.cpp
    hle/d3d12/pipeline_state.cpp
    hle/d3d12/command_signature.cpp
    hle/d3d12/root_signature.h
    hle/d3d12/root_signature.cpp
    hle/d3d12/page_pool.cpp
    hle/d3d12/texture_sync.h
    hle/d3d12/texture_sync.cpp
    hle/d3d12/views.h
    hle/d3d12/views.cpp
    hle/d3d12/command_allocator.cpp
    hle/d3d12/command_list.h
    hle/d3d12/command_list.cpp
    hle/d3d12/command_list_work.cpp
    hle/d3d12/barriers.h
    hle/d3d12/barriers.cpp
    hle/d3d12/frame_pacing.h
    hle/d3d12/frame_pacing.cpp
    hle/d3d12/bvh_builder.cpp
    hle/dstorage/bcpack.cpp
    hle/dstorage/dstorage.cpp

    gpu/agility.cpp
    gpu/presenter.h
    gpu/presenter.cpp
    gpu/gpu.h
    gpu/gpu.cpp
    gpu/gpu_ring.h
    gpu/gpu_ring.cpp
    gpu/texture_untile.h
    gpu/texture_untile.cpp
    gpu/gpu_window.h
    gpu/gpu_window.cpp
    gpu/tile_pool.h
    gpu/tile_pool.cpp
    gpu/page_tables.h
    gpu/page_tables.cpp
    gpu/shader_programs.h
    gpu/shader_programs.cpp
    gpu/shader_cache.h
    gpu/shader_cache.cpp
    gpu/shader_schedule.h
    gpu/shader_schedule.cpp
    gpu/formats.h
    gpu/formats.cpp
    gpu/console_layout.h
    gpu/console_layout.cpp
    hle/xg/xg_xs.cpp
    hle/xgameruntime/runtime_api.h
    hle/xgameruntime/xgameruntime.h
    hle/xgameruntime/xgameruntime.cpp
    hle/xgameruntime/task_queue.h
    hle/xgameruntime/task_queue.cpp
    hle/xgameruntime/async.h
    hle/xgameruntime/async.cpp
    hle/xgameruntime/system.cpp
    hle/xgameruntime/user.cpp
    hle/xgameruntime/game_save.cpp
    hle/xgameruntime/package.cpp
    hle/xgameruntime/store.cpp
    hle/xgameruntime/services.cpp

    platform/window.h
    platform/window.cpp
)
target_include_directories(sludgemu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Our own shaders, compiled at build time into headers holding the bytecode as `k_<name>`.
function(sludge_shader target hlsl entry profile name)
    set(out ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.h)
    add_custom_command(OUTPUT ${out}
        COMMAND ${SLUDGE_DXC_EXE} -nologo -O3 -T ${profile} -E ${entry} -Vn k_${name} -Fh ${out}
            ${CMAKE_CURRENT_SOURCE_DIR}/${hlsl}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${hlsl}
        COMMENT "Compiling ${hlsl} (${entry})")
    target_sources(${target} PRIVATE ${out})
endfunction()
sludge_shader(sludgemu gpu/shaders/present.hlsl vs_main vs_6_0 present_vs)
sludge_shader(sludgemu gpu/shaders/present.hlsl ps_main ps_6_0 present_ps)
sludge_shader(sludgemu gpu/shaders/untile.hlsl cs_main cs_6_0 untile_cs)
target_include_directories(sludgemu PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# The shader cache generation: a hash of every source that can change what a translated program comes
# out as (or how the cache stores it). Derived from the sources rather than bumped by hand, so that no
# edit can leave stale translations in use: an unneeded rebuild of the cache is the worst case.
set(SLUDGE_TRANSLATOR_SOURCES ${SLUDGE_SHADER_SOURCES}
    gpu/shader_programs.h gpu/shader_programs.cpp gpu/shader_cache.h gpu/shader_cache.cpp)
list(TRANSFORM SLUDGE_TRANSLATOR_SOURCES PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/)
set(translator_hash ${CMAKE_CURRENT_BINARY_DIR}/generated/shader_translator_hash.h)
add_custom_command(OUTPUT ${translator_hash}
    COMMAND ${CMAKE_COMMAND} -DNAME=SHADER_TRANSLATOR_HASH -DOUT=${translator_hash}
        "-DSOURCES=${SLUDGE_TRANSLATOR_SOURCES}" -P ${PROJECT_SOURCE_DIR}/cmake/source_hash.cmake
    DEPENDS ${SLUDGE_TRANSLATOR_SOURCES} ${PROJECT_SOURCE_DIR}/cmake/source_hash.cmake
    COMMENT "Hashing the shader translator sources")
target_sources(sludgemu PRIVATE ${translator_hash})
target_link_libraries(sludgemu PRIVATE sludge_shader agility sludge_options shell32 user32 synchronization delayimp
    dstorage libdeflate::libdeflate_static opus addrlib d3d12 dxgi dxc)

# Loaded only when a title uses DirectStorage.
target_link_options(sludgemu PRIVATE /DELAYLOAD:dstorage.dll)
add_custom_command(TARGET sludgemu POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SLUDGE_DSTORAGE_DLLS} $<TARGET_FILE_DIR:sludgemu>
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SLUDGE_DXC_DLLS} $<TARGET_FILE_DIR:sludgemu>
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:sludgemu>/D3D12
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SLUDGE_AGILITY_DLLS} $<TARGET_FILE_DIR:sludgemu>/D3D12)

# sludgemu.exe's implicit TLS slot (index 0) belongs to the title exe (see loader/tls_reserve.h),
# so nothing else may use implicit TLS: no thread_local in our code, and no thread-safe static
# initialization (the CRT implements it with a thread_local epoch counter).
target_compile_options(sludgemu PRIVATE /Zc:threadSafeInit-)

# Keep our own image far away from where titles want to live (0x140000000 and up), so the title
# can be mapped at its preferred base and addresses match the disassembler.
target_link_options(sludgemu PRIVATE /BASE:0x7FF600000000 /DYNAMICBASE /HIGHENTROPYVA /LARGEADDRESSAWARE
    /INCREMENTAL:NO) # incremental linking pads section contributions, including the TLS reserve
