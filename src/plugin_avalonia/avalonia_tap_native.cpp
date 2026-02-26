// avalonia_tap_native.cpp — Native DLL injected into Avalonia target process.
// Hosts the .NET CLR via hostfxr and calls managed AvaloniaTreeWalker.CollectTree().
// Avalonia apps are always .NET Core/.NET 5+, so only the hostfxr path is needed.

#include <Windows.h>
#include <string>
#include <cstdio>

static void LogMsg(const char* fmt, ...) {
    static FILE* logFile = nullptr;
    if (!logFile) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        wcscat_s(tmp, L"lvt_avalonia_tap.log");
        logFile = _wfopen(tmp, L"a");
        if (!logFile) return;
    }
    fprintf(logFile, "[%lu] ", GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logFile, fmt, ap);
    va_end(ap);
    fprintf(logFile, "\n");
    fflush(logFile);
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
    std::wstring path = dir + L"\\lvt_avalonia_pipe.txt";

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        LogMsg("Failed to open pipe name file: %lu", GetLastError());
        return {};
    }

    char buf[256]{};
    DWORD bytesRead = 0;
    ReadFile(hFile, buf, sizeof(buf) - 1, &bytesRead, nullptr);
    CloseHandle(hFile);

    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, bytesRead, nullptr, 0);
    std::wstring result(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf, bytesRead, result.data(), wlen);

    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
        result.pop_back();

    LogMsg("Pipe name read: %ls", result.c_str());
    return result;
}

// Host .NET via hostfxr (Avalonia apps always use .NET Core/.NET 5+)
using hostfxr_initialize_fn = int(STDMETHODCALLTYPE*)(
    const wchar_t* runtime_config_path,
    const void* parameters,
    void** host_context_handle);
using hostfxr_get_delegate_fn = int(STDMETHODCALLTYPE*)(
    void* host_context_handle,
    int type,
    void** delegate);
using hostfxr_close_fn = int(STDMETHODCALLTYPE*)(void* host_context_handle);

static bool TryNetCore(const std::wstring& assemblyPath, const std::wstring& pipeName) {
    HMODULE hHostfxr = GetModuleHandleW(L"hostfxr.dll");
    if (!hHostfxr) {
        LogMsg("hostfxr.dll not loaded, trying to find it");
        wchar_t progFiles[MAX_PATH];
        GetEnvironmentVariableW(L"ProgramFiles", progFiles, MAX_PATH);
        std::wstring dotnetDir = std::wstring(progFiles) + L"\\dotnet\\host\\fxr";

        WIN32_FIND_DATAW fd;
        HANDLE hFind = FindFirstFileW((dotnetDir + L"\\*").c_str(), &fd);
        std::wstring latestFxr;
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY && fd.cFileName[0] != L'.') {
                    latestFxr = dotnetDir + L"\\" + fd.cFileName + L"\\hostfxr.dll";
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }

        if (!latestFxr.empty()) {
            hHostfxr = LoadLibraryW(latestFxr.c_str());
            LogMsg("Loaded hostfxr from: %ls -> %p", latestFxr.c_str(), hHostfxr);
        }
    }

    if (!hHostfxr) {
        LogMsg("Could not find hostfxr.dll");
        return false;
    }

    auto init_fn = reinterpret_cast<hostfxr_initialize_fn>(
        GetProcAddress(hHostfxr, "hostfxr_initialize_for_runtime_config"));
    auto get_delegate_fn = reinterpret_cast<hostfxr_get_delegate_fn>(
        GetProcAddress(hHostfxr, "hostfxr_get_runtime_delegate"));
    auto close_fn = reinterpret_cast<hostfxr_close_fn>(
        GetProcAddress(hHostfxr, "hostfxr_close"));

    if (!init_fn || !get_delegate_fn || !close_fn) {
        LogMsg("Failed to get hostfxr exports");
        return false;
    }

    // Find the runtimeconfig.json next to the managed assembly
    std::wstring configPath = assemblyPath;
    auto dotPos = configPath.rfind(L'.');
    if (dotPos != std::wstring::npos)
        configPath = configPath.substr(0, dotPos);
    configPath += L".runtimeconfig.json";

    void* hostContext = nullptr;
    int rc = init_fn(configPath.c_str(), nullptr, &hostContext);
    LogMsg("hostfxr_initialize_for_runtime_config returned 0x%08X, context=%p", rc, hostContext);

    // rc == 0 means success, rc == 1 means "already initialized" (which is fine)
    if (rc < 0 || !hostContext) {
        LogMsg("hostfxr init failed");
        if (hostContext) close_fn(hostContext);
        return false;
    }

    // hdt_load_assembly_and_get_function_pointer = 5
    void* loadAndGet = nullptr;
    rc = get_delegate_fn(hostContext, 5, &loadAndGet);
    LogMsg("hostfxr_get_runtime_delegate(hdt_load_assembly_and_get_function_pointer) returned 0x%08X", rc);

    if (rc < 0 || !loadAndGet) {
        close_fn(hostContext);
        return false;
    }

    using load_assembly_fn = int(STDMETHODCALLTYPE*)(
        const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, void*, void**);
    auto loadAssembly = reinterpret_cast<load_assembly_fn>(loadAndGet);

    using CollectTreeFn = int(STDMETHODCALLTYPE*)(const wchar_t*, int);
    CollectTreeFn collectTree = nullptr;

    rc = loadAssembly(
        assemblyPath.c_str(),
        L"LvtAvaloniaTreeWalker.AvaloniaTreeWalker, LvtAvaloniaTreeWalker",
        L"CollectTree",
        L"LvtAvaloniaTreeWalker.AvaloniaTreeWalker+CollectTreeDelegate, LvtAvaloniaTreeWalker",
        nullptr,
        reinterpret_cast<void**>(&collectTree));

    LogMsg("load_assembly_and_get_function_pointer returned 0x%08X, fn=%p", rc, collectTree);

    if (rc < 0 || !collectTree) {
        close_fn(hostContext);
        return false;
    }

    int retVal = collectTree(pipeName.c_str(), static_cast<int>(pipeName.size() * sizeof(wchar_t)));
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
        if (hSelf) FreeLibraryAndExitThread(hSelf, 1);
        return 1;
    }

    std::wstring dir = GetDllDirectory();
    std::wstring assemblyPath = dir + L"\\LvtAvaloniaTreeWalker.dll";

    if (GetFileAttributesW(assemblyPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogMsg("Managed assembly not found: %ls", assemblyPath.c_str());
        if (hSelf) FreeLibraryAndExitThread(hSelf, 1);
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

    if (hSelf) FreeLibraryAndExitThread(hSelf, result);
    return result;
}

extern "C" {

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        LogMsg("DllMain: DLL_PROCESS_ATTACH");

        HANDLE hThread = CreateThread(nullptr, 0, WorkerThread, reinterpret_cast<LPVOID>(hMod), 0, nullptr);
        if (hThread) CloseHandle(hThread);
    }
    return TRUE;
}

} // extern "C"
