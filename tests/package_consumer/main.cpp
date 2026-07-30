// Smoke test for the installed lvt package.
//
// Built as a standalone CMake project against an installed lvt, so it only
// sees what the package actually exports. It checks the parts of the packaging
// contract that are easy to break without noticing:
//
//   * the public headers are reachable as <lvt/...>
//   * lvt::core links, including its private wil/nlohmann_json dependencies
//   * lvt_config.h reports what was compiled in
//   * lvt_copy_tap_dlls() put the injectable TAP DLLs next to this executable,
//     which is where a statically linked lvt_core looks for them
//
// Deliberately does not require a GUI application to be running, so it is safe
// on a headless CI runner. Walking a live tree is covered by the integration
// tests instead.

#include <lvt/framework_detector.h>
#include <lvt/json_serializer.h>
#include <lvt/lvt_config.h>
#include <lvt/module_util.h>
#include <lvt/target.h>
#include <lvt/tree_builder.h>

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

int main() {
    int failures = 0;

    std::printf("compiled-in: xaml=%d winui3=%d wpf=%d winforms=%d avalonia=%d chromium=%d managed=%d\n",
                LVT_ENABLE_XAML, LVT_ENABLE_WINUI3, LVT_ENABLE_WPF, LVT_ENABLE_WINFORMS,
                LVT_ENABLE_AVALONIA, LVT_ENABLE_CHROMIUM, LVT_WITH_MANAGED);

    const fs::path tapDir = lvt::get_tap_directory();
    std::printf("tap directory: %s\n", tapDir.string().c_str());

    if (tapDir.empty()) {
        std::printf("FAIL: get_tap_directory() returned nothing\n");
        ++failures;
    }

    // lvt_copy_tap_dlls() should have staged these beside the executable.
    std::vector<std::wstring> expected{L"lvt_tap"};
#if LVT_ENABLE_WPF
    expected.push_back(L"lvt_wpf_tap");
#endif
#if LVT_ENABLE_WINFORMS
    expected.push_back(L"lvt_winforms_tap");
#endif

    for (const auto& stem : expected) {
        const fs::path dll = tapDir / lvt::tap_dll_name(stem);
        if (fs::exists(dll)) {
            std::printf("ok:   %s\n", dll.filename().string().c_str());
        } else {
            std::printf("FAIL: missing %s\n", dll.string().c_str());
            ++failures;
        }
    }

    // Exercise the API surface against the desktop window. This must not depend
    // on any particular application running.
    const HWND desktop = GetDesktopWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(desktop, &pid);

    auto target = lvt::resolve_target(desktop, pid);

    // Detection is passive, so it is safe to call here.
    auto frameworks = lvt::detect_frameworks(target.hwnd, target.pid);
    std::printf("ok:   detected %zu framework(s) on the desktop window\n", frameworks.size());

    std::vector<std::string> names;
    for (const auto& fi : frameworks)
        names.push_back(lvt::framework_display_name(fi));

    // Build with no frameworks so only the Win32 walk runs. Passing the detected
    // list would make the providers inject into whatever owns the desktop
    // window, which is both unnecessary here and flaky on a CI runner; live
    // injection is covered by the integration tests.
    auto tree = lvt::build_tree(target.hwnd, target.pid, /*frameworks=*/{}, 1);
    lvt::assign_element_ids(tree);
    auto json = lvt::serialize_to_json(tree, target.hwnd, target.pid, target.processName, names);

    if (json.empty()) {
        std::printf("FAIL: serialize_to_json produced nothing\n");
        ++failures;
    } else {
        std::printf("ok:   walked root '%s', serialized %zu bytes\n",
                    tree.type.c_str(), json.size());
    }

    if (failures)
        std::printf("\nFAILED (%d)\n", failures);
    else
        std::printf("\nPASSED\n");
    return failures == 0 ? 0 : 1;
}
