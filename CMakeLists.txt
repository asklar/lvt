cmake_minimum_required(VERSION 3.20)
project(lvt LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(wil CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(lvt
    src/main.cpp
    src/target.cpp
    src/framework_detector.cpp
    src/tree_builder.cpp
    src/json_serializer.cpp
    src/screenshot.cpp
    src/plugin_loader.cpp
    src/providers/win32_provider.cpp
    src/providers/comctl_provider.cpp
    src/providers/xaml_provider.cpp
    src/providers/winui3_provider.cpp
    src/providers/wpf_provider.cpp
    src/providers/wpf_inject.cpp
    src/providers/xaml_diag_common.cpp
)

target_include_directories(lvt PRIVATE src)

target_link_libraries(lvt PRIVATE
    WIL::WIL
    nlohmann_json::nlohmann_json
    dwmapi
    d3d11
    dxgi
    windowsapp
    windowscodecs
)

target_compile_definitions(lvt PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINRT_LEAN_AND_MEAN
)

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
add_library(lvt_tap SHARED
    src/tap/lvt_tap.cpp
    src/tap/lvt_tap.def
)
target_compile_definitions(lvt_tap PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(lvt_tap PRIVATE src)
target_link_libraries(lvt_tap PRIVATE ole32)
set_property(TARGET lvt_tap PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Place lvt_tap_{arch}.dll next to lvt.exe
set_target_properties(lvt_tap PROPERTIES
    OUTPUT_NAME "lvt_tap_${TAP_ARCH_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:lvt>
)

# WPF TAP DLL — native loader injected into WPF target process
# Hosts CLR and calls managed WpfTreeWalker assembly
add_library(lvt_wpf_tap SHARED
    src/tap_wpf/wpf_tap_native.cpp
    src/tap_wpf/wpf_tap_native.def
)
target_compile_definitions(lvt_wpf_tap PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(lvt_wpf_tap PRIVATE src)
target_link_libraries(lvt_wpf_tap PRIVATE ole32)
set_property(TARGET lvt_wpf_tap PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set_target_properties(lvt_wpf_tap PROPERTIES
    OUTPUT_NAME "lvt_wpf_tap_${TAP_ARCH_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:lvt>
)

# Build managed WPF tree walker assembly and copy to output
# Pre-build with: dotnet build src/tap_wpf/LvtWpfTap.csproj -c Release
# The managed DLL is AnyCPU so the same binary works for all architectures.
add_custom_command(TARGET lvt_wpf_tap POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/tap_wpf/bin/Release/LvtWpfTap.dll"
        "$<TARGET_FILE_DIR:lvt>/LvtWpfTap.dll"
    COMMENT "Copying managed WPF tree walker assembly"
)

# Avalonia plugin DLL — runtime-loaded plugin for Avalonia UI framework support
add_library(lvt_avalonia_plugin SHARED
    src/plugin_avalonia/lvt_avalonia_plugin.cpp
)
target_compile_definitions(lvt_avalonia_plugin PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(lvt_avalonia_plugin PRIVATE src)
target_link_libraries(lvt_avalonia_plugin PRIVATE ole32 version)
set_target_properties(lvt_avalonia_plugin PROPERTIES
    OUTPUT_NAME "lvt_avalonia_plugin"
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:lvt>/plugins
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
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:lvt>/plugins/avalonia
)

# Build managed Avalonia tree walker assembly and copy to output
# Pre-build with: dotnet build src/plugin_avalonia/LvtAvaloniaTreeWalker/LvtAvaloniaTreeWalker.csproj -c Release
# The csproj sets AppendTargetFrameworkToOutputPath=false so output is bin/Release/,
# but as a fallback we also build via a custom command that publishes to a known location.
add_custom_command(TARGET lvt_avalonia_tap POST_BUILD
    COMMAND dotnet publish "${CMAKE_SOURCE_DIR}/src/plugin_avalonia/LvtAvaloniaTreeWalker/LvtAvaloniaTreeWalker.csproj"
        -c Release -o "$<TARGET_FILE_DIR:lvt>/plugins/avalonia" --no-restore
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/src/plugin_avalonia/LvtAvaloniaTreeWalker/LvtAvaloniaTreeWalker.runtimeconfig.json"
        "$<TARGET_FILE_DIR:lvt>/plugins/avalonia/LvtAvaloniaTreeWalker.runtimeconfig.json"
    COMMENT "Publishing managed Avalonia tree walker assembly"
)

# Chromium plugin DLL — runtime-loaded plugin for Chrome/Edge DOM tree support
add_library(lvt_chromium_plugin SHARED
    src/plugin_chromium/lvt_chromium_plugin.cpp
)
target_compile_definitions(lvt_chromium_plugin PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(lvt_chromium_plugin PRIVATE src)
target_link_libraries(lvt_chromium_plugin PRIVATE nlohmann_json::nlohmann_json ole32 version)
set_target_properties(lvt_chromium_plugin PROPERTIES
    OUTPUT_NAME "lvt_chromium_plugin"
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:lvt>/plugins
)

# Chromium native messaging host — relay between Chrome extension and lvt
add_executable(lvt_chromium_host
    src/plugin_chromium/chromium_host.cpp
)
target_compile_definitions(lvt_chromium_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(lvt_chromium_host PRIVATE advapi32)
set_target_properties(lvt_chromium_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:lvt>/plugins/chromium
)

# Copy Chromium extension files to output
add_custom_command(TARGET lvt_chromium_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/src/plugin_chromium/extension"
        "$<TARGET_FILE_DIR:lvt>/plugins/chromium/extension"
    COMMENT "Copying Chromium extension files"
)

# --- Tests ---
enable_testing()
find_package(GTest CONFIG REQUIRED)

# Unit tests — pure logic, no live HWND needed
add_executable(lvt_unit_tests
    tests/unit_tests.cpp
    src/tree_builder.cpp
    src/json_serializer.cpp
    src/framework_detector.cpp
    src/target.cpp
    src/plugin_loader.cpp
    src/providers/win32_provider.cpp
    src/providers/comctl_provider.cpp
    src/providers/xaml_provider.cpp
    src/providers/winui3_provider.cpp
    src/providers/wpf_provider.cpp
    src/providers/wpf_inject.cpp
    src/providers/xaml_diag_common.cpp
)
target_include_directories(lvt_unit_tests PRIVATE src)
target_compile_definitions(lvt_unit_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX WINRT_LEAN_AND_MEAN)
target_link_libraries(lvt_unit_tests PRIVATE
    GTest::gtest GTest::gtest_main
    WIL::WIL nlohmann_json::nlohmann_json
    dwmapi windowsapp
)
add_test(NAME unit_tests COMMAND lvt_unit_tests)

# Integration tests — require a running Notepad instance
add_executable(lvt_integration_tests
    tests/integration_tests.cpp
)
target_include_directories(lvt_integration_tests PRIVATE src)
target_compile_definitions(lvt_integration_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX WINRT_LEAN_AND_MEAN)
target_link_libraries(lvt_integration_tests PRIVATE
    GTest::gtest GTest::gtest_main
    WIL::WIL nlohmann_json::nlohmann_json
)
add_test(NAME integration_tests COMMAND lvt_integration_tests)

# Chromium plugin tests — DOM JSON format and native messaging protocol
add_executable(lvt_chromium_tests
    tests/chromium_tests.cpp
)
target_include_directories(lvt_chromium_tests PRIVATE src)
target_compile_definitions(lvt_chromium_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(lvt_chromium_tests PRIVATE
    GTest::gtest GTest::gtest_main
    nlohmann_json::nlohmann_json
)
add_test(NAME chromium_tests COMMAND lvt_chromium_tests)
