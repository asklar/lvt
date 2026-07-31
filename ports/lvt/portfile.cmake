vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO asklar/lvt
    REF "v${VERSION}"
    SHA512 0
    HEAD_REF main
)

vcpkg_check_features(
    OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        xaml     LVT_ENABLE_XAML
        winui3   LVT_ENABLE_WINUI3
        wpf      LVT_ENABLE_WPF
        winforms LVT_ENABLE_WINFORMS
        avalonia LVT_ENABLE_AVALONIA
        chromium LVT_ENABLE_CHROMIUM
        tools    LVT_BUILD_TOOL
)

# WinUI 3 enrichment (TransformToVisual, TextBlock.Text) needs C++/WinRT headers
# projected from the Windows App SDK winmd. There is no vcpkg port for the
# Windows App SDK, so fetch the NuGet package - a plain zip - and hand the build
# the directory holding the .winmd files.
#
# The generator itself comes from the cppwinrt port rather than the Windows SDK,
# so that the Microsoft.* projection we generate and the winrt/base.h we compile
# it against are always produced by the same cppwinrt version. It also means a
# consumer that already depends on cppwinrt picks the version for both, since
# vcpkg installs only one version of a port per triplet.
set(LVT_CPPWINRT_OPTIONS "")
if("winui3" IN_LIST FEATURES OR "xaml" IN_LIST FEATURES)
    list(APPEND LVT_CPPWINRT_OPTIONS
        "-DLVT_CPPWINRT_EXE=${CURRENT_INSTALLED_DIR}/tools/cppwinrt/cppwinrt.exe")
endif()

if("winui3" IN_LIST FEATURES)
    set(WASDK_VERSION "1.5.240607001")
    vcpkg_download_distfile(WASDK_NUPKG
        URLS "https://api.nuget.org/v3-flatcontainer/microsoft.windowsappsdk/${WASDK_VERSION}/microsoft.windowsappsdk.${WASDK_VERSION}.nupkg"
        # A .nupkg is a zip; name it .zip so the extractor recognises it.
        FILENAME "microsoft.windowsappsdk.${WASDK_VERSION}.zip"
        SHA512 5012c63c06dd1fdd5d5663ee7a6816cd7ff59ac8d1f540e6f6b0786ec07846ee5b9234b96365d2597257d607cb2afe6b429551f9f42e5a3e436e270a823898f7
    )
    vcpkg_extract_source_archive(WASDK_DIR
        ARCHIVE "${WASDK_NUPKG}"
        NO_REMOVE_ONE_LEVEL
    )
    list(APPEND LVT_CPPWINRT_OPTIONS
        "-DLVT_WASDK_WINMD_DIR=${WASDK_DIR}/lib/uap10.0")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        ${LVT_CPPWINRT_OPTIONS}
        -DLVT_BUILD_TESTS=OFF
        # Every dotnet/msbuild invocation sits behind this. vcpkg builds have no
        # network access and no .NET SDK, so the managed tree-walker assemblies
        # for WPF/WinForms/Avalonia are not built; their native halves are.
        -DLVT_BUILD_MANAGED=OFF
        -DLVT_INSTALL_TOOLSDIR=tools/${PORT}
    MAYBE_UNUSED_VARIABLES
        LVT_INSTALL_TOOLSDIR
        LVT_CPPWINRT_EXE
        LVT_WASDK_WINMD_DIR
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME lvt CONFIG_PATH share/lvt)

# The tool and the injected TAP DLLs are only meaningful in a release flavour.
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/debug/tools"
)

if("tools" IN_LIST FEATURES)
    vcpkg_copy_tool_dependencies("${CURRENT_PACKAGES_DIR}/tools/${PORT}")
endif()

# The TAP DLLs are injected into a foreign process, never linked against, so
# they ship without import libraries.
set(VCPKG_POLICY_DLLS_WITHOUT_LIBS enabled)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
