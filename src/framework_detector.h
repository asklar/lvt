#pragma once
#include <Windows.h>
#include <string>
#include <vector>

namespace lvt {

enum class Framework {
    Win32,
    ComCtl,
    Xaml,
    WinUI3,
    Wpf,
};

struct FrameworkInfo {
    Framework type;
    std::string version; // e.g. "3.1.7.2602" for WinUI3, "6.10" for comctl
};

std::string framework_to_string(Framework f);

// Detect which UI frameworks are in use for the given window/process.
std::vector<FrameworkInfo> detect_frameworks(HWND hwnd, DWORD pid);

} // namespace lvt
