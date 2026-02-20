#pragma once
#include <Windows.h>
#include <string>
#include <cstdint>

namespace lvt {

struct TargetInfo {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::string processName;
};

// Resolve a target from either an HWND or PID.
// If pid is nonzero, heuristically finds the main window.
TargetInfo resolve_target(HWND hwnd, DWORD pid);

} // namespace lvt
