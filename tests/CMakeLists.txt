# Unit tests for platform-independent parts of sludgemu. Run with ctest or directly.
add_executable(sludgemu_tests
    test.h
    memory_test.cpp
    audio_test.cpp
    dsp_test.cpp
    xma_test.cpp
    layout_test.cpp
    bcpack_test.cpp
    shader_translate_test.cpp
    shader_lanes_test.cpp
    shader_cache_test.cpp
    shader_schedule_test.cpp
    ${PROJECT_SOURCE_DIR}/src/common/log.cpp
    ${PROJECT_SOURCE_DIR}/src/core/memory.cpp
    ${PROJECT_SOURCE_DIR}/src/core/memory_tracking.cpp
    ${PROJECT_SOURCE_DIR}/src/gpu/console_layout.cpp
    ${PROJECT_SOURCE_DIR}/src/gpu/formats.cpp
    ${PROJECT_SOURCE_DIR}/src/gpu/shader_cache.cpp
    ${PROJECT_SOURCE_DIR}/src/gpu/shader_schedule.cpp
    ${PROJECT_SOURCE_DIR}/src/hle/dstorage/bcpack.cpp
    ${PROJECT_SOURCE_DIR}/src/hle/d3d12/descriptor_shadow.cpp
    ${PROJECT_SOURCE_DIR}/src/hle/audio/xapu.cpp
    ${PROJECT_SOURCE_DIR}/src/hle/audio/xdsp.cpp
    ${PROJECT_SOURCE_DIR}/src/hle/audio/xma.cpp
    ${PROJECT_SOURCE_DIR}/src/hle/audio/xma_decoder.cpp
    ${PROJECT_SOURCE_DIR}/src/gpu/agility.cpp
)
target_include_directories(sludgemu_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(sludgemu_tests PRIVATE sludge_options sludge_shader opus addrlib agility dxc d3d12)
add_test(NAME memory COMMAND sludgemu_tests)
