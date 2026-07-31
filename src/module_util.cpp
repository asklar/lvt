#include "module_util.h"
#include "target.h"

#include <Windows.h>
#include <mutex>

namespace lvt {

namespace {

std::mutex g_tapDirMutex;
std::wstring g_tapDirOverride;

std::wstring parent_directory(std::wstring path) {
    const auto pos = path.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        path.resize(pos);
    return path;
}

// Anchor whose address is guaranteed to live in the module containing lvt_core.
void module_anchor() {}

} // namespace

std::wstring get_module_dir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&module_anchor),
            &self)) {
        self = nullptr; // fall back to the process image below
    }

    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
        if (copied == 0)
            return {};
        if (copied < path.size()) {
            path.resize(copied);
            break;
        }
        path.resize(path.size() * 2); // ERROR_INSUFFICIENT_BUFFER: truncated
    }
    return parent_directory(std::move(path));
}

void set_tap_directory(const std::wstring& dir) {
    std::lock_guard<std::mutex> lock(g_tapDirMutex);
    g_tapDirOverride = dir;
}

std::wstring get_tap_directory() {
    {
        std::lock_guard<std::mutex> lock(g_tapDirMutex);
        if (!g_tapDirOverride.empty())
            return g_tapDirOverride;
    }

    std::wstring buffer(MAX_PATH, L'\0');
    DWORD len = GetEnvironmentVariableW(L"LVT_TAP_DIR", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len > buffer.size()) {
        buffer.resize(len);
        len = GetEnvironmentVariableW(L"LVT_TAP_DIR", buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    if (len > 0) {
        buffer.resize(len);
        return buffer;
    }

    return get_module_dir();
}

std::wstring tap_dll_name(const std::wstring& stem) {
    const wchar_t* arch = (get_host_architecture() == Architecture::arm64) ? L"arm64"
                        : (sizeof(void*) == 4)                            ? L"x86"
                                                                          : L"x64";
    return stem + L"_" + arch + L".dll";
}

std::wstring tap_dll_path(const std::wstring& stem) {
    return get_tap_directory() + L"\\" + tap_dll_name(stem);
}

} // namespace lvt
