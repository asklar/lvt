#include "managed_tap_host.h"

#include <metahost.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <cstdio>
#include <string>

namespace {

HMODULE g_module = nullptr;
ManagedTapHostConfig g_config{};
FILE* g_logFile = nullptr;

void LogMsg(const char* format, ...) {
    if (!g_logFile) {
        wchar_t path[MAX_PATH];
        if (GetTempPathW(MAX_PATH, path) == 0)
            return;
        wcscat_s(path, g_config.logFileName);
        g_logFile = _wfopen(path, L"a");
        if (!g_logFile)
            return;
    }

    fprintf(g_logFile, "[pid=%lu tid=%lu] ", GetCurrentProcessId(), GetCurrentThreadId());
    va_list arguments;
    va_start(arguments, format);
    vfprintf(g_logFile, format, arguments);
    va_end(arguments);
    fputc('\n', g_logFile);
    fflush(g_logFile);
}

std::wstring GetDllDirectory() {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(g_module, path, MAX_PATH) == 0)
        return {};
    std::wstring directory(path);
    const auto separator = directory.find_last_of(L"\\/");
    if (separator != std::wstring::npos)
        directory.resize(separator);
    return directory;
}

std::wstring ReadPipeName() {
    const std::wstring path =
        GetDllDirectory() + L"\\" + g_config.sidecarStem + L"_" +
        std::to_wstring(GetCurrentProcessId()) + L".txt";
    wil::unique_hfile file(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr));
    if (!file) {
        LogMsg("Failed to open pipe bootstrap file: %lu", GetLastError());
        return {};
    }

    char buffer[512]{};
    DWORD bytesRead = 0;
    if (!ReadFile(file.get(), buffer, sizeof(buffer), &bytesRead, nullptr) ||
        bytesRead == 0) {
        LogMsg("Failed to read pipe bootstrap file: %lu", GetLastError());
        return {};
    }

    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, buffer, static_cast<int>(bytesRead),
        nullptr, 0);
    if (length <= 0)
        return {};
    std::wstring pipeName(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, buffer, static_cast<int>(bytesRead),
        pipeName.data(), length);
    while (!pipeName.empty() &&
           (pipeName.back() == L'\r' || pipeName.back() == L'\n' ||
            pipeName.back() == L' ' || pipeName.back() == L'\t')) {
        pipeName.pop_back();
    }
    return pipeName;
}

bool TryNetFramework(const std::wstring& assemblyPath, const std::wstring& pipeName) {
    wil::unique_hmodule mscoree(LoadLibraryW(L"mscoree.dll"));
    if (!mscoree)
        return false;

    using CLRCreateInstanceFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
    auto createInstance = reinterpret_cast<CLRCreateInstanceFn>(
        GetProcAddress(mscoree.get(), "CLRCreateInstance"));
    if (!createInstance)
        return false;

    wil::com_ptr<ICLRMetaHost> metaHost;
    HRESULT result =
        createInstance(CLSID_CLRMetaHost, IID_PPV_ARGS(metaHost.put()));
    if (FAILED(result))
        return false;

    wil::com_ptr<IEnumUnknown> runtimes;
    result = metaHost->EnumerateLoadedRuntimes(GetCurrentProcess(), runtimes.put());
    if (FAILED(result))
        return false;

    wil::com_ptr<ICLRRuntimeInfo> runtimeInfo;
    wil::com_ptr<IUnknown> unknown;
    ULONG fetched = 0;
    while (runtimes->Next(1, unknown.put(), &fetched) == S_OK) {
        if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(runtimeInfo.put()))))
            break;
        unknown.reset();
    }
    if (!runtimeInfo)
        return false;

    wil::com_ptr<ICLRRuntimeHost> runtimeHost;
    result = runtimeInfo->GetInterface(
        CLSID_CLRRuntimeHost, IID_PPV_ARGS(runtimeHost.put()));
    if (FAILED(result) || !runtimeHost)
        return false;

    DWORD returnValue = 1;
    result = runtimeHost->ExecuteInDefaultAppDomain(
        assemblyPath.c_str(), g_config.frameworkTypeName, g_config.methodName,
        pipeName.c_str(), &returnValue);
    LogMsg("ExecuteInDefaultAppDomain returned 0x%08X, value=%lu", result, returnValue);
    return SUCCEEDED(result);
}

HMODULE LoadHostFxr() {
    if (HMODULE loaded = GetModuleHandleW(L"hostfxr.dll"))
        return loaded;

    wchar_t programFiles[MAX_PATH];
    if (GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH) == 0)
        return nullptr;
    const std::wstring fxrDirectory =
        std::wstring(programFiles) + L"\\dotnet\\host\\fxr";

    WIN32_FIND_DATAW findData{};
    wil::unique_hfind find(FindFirstFileW((fxrDirectory + L"\\*").c_str(), &findData));
    std::wstring hostFxrPath;
    if (find) {
        do {
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                findData.cFileName[0] != L'.') {
                const std::wstring candidate =
                    fxrDirectory + L"\\" + findData.cFileName + L"\\hostfxr.dll";
                if (hostFxrPath.empty() || candidate > hostFxrPath)
                    hostFxrPath = candidate;
            }
        } while (FindNextFileW(find.get(), &findData));
    }
    return hostFxrPath.empty() ? nullptr : LoadLibraryW(hostFxrPath.c_str());
}

bool TryNetCore(const std::wstring& assemblyPath, const std::wstring& pipeName) {
    HMODULE hostFxr = LoadHostFxr();
    if (!hostFxr)
        return false;

    using InitializeFn = int(STDMETHODCALLTYPE*)(
        const wchar_t*, const void*, void**);
    using GetDelegateFn = int(STDMETHODCALLTYPE*)(void*, int, void**);
    using CloseFn = int(STDMETHODCALLTYPE*)(void*);
    auto initialize = reinterpret_cast<InitializeFn>(
        GetProcAddress(hostFxr, "hostfxr_initialize_for_runtime_config"));
    auto getDelegate = reinterpret_cast<GetDelegateFn>(
        GetProcAddress(hostFxr, "hostfxr_get_runtime_delegate"));
    auto close = reinterpret_cast<CloseFn>(
        GetProcAddress(hostFxr, "hostfxr_close"));
    if (!initialize || !getDelegate || !close)
        return false;

    std::wstring runtimeConfig = assemblyPath;
    const auto extension = runtimeConfig.rfind(L'.');
    if (extension != std::wstring::npos)
        runtimeConfig.resize(extension);
    runtimeConfig += L".runtimeconfig.json";
    if (GetFileAttributesW(runtimeConfig.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogMsg("Runtime config not found: %ls", runtimeConfig.c_str());
        return false;
    }

    void* hostContext = nullptr;
    int result = initialize(runtimeConfig.c_str(), nullptr, &hostContext);
    if (result < 0 || !hostContext) {
        if (hostContext)
            close(hostContext);
        return false;
    }

    void* loadAndGetPointer = nullptr;
    result = getDelegate(hostContext, 5, &loadAndGetPointer);
    if (result < 0 || !loadAndGetPointer) {
        close(hostContext);
        return false;
    }

    using LoadAssemblyFn = int(STDMETHODCALLTYPE*)(
        const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*, void*, void**);
    auto loadAssembly = reinterpret_cast<LoadAssemblyFn>(loadAndGetPointer);
    using RunServerFn = int(STDMETHODCALLTYPE*)(const wchar_t*, int);
    RunServerFn runServer = nullptr;
    result = loadAssembly(
        assemblyPath.c_str(), g_config.coreTypeName, g_config.methodName,
        g_config.coreDelegateTypeName, nullptr,
        reinterpret_cast<void**>(&runServer));
    if (result < 0 || !runServer) {
        close(hostContext);
        return false;
    }

    const int returnValue =
        runServer(pipeName.c_str(), static_cast<int>(pipeName.size() * sizeof(wchar_t)));
    LogMsg("Managed server returned %d", returnValue);
    close(hostContext);
    return returnValue == 0;
}

DWORD WINAPI WorkerThread(LPVOID) {
    LogMsg("Persistent managed TAP worker starting");
    const std::wstring pipeName = ReadPipeName();
    const std::wstring assemblyPath =
        GetDllDirectory() + L"\\" + g_config.managedAssemblyName;

    DWORD result = 1;
    if (pipeName.empty()) {
        LogMsg("Pipe name is unavailable");
    } else if (GetFileAttributesW(assemblyPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogMsg("Managed assembly not found: %ls", assemblyPath.c_str());
    } else if (TryNetFramework(assemblyPath, pipeName)) {
        result = 0;
    } else if (TryNetCore(assemblyPath, pipeName)) {
        result = 0;
    } else {
        LogMsg("All CLR hosting attempts failed");
    }

    LogMsg("Persistent managed TAP worker exiting");
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
    FreeLibraryAndExitThread(g_module, result);
    return result;
}

} // namespace

bool StartManagedTapHost(HMODULE module, const ManagedTapHostConfig& config) {
    g_module = module;
    g_config = config;
    HANDLE thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (!thread)
        return false;
    CloseHandle(thread);
    return true;
}
