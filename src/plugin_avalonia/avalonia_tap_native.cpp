// avalonia_tap_native.cpp — Native DLL injected into Avalonia target process.
// Hosts the .NET CLR via hostfxr and calls managed AvaloniaTreeWalker.CollectTree().
// Avalonia apps are CoreCLR-based, so only the hostfxr path is needed.

#include "managed_host_abi.h"

#include <Windows.h>
#include <wil/resource.h>

#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

static FILE* g_logFile = nullptr;

static void LogMsg(const char* fmt, ...) {
    if (!g_logFile) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        wcscat_s(tmp, L"lvt_avalonia_tap.log");
        g_logFile = _wfopen(tmp, L"a");
        if (!g_logFile) return;
    }
    fprintf(g_logFile, "[pid=%lu tid=%lu] ",
            GetCurrentProcessId(), GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFile, fmt, ap);
    va_end(ap);
    fprintf(g_logFile, "\n");
    fflush(g_logFile);
}

static void CloseLog() {
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

static DWORD WINAPI WorkerThread(LPVOID);

static std::wstring GetDllDirectory() {
    wchar_t path[MAX_PATH];
    HMODULE hm = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LogMsg), &hm);
    GetModuleFileNameW(hm, path, MAX_PATH);
    std::wstring dir(path);
    auto pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) dir.resize(pos);
    return dir;
}

// Read pipe name from a sidecar file written by lvt plugin before injection.
static std::wstring ReadPipeName() {
    std::wstring dir = GetDllDirectory();
    std::wstring path =
        dir + L"\\lvt_avalonia_pipe_" +
        std::to_wstring(GetCurrentProcessId()) + L".txt";

    wil::unique_hfile hFile(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr));
    if (!hFile) {
        LogMsg("Failed to open pipe name file: %lu", GetLastError());
        return {};
    }

    char buf[256]{};
    DWORD bytesRead = 0;
    if (!ReadFile(
            hFile.get(), buf, sizeof(buf) - 1, &bytesRead, nullptr) ||
        bytesRead == 0) {
        LogMsg("Failed to read pipe name file: %lu", GetLastError());
        return {};
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, bytesRead, nullptr, 0);
    if (wlen <= 0) {
        LogMsg("Pipe name file is not valid UTF-8: %lu", GetLastError());
        return {};
    }
    std::wstring result(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf, bytesRead, result.data(), wlen);

    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
        result.pop_back();

    LogMsg("Pipe name read: %ls", result.c_str());
    return result;
}

static HMODULE LoadHostFxr() {
    if (HMODULE loaded = GetModuleHandleW(L"hostfxr.dll"))
        return loaded;

    wchar_t programFiles[MAX_PATH];
    if (GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH) == 0)
        return nullptr;
    const std::wstring fxrDirectory =
        std::wstring(programFiles) + L"\\dotnet\\host\\fxr";

    WIN32_FIND_DATAW findData{};
    wil::unique_hfind find(
        FindFirstFileW((fxrDirectory + L"\\*").c_str(), &findData));
    std::wstring hostFxrPath;
    unsigned latestMajor = 0, latestMinor = 0, latestPatch = 0;
    if (find) {
        do {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                findData.cFileName[0] != L'.') {
                unsigned major = 0, minor = 0, patch = 0;
                if (swscanf_s(
                        findData.cFileName, L"%u.%u.%u",
                        &major, &minor, &patch) != 3) {
                    continue;
                }
                if (hostFxrPath.empty() ||
                    std::tie(major, minor, patch) >
                        std::tie(latestMajor, latestMinor, latestPatch)) {
                    latestMajor = major;
                    latestMinor = minor;
                    latestPatch = patch;
                    hostFxrPath =
                        fxrDirectory + L"\\" + findData.cFileName +
                        L"\\hostfxr.dll";
                }
            }
        } while (FindNextFileW(find.get(), &findData));
    }
    if (hostFxrPath.empty())
        return nullptr;

    // hostfxr owns runtime resolution for the remainder of the target process.
    // Keep a fallback-loaded copy resident just like the other managed TAPs.
    HMODULE hostFxr = LoadLibraryW(hostFxrPath.c_str());
    LogMsg("Loaded hostfxr from: %ls -> %p", hostFxrPath.c_str(), hostFxr);
    return hostFxr;
}

static int LoadedCoreClrMajorVersion() {
    HMODULE coreClr = GetModuleHandleW(L"coreclr.dll");
    if (!coreClr)
        return 0;

    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(coreClr, path, MAX_PATH) == 0)
        return 0;
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path, &ignored);
    if (bytes == 0)
        return 0;
    std::vector<unsigned char> version(bytes);
    if (!GetFileVersionInfoW(path, 0, bytes, version.data()))
        return 0;
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixedBytes = 0;
    if (!VerQueryValueW(
            version.data(), L"\\", reinterpret_cast<void**>(&fixed),
            &fixedBytes) ||
        !fixed || fixedBytes < sizeof(VS_FIXEDFILEINFO)) {
        return 0;
    }
    return HIWORD(fixed->dwProductVersionMS);
}

static bool TryNetCore(
    const std::wstring& assemblyPath, const std::wstring& pipeName) {
    using namespace lvt::managed_host_abi;

    const int coreClrMajor = LoadedCoreClrMajorVersion();
    if (coreClrMajor > 0 && coreClrMajor < 8) {
        LogMsg(
            "CoreCLR %d is unsupported; Avalonia TAP requires .NET 8 or newer",
            coreClrMajor);
        return false;
    }
    if (coreClrMajor > 0)
        LogMsg("Hosting through the loaded CoreCLR %d", coreClrMajor);

    HMODULE hHostfxr = LoadHostFxr();
    if (!hHostfxr) {
        LogMsg("Could not find hostfxr.dll");
        return false;
    }

    auto init_fn =
        reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
            GetProcAddress(
                hHostfxr, "hostfxr_initialize_for_runtime_config"));
    auto get_delegate_fn =
        reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
            GetProcAddress(hHostfxr, "hostfxr_get_runtime_delegate"));
    auto close_fn = reinterpret_cast<hostfxr_close_fn>(
        GetProcAddress(hHostfxr, "hostfxr_close"));

    if (!init_fn || !get_delegate_fn || !close_fn) {
        LogMsg("Failed to get hostfxr exports");
        return false;
    }

    std::wstring configPath = assemblyPath;
    const auto dotPos = configPath.rfind(L'.');
    if (dotPos != std::wstring::npos)
        configPath.resize(dotPos);
    configPath += L".runtimeconfig.json";
    if (GetFileAttributesW(configPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogMsg("Runtime config not found: %ls", configPath.c_str());
        return false;
    }

    hostfxr_handle hostContext = nullptr;
    int rc = init_fn(configPath.c_str(), nullptr, &hostContext);
    LogMsg(
        "hostfxr_initialize_for_runtime_config returned 0x%08X, context=%p",
        rc, hostContext);

    if (rc < 0 || !hostContext) {
        LogMsg("hostfxr init failed");
        if (hostContext)
            close_fn(hostContext);
        return false;
    }

    void* loadAndGet = nullptr;
    rc = get_delegate_fn(
        hostContext,
        hostfxr_delegate_type::load_assembly_and_get_function_pointer,
        &loadAndGet);
    LogMsg(
        "hostfxr_get_runtime_delegate("
        "hdt_load_assembly_and_get_function_pointer) returned 0x%08X",
        rc);

    if (rc < 0 || !loadAndGet) {
        close_fn(hostContext);
        return false;
    }

    auto loadAssembly =
        reinterpret_cast<load_assembly_and_get_function_pointer_fn>(
            loadAndGet);
    component_entry_point_fn collectTree = nullptr;

    rc = loadAssembly(
        assemblyPath.c_str(),
        L"LvtAvaloniaTreeWalker.AvaloniaTreeWalker, LvtAvaloniaTreeWalker",
        L"CollectTree",
        nullptr,
        nullptr,
        reinterpret_cast<void**>(&collectTree));

    LogMsg(
        "load_assembly_and_get_function_pointer returned 0x%08X, fn=%p",
        rc, collectTree);

    if (rc < 0 || !collectTree) {
        close_fn(hostContext);
        return false;
    }

    const int retVal = collectTree(
        const_cast<wchar_t*>(pipeName.c_str()),
        static_cast<int>(pipeName.size() * sizeof(wchar_t)));
    LogMsg("CollectTree returned %d", retVal);

    close_fn(hostContext);
    return retVal == 0;
}

static DWORD WINAPI WorkerThread(LPVOID lpParameter) {
    HMODULE hSelf = reinterpret_cast<HMODULE>(lpParameter);
    LogMsg("WorkerThread starting");

    std::wstring pipeName = ReadPipeName();
    if (pipeName.empty()) {
        LogMsg("No pipe name, exiting");
        CloseLog();
        if (hSelf)
            FreeLibraryAndExitThread(hSelf, 1);
        return 1;
    }

    std::wstring dir = GetDllDirectory();
    std::wstring assemblyPath = dir + L"\\LvtAvaloniaTreeWalker.dll";

    if (GetFileAttributesW(assemblyPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogMsg("Managed assembly not found: %ls", assemblyPath.c_str());
        CloseLog();
        if (hSelf)
            FreeLibraryAndExitThread(hSelf, 1);
        return 1;
    }

    DWORD result = 1;
    LogMsg("Attempting .NET Core hosting...");
    if (TryNetCore(assemblyPath, pipeName)) {
        LogMsg("Tree collection succeeded via .NET Core");
        result = 0;
    } else {
        LogMsg("CLR hosting failed");
    }

    CloseLog();
    if (hSelf)
        FreeLibraryAndExitThread(hSelf, result);
    return result;
}

extern "C" {

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);

        HANDLE thread = CreateThread(
            nullptr, 0, WorkerThread, reinterpret_cast<LPVOID>(hMod),
            0, nullptr);
        if (!thread)
            return FALSE;
        CloseHandle(thread);
    }
    return TRUE;
}

} // extern "C"
