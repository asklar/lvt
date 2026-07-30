#include "winforms_inject.h"
#include "../debug.h"
#include "../target.h"
#include "../module_util.h"

#include <Windows.h>
#include <objbase.h>
#include <sddl.h>
#include <wil/resource.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

namespace lvt {

static std::wstring make_pipe_name() {
    GUID guid;
    CoCreateGuid(&guid);
    wchar_t buf[96];
    swprintf_s(buf, L"\\\\.\\pipe\\lvt_winforms_%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
        guid.Data1, guid.Data2, guid.Data3,
        guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
        guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return buf;
}


static std::string sanitize(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (static_cast<unsigned char>(c) >= 0x20 || c == '\t')
            r += c;
    }
    return r;
}

static uintptr_t parse_hwnd_value(const std::string& text) {
    if (text.empty()) return 0;
    size_t index = 0;
    int base = 16;
    if (text.starts_with("0x") || text.starts_with("0X"))
        index = 2;
    try {
        return static_cast<uintptr_t>(std::stoull(text.substr(index), nullptr, base));
    } catch (...) {
        return 0;
    }
}

static std::string simple_type_name(const std::string& fullType) {
    auto lastDot = fullType.rfind('.');
    return lastDot == std::string::npos ? fullType : fullType.substr(lastDot + 1);
}

static void index_by_hwnd(Element& el, std::unordered_map<uintptr_t, Element*>& index) {
    if (el.nativeHandle != 0)
        index[el.nativeHandle] = &el;
    auto it = el.properties.find("hwnd");
    if (it != el.properties.end()) {
        auto hwnd = parse_hwnd_value(it->second);
        if (hwnd != 0)
            index[hwnd] = &el;
    }
    for (auto& child : el.children)
        index_by_hwnd(child, index);
}

static bool apply_control_node(const json& node, std::unordered_map<uintptr_t, Element*>& index) {
    if (!node.is_object()) return false;

    bool applied = false;
    auto hwnd = parse_hwnd_value(node.value("hwnd", ""));
    auto found = index.find(hwnd);
    if (found != index.end() && found->second) {
        Element& el = *found->second;
        auto fullType = sanitize(node.value("type", ""));
        auto name = sanitize(node.value("name", ""));
        auto text = sanitize(node.value("text", ""));

        el.framework = "winforms";
        if (!fullType.empty()) {
            el.properties["winforms.type"] = fullType;
            el.type = simple_type_name(fullType);
        }
        if (!name.empty()) {
            el.properties["name"] = name;
            el.properties["winforms.name"] = name;
        }
        if (!text.empty())
            el.properties["winforms.text"] = text;
        if (node.contains("visible") && node["visible"].is_boolean())
            el.properties["winforms.visible"] = node["visible"].get<bool>() ? "true" : "false";
        if (node.contains("enabled") && node["enabled"].is_boolean())
            el.properties["winforms.enabled"] = node["enabled"].get<bool>() ? "true" : "false";
        if (node.contains("readOnly") && node["readOnly"].is_boolean())
            el.properties["readOnly"] = node["readOnly"].get<bool>() ? "true" : "false";
        if (node.contains("autoSize") && node["autoSize"].is_boolean())
            el.properties["autoSize"] = node["autoSize"].get<bool>() ? "true" : "false";
        applied = true;
    }

    if (node.contains("children") && node["children"].is_array()) {
        for (auto& child : node["children"])
            applied = apply_control_node(child, index) || applied;
    }
    return applied;
}

bool apply_winforms_control_json(Element& root, const std::string& jsonText) {
    if (jsonText.empty()) return false;
    json treeJson;
    try {
        treeJson = json::parse(jsonText);
    } catch (const json::parse_error&) {
        return false;
    }

    std::unordered_map<uintptr_t, Element*> index;
    index_by_hwnd(root, index);

    bool applied = false;
    if (treeJson.is_array()) {
        for (auto& node : treeJson)
            applied = apply_control_node(node, index) || applied;
    } else if (treeJson.is_object()) {
        applied = apply_control_node(treeJson, index);
    }
    return applied;
}

static bool write_pipe_name_file(const std::wstring& dir, const std::wstring& pipeName) {
    std::wstring path = dir + L"\\lvt_winforms_pipe.txt";
    int len = WideCharToMultiByte(CP_UTF8, 0, pipeName.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, pipeName.c_str(), -1, utf8.data(), len, nullptr, nullptr);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(utf8.data(), utf8.size());
    return true;
}

static bool inject_dll(DWORD pid, const std::wstring& dllPath) {
    wil::unique_handle proc(OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
        FALSE, pid));
    if (!proc) {
        if (g_debug) fprintf(stderr, "lvt: failed to open WinForms target process %lu (error %lu)\n", pid, GetLastError());
        return false;
    }

    size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(proc.get(), nullptr, pathBytes, MEM_COMMIT, PAGE_READWRITE);
    if (!remoteMem) {
        if (g_debug) fprintf(stderr, "lvt: WinForms VirtualAllocEx failed (error %lu)\n", GetLastError());
        return false;
    }

    if (!WriteProcessMemory(proc.get(), remoteMem, dllPath.c_str(), pathBytes, nullptr)) {
        if (g_debug) fprintf(stderr, "lvt: WinForms WriteProcessMemory failed (error %lu)\n", GetLastError());
        VirtualFreeEx(proc.get(), remoteMem, 0, MEM_RELEASE);
        return false;
    }

    auto loadLibAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

    wil::unique_handle thread(CreateRemoteThread(proc.get(), nullptr, 0, loadLibAddr, remoteMem, 0, nullptr));
    if (!thread) {
        if (g_debug) fprintf(stderr, "lvt: WinForms CreateRemoteThread failed (error %lu)\n", GetLastError());
        VirtualFreeEx(proc.get(), remoteMem, 0, MEM_RELEASE);
        return false;
    }

    WaitForSingleObject(thread.get(), 5000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread.get(), &exitCode);
    VirtualFreeEx(proc.get(), remoteMem, 0, MEM_RELEASE);

    if (exitCode == 0) {
        if (g_debug) fprintf(stderr, "lvt: WinForms LoadLibraryW failed in target process\n");
        return false;
    }
    return true;
}

bool inject_and_collect_winforms_tree(Element& root, HWND /*hwnd*/, DWORD pid) {
    wil::unique_handle proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (proc) {
        BOOL isWow64 = FALSE;
        if (IsWow64Process(proc.get(), &isWow64) && isWow64) {
#if defined(_M_X64) || defined(_M_ARM64)
            if (g_debug) fprintf(stderr, "lvt: WinForms target is 32-bit (WoW64); skipping managed enrichment\n");
            return false;
#endif
        }
    }

    std::wstring exeDir = get_tap_directory();
    std::wstring tapDll = tap_dll_path(L"lvt_winforms_tap");
    std::wstring managedDll = exeDir + L"\\LvtWinFormsTap.dll";

    if (GetFileAttributesW(tapDll.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(managedDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (g_debug) fprintf(stderr, "lvt: WinForms TAP binaries not found\n");
        return false;
    }

    std::wstring pipeName = make_pipe_name();
    if (!write_pipe_name_file(exeDir, pipeName))
        return false;

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(A;;GRGW;;;WD)(A;;GRGW;;;AC)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);

    HANDLE pipe = CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 0, 1024 * 1024, 10000, &sa);
    LocalFree(sa.lpSecurityDescriptor);

    if (pipe == INVALID_HANDLE_VALUE) {
        DeleteFileW((exeDir + L"\\lvt_winforms_pipe.txt").c_str());
        return false;
    }

    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ConnectNamedPipe(pipe, &ov);
    DWORD connectErr = GetLastError();

    if (!inject_dll(pid, tapDll)) {
        CancelIo(pipe);
        CloseHandle(ov.hEvent);
        CloseHandle(pipe);
        DeleteFileW((exeDir + L"\\lvt_winforms_pipe.txt").c_str());
        return false;
    }

    if (connectErr == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 15000) != WAIT_OBJECT_0) {
            CancelIo(pipe);
            CloseHandle(ov.hEvent);
            CloseHandle(pipe);
            DeleteFileW((exeDir + L"\\lvt_winforms_pipe.txt").c_str());
            return false;
        }
    } else if (connectErr != ERROR_PIPE_CONNECTED) {
        CloseHandle(ov.hEvent);
        CloseHandle(pipe);
        DeleteFileW((exeDir + L"\\lvt_winforms_pipe.txt").c_str());
        return false;
    }
    CloseHandle(ov.hEvent);

    std::string data;
    char buf[4096];
    DWORD bytesRead = 0;
    OVERLAPPED readOv = {};
    readOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (;;) {
        ResetEvent(readOv.hEvent);
        BOOL ok = ReadFile(pipe, buf, sizeof(buf), &bytesRead, &readOv);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                if (WaitForSingleObject(readOv.hEvent, 15000) != WAIT_OBJECT_0) {
                    CancelIo(pipe);
                    break;
                }
                if (!GetOverlappedResult(pipe, &readOv, &bytesRead, FALSE) || bytesRead == 0)
                    break;
            } else {
                break;
            }
        } else if (bytesRead == 0) {
            break;
        }
        data.append(buf, bytesRead);
    }
    CloseHandle(readOv.hEvent);
    CloseHandle(pipe);
    DeleteFileW((exeDir + L"\\lvt_winforms_pipe.txt").c_str());

    if (data.empty())
        return false;
    return apply_winforms_control_json(root, data);
}

} // namespace lvt
