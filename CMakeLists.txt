cmake_minimum_required(VERSION 3.20)

# Declared before project() because the vcpkg toolchain runs its install during
# project(), and gtest is only wanted when the tests are actually built. Without
# this the dependency is unconditional, so a packaging or cross-compilation
# build fails if gtest cannot be built for the target triplet even though
# nothing would have linked it.
option(LVT_BUILD_TESTS "Build lvt's tests" ON)
if(LVT_BUILD_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()

# VERSION is the single source of truth for lvt's product version: every part
# of the build (LVT_VERSION / --version, the Win32 VERSIONINFO resources
# stamped onto lvt.exe and its DLLs, the MCP server's reported version, and
# Directory.Build.props for the managed .NET tree walkers) derives from this
# one file, so bumping a release means editing it in exactly one place. See
# also mcp/Cargo.toml, which cannot read this file for its own manifest
# version (Cargo has no equivalent to configure_file) and is documented there
# as decorative rather than authoritative.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LVT_VERSION_STRING LIMIT_COUNT 1)
string(STRIP "${LVT_VERSION_STRING}" LVT_VERSION_STRING)

project(lvt VERSION ${LVT_VERSION_STRING} LANGUAGES CXX)

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
option(LVT_ENABLE_UIA      "UI Automation tree output (--uia)"                    ON)
# The MCP server needs a Rust toolchain, which not every contributor has, so it
# is opt-in locally. CI and the release build turn it on explicitly.
option(LVT_ENABLE_MCP      "MCP server mode (lvt mcp); requires a Rust toolchain" OFF)

option(LVT_BUILD_TOOL    "Build the lvt command-line tool"                     ON)
option(LVT_BUILD_MANAGED "Build managed .NET assemblies (requires .NET SDK)"   ON)
# Off by default: it's a separate WPF app built by its own `dotnet build`, not
# something most contributors touching lvt_core need. See src/viewer/README.md
# for the architecture (it drives lvt.exe as a subprocess; it never links
# lvt_core) and why it is not part of the default build.
option(LVT_BUILD_VIEWER  "Build the lvt Viewer WPF app (requires .NET SDK)"    OFF)
# LVT_BUILD_TESTS is declared above project(), before the vcpkg toolchain runs.

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

# Stamps a Win32 VERSIONINFO resource (CompanyName, FileDescription, version
# numbers, etc.) onto an EXE or DLL target so Explorer's Properties dialog and
# tools like `Get-Item .\lvt.exe | Select VersionInfo` show real metadata
# instead of blanks. filetype is VFT_APP for an .exe or VFT_DLL for a .dll.
function(lvt_add_version_resource target description internal_name original_filename filetype)
    set(LVT_VERSION_RC_DESCRIPTION "${description}")
    set(LVT_VERSION_RC_INTERNALNAME "${internal_name}")
    set(LVT_VERSION_RC_ORIGINALFILENAME "${original_filename}")
    set(LVT_VERSION_RC_FILETYPE "${filetype}")
    set(_lvt_version_rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    configure_file(
        "${CMAKE_SOURCE_DIR}/cmake/version_resource.rc.in"
        "${_lvt_version_rc}"
        @ONLY
    )
    target_sources(${target} PRIVATE "${_lvt_version_rc}")
endfunction()

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
    src/input.h
    src/lvt_api.h
    src/screenshot.h
    src/target.h
    src/tree_builder.h
    src/watch_diff.h
    src/wil_diagnostics.h
    "${CMAKE_CURRENT_BINARY_DIR}/include/lvt/lvt_config.h"
)
# tree_builder.h exposes ConnectionLookup in its public API and includes this
# header using its source-tree-relative providers/ path. Preserve that layout
# in the install tree rather than flattening the provider header beside it.
set(LVT_PUBLIC_PROVIDER_HEADERS
    src/providers/framework_connection.h
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
    src/input.cpp
    src/lvt_api.cpp
    src/plugin_loader.cpp
    src/wil_diagnostics.cpp
    src/providers/win32_provider.cpp
    src/providers/comctl_provider.cpp
    src/providers/connection_registry.cpp
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
if(LVT_ENABLE_UIA)
    list(APPEND LVT_CORE_SOURCES
        src/providers/uia_provider.cpp
        src/providers/uia_props.cpp
        src/providers/uia_actions.cpp
    )
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
    # PUBLIC: plugin_loader.h is an installed header and exposes wil::unique_hmodule
    # in LoadedPlugin, so consumers need WIL's include directories. The generated
    # package config already does find_dependency(wil CONFIG).
    WIL::WIL
)
if(LVT_ENABLE_UIA)
    target_link_libraries(lvt_core PUBLIC uiautomationcore)
    target_compile_definitions(lvt_core PUBLIC LVT_ENABLE_UIA)
endif()
target_link_libraries(lvt_core PRIVATE
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
    lvt_add_version_resource(lvt
        "lvt - Live Visual Tree CLI" "lvt" "lvt.exe" VFT_APP)
endif()

# --- lvt Viewer (WPF, .NET) --------------------------------------------------
# A separate desktop GUI, built by its own `dotnet build` rather than CMake
# compiling anything: it drives lvt.exe as a subprocess (dump/watch/action
# verbs) instead of linking lvt_core, so nothing here needs a C++/.NET
# interop seam. See src/viewer/README.md for the architecture.
if(LVT_BUILD_VIEWER)
    find_program(LVT_DOTNET_EXE NAMES dotnet)
    if(NOT LVT_DOTNET_EXE)
        message(FATAL_ERROR
            "LVT_BUILD_VIEWER is ON but dotnet was not found. Install the .NET SDK, "
            "or configure with -DLVT_BUILD_VIEWER=OFF to build without the viewer.")
    endif()

    set(LVT_VIEWER_PROJECT "${CMAKE_CURRENT_SOURCE_DIR}/src/viewer/LvtViewer/LvtViewer.csproj")
    set(LVT_VIEWER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/viewer")
    add_custom_target(lvt_viewer ALL
        COMMAND "${LVT_DOTNET_EXE}" build "${LVT_VIEWER_PROJECT}" -c Release -o "${LVT_VIEWER_OUTPUT_DIR}"
        BYPRODUCTS "${LVT_VIEWER_OUTPUT_DIR}/LvtViewer.exe"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building lvt Viewer (WPF)"
    )
    if(LVT_BUILD_TOOL)
        # A copy of lvt.exe with none of its TAP DLLs/plugins next to it is
        # actively worse than no copy: it resolves (get_tap_directory() finds
        # this directory first) but every framework-specific enrichment it
        # would have provided silently stops working, indistinguishable from
        # the target simply not using that framework - exactly the failure
        # mode a user hit the first time this copied everything but the TAP
        # DLLs. The rest of that copying (mirroring the release packaging
        # step in .github/workflows/release.yml and the install() rules
        # below) lives further down this file, after the TAP DLL and plugin
        # targets it references are actually defined - if(TARGET ...) can
        # only see targets CMake has already processed by that point in the
        # file, so doing it here (before those add_library() calls) would
        # silently no-op every one of those checks.
        add_custom_command(TARGET lvt_viewer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:lvt>" "${LVT_VIEWER_OUTPUT_DIR}/lvt.exe"
            COMMENT "Copying lvt.exe next to the viewer"
        )
        add_dependencies(lvt_viewer lvt)
    endif()
endif()

# --- MCP server (Rust) -----------------------------------------------------
# The MCP protocol layer is a Rust staticlib linked into lvt.exe, so `lvt mcp`
# is served by the same binary the rest of the CLI is — no second executable and
# no DLL in the release zip. Corrosion would work here too, but a single
# staticlib with no generated bindings does not justify the extra dependency.
if(LVT_ENABLE_MCP)
    find_program(LVT_CARGO_EXE NAMES cargo)
    if(NOT LVT_CARGO_EXE)
        message(FATAL_ERROR
            "LVT_ENABLE_MCP is ON but cargo was not found. Install Rust from https://rustup.rs, "
            "or configure with -DLVT_ENABLE_MCP=OFF to build without the MCP server.")
    endif()

    # Rust's target triple has to follow the C++ one or the objects will not
    # link; the release matrix builds all three.
    if(VCPKG_TARGET_TRIPLET MATCHES "arm64")
        set(LVT_RUST_TARGET "aarch64-pc-windows-msvc")
    elseif(VCPKG_TARGET_TRIPLET MATCHES "x86")
        set(LVT_RUST_TARGET "i686-pc-windows-msvc")
    else()
        set(LVT_RUST_TARGET "x86_64-pc-windows-msvc")
    endif()

    # Always the release profile. Rust links the release CRT regardless of
    # profile, so a debug build would buy nothing but a CRT mismatch and a much
    # larger binary; the MCP layer is thin enough that stepping through it is
    # not the debugging strategy anyway.
    set(LVT_MCP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mcp")
    set(LVT_MCP_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/mcp-target")
    set(LVT_MCP_LIB "${LVT_MCP_TARGET_DIR}/${LVT_RUST_TARGET}/release/lvt_mcp.lib")

    # GLOB_RECURSE with CONFIGURE_DEPENDS, not a plain GLOB. A plain GLOB is
    # evaluated once at configure time, so a Rust file added afterwards is not a
    # dependency of anything: cargo is never re-run, lvt.exe keeps the previous
    # MCP layer, and the build reports success. That silence is the problem —
    # tests then exercise a binary that does not contain the change under test,
    # and pass. CONFIGURE_DEPENDS makes the build re-check the file set, and
    # RECURSE covers any future subdirectory.
    file(GLOB_RECURSE LVT_MCP_SOURCES CONFIGURE_DEPENDS "${LVT_MCP_DIR}/src/*.rs")
    add_custom_command(
        OUTPUT "${LVT_MCP_LIB}"
        COMMAND "${CMAKE_COMMAND}" -E env
                "CARGO_TARGET_DIR=${LVT_MCP_TARGET_DIR}"
                "${LVT_CARGO_EXE}" build --release --target ${LVT_RUST_TARGET}
                --manifest-path "${LVT_MCP_DIR}/Cargo.toml"
        DEPENDS ${LVT_MCP_SOURCES} "${LVT_MCP_DIR}/Cargo.toml" "${LVT_MCP_DIR}/Cargo.lock"
        WORKING_DIRECTORY "${LVT_MCP_DIR}"
        COMMENT "Building the MCP server (cargo, ${LVT_RUST_TARGET})"
        VERBATIM)
    add_custom_target(lvt_mcp_rust DEPENDS "${LVT_MCP_LIB}")

    if(LVT_BUILD_TOOL)
        add_dependencies(lvt lvt_mcp_rust)
        # These are what `cargo rustc -- --print=native-static-libs` reports for
        # this dependency set; tokio and rmcp pull in sockets, RNG and the
        # backtrace machinery.
        target_link_libraries(lvt PRIVATE
            "${LVT_MCP_LIB}"
            ws2_32 userenv ntdll dbghelp bcrypt advapi32)
        target_compile_definitions(lvt PRIVATE LVT_ENABLE_MCP)
    endif()
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
#
# Only the Microsoft.* (Windows App SDK) half is generated here; winrt/base.h
# and the Windows.* projection come from elsewhere on the include path. Those
# two halves must be produced by the *same* cppwinrt version or the generated
# code fails to compile against base.h's macros ("Mismatched C++/WinRT
# headers"). Preferring vcpkg's cppwinrt port keeps them in lockstep, and lets
# a consumer that already depends on cppwinrt dictate the single version used.
set(WINUI3_CPPWINRT_DIR "${CMAKE_SOURCE_DIR}/src/tap/winui3")
set(WINUI3_SIGNATURE_FILE "${WINUI3_CPPWINRT_DIR}/.cppwinrt-signature")

set(LVT_WASDK_WINMD_DIR "" CACHE PATH
    "Directory holding the Windows App SDK .winmd files (lib/uap10.0 of the nupkg)")

if(LVT_ENABLE_WINUI3)
    # --- cppwinrt.exe -------------------------------------------------------
    # LVT_CPPWINRT_EXE may be pre-set (the vcpkg port does this), in which case
    # find_program is a no-op.
    set(_cppwinrt_hints "")

    # 1. The cppwinrt vcpkg port, so the generator matches the base.h we build
    #    against. This is the preferred source.
    if(DEFINED VCPKG_INSTALLED_DIR)
        set(_lvt_vcpkg_installed "${VCPKG_INSTALLED_DIR}")
    elseif(DEFINED _VCPKG_INSTALLED_DIR)
        set(_lvt_vcpkg_installed "${_VCPKG_INSTALLED_DIR}")
    else()
        set(_lvt_vcpkg_installed "")
    endif()
    if(_lvt_vcpkg_installed)
        list(APPEND _cppwinrt_hints
            "${_lvt_vcpkg_installed}/${VCPKG_TARGET_TRIPLET}/tools/cppwinrt"
            "${_lvt_vcpkg_installed}/x64-windows/tools/cppwinrt")
    endif()

    # 2. Otherwise the active Windows SDK, preferring the one the developer
    #    command prompt selected over the newest installed.
    if(DEFINED ENV{WindowsSdkVerBinPath})
        list(APPEND _cppwinrt_hints "$ENV{WindowsSdkVerBinPath}/x64")
    endif()
    if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
        string(REGEX REPLACE "[\\/]+$" "" _lvt_sdk_ver "$ENV{WindowsSDKVersion}")
        list(APPEND _cppwinrt_hints "$ENV{WindowsSdkDir}/bin/${_lvt_sdk_ver}/x64")
    endif()
    file(GLOB _lvt_sdk_bin_dirs
        "$ENV{WindowsSdkDir}/bin/10.*"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.*")
    list(SORT _lvt_sdk_bin_dirs ORDER DESCENDING)
    foreach(_lvt_bin ${_lvt_sdk_bin_dirs})
        if(IS_DIRECTORY "${_lvt_bin}")
            list(APPEND _cppwinrt_hints "${_lvt_bin}/x64")
        endif()
    endforeach()

    find_program(LVT_CPPWINRT_EXE NAMES cppwinrt HINTS ${_cppwinrt_hints})

    # --- Windows App SDK winmds --------------------------------------------
    # There is no vcpkg port for the Windows App SDK, so this always comes from
    # a NuGet package: the vcpkg port downloads it and passes the directory in,
    # otherwise fall back to the NuGet cache or a local packages/ dir.
    #
    # Two different NuGet layouts exist and both need checking: the older,
    # unified "Microsoft.WindowsAppSDK" package (winmds under lib/uap10.0),
    # and the newer split packaging (Microsoft.WindowsAppSDK.WinUI,
    # .Foundation, .Base, ...) where each capability is its own package and
    # the winmds live under metadata/ instead - only the latter was found
    # installed on this machine (confirmed via the NuGet cache directly),
    # so a build here would otherwise silently fall back to no WinUI3
    # projection at all: CollectPositionsAndText's WinUI3-specific casts
    # compile out, meaning every genuine WinUI3 element's bounds/position/
    # text collection could only ever fail (this is what turned out to be
    # the entire root cause of the "bounds/text not captured for WinUI3
    # elements in File Explorer" bug - not a code defect).
    if(NOT LVT_WASDK_WINMD_DIR)
        file(GLOB _wasdk_candidates
            "$ENV{USERPROFILE}/.nuget/packages/microsoft.windowsappsdk/*/lib/uap10.0"
            "$ENV{NUGET_PACKAGES}/microsoft.windowsappsdk/*/lib/uap10.0"
            "${CMAKE_SOURCE_DIR}/packages/Microsoft.WindowsAppSDK.*/lib/uap10.0"
            "$ENV{USERPROFILE}/.nuget/packages/microsoft.windowsappsdk.winui/*/metadata"
            "$ENV{NUGET_PACKAGES}/microsoft.windowsappsdk.winui/*/metadata"
        )
        # Prefer the newest STABLE version when several are cached: plain
        # descending string sort would put "2.0.250930000-experimental"
        # ahead of "1.8.251222000" (lexicographically "2" > "1"), and an
        # experimental/preview SDK's winmd can reference types (seen live:
        # Microsoft.Web.WebView2.Core.CoreWebView2) that don't resolve
        # against the stable siblings selected below, failing the whole
        # projection generation. Only fall back to a pre-release version if
        # truly nothing stable is cached.
        list(SORT _wasdk_candidates ORDER DESCENDING)
        set(_wasdk_stable_candidates "${_wasdk_candidates}")
        list(FILTER _wasdk_stable_candidates EXCLUDE REGEX "-(experimental|preview|alpha|beta)[0-9.]*[/\\\\]")
        foreach(_candidate ${_wasdk_stable_candidates} ${_wasdk_candidates})
            if(EXISTS "${_candidate}/Microsoft.UI.Xaml.winmd")
                set(LVT_WASDK_WINMD_DIR "${_candidate}" CACHE PATH "" FORCE)
                break()
            endif()
        endforeach()
    endif()

    # --- Windows.winmd, for resolving system type references ----------------
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

    # --- Regenerate only when an input actually changed ----------------------
    # Hashing the generator catches an upgraded SDK or a bumped cppwinrt port,
    # which is what silently invalidates an already-generated projection.
    set(_lvt_projection_signature "none")
    if(LVT_CPPWINRT_EXE AND EXISTS "${LVT_CPPWINRT_EXE}")
        file(SHA256 "${LVT_CPPWINRT_EXE}" _lvt_cppwinrt_hash)
        set(_lvt_projection_signature "cppwinrt=${_lvt_cppwinrt_hash} wasdk=${LVT_WASDK_WINMD_DIR} union=${WIN_UNION_WINMD}")
    endif()

    set(_lvt_previous_signature "")
    if(EXISTS "${WINUI3_SIGNATURE_FILE}")
        file(READ "${WINUI3_SIGNATURE_FILE}" _lvt_previous_signature)
    endif()

    if(NOT _lvt_previous_signature STREQUAL _lvt_projection_signature)
        if(LVT_CPPWINRT_EXE AND LVT_WASDK_WINMD_DIR AND WIN_UNION_WINMD)
            if(_lvt_previous_signature)
                message(STATUS "WinUI3 C++/WinRT projection is stale; regenerating")
            endif()
            message(STATUS "Generating WinUI3 C++/WinRT headers from ${LVT_WASDK_WINMD_DIR}")
            message(STATUS "  using ${LVT_CPPWINRT_EXE}")

            file(GLOB WASDK_ALL_WINMDS "${LVT_WASDK_WINMD_DIR}/*.winmd")
            # Also include other winmd directories that belong with this
            # one - which ones depends on which of the two NuGet layouts
            # LVT_WASDK_WINMD_DIR matched above.
            if(LVT_WASDK_WINMD_DIR MATCHES "/metadata$")
                # Split-package layout (Microsoft.WindowsAppSDK.WinUI,
                # .Foundation, .Base, ...): LVT_WASDK_WINMD_DIR is
                # .../microsoft.windowsappsdk.winui/<version>/metadata.
                # Sibling packages define types Microsoft.UI.Xaml.winmd can
                # reference, so pull in the latest STABLE version of every
                # other microsoft.windowsappsdk.* package cached alongside
                # this one - explicitly not a second, different version of
                # THIS SAME package: mixing versions of one package (e.g.
                # both a cached 2.0-experimental and 1.8 stable .winui) fed
                # cppwinrt conflicting type definitions and broke the whole
                # projection ("Type ... could not be found" from a database
                # mismatch) - this is exactly the failure being guarded
                # against here.
                get_filename_component(_wasdk_version_dir "${LVT_WASDK_WINMD_DIR}/.." ABSOLUTE)
                get_filename_component(_wasdk_this_pkg_dir "${_wasdk_version_dir}/.." ABSOLUTE)
                get_filename_component(_wasdk_packages_dir "${_wasdk_this_pkg_dir}/.." ABSOLUTE)
                file(GLOB _sibling_wasdk_pkg_dirs "${_wasdk_packages_dir}/microsoft.windowsappsdk.*")
                foreach(_sibling_pkg ${_sibling_wasdk_pkg_dirs})
                    if(NOT _sibling_pkg STREQUAL _wasdk_this_pkg_dir AND IS_DIRECTORY "${_sibling_pkg}")
                        file(GLOB _sibling_versions "${_sibling_pkg}/*")
                        list(SORT _sibling_versions ORDER DESCENDING)
                        set(_sibling_stable_versions "${_sibling_versions}")
                        list(FILTER _sibling_stable_versions EXCLUDE REGEX "-(experimental|preview|alpha|beta)[0-9.]*$")
                        foreach(_sibling_version ${_sibling_stable_versions} ${_sibling_versions})
                            file(GLOB_RECURSE _sibling_winmds "${_sibling_version}/*.winmd")
                            if(_sibling_winmds)
                                list(APPEND WASDK_ALL_WINMDS ${_sibling_winmds})
                                break() # only the one (best) version of this sibling
                            endif()
                        endforeach()
                    endif()
                endforeach()
            else()
                # Unified layout (Microsoft.WindowsAppSDK): LVT_WASDK_WINMD_DIR
                # is .../microsoft.windowsappsdk/<version>/lib/uap10.0 -
                # other TFM directories under that same version's lib/ can
                # hold winmds too.
                get_filename_component(_wasdk_root "${LVT_WASDK_WINMD_DIR}/../.." ABSOLUTE)
                file(GLOB_RECURSE _extra_winmds "${_wasdk_root}/*.winmd")
                list(APPEND WASDK_ALL_WINMDS ${_extra_winmds})
            endif()

            # Microsoft.UI.Xaml.winmd references WebView2 types (WinUI3's
            # WebView2 control), which live in an entirely separate NuGet
            # package (Microsoft.Web.WebView2, not part of WindowsAppSDK at
            # all) - missing this reference is what actually broke
            # generation here ("Type 'Microsoft.Web.WebView2.Core.CoreWebView2'
            # could not be found") once the WindowsAppSDK winmds themselves
            # were correctly found above.
            file(GLOB _webview2_version_dirs
                "$ENV{USERPROFILE}/.nuget/packages/microsoft.web.webview2/*"
                "$ENV{NUGET_PACKAGES}/microsoft.web.webview2/*")
            list(SORT _webview2_version_dirs ORDER DESCENDING)
            set(_webview2_stable_dirs "${_webview2_version_dirs}")
            list(FILTER _webview2_stable_dirs EXCLUDE REGEX "-(experimental|preview|prerelease|alpha|beta)[0-9.]*$")
            foreach(_webview2_dir ${_webview2_stable_dirs} ${_webview2_version_dirs})
                if(EXISTS "${_webview2_dir}/lib/Microsoft.Web.WebView2.Core.winmd")
                    list(APPEND WASDK_ALL_WINMDS "${_webview2_dir}/lib/Microsoft.Web.WebView2.Core.winmd")
                    break()
                endif()
            endforeach()

            list(REMOVE_DUPLICATES WASDK_ALL_WINMDS)

            # A stale projection must not be left behind on failure.
            file(REMOVE_RECURSE "${WINUI3_CPPWINRT_DIR}")
            execute_process(
                COMMAND "${LVT_CPPWINRT_EXE}"
                    -in ${WASDK_ALL_WINMDS}
                    -ref "${WIN_UNION_WINMD}"
                    -out "${WINUI3_CPPWINRT_DIR}"
                RESULT_VARIABLE _cppwinrt_result
            )
            if(_cppwinrt_result EQUAL 0)
                file(WRITE "${WINUI3_SIGNATURE_FILE}" "${_lvt_projection_signature}")
                message(STATUS "WinUI3 C++/WinRT headers generated in ${WINUI3_CPPWINRT_DIR}")
            else()
                message(WARNING "cppwinrt failed (${_cppwinrt_result}); TAP DLL WinUI3 features will be limited")
            endif()
        else()
            message(WARNING "Cannot generate WinUI3 C++/WinRT headers:"
                " cppwinrt=${LVT_CPPWINRT_EXE} winmd_dir=${LVT_WASDK_WINMD_DIR} union=${WIN_UNION_WINMD}")
        endif()
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
lvt_add_version_resource(lvt_tap
    "lvt XAML/WinUI3 TAP DLL (injected diagnostics helper)"
    "lvt_tap" "lvt_tap_${TAP_ARCH_SUFFIX}.dll" VFT_DLL)
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
lvt_add_version_resource(lvt_wpf_tap
    "lvt WPF TAP DLL (injected diagnostics helper)"
    "lvt_wpf_tap" "lvt_wpf_tap_${TAP_ARCH_SUFFIX}.dll" VFT_DLL)

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
lvt_add_version_resource(lvt_winforms_tap
    "lvt WinForms TAP DLL (injected diagnostics helper)"
    "lvt_winforms_tap" "lvt_winforms_tap_${TAP_ARCH_SUFFIX}.dll" VFT_DLL)

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
target_link_libraries(lvt_avalonia_plugin PRIVATE WIL::WIL ole32 version)
set_target_properties(lvt_avalonia_plugin PROPERTIES
    OUTPUT_NAME "lvt_avalonia_plugin"
    RUNTIME_OUTPUT_DIRECTORY ${LVT_RUNTIME_OUTPUT_DIR}/plugins
)
lvt_add_version_resource(lvt_avalonia_plugin
    "lvt Avalonia plugin DLL"
    "lvt_avalonia_plugin" "lvt_avalonia_plugin.dll" VFT_DLL)

# Avalonia TAP DLL — injected into Avalonia target process
# Uses static CRT (/MT) to avoid CRT version conflicts in the target process
add_library(lvt_avalonia_tap SHARED
    src/plugin_avalonia/avalonia_tap_native.cpp
    src/plugin_avalonia/avalonia_tap_native.def
)
target_compile_definitions(lvt_avalonia_tap PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(lvt_avalonia_tap PRIVATE WIL::WIL)
set_property(TARGET lvt_avalonia_tap PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set_target_properties(lvt_avalonia_tap PROPERTIES
    OUTPUT_NAME "lvt_avalonia_tap_${TAP_ARCH_SUFFIX}"
    RUNTIME_OUTPUT_DIRECTORY ${LVT_RUNTIME_OUTPUT_DIR}/plugins/avalonia
)
lvt_add_version_resource(lvt_avalonia_tap
    "lvt Avalonia TAP DLL (injected diagnostics helper)"
    "lvt_avalonia_tap" "lvt_avalonia_tap_${TAP_ARCH_SUFFIX}.dll" VFT_DLL)

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
target_link_libraries(lvt_chromium_plugin PRIVATE WIL::WIL nlohmann_json::nlohmann_json ole32 version)
set_target_properties(lvt_chromium_plugin PROPERTIES
    OUTPUT_NAME "lvt_chromium_plugin"
    RUNTIME_OUTPUT_DIRECTORY ${LVT_RUNTIME_OUTPUT_DIR}/plugins
)
lvt_add_version_resource(lvt_chromium_plugin
    "lvt Chromium (Chrome/Edge DOM) plugin DLL"
    "lvt_chromium_plugin" "lvt_chromium_plugin.dll" VFT_DLL)

# Chromium native messaging host — relay between Chrome extension and lvt
add_executable(lvt_chromium_host
    src/plugin_chromium/chromium_host.cpp
)
target_compile_definitions(lvt_chromium_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(lvt_chromium_host PRIVATE WIL::WIL advapi32)
set_target_properties(lvt_chromium_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${LVT_RUNTIME_OUTPUT_DIR}/plugins/chromium
)
lvt_add_version_resource(lvt_chromium_host
    "lvt Chromium native messaging host"
    "lvt_chromium_host" "lvt_chromium_host.exe" VFT_APP)

# Copy Chromium extension files to output
add_custom_command(TARGET lvt_chromium_plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/src/plugin_chromium/extension"
        "${LVT_RUNTIME_OUTPUT_DIR}/plugins/chromium/extension"
    COMMENT "Copying Chromium extension files"
)
endif()

# lvt Viewer: copy the TAP DLLs and plugins lvt.exe needs, alongside it.
# Deliberately placed here rather than in the LVT_BUILD_VIEWER block above:
# if(TARGET lvt_tap) and friends can only see targets CMake has already
# processed by this point in the file, so this has to come after every
# add_library()/add_executable() call it references.
if(LVT_BUILD_VIEWER AND LVT_BUILD_TOOL)
    foreach(_tap lvt_tap lvt_wpf_tap lvt_winforms_tap)
        if(TARGET ${_tap})
            add_custom_command(TARGET lvt_viewer POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${_tap}>" "${LVT_VIEWER_OUTPUT_DIR}/"
                COMMENT "Copying ${_tap} next to the viewer"
            )
            add_dependencies(lvt_viewer ${_tap})
        endif()
    endforeach()
    unset(_tap)

    if(TARGET lvt_wpf_tap AND LVT_BUILD_MANAGED)
        add_custom_command(TARGET lvt_viewer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LVT_RUNTIME_OUTPUT_DIR}/LvtWpfTap.dll" "${LVT_VIEWER_OUTPUT_DIR}/LvtWpfTap.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LVT_RUNTIME_OUTPUT_DIR}/LvtWpfTap.runtimeconfig.json"
                "${LVT_VIEWER_OUTPUT_DIR}/LvtWpfTap.runtimeconfig.json"
            COMMENT "Copying the managed WPF tree walker assembly next to the viewer"
        )
    endif()

    if(TARGET lvt_winforms_tap AND LVT_BUILD_MANAGED)
        add_custom_command(TARGET lvt_viewer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LVT_RUNTIME_OUTPUT_DIR}/LvtWinFormsTap.dll" "${LVT_VIEWER_OUTPUT_DIR}/LvtWinFormsTap.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LVT_RUNTIME_OUTPUT_DIR}/LvtWinFormsTap.runtimeconfig.json"
                "${LVT_VIEWER_OUTPUT_DIR}/LvtWinFormsTap.runtimeconfig.json"
            COMMENT "Copying the managed WinForms tree walker assembly next to the viewer"
        )
    endif()

    foreach(_plugin lvt_avalonia_plugin lvt_chromium_plugin)
        if(TARGET ${_plugin})
            add_custom_command(TARGET lvt_viewer POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${LVT_VIEWER_OUTPUT_DIR}/plugins"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "$<TARGET_FILE:${_plugin}>" "${LVT_VIEWER_OUTPUT_DIR}/plugins/"
                COMMENT "Copying ${_plugin} next to the viewer"
            )
            add_dependencies(lvt_viewer ${_plugin})
        endif()
    endforeach()
    unset(_plugin)

    if(TARGET lvt_avalonia_tap)
        add_custom_command(TARGET lvt_viewer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${LVT_VIEWER_OUTPUT_DIR}/plugins/avalonia"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:lvt_avalonia_tap>" "${LVT_VIEWER_OUTPUT_DIR}/plugins/avalonia/"
            COMMENT "Copying the Avalonia TAP DLL next to the viewer"
        )
        add_dependencies(lvt_viewer lvt_avalonia_tap)
        if(LVT_BUILD_MANAGED)
            add_custom_command(TARGET lvt_viewer POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${LVT_RUNTIME_OUTPUT_DIR}/plugins/avalonia/LvtAvaloniaTreeWalker.dll"
                    "${LVT_VIEWER_OUTPUT_DIR}/plugins/avalonia/LvtAvaloniaTreeWalker.dll"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${LVT_RUNTIME_OUTPUT_DIR}/plugins/avalonia/LvtAvaloniaTreeWalker.runtimeconfig.json"
                    "${LVT_VIEWER_OUTPUT_DIR}/plugins/avalonia/LvtAvaloniaTreeWalker.runtimeconfig.json"
                COMMENT "Copying the managed Avalonia tree walker assembly next to the viewer"
            )
        endif()
    endif()

    if(TARGET lvt_chromium_plugin)
        add_custom_command(TARGET lvt_viewer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${LVT_RUNTIME_OUTPUT_DIR}/plugins/chromium" "${LVT_VIEWER_OUTPUT_DIR}/plugins/chromium"
            COMMENT "Copying the Chromium native host + extension next to the viewer"
        )
    endif()
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

# Compile-only guard: links lvt_core with no include dirs of its own, so it sees
# only what lvt_core propagates. Catches a public header depending on something
# lvt_core links PRIVATE before a downstream consumer does.
add_executable(lvt_public_headers_test tests/public_headers_test.cpp)
target_compile_definitions(lvt_public_headers_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX WINRT_LEAN_AND_MEAN)
target_link_libraries(lvt_public_headers_test PRIVATE lvt_core)

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

# Native Win32/Common Controls sample app used by integration tests and future
# property-editing coverage. It deliberately builds for the active CMake
# architecture, so the same fixture source is usable in x64 and x86 test builds.
add_executable(lvt_native_controls_fixture WIN32
    tests/apps/NativeControlsFixture/main.cpp
    tests/apps/NativeControlsFixture/NativeControlsFixture.rc
)
target_compile_definitions(lvt_native_controls_fixture PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
)
target_link_libraries(lvt_native_controls_fixture PRIVATE comctl32)
target_link_options(lvt_native_controls_fixture PRIVATE "/MANIFEST:NO")

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
    "NATIVE_CONTROLS_FIXTURE_EXE_PATH=\"$<TARGET_FILE:lvt_native_controls_fixture>\""
    # Lets tests assert on repository files, e.g. that the packaged skill has
    # not drifted from the one this repo's own agent reads.
    "LVT_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\""
)
target_link_libraries(lvt_integration_tests PRIVATE
    GTest::gtest GTest::gtest_main
    WIL::WIL nlohmann_json::nlohmann_json
    comctl32
)
target_link_options(lvt_integration_tests PRIVATE "/MANIFEST:NO")
set(LVT_INTEGRATION_DEPS
    wpf_sample_app
    winui3_sample_app
    lvt_native_controls_fixture
)
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
# --- MCP server tests ------------------------------------------------------
# A separate binary because it needs a live JSON-RPC conversation over pipes
# rather than the one-shot commands the other integration tests run, and
# because it only exists when the MCP server was built.
if(LVT_ENABLE_MCP AND LVT_BUILD_TOOL)
    add_executable(lvt_mcp_tests tests/mcp_tests.cpp)
    target_include_directories(lvt_mcp_tests PRIVATE src)
    target_compile_definitions(lvt_mcp_tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX
        WINUI3_SAMPLE_EXE_PATH="${WINUI3_SAMPLE_EXE_ESCAPED}"
    )
    target_link_libraries(lvt_mcp_tests PRIVATE
        GTest::gtest GTest::gtest_main
        WIL::WIL
        nlohmann_json::nlohmann_json
    )
    add_dependencies(lvt_mcp_tests lvt)
    if(TARGET lvt_tap)
        add_dependencies(lvt_mcp_tests lvt_tap)
    endif()
    if(TARGET winui3_sample_app)
        add_dependencies(lvt_mcp_tests winui3_sample_app)
    endif()
    add_test(NAME mcp_tests COMMAND lvt_mcp_tests)
endif()

add_executable(lvt_chromium_tests    tests/chromium_tests.cpp
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
install(FILES ${LVT_PUBLIC_PROVIDER_HEADERS}
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/lvt/providers")

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
