#pragma once
#include <Windows.h>
#include <string>
#include <vector>
#include <cstdint>

namespace lvt {

enum class Architecture { unknown, x64, arm64 };

const char* architecture_name(Architecture arch);
Architecture get_host_architecture();
Architecture detect_process_architecture(DWORD pid);

struct TargetInfo {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::string processName;
    Architecture architecture = Architecture::unknown;
};

struct WindowMatch {
    HWND hwnd;
    DWORD pid;
    std::string processName;
    std::string windowTitle;
};

// Resolve a target from either an HWND or PID.
TargetInfo resolve_target(HWND hwnd, DWORD pid);

// Find windows by process name (e.g. "notepad.exe" or "notepad").
std::vector<WindowMatch> find_by_process_name(const std::string& name);

// Find windows by title substring (case-insensitive).
std::vector<WindowMatch> find_by_title(const std::string& title);

} // namespace lvt
