// wpf_tap_native.cpp — Native DLL injected into WPF target process.
// Hosts the .NET CLR and calls managed WpfTreeWalker.CollectTree().
// Supports both .NET Framework (ICLRMetaHost) and .NET Core (hostfxr).

#include <Windows.h>
#include <metahost.h>
#include <string>
#include <cstdio>

static void LogMsg(const char* fmt, ...) {
    static FILE* logFile = nullptr;
    if (!logFile) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        wcscat_s(tmp, L"lvt_wpf_tap.log");
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

// Read pipe name from a sidecar file written by lvt.exe before injection.
static std::wstring ReadPipeName() {
    std::wstring dir = GetDllDirectory();
    std::wstring path = dir + L"\\lvt_wpf_pipe.txt";

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

    // Convert UTF-8 to wide
    int wlen = MultiByteToWideChar(CP_UTF8, 0, buf, bytesRead, nullptr, 0);
    std::wstring result(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf, bytesRead, result.data(), wlen);

    // Trim whitespace
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
        result.pop_back();

    LogMsg("Pipe name read: %ls", result.c_str());
    return result;
}

// Try .NET Framework hosting via ICLRMetaHost
static bool TryNetFramework(const std::wstring& assemblyPath, const std::wstring& pipeName) {
    HMODULE hMscoree = LoadLibraryW(L"mscoree.dll");
    if (!hMscoree) {
        LogMsg("mscoree.dll not found");
        return false;
    }

    using CLRCreateInstanceFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto pCLRCreateInstance = reinterpret_cast<CLRCreateInstanceFn>(
        GetProcAddress(hMscoree, "CLRCreateInstance"));
    if (!pCLRCreateInstance) {
        LogMsg("CLRCreateInstance not found in mscoree.dll");
        FreeLibrary(hMscoree);
        return false;
    }

    ICLRMetaHost* metaHost = nullptr;
    HRESULT hr = pCLRCreateInstance(CLSID_CLRMetaHost, IID_ICLRMetaHost, (void**)&metaHost);
    if (FAILED(hr)) {
        LogMsg("CLRCreateInstance failed: 0x%08X", hr);
        FreeLibrary(hMscoree);
        return false;
    }

    // Enumerate loaded runtimes to find the one already running
    IEnumUnknown* pEnum = nullptr;
    hr = metaHost->EnumerateLoadedRuntimes(GetCurrentProcess(), &pEnum);
    if (FAILED(hr)) {
        LogMsg("EnumerateLoadedRuntimes failed: 0x%08X", hr);
        metaHost->Release();
        return false;
    }

    ICLRRuntimeInfo* runtimeInfo = nullptr;
    IUnknown* pUnk = nullptr;
    ULONG fetched = 0;
    while (pEnum->Next(1, &pUnk, &fetched) == S_OK) {
        hr = pUnk->QueryInterface(IID_ICLRRuntimeInfo, (void**)&runtimeInfo);
        pUnk->Release();
        if (SUCCEEDED(hr)) break;
    }
    pEnum->Release();

    if (!runtimeInfo) {
        LogMsg("No loaded CLR runtime found");
        metaHost->Release();
        return false;
    }

    wchar_t version[64]{};
    DWORD versionLen = 64;
    runtimeInfo->GetVersionString(version, &versionLen);
    LogMsg("Found CLR runtime: %ls", version);

    ICLRRuntimeHost* runtimeHost = nullptr;
    hr = runtimeInfo->GetInterface(CLSID_CLRRuntimeHost, IID_ICLRRuntimeHost, (void**)&runtimeHost);
    runtimeInfo->Release();
    metaHost->Release();

    if (FAILED(hr) || !runtimeHost) {
        LogMsg("GetInterface for ICLRRuntimeHost failed: 0x%08X", hr);
        return false;
    }

    // ExecuteInDefaultAppDomain calls a static method with signature:
    // static int MethodName(string argument)
    DWORD retVal = 0;
    hr = runtimeHost->ExecuteInDefaultAppDomain(
        assemblyPath.c_str(),
        L"LvtWpfTap.WpfTreeWalker",
        L"CollectTree",
        pipeName.c_str(),
        &retVal);

    runtimeHost->Release();

    LogMsg("ExecuteInDefaultAppDomain returned 0x%08X, retVal=%lu", hr, retVal);
    return SUCCEEDED(hr);
}

// Try .NET Core hosting by finding the already-loaded coreclr.dll
// and calling managed code via the CLR hosting API.
typedef int (STDMETHODCALLTYPE *coreclr_create_delegate_fn)(
    void* hostHandle, unsigned int domainId,
    const char* entryPointAssemblyName,
    const char* entryPointTypeName,
    const char* entryPointMethodName,
    void** delegate);

typedef int (STDMETHODCALLTYPE *coreclr_execute_assembly_fn)(
    void* hostHandle, unsigned int domainId,
    int argc, const char** argv,
    unsigned int* exitCode);

// For .NET Core, we use a different approach: since the CLR is already running,
// we find coreclr.dll and use its exports to call into managed code.
// However, coreclr_create_delegate requires the host handle and domain ID,
// which we don't have from an injected DLL.
//
// Instead, we use the .NET hosting API (hostfxr) to load and call into
// a component assembly. Since the runtime is already initialized, hostfxr
// will reuse the existing runtime.
static bool TryNetCore(const std::wstring& assemblyPath, const std::wstring& pipeName) {
    // Find hostfxr.dll - it should be loadable if .NET is installed
    // First check if it's already loaded in the process
    HMODULE hHostfxr = GetModuleHandleW(L"hostfxr.dll");
    if (!hHostfxr) {
        // Try to load it from the .NET installation
        // Use nethost to find it
        LogMsg("hostfxr.dll not loaded, trying to find it");

        // Search common .NET install locations
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

    // Get hostfxr exports
    using hostfxr_initialize_fn = int(STDMETHODCALLTYPE*)(
        const wchar_t* runtime_config_path,
        const void* parameters,
        void** host_context_handle);
    using hostfxr_get_delegate_fn = int(STDMETHODCALLTYPE*)(
        void* host_context_handle,
        int type,
        void** delegate);
    using hostfxr_close_fn = int(STDMETHODCALLTYPE*)(void* host_context_handle);

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

    // If no runtimeconfig.json exists, create a minimal one
    if (GetFileAttributesW(configPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = _wfopen(configPath.c_str(), L"w");
        if (f) {
            fprintf(f, "{\n  \"runtimeOptions\": {\n"
                       "    \"framework\": {\n"
                       "      \"name\": \"Microsoft.WindowsDesktop.App\",\n"
                       "      \"version\": \"8.0.0\"\n"
                       "    }\n  }\n}\n");
            fclose(f);
            LogMsg("Created runtimeconfig.json");
        }
    }

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

    // Signature for load_assembly_and_get_function_pointer:
    // int fn(const wchar_t* assembly_path, const wchar_t* type_name,
    //        const wchar_t* method_name, const wchar_t* delegate_type_name,
    //        void* reserved, void** delegate)
    using load_assembly_fn = int(STDMETHODCALLTYPE*)(
        const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, void*, void**);
    auto loadAssembly = reinterpret_cast<load_assembly_fn>(loadAndGet);

    // Get function pointer to WpfTreeWalker.CollectTree
    // Using UNMANAGEDCALLERSONLY_METHOD delegate type (-1) for simpler interop
    // But since our method takes a string, we'll use the default delegate
    using CollectTreeFn = int(STDMETHODCALLTYPE*)(const wchar_t*, int);
    CollectTreeFn collectTree = nullptr;

    rc = loadAssembly(
        assemblyPath.c_str(),
        L"LvtWpfTap.WpfTreeWalker, LvtWpfTap",
        L"CollectTree",
        L"LvtWpfTap.WpfTreeWalker+CollectTreeDelegate, LvtWpfTap",
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

static DWORD WINAPI WorkerThread(LPVOID) {
    LogMsg("WorkerThread starting");

    std::wstring pipeName = ReadPipeName();
    if (pipeName.empty()) {
        LogMsg("No pipe name, exiting");
        return 1;
    }

    std::wstring dir = GetDllDirectory();
    std::wstring assemblyPath = dir + L"\\LvtWpfTap.dll";

    if (GetFileAttributesW(assemblyPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogMsg("Managed assembly not found: %ls", assemblyPath.c_str());
        return 1;
    }

    LogMsg("Attempting .NET Framework hosting...");
    if (TryNetFramework(assemblyPath, pipeName)) {
        LogMsg("Tree collection succeeded via .NET Framework");
        return 0;
    }

    LogMsg("Attempting .NET Core hosting...");
    if (TryNetCore(assemblyPath, pipeName)) {
        LogMsg("Tree collection succeeded via .NET Core");
        return 0;
    }

    LogMsg("All CLR hosting attempts failed");
    return 1;
}

extern "C" {

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        LogMsg("DllMain: DLL_PROCESS_ATTACH");

        // Spawn worker thread to avoid blocking DllMain
        HANDLE hThread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
        if (hThread) CloseHandle(hThread);
    }
    return TRUE;
}

} // extern "C"
