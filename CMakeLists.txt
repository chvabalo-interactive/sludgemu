cmake_minimum_required(VERSION 3.24)
project(sludgemu LANGUAGES C CXX)

if(NOT MSVC)
    message(FATAL_ERROR "sludgemu currently requires MSVC (the title ABI is MSVC x64).")
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

# Common compile options for everything we build.
add_library(sludge_options INTERFACE)
target_compile_options(sludge_options INTERFACE
    /W4 /WX- /permissive- /utf-8 /Zi /Zc:__cplusplus /Zc:preprocessor
    /GR-          # no RTTI
    /EHs-c-       # no C++ exceptions
    /wd4100       # unreferenced parameter (stubs have lots of these)
    /wd4201       # nameless struct/union
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

# Optimized builds: full inlining (CMake's RelWithDebInfo default is /Ob1), whole-program optimization
# across translation units, and dropping unreferenced and identical code (/DEBUG turns that off unless
# asked for).
target_compile_options(sludge_options INTERFACE "$<$<NOT:$<CONFIG:Debug>>:/Ob2;/Gw>")
target_link_options(sludge_options INTERFACE "$<$<NOT:$<CONFIG:Debug>>:/OPT:REF;/OPT:ICF>")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)

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
