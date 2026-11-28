# Toolchain for single-configuration generators (Ninja): clang-cl and lld-link from Visual Studio 18
# (2026), usable from a plain shell.
#
# Visual Studio is located with vswhere. The MSVC libraries and headers and the Windows SDK are passed
# to clang-cl and lld-link explicitly instead of through the INCLUDE / LIB variables of a developer
# prompt, so a build sees the same toolchain whatever shell it is started from, and never picks up
# another Visual Studio installation's environment.
#
# Overrides (cache variables): SLUDGE_VS_PATH (Visual Studio root), SLUDGE_WINSDK_DIR (Windows Kits
# root) and SLUDGE_WINSDK_VERSION (e.g. 10.0.26100.0).

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT SLUDGE_VS_PATH)
    set(vswhere "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/Installer/vswhere.exe")
    if(NOT EXISTS "${vswhere}")
        message(FATAL_ERROR "vswhere.exe not found at ${vswhere}; set SLUDGE_VS_PATH to the Visual Studio 18 root.")
    endif()
    execute_process(
        COMMAND "${vswhere}" -version "[18.0,19.0)" -products * -latest
            -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
        OUTPUT_VARIABLE vs_path OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT vs_path)
        message(FATAL_ERROR "No Visual Studio 18 (2026) with the C++ Clang tools found. Install the "
            "\"C++ Clang Compiler for Windows\" component, or set SLUDGE_VS_PATH.")
    endif()
    file(TO_CMAKE_PATH "${vs_path}" vs_path)
    set(SLUDGE_VS_PATH "${vs_path}" CACHE PATH "Visual Studio 18 installation root")
endif()

file(READ "${SLUDGE_VS_PATH}/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt" msvc_version)
string(STRIP "${msvc_version}" msvc_version)
set(msvc_dir "${SLUDGE_VS_PATH}/VC/Tools/MSVC/${msvc_version}")
set(llvm_bin "${SLUDGE_VS_PATH}/VC/Tools/Llvm/x64/bin")

if(NOT SLUDGE_WINSDK_DIR)
    cmake_host_system_information(RESULT kits_root QUERY WINDOWS_REGISTRY
        "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10 VIEW 64_32)
    if(NOT kits_root)
        message(FATAL_ERROR "Windows 10/11 SDK not found; set SLUDGE_WINSDK_DIR.")
    endif()
    file(TO_CMAKE_PATH "${kits_root}" kits_root)
    string(REGEX REPLACE "/$" "" kits_root "${kits_root}")
    set(SLUDGE_WINSDK_DIR "${kits_root}" CACHE PATH "Windows Kits 10 root")
endif()
if(NOT SLUDGE_WINSDK_VERSION)
    # The newest SDK that has both headers and libraries.
    file(GLOB sdk_dirs LIST_DIRECTORIES true "${SLUDGE_WINSDK_DIR}/Include/10.*")
    set(sdk_version "")
    foreach(dir IN LISTS sdk_dirs)
        get_filename_component(v "${dir}" NAME)
        if(EXISTS "${dir}/um/windows.h" AND EXISTS "${SLUDGE_WINSDK_DIR}/Lib/${v}/um/x64/kernel32.lib"
           AND v VERSION_GREATER sdk_version)
            set(sdk_version "${v}")
        endif()
    endforeach()
    if(NOT sdk_version)
        message(FATAL_ERROR "No usable Windows SDK under ${SLUDGE_WINSDK_DIR}; set SLUDGE_WINSDK_VERSION.")
    endif()
    set(SLUDGE_WINSDK_VERSION "${sdk_version}" CACHE STRING "Windows SDK version")
endif()

set(CMAKE_C_COMPILER "${llvm_bin}/clang-cl.exe")
set(CMAKE_CXX_COMPILER "${llvm_bin}/clang-cl.exe")
set(CMAKE_LINKER "${llvm_bin}/lld-link.exe")
set(CMAKE_AR "${llvm_bin}/llvm-lib.exe")
set(CMAKE_RC_COMPILER "${llvm_bin}/llvm-rc.exe")
set(CMAKE_MT "${llvm_bin}/llvm-mt.exe")

# llvm-rc does not search the SDK by itself.
set(CMAKE_RC_FLAGS_INIT "/I \"${msvc_dir}/include\" /I \"${SLUDGE_WINSDK_DIR}/Include/${SLUDGE_WINSDK_VERSION}/um\" /I \"${SLUDGE_WINSDK_DIR}/Include/${SLUDGE_WINSDK_VERSION}/shared\"")

set(sysroot_compile "/vctoolsdir \"${msvc_dir}\" /winsdkdir \"${SLUDGE_WINSDK_DIR}\" /winsdkversion ${SLUDGE_WINSDK_VERSION}")
set(sysroot_link "\"/vctoolsdir:${msvc_dir}\" \"/winsdkdir:${SLUDGE_WINSDK_DIR}\" /winsdkversion:${SLUDGE_WINSDK_VERSION}")
set(CMAKE_C_FLAGS_INIT "${sysroot_compile}")
set(CMAKE_CXX_FLAGS_INIT "${sysroot_compile}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${sysroot_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${sysroot_link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${sysroot_link}")

# try_compile projects run in their own scope: hand them the toolchain location too.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SLUDGE_VS_PATH SLUDGE_WINSDK_DIR SLUDGE_WINSDK_VERSION)
