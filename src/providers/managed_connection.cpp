#include "managed_connection.h"

#include "../debug.h"
#include "../module_util.h"
#include "../target.h"
#include "overlapped_io.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <nlohmann/json.hpp>
#include <objbase.h>
#include <sddl.h>
#include <wil/resource.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lvt {

using json = nlohmann::json;

static bool fail_tree_for_testing() {
    char eventName[256]{};
    const DWORD length = GetEnvironmentVariableA(
        "LVT_TEST_MANAGED_FAIL_TREE_EVENT",
        eventName, static_cast<DWORD>(_countof(eventName)));
    if (length == 0 || length >= _countof(eventName))
        return false;
    wil::unique_handle event(OpenEventA(
        SYNCHRONIZE, FALSE, eventName));
    return event &&
           WaitForSingleObject(event.get(), 0) ==
               WAIT_OBJECT_0;
}

namespace detail {

bool managed_pipe_client_matches_pid(HANDLE pipe, DWORD expectedPid) {
    ULONG clientPid = 0;
    return GetNamedPipeClientProcessId(pipe, &clientPid) &&
           clientPid == expectedPid;
}

} // namespace detail

namespace {

constexpr size_t kMaximumLineBytes = 64 * 1024 * 1024;

std::wstring make_pipe_name(const std::wstring& prefix) {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        return {};

    wchar_t suffix[80];
    swprintf_s(suffix, L"%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
               guid.Data1, guid.Data2, guid.Data3,
               guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
               guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return L"\\\\.\\pipe\\" + prefix + suffix;
}

std::wstring sidecar_path(const ManagedConnectionOptions& options, DWORD pid) {
    return get_tap_directory() + L"\\" + options.sidecarStem + L"_" +
           std::to_wstring(pid) + L".txt";
}

struct RestrictedSecurity {
    wil::unique_hlocal descriptor;
    SECURITY_ATTRIBUTES attributes{};

    explicit operator bool() const {
        return descriptor != nullptr;
    }
};

std::wstring process_user_sid(DWORD pid) {
    wil::unique_handle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process)
        return {};
    wil::unique_handle token;
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(process.get(), TOKEN_QUERY, &rawToken))
        return {};
    token.reset(rawToken);

    DWORD bytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    if (bytes == 0)
        return {};
    std::vector<unsigned char> buffer(bytes);
    if (!GetTokenInformation(
            token.get(), TokenUser, buffer.data(), bytes, &bytes))
        return {};

    const auto* tokenUser =
        reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR rawSid = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &rawSid))
        return {};
    wil::unique_hlocal sid(rawSid);
    return rawSid;
}

RestrictedSecurity make_restricted_security(DWORD targetPid) {
    const std::wstring currentSid = process_user_sid(GetCurrentProcessId());
    const std::wstring targetSid = process_user_sid(targetPid);
    if (currentSid.empty())
        return {};

    std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + currentSid + L")";
    if (!targetSid.empty() && targetSid != currentSid)
        sddl += L"(A;;GA;;;" + targetSid + L")";

    PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &rawDescriptor, nullptr))
        return {};

    RestrictedSecurity security;
    security.descriptor.reset(rawDescriptor);
    security.attributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    security.attributes.lpSecurityDescriptor = security.descriptor.get();
    security.attributes.bInheritHandle = FALSE;
    return security;
}

class CrossProcessConnectLock {
public:
    bool acquire(
        DWORD targetPid, const std::string& frameworkLabel,
        SECURITY_ATTRIBUTES* securityAttributes, HANDLE targetProcess,
        DWORD timeoutMs) {
        std::wstring label;
        label.reserve(frameworkLabel.size());
        for (unsigned char character : frameworkLabel) {
            label.push_back(std::isalnum(character)
                                ? static_cast<wchar_t>(character)
                                : L'_');
        }
        const std::wstring name =
            L"Local\\lvt-managed-connect-" + label + L"-" +
            std::to_wstring(targetPid);
        m_mutex.reset(CreateMutexW(
            securityAttributes, FALSE, name.c_str()));
        if (!m_mutex)
            return false;

        HANDLE waits[2] = {m_mutex.get(), targetProcess};
        const DWORD count = targetProcess ? 2 : 1;
        const DWORD wait =
            WaitForMultipleObjects(count, waits, FALSE, timeoutMs);
        m_owned =
            wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED_0;
        return m_owned;
    }

    ~CrossProcessConnectLock() {
        if (m_owned)
            ReleaseMutex(m_mutex.get());
    }

private:
    wil::unique_handle m_mutex;
    bool m_owned = false;
};

bool write_pipe_name_file(
    const std::wstring& path, const std::wstring& pipeName,
    SECURITY_ATTRIBUTES* securityAttributes) {
    const int byteCount = WideCharToMultiByte(
        CP_UTF8, 0, pipeName.c_str(), static_cast<int>(pipeName.size()),
        nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0)
        return false;

    std::string utf8(static_cast<size_t>(byteCount), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, 0, pipeName.c_str(), static_cast<int>(pipeName.size()),
            utf8.data(), byteCount, nullptr, nullptr) != byteCount) {
        return false;
    }

    wil::unique_hfile file(CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, securityAttributes, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY, nullptr));
    if (!file)
        return false;
    DWORD written = 0;
    return WriteFile(
               file.get(), utf8.data(), static_cast<DWORD>(utf8.size()),
               &written, nullptr) &&
           written == static_cast<DWORD>(utf8.size()) &&
           FlushFileBuffers(file.get());
}

std::wstring file_name(const std::wstring& path) {
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

bool remote_module_loaded(DWORD pid, const std::wstring& moduleName, bool& known) {
    known = false;
    const HANDLE rawSnapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (rawSnapshot == INVALID_HANDLE_VALUE)
        return false;
    wil::unique_handle snapshot(rawSnapshot);

    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    if (!Module32FirstW(snapshot.get(), &module))
        return false;

    known = true;
    do {
        if (_wcsicmp(module.szModule, moduleName.c_str()) == 0)
            return true;
    } while (Module32NextW(snapshot.get(), &module));
    return false;
}

bool wait_for_module_unload(DWORD pid, const std::wstring& moduleName, DWORD timeoutMs) {
    const auto deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        bool known = false;
        const bool loaded = remote_module_loaded(pid, moduleName, known);
        if (known && !loaded)
            return true;
        if (!known) {
            wil::unique_handle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
            if (!process || WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0)
                return true;
        }
        if (GetTickCount64() >= deadline)
            return false;
        Sleep(50);
    }
}

bool inject_dll(DWORD pid, const std::wstring& dllPath, const std::string& frameworkLabel) {
    wil::unique_handle process(OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
            PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
        FALSE, pid));
    if (!process) {
        fprintf(stderr, "lvt: failed to open %s target process %lu (error %lu)\n",
                frameworkLabel.c_str(), pid, GetLastError());
        return false;
    }

    const size_t pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(
        process.get(), nullptr, pathBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) {
        fprintf(stderr, "lvt: %s VirtualAllocEx failed (error %lu)\n",
                frameworkLabel.c_str(), GetLastError());
        return false;
    }

    if (!WriteProcessMemory(process.get(), remotePath, dllPath.c_str(), pathBytes, nullptr)) {
        fprintf(stderr, "lvt: %s WriteProcessMemory failed (error %lu)\n",
                frameworkLabel.c_str(), GetLastError());
        VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
        return false;
    }

    auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!loadLibrary) {
        VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
        return false;
    }

    wil::unique_handle thread(CreateRemoteThread(
        process.get(), nullptr, 0, loadLibrary, remotePath, 0, nullptr));
    if (!thread) {
        fprintf(stderr, "lvt: %s CreateRemoteThread failed (error %lu)\n",
                frameworkLabel.c_str(), GetLastError());
        VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(thread.get(), 15000);
    if (waitResult != WAIT_OBJECT_0) {
        fprintf(stderr, "lvt: %s LoadLibraryW did not finish (timeout)\n",
                frameworkLabel.c_str());
        return false;
    }

    DWORD exitCode = 0;
    const bool loaded =
        GetExitCodeThread(thread.get(), &exitCode) && exitCode != 0;
    VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
    if (!loaded) {
        fprintf(stderr, "lvt: %s LoadLibraryW failed in target process\n",
                frameworkLabel.c_str());
        return false;
    }
    return true;
}

enum class IoWaitResult { completed, targetExited, timedOut, failed };

IoWaitResult wait_for_io(HANDLE event, HANDLE process, DWORD timeoutMs) {
    HANDLE handles[2] = {event, process};
    const DWORD count = process ? 2 : 1;
    const DWORD result = WaitForMultipleObjects(count, handles, FALSE, timeoutMs);
    if (result == WAIT_OBJECT_0)
        return IoWaitResult::completed;
    if (count == 2 && result == WAIT_OBJECT_0 + 1)
        return IoWaitResult::targetExited;
    if (result == WAIT_TIMEOUT)
        return IoWaitResult::timedOut;
    return IoWaitResult::failed;
}

class DuplexPipeLineIo {
public:
    DuplexPipeLineIo(HANDLE pipe, HANDLE process)
        : m_pipe(pipe),
          m_process(process),
          m_readEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)),
          m_writeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    }

    bool valid() const {
        return m_pipe && m_readEvent && m_writeEvent;
    }

    bool read_line(DWORD timeoutMs, std::string& line) {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;) {
            const auto newline = m_buffer.find('\n');
            if (newline != std::string::npos) {
                line = m_buffer.substr(0, newline);
                m_buffer.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return true;
            }
            if (m_buffer.size() >= kMaximumLineBytes)
                return false;

            const ULONGLONG now = GetTickCount64();
            if (now >= deadline)
                return false;
            const DWORD remaining = static_cast<DWORD>(
                std::min<ULONGLONG>(deadline - now, std::numeric_limits<DWORD>::max()));

            char chunk[4096];
            DWORD bytesRead = 0;
            OVERLAPPED overlapped{};
            ResetEvent(m_readEvent.get());
            overlapped.hEvent = m_readEvent.get();
            BOOL ok = ReadFile(
                m_pipe, chunk, static_cast<DWORD>(sizeof(chunk)), &bytesRead, &overlapped);
            if (!ok) {
                const DWORD error = GetLastError();
                if (error != ERROR_IO_PENDING)
                    return false;
                const auto wait = wait_for_io(m_readEvent.get(), m_process, remaining);
                if (wait != IoWaitResult::completed) {
                    detail::cancel_and_complete_overlapped(m_pipe, overlapped);
                    return false;
                }
                if (!GetOverlappedResult(m_pipe, &overlapped, &bytesRead, FALSE))
                    return false;
            }
            if (bytesRead == 0)
                return false;
            m_buffer.append(chunk, bytesRead);
        }
    }

    bool write_line(const std::string& line, DWORD timeoutMs) {
        std::string message = line;
        message.push_back('\n');
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        size_t offset = 0;
        while (offset < message.size()) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline)
                return false;
            const DWORD remaining = static_cast<DWORD>(
                std::min<ULONGLONG>(deadline - now, std::numeric_limits<DWORD>::max()));
            const DWORD requested = static_cast<DWORD>(
                std::min<size_t>(message.size() - offset, std::numeric_limits<DWORD>::max()));

            DWORD bytesWritten = 0;
            OVERLAPPED overlapped{};
            ResetEvent(m_writeEvent.get());
            overlapped.hEvent = m_writeEvent.get();
            BOOL ok = WriteFile(
                m_pipe, message.data() + offset, requested, &bytesWritten, &overlapped);
            if (!ok) {
                const DWORD error = GetLastError();
                if (error != ERROR_IO_PENDING)
                    return false;
                const auto wait = wait_for_io(m_writeEvent.get(), m_process, remaining);
                if (wait != IoWaitResult::completed) {
                    detail::cancel_and_complete_overlapped(m_pipe, overlapped);
                    return false;
                }
                if (!GetOverlappedResult(m_pipe, &overlapped, &bytesWritten, FALSE))
                    return false;
            }
            if (bytesWritten == 0)
                return false;
            offset += bytesWritten;
        }
        return true;
    }

private:
    HANDLE m_pipe = nullptr;
    HANDLE m_process = nullptr;
    wil::unique_event m_readEvent;
    wil::unique_event m_writeEvent;
    std::string m_buffer;
};

bool wait_for_pipe_connection(
    HANDLE pipe, HANDLE process, OVERLAPPED& overlapped, DWORD connectError,
    DWORD timeoutMs) {
    if (connectError == ERROR_SUCCESS || connectError == ERROR_PIPE_CONNECTED)
        return true;
    if (connectError != ERROR_IO_PENDING)
        return false;

    const auto wait = wait_for_io(overlapped.hEvent, process, timeoutMs);
    if (wait != IoWaitResult::completed) {
        detail::cancel_and_complete_overlapped(pipe, overlapped);
        return false;
    }

    DWORD transferred = 0;
    return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE ||
           GetLastError() == ERROR_PIPE_CONNECTED;
}

bool wait_for_expected_pipe_client_impl(
    HANDLE pipe, HANDLE process, DWORD expectedPid, OVERLAPPED& overlapped,
    DWORD connectError, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    bool connectPending = connectError == ERROR_IO_PENDING;
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            if (connectPending)
                detail::cancel_and_complete_overlapped(pipe, overlapped);
            return false;
        }
        const DWORD remaining = static_cast<DWORD>(
            std::min<ULONGLONG>(
                deadline - now, std::numeric_limits<DWORD>::max()));
        if (!wait_for_pipe_connection(
                pipe, process, overlapped, connectError, remaining)) {
            connectPending = false;
            return false;
        }
        connectPending = false;
        if (detail::managed_pipe_client_matches_pid(pipe, expectedPid))
            return true;

        ULONG actualPid = 0;
        GetNamedPipeClientProcessId(pipe, &actualPid);
        if (g_debug) {
            fprintf(stderr,
                    "lvt: rejected managed pipe client pid %lu; expected %lu\n",
                    static_cast<DWORD>(actualPid), expectedPid);
        }
        DisconnectNamedPipe(pipe);
        const HANDLE event = overlapped.hEvent;
        ResetEvent(event);
        ZeroMemory(&overlapped, sizeof(overlapped));
        overlapped.hEvent = event;
        const BOOL connectedSynchronously =
            ConnectNamedPipe(pipe, &overlapped);
        connectError =
            connectedSynchronously ? ERROR_SUCCESS : GetLastError();
        connectPending = connectError == ERROR_IO_PENDING;
    }
}

struct ManagedCommandResponse {
    bool completed = false;
    bool success = false;
    std::string payload;
};

class ManagedFrameworkConnection final : public IFrameworkConnection {
public:
    static std::shared_ptr<ManagedFrameworkConnection> connect(
        HWND hwnd, DWORD pid, ManagedConnectionOptions options, ManagedTreeApplier applyTree) {
        (void)hwnd;
        const Architecture hostArchitecture = get_host_architecture();
        const Architecture targetArchitecture = detect_process_architecture(pid);
        if (hostArchitecture != Architecture::unknown &&
            targetArchitecture != Architecture::unknown &&
            hostArchitecture != targetArchitecture) {
            fprintf(stderr,
                    "lvt: %s target is %s but this lvt process is %s; use a matching build\n",
                    options.frameworkLabel.c_str(), architecture_name(targetArchitecture),
                    architecture_name(hostArchitecture));
            return nullptr;
        }

        const std::wstring tapPath = tap_dll_path(options.tapStem);
        const std::wstring managedPath =
            get_tap_directory() + L"\\" + options.managedAssemblyName;
        if (GetFileAttributesW(tapPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (g_debug)
                fprintf(stderr, "lvt: %s TAP DLL not found: %ls\n",
                        options.frameworkLabel.c_str(), tapPath.c_str());
            return nullptr;
        }
        if (GetFileAttributesW(managedPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (g_debug)
                fprintf(stderr, "lvt: %s managed assembly not found: %ls\n",
                        options.frameworkLabel.c_str(), managedPath.c_str());
            return nullptr;
        }

        wil::unique_handle process(OpenProcess(
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!process)
            return nullptr;

        auto security = make_restricted_security(pid);
        if (!security) {
            fprintf(stderr,
                    "lvt: could not create restricted %s connection security\n",
                    options.frameworkLabel.c_str());
            return nullptr;
        }

        CrossProcessConnectLock connectLock;
        if (!connectLock.acquire(
                pid, options.frameworkLabel, &security.attributes,
                process.get(), options.connectTimeoutMs + 5000)) {
            if (g_debug) {
                fprintf(stderr,
                        "lvt: could not acquire the %s connection ownership mutex\n",
                        options.frameworkLabel.c_str());
            }
            return nullptr;
        }

        const std::wstring tapName = file_name(tapPath);
        bool moduleStateKnown = false;
        if (remote_module_loaded(pid, tapName, moduleStateKnown)) {
            if (g_debug)
                fprintf(stderr,
                        "lvt: waiting for previous %s TAP session to finish unloading\n",
                        options.frameworkLabel.c_str());
            if (!wait_for_module_unload(pid, tapName, 5000)) {
                fprintf(stderr, "lvt: %s TAP DLL is already active in target process\n",
                        options.frameworkLabel.c_str());
                return nullptr;
            }
        }

        const std::wstring pipeName = make_pipe_name(options.pipePrefix);
        if (pipeName.empty())
            return nullptr;
        const std::wstring sidecar = sidecar_path(options, pid);
        if (!write_pipe_name_file(
                sidecar, pipeName, &security.attributes)) {
            fprintf(stderr, "lvt: failed to write %s pipe bootstrap file\n",
                    options.frameworkLabel.c_str());
            return nullptr;
        }

        wil::unique_hfile pipe(CreateNamedPipeW(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 1024 * 1024, 1024 * 1024, options.commandTimeoutMs,
            &security.attributes));
        if (!pipe) {
            DeleteFileW(sidecar.c_str());
            fprintf(stderr, "lvt: failed to create %s named pipe (error %lu)\n",
                    options.frameworkLabel.c_str(), GetLastError());
            return nullptr;
        }

        OVERLAPPED connectOverlapped{};
        wil::unique_event connectEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!connectEvent) {
            DeleteFileW(sidecar.c_str());
            return nullptr;
        }
        connectOverlapped.hEvent = connectEvent.get();
        const BOOL connectedSynchronously = ConnectNamedPipe(pipe.get(), &connectOverlapped);
        const DWORD connectError =
            connectedSynchronously ? ERROR_SUCCESS : GetLastError();

        if (!inject_dll(pid, tapPath, options.frameworkLabel)) {
            if (connectError == ERROR_IO_PENDING) {
                detail::cancel_and_complete_overlapped(
                    pipe.get(), connectOverlapped);
            }
            DeleteFileW(sidecar.c_str());
            return nullptr;
        }

        const bool connected = detail::wait_for_expected_pipe_client(
            pipe.get(), process.get(), pid, connectOverlapped, connectError,
            options.connectTimeoutMs);
        DeleteFileW(sidecar.c_str());
        if (!connected) {
            fprintf(stderr, "lvt: %s TAP DLL did not connect (timeout or target exit)\n",
                    options.frameworkLabel.c_str());
            return nullptr;
        }

        auto io = std::make_unique<DuplexPipeLineIo>(pipe.get(), process.get());
        if (!io->valid())
            return nullptr;

        std::string readyLine;
        if (!io->read_line(options.connectTimeoutMs, readyLine) ||
            readyLine.rfind("READY\t", 0) != 0) {
            fprintf(stderr, "lvt: %s TAP DLL did not send a valid READY handshake\n",
                    options.frameworkLabel.c_str());
            return nullptr;
        }

        const json ready = json::parse(readyLine.substr(6), nullptr, false);
        if (ready.is_discarded() || !ready.is_object() ||
            ready.value("protocol", 0u) != 1u) {
            fprintf(stderr, "lvt: %s TAP DLL reported an unsupported protocol\n",
                    options.frameworkLabel.c_str());
            return nullptr;
        }

        ManagedConnectionCapabilities capabilities;
        capabilities.protocolVersion = ready.value("protocol", 0u);
        capabilities.connectionId = ready.value("connectionId", "");
        capabilities.assemblyInstanceId = ready.value("assemblyInstanceId", "");
        capabilities.serverStartCount = ready.value("serverStartCount", 0u);
        if (auto commands = ready.find("commands");
            commands != ready.end() && commands->is_array()) {
            for (const auto& command : *commands) {
                if (command.is_string())
                    capabilities.commands.push_back(command.get<std::string>());
            }
        }
        if (capabilities.connectionId.empty() ||
            capabilities.assemblyInstanceId.empty()) {
            return nullptr;
        }

        if (g_debug) {
            fprintf(stderr, "lvt: %s managed connection %s ready\n",
                    options.frameworkLabel.c_str(), capabilities.connectionId.c_str());
        }

        return std::shared_ptr<ManagedFrameworkConnection>(
            new ManagedFrameworkConnection(
                pid, tapName, std::move(options), std::move(process), std::move(pipe),
                std::move(io), std::move(applyTree), std::move(capabilities)));
    }

    ~ManagedFrameworkConnection() override {
        {
            std::lock_guard<std::mutex> lock(m_commandMutex);
            if (m_alive.load()) {
                (void)send_command_locked("DISCONNECT", "{}", 2000);
            }
            m_alive.store(false);
            CancelIoEx(m_pipe.get(), nullptr);
            DisconnectNamedPipe(m_pipe.get());
            m_io.reset();
            m_pipe.reset();
        }
        if (m_process && WaitForSingleObject(m_process.get(), 0) == WAIT_TIMEOUT) {
            if (!wait_for_module_unload(m_pid, m_tapModuleName, 5000) && g_debug) {
                fprintf(stderr, "lvt: %s TAP DLL did not unload before timeout\n",
                        m_options.frameworkLabel.c_str());
            }
        }
    }

    bool get_tree(Element& root, bool,
                  const std::string& = {}) override {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        if (!connection_alive())
            return false;
        if (fail_tree_for_testing())
            return false;

        auto response = send_command_locked(
            "GET_TREE", "{}", m_options.commandTimeoutMs);
        if (!response.completed || !response.success) {
            return false;
        }
        return m_applyTree && m_applyTree(root, response.payload);
    }

    std::vector<ConnectionEvent> poll_events() override {
        return {};
    }

    bool is_alive() const override {
        return connection_alive();
    }

    PropertySnapshotResult get_property_snapshot(uint64_t handle) override {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        PropertySnapshotResult result;
        if (!supports_command("GET_PROPERTIES")) {
            result.hresult = E_NOTIMPL;
            result.error =
                "This managed TAP does not advertise typed property snapshots";
            return result;
        }

        auto response = send_command_locked(
            "GET_PROPERTIES", std::to_string(handle),
            m_options.commandTimeoutMs);
        if (response.completed && !response.success) {
            PropertySnapshotResult firstFailure;
            set_command_error(response.payload, firstFailure);
            if (firstFailure.hresult ==
                HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
                // A new persistent connection has an empty reverse identity
                // map until its first tree walk. Hydrate it once and retry the
                // stable provider handle; a genuinely stale/new-assembly
                // handle remains rejected by the second response.
                auto hydration = send_command_locked(
                    "GET_TREE", "{}", m_options.commandTimeoutMs);
                if (hydration.completed && hydration.success) {
                    response = send_command_locked(
                        "GET_PROPERTIES", std::to_string(handle),
                        m_options.commandTimeoutMs);
                }
            }
        }
        if (!response.completed) {
            set_transport_error(result);
            return result;
        }
        if (!response.success) {
            set_command_error(response.payload, result);
            return result;
        }

        const json payload = json::parse(response.payload, nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) {
            result.hresult = E_FAIL;
            result.error = "The managed TAP returned an invalid property snapshot";
            return result;
        }
        const std::string schemaId = payload.value("schemaId", "");
        if (schemaId.empty()) {
            result.hresult = E_FAIL;
            result.error = "The managed TAP property snapshot has no schema ID";
            return result;
        }

        auto cached = m_schemasById.find(schemaId);
        if (cached != m_schemasById.end()) {
            result.schema = cached->second;
        } else {
            auto schema = parse_schema(payload, schemaId);
            if (!schema) {
                result.hresult = E_FAIL;
                result.error = "The managed TAP returned an invalid property schema";
                return result;
            }
            result.schema = schema;
            m_schemasById.emplace(schemaId, std::move(schema));
        }

        auto values = payload.find("values");
        if (values == payload.end() || !values->is_array()) {
            result.hresult = E_FAIL;
            result.error = "The managed TAP property snapshot has no live values";
            result.schema.reset();
            return result;
        }
        for (const auto& item : *values) {
            if (!item.is_object())
                continue;
            PropertyValue value;
            value.descriptorId = item.value("descriptorId", "");
            value.value = item.value("value", "");
            value.runtimeType = item.value("runtimeType", "");
            value.canClear = item.value("canClear", false);
            value.overridden = item.value("overridden", false);
            value.source = item.value("source", "");
            value.unavailableReason = item.value("unavailableReason", "");
            value.readOnlyReason = item.value("readOnlyReason", "");
            if (!value.descriptorId.empty())
                result.values.push_back(std::move(value));
        }
        result.ok = true;
        result.hresult = S_OK;
        result.error.clear();
        return result;
    }

    PropertyMutationResult set_property(
        uint64_t handle, const std::string& descriptorId,
        const std::string& value) override {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        PropertyMutationResult result;
        if (!supports_command("SET_PROPERTY")) {
            result.hresult = E_NOTIMPL;
            result.error =
                "This managed TAP does not advertise typed property mutation";
            return result;
        }
        const std::string arguments =
            std::to_string(handle) + " " + hex_encode(descriptorId) + " " +
            hex_encode(value);
        return parse_mutation_response(
            send_command_locked(
                "SET_PROPERTY", arguments, m_options.commandTimeoutMs),
            false);
    }

    PropertyMutationResult clear_property(
        uint64_t handle, const std::string& descriptorId) override {
        std::lock_guard<std::mutex> lock(m_commandMutex);
        PropertyMutationResult result;
        if (!supports_command("CLEAR_PROPERTY")) {
            result.hresult = E_NOTIMPL;
            result.error =
                "This managed TAP does not advertise typed property clearing";
            return result;
        }
        const std::string arguments =
            std::to_string(handle) + " " + hex_encode(descriptorId);
        return parse_mutation_response(
            send_command_locked(
                "CLEAR_PROPERTY", arguments, m_options.commandTimeoutMs),
            true);
    }

    const ManagedConnectionCapabilities& capabilities() const {
        return m_capabilities;
    }

private:
    ManagedFrameworkConnection(
        DWORD pid, std::wstring tapModuleName, ManagedConnectionOptions options,
        wil::unique_handle process, wil::unique_hfile pipe,
        std::unique_ptr<DuplexPipeLineIo> io, ManagedTreeApplier applyTree,
        ManagedConnectionCapabilities capabilities)
        : m_pid(pid),
          m_tapModuleName(std::move(tapModuleName)),
          m_options(std::move(options)),
          m_process(std::move(process)),
          m_pipe(std::move(pipe)),
          m_io(std::move(io)),
          m_applyTree(std::move(applyTree)),
          m_capabilities(std::move(capabilities)) {
        m_alive.store(true);
    }

    bool connection_alive() const {
        if (!m_alive.load())
            return false;
        if (!m_process || WaitForSingleObject(m_process.get(), 0) == WAIT_OBJECT_0) {
            m_alive.store(false);
            return false;
        }
        DWORD available = 0;
        if (!m_pipe || !PeekNamedPipe(m_pipe.get(), nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED ||
                error == ERROR_NO_DATA || error == ERROR_INVALID_HANDLE) {
                m_alive.store(false);
                return false;
            }
        }
        return true;
    }

    bool supports_command(const std::string& command) const {
        return std::find(
                   m_capabilities.commands.begin(),
                   m_capabilities.commands.end(), command) !=
               m_capabilities.commands.end();
    }

    static std::string hex_encode(const std::string& value) {
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string encoded;
        encoded.reserve(value.size() * 2);
        for (unsigned char byte : value) {
            encoded.push_back(digits[byte >> 4]);
            encoded.push_back(digits[byte & 0x0F]);
        }
        return encoded.empty() ? "-" : encoded;
    }

    static PropertyEditorKind parse_editor_kind(const std::string& kind) {
        if (kind == "string") return PropertyEditorKind::string;
        if (kind == "boolean") return PropertyEditorKind::boolean;
        if (kind == "integer") return PropertyEditorKind::integer;
        if (kind == "number") return PropertyEditorKind::number;
        if (kind == "enum") return PropertyEditorKind::enumeration;
        return PropertyEditorKind::readonly;
    }

    std::shared_ptr<const PropertySchema> parse_schema(
        const json& payload, const std::string& schemaId) const {
        auto descriptors = payload.find("descriptors");
        if (descriptors == payload.end() || !descriptors->is_array())
            return nullptr;

        auto schema = std::make_shared<PropertySchema>();
        schema->schemaId = schemaId;
        for (const auto& item : *descriptors) {
            if (!item.is_object())
                return nullptr;
            PropertyDescriptor descriptor;
            descriptor.descriptorId = item.value("descriptorId", "");
            descriptor.name = item.value("name", "");
            descriptor.displayName = item.value("displayName", descriptor.name);
            descriptor.provider =
                item.value("provider", m_options.frameworkLabel);
            descriptor.framework =
                item.value("framework", m_options.frameworkLabel);
            descriptor.declaringType = item.value("declaringType", "");
            descriptor.propertyType = item.value("propertyType", "");
            descriptor.kind =
                parse_editor_kind(item.value("kind", "readonly"));
            descriptor.writable = item.value("writable", false);
            descriptor.supportsClear = item.value("supportsClear", false);
            descriptor.description = item.value("description", "");
            if (auto choices = item.find("choices");
                choices != item.end() && choices->is_array()) {
                for (const auto& choiceItem : *choices) {
                    if (!choiceItem.is_object())
                        continue;
                    PropertyChoice choice;
                    choice.value = choiceItem.value("value", "");
                    choice.label = choiceItem.value("label", choice.value);
                    descriptor.choices.push_back(std::move(choice));
                }
            }
            if (auto minimum = item.find("minimum");
                minimum != item.end() && minimum->is_number())
                descriptor.minimum = minimum->get<double>();
            if (auto maximum = item.find("maximum");
                maximum != item.end() && maximum->is_number())
                descriptor.maximum = maximum->get<double>();
            if (auto step = item.find("step");
                step != item.end() && step->is_number())
                descriptor.step = step->get<double>();
            if (descriptor.descriptorId.empty() || descriptor.name.empty())
                return nullptr;
            schema->descriptors.push_back(std::move(descriptor));
        }
        return schema;
    }

    static HRESULT parse_hresult(const json& payload) {
        const auto hresult = payload.find("hresult");
        if (hresult == payload.end() || !hresult->is_string())
            return E_FAIL;
        std::string text = hresult->get<std::string>();
        const char* first = text.data();
        if (text.rfind("0x", 0) == 0 || text.rfind("0X", 0) == 0)
            first += 2;
        uint32_t raw = 0;
        const auto parsed =
            std::from_chars(first, text.data() + text.size(), raw, 16);
        return parsed.ec == std::errc() &&
                       parsed.ptr == text.data() + text.size()
                   ? static_cast<HRESULT>(raw)
                   : E_FAIL;
    }

    template <typename Result>
    void set_transport_error(Result& result) const {
        result.hresult = HRESULT_FROM_WIN32(ERROR_BROKEN_PIPE);
        result.error =
            "The " + m_options.frameworkLabel +
            " managed property connection is no longer available";
    }

    template <typename Result>
    static void set_command_error(
        const std::string& payloadText, Result& result) {
        const json payload = json::parse(payloadText, nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) {
            result.hresult = E_FAIL;
            result.error = payloadText.empty()
                ? "The managed property command failed"
                : payloadText;
            return;
        }
        result.hresult = parse_hresult(payload);
        result.error = payload.value("message", "The managed property command failed");
    }

    PropertyMutationResult parse_mutation_response(
        ManagedCommandResponse response, bool clearing) {
        PropertyMutationResult result;
        if (!response.completed) {
            set_transport_error(result);
            return result;
        }
        if (!response.success) {
            set_command_error(response.payload, result);
            return result;
        }
        const json payload = json::parse(response.payload, nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) {
            result.hresult = E_FAIL;
            result.error = "The managed TAP returned an invalid mutation result";
            return result;
        }
        if (auto value = payload.find("value");
            value != payload.end() && value->is_string()) {
            result.hasValue = true;
            result.value = value->get<std::string>();
        }
        result.cleared = clearing && payload.value("cleared", false);
        result.ok = true;
        result.hresult = S_OK;
        result.error.clear();
        return result;
    }

    ManagedCommandResponse send_command_locked(
        const std::string& command, const std::string& arguments,
        DWORD timeoutMs) {
        ManagedCommandResponse result;
        if (!connection_alive() || !m_io)
            return result;

        const uint64_t commandId = m_nextCommandId++;
        const std::string id = std::to_string(commandId);
        if (!m_io->write_line(
                "REQUEST\t" + id + "\t" + command + "\t" + arguments, timeoutMs)) {
            m_alive.store(false);
            return result;
        }

        std::string response;
        if (!m_io->read_line(timeoutMs, response)) {
            m_alive.store(false);
            return result;
        }

        const std::string prefix = "RESPONSE\t" + id + "\t";
        if (response.rfind(prefix, 0) != 0) {
            fprintf(stderr, "lvt: %s TAP DLL returned an uncorrelated response\n",
                    m_options.frameworkLabel.c_str());
            m_alive.store(false);
            return result;
        }
        const size_t statusEnd = response.find('\t', prefix.size());
        if (statusEnd == std::string::npos) {
            m_alive.store(false);
            return result;
        }
        const std::string status =
            response.substr(prefix.size(), statusEnd - prefix.size());
        result.payload = response.substr(statusEnd + 1);
        result.completed = true;
        if (status == "OK") {
            result.success = true;
            return result;
        }
        if (status == "ERROR") {
            if (g_debug)
                fprintf(stderr, "lvt: %s managed command %s failed: %s\n",
                        m_options.frameworkLabel.c_str(), command.c_str(),
                        result.payload.c_str());
            return result;
        }

        m_alive.store(false);
        result.completed = false;
        return result;
    }

    DWORD m_pid = 0;
    std::wstring m_tapModuleName;
    ManagedConnectionOptions m_options;
    wil::unique_handle m_process;
    wil::unique_hfile m_pipe;
    std::unique_ptr<DuplexPipeLineIo> m_io;
    ManagedTreeApplier m_applyTree;
    ManagedConnectionCapabilities m_capabilities;
    std::unordered_map<std::string, std::shared_ptr<const PropertySchema>>
        m_schemasById;
    mutable std::atomic_bool m_alive = false;
    uint64_t m_nextCommandId = 1;
    std::mutex m_commandMutex;
};

} // namespace

namespace detail {

bool wait_for_expected_pipe_client(
    HANDLE pipe, HANDLE process, DWORD expectedPid, OVERLAPPED& overlapped,
    DWORD connectError, DWORD timeoutMs) {
    return wait_for_expected_pipe_client_impl(
        pipe, process, expectedPid, overlapped, connectError, timeoutMs);
}

} // namespace detail

std::shared_ptr<IFrameworkConnection> open_managed_framework_connection(
    HWND hwnd, DWORD pid, ManagedConnectionOptions options, ManagedTreeApplier applyTree) {
    return ManagedFrameworkConnection::connect(
        hwnd, pid, std::move(options), std::move(applyTree));
}

std::optional<ManagedConnectionCapabilities> managed_connection_capabilities(
    IFrameworkConnection& connection) {
    auto* managed = dynamic_cast<ManagedFrameworkConnection*>(&connection);
    if (!managed)
        return std::nullopt;
    return managed->capabilities();
}

} // namespace lvt
