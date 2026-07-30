cmake_minimum_required(VERSION 3.20)
project(lvt VERSION 0.1.8 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Options ---------------------------------------------------------------
# Framework support is per-provider so that consumers (and package managers)
# can drop the pieces they can't build. Win32 and ComCtl are always present:
# they are the base layer every other provider enriches.
#
# XAML and WinUI 3 are pure C++ and need no .NET SDK. WPF, WinForms and
# Avalonia have a native half that always builds, plus a managed helper
# assembly gated behind LVT_BUILD_MANAGED, which is the only thing here that
# shells out to `dotnet`.
option(LVT_ENABLE_XAML     "System XAML (UWP / DesktopWindowXamlSource) support" ON)
option(LVT_ENABLE_WINUI3   "WinUI 3 (Windows App SDK) support"                   ON)
option(LVT_ENABLE_WPF      "WPF support"                                         ON)
option(LVT_ENABLE_WINFORMS "WinForms support"                                    ON)
option(LVT_ENABLE_AVALONIA "Avalonia plugin"                                     ON)
option(LVT_ENABLE_CHROMIUM "Chromium (Chrome/Edge DOM) plugin"                   ON)

option(LVT_BUILD_TOOL    "Build the lvt command-line tool"                     ON)
option(LVT_BUILD_MANAGED "Build managed .NET assemblies (requires .NET SDK)"   ON)
option(LVT_BUILD_TESTS   "Build lvt's tests"                                   ON)

if(LVT_BUILD_MANAGED AND NOT (LVT_ENABLE_WPF OR LVT_ENABLE_WINFORMS OR LVT_ENABLE_AVALONIA))
    message(STATUS "lvt: no managed-backed framework enabled; forcing LVT_BUILD_MANAGED=OFF")
    set(LVT_BUILD_MANAGED OFF)
endif()

set(LVT_WITH_MANAGED ${LVT_BUILD_MANAGED})
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/lvt_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/lvt/lvt_config.h"
    @ONLY
)

# The TAP DLLs and plugins must sit next to the consuming binary at runtime.
# Keep this independent of the lvt target so they still build with LVT_BUILD_TOOL=OFF.
set(LVT_RUNTIME_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")

find_package(wil CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# Public headers, installed to include/lvt/. They include each other with quoted
# includes, which resolve relative to the including file, so the flat layout in
# src/ and the installed lvt/ layout both work without touching sources.
set(LVT_PUBLIC_HEADERS
    src/element.h
    src/element_key.h
    src/framework_detector.h
    src/json_serializer.h
    src/module_util.h
    src/plugin.h
    src/plugin_loader.h
    src/screenshot.h
    src/target.h
    src/tree_builder.h
    src/watch_diff.h
    "${CMAKE_CURRENT_BINARY_DIR}/include/lvt/lvt_config.h"
)

# --- lvt_core: the reusable library behind the CLI ---
set(LVT_CORE_SOURCES
    src/target.cpp
    src/framework_detector.cpp
    src/tree_builder.cpp
    src/element_key.cpp
    src/json_serializer.cpp
    src/watch_diff.cpp
    src/screenshot.cpp
    src/module_util.cpp
    src/plugin_loader.cpp
    src/providers/win32_provider.cpp
    src/providers/comctl_provider.cpp
)

# xaml_diag_common backs both the XAML and WinUI 3 providers.
if(LVT_ENABLE_XAML OR LVT_ENABLE_WINUI3)
    list(APPEND LVT_CORE_SOURCES src/providers/xaml_diag_common.cpp)
endif()
if(LVT_ENABLE_XAML)
    list(APPEND LVT_CORE_SOURCES src/providers/xaml_provider.cpp)
endif()
if(LVT_ENABLE_WINUI3)
    list(APPEND LVT_CORE_SOURCES src/providers/winui3_provider.cpp)
endif()
if(LVT_ENABLE_WPF)
    list(APPEND LVT_CORE_SOURCES src/providers/wpf_provider.cpp src/providers/wpf_inject.cpp)
endif()
if(LVT_ENABLE_WINFORMS)
    list(APPEND LVT_CORE_SOURCES src/providers/winforms_provider.cpp src/providers/winforms_inject.cpp)
endif()

add_library(lvt_core STATIC ${LVT_CORE_SOURCES})
add_library(lvt::core ALIAS lvt_core)
# Keep the exported name identical to the build-tree alias, so add_subdirectory()
# and find_package() consumers both spell it lvt::core.
set_target_properties(lvt_core PROPERTIES EXPORT_NAME core)

target_include_directories(lvt_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include/lvt>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
    $<INSTALL_INTERFACE:include/lvt>
)

target_link_libraries(lvt_core PUBLIC
    dwmapi
    d3d11
    dxgi
    windowsapp
    windowscodecs
)
target_link_libraries(lvt_core PRIVATE
    WIL::WIL
    nlohmann_json::nlohmann_json
)

target_compile_definitions(lvt_core PUBLIC
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINRT_LEAN_AND_MEAN
)

if(LVT_BUILD_TOOL)
    add_executable(lvt src/main.cpp)
    target_link_libraries(lvt PRIVATE lvt_core WIL::WIL nlohmann_json::nlohmann_json)
endif()

# Determine TAP DLL architecture suffix from vcpkg triplet
if(VCPKG_TARGET_TRIPLET MATCHES "arm64")
    set(TAP_ARCH_SUFFIX "arm64")
elseif(VCPKG_TARGET_TRIPLET MATCHES "x86")
    set(TAP_ARCH_SUFFIX "x86")
else()
    set(TAP_ARCH_SUFFIX "x64")
endif()

# TAP DLL — injected into target process for XAML diagnostics
# Uses static CRT (/MT) to avoid CRT version conflicts in the target process

# Generate C++/WinRT projected headers from the Windows App SDK winmd.
# These are needed for TransformToVisual / TextBlock.Text in the TAP DLL.
set(WINUI3_CPPWINRT_DIR "${CMAKE_SOURCE_DIR}/src/tap/winui3")
set(WINUI3_CPPWINRT_STAMP "${WINUI3_CPPWINRT_DIR}/winrt/Microsoft.UI.Xaml.h")
if(LVT_ENABLE_WINUI3 AND NOT EXISTS "${WINUI3_CPPWINRT_STAMP}")
    # Find cppwinrt.exe from the *active* Windows SDK. The generated projection
    # embeds the cppwinrt version and is checked (via static_assert) against the
    # <winrt/base.h> on the compiler's INCLUDE path. Hardcoding an SDK version
    # breaks on machines whose active/newest SDK differs from that version
    # ("Mismatched C++/WinRT headers"), so prefer the SDK selected by the current
    # developer command prompt and fall back to the newest installed SDK.
    set(_cppwinrt_hints "")
    # 1. Exact versioned bin dir of the active SDK (set by the VS dev prompt).
    if(DEFINED ENV{WindowsSdkVerBinPath})
        list(APPEND _cppwinrt_hints "$ENV{WindowsSdkVerBinPath}/x64")
    endif()
    # 2. Compose from WindowsSdkDir + WindowsSDKVersion (may have a trailing slash).
    if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
        string(REGEX REPLACE "[\\/]+$" "" _lvt_sdk_ver "$ENV{WindowsSDKVersion}")
        list(APPEND _cppwinrt_hints "$ENV{WindowsSdkDir}/bin/${_lvt_sdk_ver}/x64")
    endif()
    # 3. Fall back to the newest installed Windows 10/11 SDK bin dir.
    file(GLOB _lvt_sdk_bin_dirs
        "$ENV{WindowsSdkDir}/bin/10.*"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.*")
    list(SORT _lvt_sdk_bin_dirs ORDER DESCENDING)
    foreach(_lvt_bin ${_lvt_sdk_bin_dirs})
        if(IS_DIRECTORY "${_lvt_bin}")
            list(APPEND _cppwinrt_hints "${_lvt_bin}/x64")
        endif()
    endforeach()
    find_program(CPPWINRT_EXE cppwinrt HINTS ${_cppwinrt_hints})
    if(NOT CPPWINRT_EXE)
        # Fallback: search PATH
        find_program(CPPWINRT_EXE cppwinrt)
    endif()

    # Find Windows App SDK NuGet package (winmd files).
    # Search NuGet global cache and local packages/ dir (CI installs here).
    set(WASDK_WINMD_DIR "")
    file(GLOB _wasdk_candidates
        "$ENV{USERPROFILE}/.nuget/packages/microsoft.windowsappsdk/*/lib/uap10.0"
        "$ENV{NUGET_PACKAGES}/microsoft.windowsappsdk/*/lib/uap10.0"
        "${CMAKE_SOURCE_DIR}/packages/Microsoft.WindowsAppSDK.*/lib/uap10.0"
    )
    foreach(_candidate ${_wasdk_candidates})
        if(EXISTS "${_candidate}/Microsoft.UI.Xaml.winmd")
            set(WASDK_WINMD_DIR "${_candidate}")
            break()
        endif()
    endforeach()

    # Find Windows.winmd for system type references
    set(WIN_UNION_WINMD "")
    if(DEFINED ENV{WindowsSdkDir})
        file(GLOB _union "$ENV{WindowsSdkDir}/UnionMetadata/10.*/Windows.winmd")
        if(_union)
            list(SORT _union ORDER DESCENDING)
            list(GET _union 0 WIN_UNION_WINMD)
        endif()
    endif()
    if(NOT WIN_UNION_WINMD)
        file(GLOB _union "C:/Program Files (x86)/Windows Kits/10/UnionMetadata/10.*/Windows.winmd")
        if(_union)
            list(SORT _union ORDER DESCENDING)
            list(GET _union 0 WIN_UNION_WINMD)
        endif()
    endif()

    if(CPPWINRT_EXE AND WASDK_WINMD_DIR AND WIN_UNION_WINMD)
        message(STATUS "Generating WinUI3 C++/WinRT headers from ${WASDK_WINMD_DIR}")
        file(GLOB WASDK_ALL_WINMDS "${WASDK_WINMD_DIR}/*.winmd")
        # Also include other winmd directories in the package
        get_filename_component(_wasdk_root "${WASDK_WINMD_DIR}/../.." ABSOLUTE)
        file(GLOB_RECURSE _extra_winmds "${_wasdk_root}/*.winmd")
        list(APPEND WASDK_ALL_WINMDS ${_extra_winmds})
        list(REMOVE_DUPLICATES WASDK_ALL_WINMDS)
        execute_process(
            COMMAND "${CPPWINRT_EXE}" -in ${WASDK_ALL_WINMDS} -ref "${WIN_UNION_WINMD}" -out "${WINUI3_CPPWINRT_DIR}"
            RESULT_VARIABLE _cppwinrt_result
        )
        if(_cppwinrt_result EQUAL 0)
            message(STATUS "WinUI3 C++/WinRT headers generated in ${WINUI3_CPPWINRT_DIR}")
        else()
            message(WARNING "cppwinrt failed (${_cppwinrt_result}); TAP DLL WinUI3 features will be limited")
        endif()
    else()
        message(WARNING "Cannot generate WinUI3 C++/WinRT headers:"
            " cppwinrt=${CPPWINRT_EXE} winmd_dir=${WASDK_WINMD_DIR} union=${WIN_UNION_WINMD}")
    endif()
endif()

# The XAML/WinUI 3 TAP DLL is pure C++ (no .NET SDK needed); it only wants the
# Windows App SDK winmd projection above for the WinUI 3 specific enrichment.
if(LVT_ENABLE_XAML OR LVT_ENABLE_WINUI3)
add_library(lvt_tap SHARED
    src/tap/lvt_tap.cpp
    src/tap/lvt_tap.def
)
target_compile_definitions(lvt_tap PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX WINRT_LEAN_AND_MEAN)
target_include_directories(lvt_tap PRIVATE src src/tap/winui3)
target_link_libraries(lvt_tap PRIVATE WIL::WIL ole32)
set_property(TARGET lvt_tap PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Place lvt_tap_{arch}.dll next to lvt.exe
set_target_properties(lvt_tap PROPERTIES
    OUTPUT_NAME "lvt_tap_${TAP_ARCH_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY ${LVT_RUNTIME_OUTPUT_DIR}
)
endif()

# WPF TAP DLL — native loader injected into WPF target process
# Hosts CLR and calls managed WpfTreeWalker assembly.
# The native half needs no .NET SDK; only the managed assembly below does.
if(LVT_ENABLE_WPF)
add_library(lvt_wpf_tap SHARED
    src/tap_wpf/wpf_tap_native.cpp
    src/tap_wpf/wpf_tap_native.def
)
target_compile_definitions(lvt_wpf_tap PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(lvt_wpf_tap PRIVATE src)
target_link_libraries(lvt_wpf_tap PRIVATE WIL::WIL ole32)
set_property(TARGET lvt_wpf_tap PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set_target_properties(lvt_wpf_tap PROPERTIES
    OUTPUT_NAME "lvt_wpf_tap_${TAP_ARCH_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY ${LVT_RUNTIME_OUTPUT_DIR}
)

# Build managed WPF tree walker assembly and copy to output
# Pre-build with: dotnet build src/tap_wpf/LvtWpfTap.csproj -c Release
# The managed DLL is AnyCPU so the same binary works for all architectures.
if(LVT_BUILD_MANAGED)
add_custom_command(TARGET lvt_wpf_tap POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/tap_wpf/bin/Release/LvtWpfTap.dll"
        "${LVT_RUNTIME_OUTPUT_DIR}/LvtWpfTap.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/tap_wpf/LvtWpfTap.runtimeconfig.json"
        "${LVT_RUNTIME_OUTPUT_DIR}/LvtWpfTap.runtimeconfig.json"
    COMMENT "Copying managed WPF tree walker assembly"
)
endif()
endif()

# WinForms TAP DLL — native loader injected into WinForms target process
if(LVT_ENABLE_WINFORMS)
add_library(lvt_winforms_tap SHARED
    src/tap_winforms/winforms_tap_native.cpp
    src/tap_winforms/winforms_tap_native.def
)
target_compile_definitions(lvt_winforms_tap PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(lvt_winforms_tap PRIVATE src)
target_link_libraries(lvt_winforms_tap PRIVATE WIL::WIL ole32)
set_property(TARGET lvt_winforms_tap PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set_target_properties(lvt_winforms_tap PROPERTIES
    OUTPUT_NAME "lvt_winforms_tap_${TAP_ARCH_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY ${LVT_RUNTIME_OUTPUT_DIR}
)

if(LVT_BUILD_MANAGED)
set(LVT_WINFORMS_MANAGED_DLL "${CMAKE_SOURCE_DIR}/src/tap_winforms/bin/Release/LvtWinFormsTap.dll")
add_custom_command(
    OUTPUT "${LVT_WINFORMS_MANAGED_DLL}"
    COMMAND dotnet build "${CMAKE_SOURCE_DIR}/src/tap_winforms/LvtWinFormsTap.csproj" -c Release --nologo
    DEPENDS
        "${CMAKE_SOURCE_DIR}/src/tap_winforms/LvtWinFormsTap.csproj"
        "${CMAKE_SOURCE_DIR}/src/tap_winforms/WinFormsTreeWalker.cs"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Building managed WinForms tree walker assembly"
    VERBATIM
)
add_custom_target(lvt_winforms_managed ALL
    DEPENDS "${LVT_WINFORMS_MANAGED_DLL}"
)
add_dependencies(lvt_winforms_tap lvt_winforms_managed)
add_custom_command(TARGET lvt_winforms_tap POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${LVT_WINFORMS_MANAGED_DLL}"
        "${LVT_RUNTIME_OUTPUT_DIR}/LvtWinFormsTap.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/tap_winforms/LvtWinFormsTap.runtimeconfig.json"
        "${LVT_RUNTIME_OUTPUT_DIR}/LvtWinFormsTap.runtimeconfig.json"
    COMMENT "Copying managed WinForms tree walker assembly"
)
endif()
endif()

# Avalonia plugin DLL — runtime-loaded plugin for Avalonia UI framework support
if(LVT_ENABLE_AVALONIA)
add_library(lvt_avalonia_plugin SHARED
    src/plugin_avalonia/lvt_avalonia_plugin.cpp
)
target_compile_definitions(lvt_avalonia_plugin PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(lvt_avalonia_plugin PRIVATE src)
target_link_libraries(lvt_avalonia_plugin PRIVATE ole32 version)
set_target_properties(lvt_avalonia_plugin PROPERTIES
    OUTPUT_NAME "lvt_avalonia_plugin"
    RUNTIME_OUTPUT_DIRECTORY ${LVT_RUNTIME_OUTPUT_DIR}/plugins
)

# Avalonia TAP DLL — injected into Avalonia target process
# Uses static CRT (/MT) to avoid CRT version conflicts in the target process
add_library(lvt_avalonia_tap SHARED
    src/plugin_avalonia/avalonia_tap_native.cpp
    src/plugin_avalonia/avalonia_tap_native.def
)
target_compile_definitions(lvt_avalonia_tap PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET lvt_avalonia_tap PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(lvt_avalonia_tap PROPERTIES
    OUTPUT_NAME "lvt_avalonia_tap_${TAP_ARCH_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY ${LVT_RUNTIME_OUTPUT_DIR}/plugins/avalonia
)

# Build managed Avalonia tree walker assembly and copy to output
# Pre-build with: dotnet build src/plugin_avalonia/LvtAvaloniaTreeWalker/LvtAvaloniaTreeWalker.csproj -c Release
# The csproj sets AppendTargetFrameworkToOutputPath=false so output is bin/Release/,
# but as a fallback we also build via a custom command that publishes to a known location.
if(LVT_BUILD_MANAGED)
add_custom_command(TARGET lvt_avalonia_tap POST_BUILD
    COMMAND dotnet publish "${CMAKE_SOURCE_DIR}/src/plugin_avalonia/LvtAvaloniaTreeWalker/LvtAvaloniaTreeWalker.csproj"
        -c Release -o "${LVT_RUNTIME_OUTPUT_DIR}/plugins/avalonia" --no-restore
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/plugin_avalonia/LvtAvaloniaTreeWalker/LvtAvaloniaTreeWalker.runtimeconfig.json"
        "${LVT_RUNTIME_OUTPUT_DIR}/plugins/avalonia/LvtAvaloniaTreeWalker.runtimeconfig.json"
    COMMENT "Publishing managed Avalonia tree walker assembly"
)

add_custom_target(lvt_avalonia_tree_walker
    COMMAND ${CMAKE_COMMAND} -E make_directory "${LVT_RUNTIME_OUTPUT_DIR}/plugins/avalonia"
    COMMAND dotnet publish "${CMAKE_SOURCE_DIR}/src/plugin_avalonia/LvtAvaloniaTreeWalker/LvtAvaloniaTreeWalker.csproj"
        -c Release -o "${LVT_RUNTIME_OUTPUT_DIR}/plugins/avalonia"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/plugin_avalonia/LvtAvaloniaTreeWalker/LvtAvaloniaTreeWalker.runtimeconfig.json"
        "${LVT_RUNTIME_OUTPUT_DIR}/plugins/avalonia/LvtAvaloniaTreeWalker.runtimeconfig.json"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Publishing managed Avalonia tree walker assembly"
)
endif()
endif()

# Chromium plugin DLL — runtime-loaded plugin for Chrome/Edge DOM tree support
if(LVT_ENABLE_CHROMIUM)
add_library(lvt_chromium_plugin SHARED
    src/plugin_chromium/lvt_chromium_plugin.cpp
)
target_compile_definitions(lvt_chromium_plugin PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(lvt_chromium_plugin PRIVATE src)
target_link_libraries(lvt_chromium_plugin PRIVATE nlohmann_json::nlohmann_json ole32 version)
set_target_properties(lvt_chromium_plugin PROPERTIES
    OUTPUT_NAME "lvt_chromium_plugin"
    RUNTIME_OUTPUT_DIRECTORY ${LVT_RUNTIME_OUTPUT_DIR}/plugins
)

# Chromium native messaging host — relay between Chrome extension and lvt
add_executable(lvt_chromium_host
    src/plugin_chromium/chromium_host.cpp
)
target_compile_definitions(lvt_chromium_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(lvt_chromium_host PRIVATE advapi32)
set_target_properties(lvt_chromium_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${LVT_RUNTIME_OUTPUT_DIR}/plugins/chromium
)

# Copy Chromium extension files to output
add_custom_command(TARGET lvt_chromium_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/src/plugin_chromium/extension"
        "${LVT_RUNTIME_OUTPUT_DIR}/plugins/chromium/extension"
    COMMENT "Copying Chromium extension files"
)
endif()

# --- Tests ---
if(LVT_BUILD_TESTS)
enable_testing()
find_package(GTest CONFIG REQUIRED)

# Unit tests — pure logic, no live HWND needed
add_executable(lvt_unit_tests
    tests/unit_tests.cpp
)
target_include_directories(lvt_unit_tests PRIVATE src)
target_compile_definitions(lvt_unit_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX WINRT_LEAN_AND_MEAN)
target_link_libraries(lvt_unit_tests PRIVATE
    lvt_core
    GTest::gtest GTest::gtest_main
    WIL::WIL nlohmann_json::nlohmann_json
    dwmapi windowsapp
)
add_test(NAME unit_tests COMMAND lvt_unit_tests)

# WPF sample app used by integration tests
set(WPF_SAMPLE_PROJECT "${CMAKE_SOURCE_DIR}/tests/apps/WpfSample/WpfSample.csproj")
set(WPF_SAMPLE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/tests/apps/WpfSample")
add_custom_target(wpf_sample_app ALL
    COMMAND dotnet build "${WPF_SAMPLE_PROJECT}" -c Release -o "${WPF_SAMPLE_OUTPUT_DIR}"
    BYPRODUCTS "${WPF_SAMPLE_OUTPUT_DIR}/WpfSample.exe"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Building WPF sample app"
)
file(TO_NATIVE_PATH "${WPF_SAMPLE_OUTPUT_DIR}/WpfSample.exe" WPF_SAMPLE_EXE_NATIVE)
string(REPLACE "\\" "\\\\" WPF_SAMPLE_EXE_ESCAPED "${WPF_SAMPLE_EXE_NATIVE}")

# WinUI3 sample app used by integration tests
set(WINUI3_SAMPLE_PROJECT "${CMAKE_SOURCE_DIR}/tests/apps/WinUI3Sample/WinUI3Sample.csproj")
set(WINUI3_SAMPLE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/tests/apps/WinUI3Sample")
set(WINUI3_SAMPLE_MSBUILD_ARGS "")
if(DEFINED ENV{VSINSTALLDIR} AND EXISTS "$ENV{VSINSTALLDIR}/MSBuild/Microsoft/VisualStudio/v18.0/AppxPackage/Microsoft.Build.Packaging.Pri.Tasks.dll")
    list(APPEND WINUI3_SAMPLE_MSBUILD_ARGS
        "-p:MSBuildExtensionsPath=$ENV{VSINSTALLDIR}/MSBuild")
endif()
add_custom_target(winui3_sample_app ALL
    COMMAND dotnet publish "${WINUI3_SAMPLE_PROJECT}"
        -c Release -p:Platform=x64 -r win-x64 --self-contained true
        -o "${WINUI3_SAMPLE_OUTPUT_DIR}"
        ${WINUI3_SAMPLE_MSBUILD_ARGS}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/tests/apps/WinUI3Sample/obj/x64/Release/net10.0-windows10.0.19041.0/win-x64/App.xbf"
        "${WINUI3_SAMPLE_OUTPUT_DIR}/App.xbf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/tests/apps/WinUI3Sample/obj/x64/Release/net10.0-windows10.0.19041.0/win-x64/MainWindow.xbf"
        "${WINUI3_SAMPLE_OUTPUT_DIR}/MainWindow.xbf"
    BYPRODUCTS "${WINUI3_SAMPLE_OUTPUT_DIR}/WinUI3Sample.exe"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Publishing WinUI3 sample app"
)
file(TO_NATIVE_PATH "${WINUI3_SAMPLE_OUTPUT_DIR}/WinUI3Sample.exe" WINUI3_SAMPLE_EXE_NATIVE)
string(REPLACE "\\" "\\\\" WINUI3_SAMPLE_EXE_ESCAPED "${WINUI3_SAMPLE_EXE_NATIVE}")

# Avalonia sample app used by integration tests
set(AVALONIA_SAMPLE_PROJECT "${CMAKE_SOURCE_DIR}/tests/avalonia_test_app/AvaloniaTestApp.csproj")
set(AVALONIA_SAMPLE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/tests/avalonia_test_app")
add_custom_target(avalonia_test_app ALL
    COMMAND dotnet build "${AVALONIA_SAMPLE_PROJECT}" -c Release -o "${AVALONIA_SAMPLE_OUTPUT_DIR}"
    BYPRODUCTS "${AVALONIA_SAMPLE_OUTPUT_DIR}/AvaloniaTestApp.exe"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Building Avalonia integration test app"
)
file(TO_NATIVE_PATH "${AVALONIA_SAMPLE_OUTPUT_DIR}/AvaloniaTestApp.exe" AVALONIA_SAMPLE_EXE_NATIVE)
string(REPLACE "\\" "\\\\" AVALONIA_SAMPLE_EXE_ESCAPED "${AVALONIA_SAMPLE_EXE_NATIVE}")

# WinForms sample app used by integration tests
set(WINFORMS_SAMPLE_PROJECT "${CMAKE_SOURCE_DIR}/tests/apps/WinFormsSample/WinFormsSample.csproj")
set(WINFORMS_SAMPLE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/tests/apps/WinFormsSample")
add_custom_target(winforms_sample_app ALL
    COMMAND dotnet build "${WINFORMS_SAMPLE_PROJECT}" -c Release -o "${WINFORMS_SAMPLE_OUTPUT_DIR}" --nologo
    BYPRODUCTS "${WINFORMS_SAMPLE_OUTPUT_DIR}/WinFormsSample.exe"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Building WinForms sample app"
)
file(TO_NATIVE_PATH "${WINFORMS_SAMPLE_OUTPUT_DIR}/WinFormsSample.exe" WINFORMS_SAMPLE_EXE_NATIVE)
string(REPLACE "\\" "\\\\" WINFORMS_SAMPLE_EXE_ESCAPED "${WINFORMS_SAMPLE_EXE_NATIVE}")

# Integration tests — require a running Notepad instance
add_executable(lvt_integration_tests
    tests/integration_tests.cpp
    tests/lvt_integration_tests.rc
)
target_include_directories(lvt_integration_tests PRIVATE src)
target_compile_definitions(lvt_integration_tests PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX WINRT_LEAN_AND_MEAN UNICODE _UNICODE
    WPF_SAMPLE_EXE_PATH="${WPF_SAMPLE_EXE_ESCAPED}"
    WINUI3_SAMPLE_EXE_PATH="${WINUI3_SAMPLE_EXE_ESCAPED}"
    AVALONIA_SAMPLE_EXE_PATH="${AVALONIA_SAMPLE_EXE_ESCAPED}"
    WINFORMS_SAMPLE_EXE_PATH="${WINFORMS_SAMPLE_EXE_ESCAPED}"
)
target_link_libraries(lvt_integration_tests PRIVATE
    GTest::gtest GTest::gtest_main
    WIL::WIL nlohmann_json::nlohmann_json
    comctl32
)
target_link_options(lvt_integration_tests PRIVATE "/MANIFEST:NO")
set(LVT_INTEGRATION_DEPS wpf_sample_app winui3_sample_app)
if(LVT_BUILD_TOOL)
    list(APPEND LVT_INTEGRATION_DEPS lvt)
endif()
if(LVT_ENABLE_WINFORMS)
    list(APPEND LVT_INTEGRATION_DEPS lvt_winforms_tap winforms_sample_app)
endif()
if(LVT_ENABLE_AVALONIA)
    list(APPEND LVT_INTEGRATION_DEPS lvt_avalonia_plugin lvt_avalonia_tap avalonia_test_app)
    if(LVT_BUILD_MANAGED)
        list(APPEND LVT_INTEGRATION_DEPS lvt_avalonia_tree_walker)
    endif()
endif()
add_dependencies(lvt_integration_tests ${LVT_INTEGRATION_DEPS})
add_test(NAME integration_tests COMMAND lvt_integration_tests)

# Chromium plugin tests — DOM JSON format and native messaging protocol
if(LVT_ENABLE_CHROMIUM)
add_executable(lvt_chromium_tests
    tests/chromium_tests.cpp
    src/element_key.cpp
)
target_include_directories(lvt_chromium_tests PRIVATE src)
target_compile_definitions(lvt_chromium_tests PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX
    "LVT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\""
    "LVT_CHROMIUM_EXTENSION_OUTPUT_DIR=\"${LVT_RUNTIME_OUTPUT_DIR}/plugins/chromium/extension\""
)
target_link_libraries(lvt_chromium_tests PRIVATE
    GTest::gtest GTest::gtest_main
    nlohmann_json::nlohmann_json
)
add_dependencies(lvt_chromium_tests lvt_chromium_plugin)
add_test(NAME chromium_tests COMMAND lvt_chromium_tests)
endif()

endif() # LVT_BUILD_TESTS

# --- Install / export -------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Tools, TAP DLLs and plugins all land in one directory: lvt_core resolves the
# TAP DLLs relative to its own module, so they must stay side by side.
# Package managers override this (vcpkg, for instance, wants tools/lvt).
set(LVT_INSTALL_TOOLSDIR "${CMAKE_INSTALL_DATAROOTDIR}/lvt/tools" CACHE STRING
    "Directory, relative to the install prefix, for lvt.exe, the TAP DLLs and plugins")

install(TARGETS lvt_core
    EXPORT lvtTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(FILES ${LVT_PUBLIC_HEADERS} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/lvt")

if(LVT_BUILD_TOOL)
    install(TARGETS lvt RUNTIME DESTINATION "${LVT_INSTALL_TOOLSDIR}")
endif()

# TAP DLLs are injected into the target process, never linked, so they are
# installed as plain runtime files rather than exported targets.
set(LVT_INSTALLED_TAP_FILES "")
foreach(_tap lvt_tap lvt_wpf_tap lvt_winforms_tap)
    if(TARGET ${_tap})
        install(TARGETS ${_tap} RUNTIME DESTINATION "${LVT_INSTALL_TOOLSDIR}")
        list(APPEND LVT_INSTALLED_TAP_FILES "${_tap}_${TAP_ARCH_SUFFIX}.dll")
    endif()
endforeach()
unset(_tap)

foreach(_plugin lvt_avalonia_plugin lvt_chromium_plugin)
    if(TARGET ${_plugin})
        install(TARGETS ${_plugin} RUNTIME DESTINATION "${LVT_INSTALL_TOOLSDIR}/plugins")
    endif()
endforeach()
unset(_plugin)

if(TARGET lvt_avalonia_tap)
    install(TARGETS lvt_avalonia_tap
        RUNTIME DESTINATION "${LVT_INSTALL_TOOLSDIR}/plugins/avalonia")
endif()
if(TARGET lvt_chromium_host)
    install(TARGETS lvt_chromium_host
        RUNTIME DESTINATION "${LVT_INSTALL_TOOLSDIR}/plugins/chromium")
endif()
if(LVT_ENABLE_CHROMIUM)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin_chromium/extension"
        DESTINATION "${LVT_INSTALL_TOOLSDIR}/plugins/chromium")
endif()

# Managed helper assemblies, when they were built.
if(LVT_BUILD_MANAGED)
    foreach(_managed
        "${CMAKE_CURRENT_SOURCE_DIR}/src/tap_wpf/bin/Release/LvtWpfTap.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/tap_wpf/LvtWpfTap.runtimeconfig.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/tap_winforms/bin/Release/LvtWinFormsTap.dll"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/tap_winforms/LvtWinFormsTap.runtimeconfig.json")
        if(EXISTS "${_managed}")
            install(FILES "${_managed}" DESTINATION "${LVT_INSTALL_TOOLSDIR}")
        endif()
    endforeach()
    unset(_managed)
endif()

install(EXPORT lvtTargets
    FILE lvtTargets.cmake
    NAMESPACE lvt::
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/lvt"
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/lvtConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/lvtConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/lvt"
    PATH_VARS LVT_INSTALL_TOOLSDIR
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/lvtConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/lvtConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/lvtConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/lvt"
)
