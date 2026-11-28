cmake_minimum_required(VERSION 3.24)
project(sludgemu LANGUAGES C CXX)

# The title ABI is MSVC x64; sludgemu is built with clang-cl (Visual Studio 18 / 2026, see
# CMakePresets.json): it speaks that ABI and its flags below are what the code is written against.
if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
    message(FATAL_ERROR "sludgemu requires clang-cl (Visual Studio 18 2026 with the C++ Clang tools); "
        "use a preset from CMakePresets.json.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "sludgemu must be built for x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# We build without C++ exceptions; drop CMake's default /EHsc so it doesn't fight /EHs-c-.
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# Static CRT: our own code must not share CRT state with the title's (host) CRT DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Optimized builds of everything, third-party libraries included: ThinLTO (cross translation unit
# inlining at link time, in parallel and cached), full inlining (CMake's RelWithDebInfo default is
# /Ob1) and one section per function and variable so the linker can drop unreferenced and fold
# identical ones (/DEBUG turns that off unless asked for).
set(SLUDGE_LTO_JOBS "" CACHE STRING "ThinLTO backend threads at link time (empty: one per hardware thread)")
add_compile_options("$<$<NOT:$<CONFIG:Debug>>:-flto=thin;/Ob2;/Gy;/Gw>")
# /INCREMENTAL:NO: lld-link never links incrementally, and CMake's default /INCREMENTAL for
# RelWithDebInfo only earns a warning next to /OPT:REF.
add_link_options("$<$<NOT:$<CONFIG:Debug>>:/OPT:REF;/OPT:ICF;/INCREMENTAL:NO>")
add_link_options("$<$<NOT:$<CONFIG:Debug>>:/lldltocache:${CMAKE_BINARY_DIR}/lto_cache>")
if(SLUDGE_LTO_JOBS)
    add_link_options("$<$<NOT:$<CONFIG:Debug>>:/opt:lldltojobs=${SLUDGE_LTO_JOBS}>")
endif()

# Common compile options for our own code.
add_library(sludge_options INTERFACE)
target_compile_options(sludge_options INTERFACE
    /W4 /WX- /permissive- /utf-8 /Zi
    /GR-                    # no RTTI
    /EHs-c-                 # no C++ exceptions
    # The code reinterprets console descriptors, command packets and title memory through other
    # types; type-based alias analysis would be free to reorder those accesses. clang-cl already
    # defaults to this (as MSVC behaves), stated so that it never silently changes.
    -fno-strict-aliasing
    -Wno-unused-parameter   # stubs and fixed-signature callbacks have lots of these
    # Partial aggregate initialization (`MEMORYSTATUSEX m = {sizeof(m)};`) zero-fills the rest by
    # definition; it is the intended idiom for Win32 and D3D12 structures, not an oversight.
    -Wno-missing-field-initializers
    # COM-compatible objects declare their interface's methods as virtual directly in the final class
    # that implements them: the virtual methods are the vtable ABI, not an invitation to override.
    -Wno-unnecessary-virtual-specifier
)
target_compile_definitions(sludge_options INTERFACE
    _HAS_EXCEPTIONS=0
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)
target_link_options(sludge_options INTERFACE /DEBUG:FULL)

# Third-party libraries are fetched at configure time; nothing is vendored into the repo.
include(FetchContent)

# libdeflate: fast zlib/deflate decompression (console DirectStorage requests).
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_COMPRESSION_SUPPORT OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_GZIP_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libdeflate
    GIT_REPOSITORY https://github.com/ebiggers/libdeflate.git
    GIT_TAG v1.26
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(libdeflate)
target_compile_options(libdeflate_static PRIVATE /w) # third-party: not our warnings to fix

# libopus: Opus decoding for the console audio decoder (XApu). It picks its C runtime itself.
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(opus)
target_compile_options(opus PRIVATE /w) # third-party: not our warnings to fix
# opus gives its SSE4.1 sources no target flag when the compiler is MSVC-like, as cl.exe accepts the
# intrinsics without one; clang-cl needs -msse4.1 on exactly those files (opus picks them at run time
# by CPU feature, so the rest of the library must stay baseline).
get_target_property(opus_sse41_sources opus SOURCES)
list(FILTER opus_sse41_sources INCLUDE REGEX "_sse4_1[.]c$")
list(TRANSFORM opus_sse41_sources PREPEND "${opus_SOURCE_DIR}/")
set_source_files_properties(${opus_sse41_sources} TARGET_DIRECTORY opus PROPERTIES COMPILE_OPTIONS -msse4.1)

include(cmake/addrlib.cmake)

include(cmake/nuget.cmake)

# DirectStorage (desktop): backs the console DirectStorage API. Redistributable DLLs are copied
# next to sludgemu.exe.
sludge_fetch_nuget(dstorage_pkg Microsoft.Direct3D.DirectStorage 1.3.0
    8ab6c1082537565388b79050d94fdd7f5f437cd0f1d909e7b19f786042b97177)
add_library(dstorage SHARED IMPORTED GLOBAL)
set_target_properties(dstorage PROPERTIES
    IMPORTED_IMPLIB ${dstorage_pkg_SOURCE_DIR}/native/lib/x64/dstorage.lib
    IMPORTED_LOCATION ${dstorage_pkg_SOURCE_DIR}/native/bin/x64/dstorage.dll
    INTERFACE_INCLUDE_DIRECTORIES ${dstorage_pkg_SOURCE_DIR}/native/include)
set(SLUDGE_DSTORAGE_DLLS
    ${dstorage_pkg_SOURCE_DIR}/native/bin/x64/dstorage.dll
    ${dstorage_pkg_SOURCE_DIR}/native/bin/x64/dstoragecore.dll)

# D3D12 Agility SDK: the desktop D3D12 runtime we translate to, independent of the OS build.
# D3D12Core.dll (and the debug layer) go to bin/<config>/D3D12/, found through the
# D3D12SDKVersion / D3D12SDKPath exports of sludgemu.exe. Its headers take precedence over the
# Windows SDK's.
sludge_fetch_nuget(agility_pkg Microsoft.Direct3D.D3D12 1.620.2
    70489669c2bbeaf318b62d72b0ac3e753d521387bfc848c9763791c4e920bfcc)
add_library(agility INTERFACE)
target_include_directories(agility BEFORE INTERFACE ${agility_pkg_SOURCE_DIR}/build/native/include)
set(SLUDGE_AGILITY_DLLS
    ${agility_pkg_SOURCE_DIR}/build/native/bin/x64/D3D12Core.dll
    ${agility_pkg_SOURCE_DIR}/build/native/bin/x64/d3d12SDKLayers.dll)

# DirectX Shader Compiler: compiles our own shaders at build time (dxc.exe) and translated title
# shaders at run time (dxcompiler.dll, with dxil.dll for validation and signing, copied next to
# sludgemu.exe).
sludge_fetch_nuget(dxc_pkg Microsoft.Direct3D.DXC 1.9.2609.5
    73cfa082245918c2cc1ec6740250eb7377e83a757b3b3a3e16e0d71535bfbfc3)
set(SLUDGE_DXC_EXE ${dxc_pkg_SOURCE_DIR}/build/native/bin/x64/dxc.exe)
add_library(dxc INTERFACE)
target_include_directories(dxc INTERFACE ${dxc_pkg_SOURCE_DIR}/build/native/include)
set(SLUDGE_DXC_DLLS
    ${dxc_pkg_SOURCE_DIR}/build/native/bin/x64/dxcompiler.dll
    ${dxc_pkg_SOURCE_DIR}/build/native/bin/x64/dxil.dll)

add_subdirectory(src)
add_subdirectory(tools)

enable_testing()
add_subdirectory(tests)
