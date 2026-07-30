#pragma once
#include <string>

namespace lvt {

// Directory containing the binary that lvt_core is linked into.
//
// This deliberately does NOT use the process executable path: when lvt_core is
// consumed as a library, the host .exe lives somewhere unrelated to the lvt
// payload, so the TAP DLLs must be located relative to lvt's own module.
std::wstring get_module_dir();

// Directory searched for the injectable TAP DLLs. Resolution order:
//   1. an explicit set_tap_directory() override
//   2. the LVT_TAP_DIR environment variable
//   3. get_module_dir()
std::wstring get_tap_directory();

// Override the TAP DLL search directory. Pass an empty string to fall back to
// the default resolution order.
void set_tap_directory(const std::wstring& dir);

// Architecture-suffixed TAP DLL name for the current host, e.g.
// tap_dll_name(L"lvt_tap") -> L"lvt_tap_x64.dll"
std::wstring tap_dll_name(const std::wstring& stem);

// Full path to a TAP DLL: get_tap_directory() + tap_dll_name(stem).
std::wstring tap_dll_path(const std::wstring& stem);

} // namespace lvt
