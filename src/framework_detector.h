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
};

std::string framework_to_string(Framework f);

// Detect which UI frameworks are in use for the given window/process.
std::vector<Framework> detect_frameworks(HWND hwnd, DWORD pid);

} // namespace lvt
