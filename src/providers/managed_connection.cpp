#include "managed_connection.h"

#include "../debug.h"
#include "../module_util.h"
#include "../target.h"

#include <Windows.h>
#include <TlHelp32.h>
#include <nlohmann/json.hpp>
#include <objbase.h>
#include <sddl.h>
#include <wil/resource.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace lvt {

using json = nlohmann::json;

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

bool write_pipe_name_file(const std::wstring& path, const std::wstring& pipeName) {
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

    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream)
        return false;
    stream.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return stream.good();
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

void cancel_and_drain(HANDLE pipe, OVERLAPPED& overlapped) {
    CancelIoEx(pipe, &overlapped);
    WaitForSingleObject(overlapped.hEvent, 1000);
    DWORD ignored = 0;
    GetOverlappedResult(pipe, &overlapped, &ignored, FALSE);
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
                    cancel_and_drain(m_pipe, overlapped);
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
                    cancel_and_drain(m_pipe, overlapped);
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
        cancel_and_drain(pipe, overlapped);
        return false;
    }

    DWORD transferred = 0;
    return GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE ||
           GetLastError() == ERROR_PIPE_CONNECTED;
}

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
        if (!write_pipe_name_file(sidecar, pipeName)) {
            fprintf(stderr, "lvt: failed to write %s pipe bootstrap file\n",
                    options.frameworkLabel.c_str());
            return nullptr;
        }

        SECURITY_ATTRIBUTES securityAttributes{};
        securityAttributes.nLength = sizeof(securityAttributes);
        PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GRGW;;;WD)(A;;GRGW;;;AC)", SDDL_REVISION_1,
                &rawDescriptor, nullptr)) {
            DeleteFileW(sidecar.c_str());
            return nullptr;
        }
        wil::unique_hlocal securityDescriptor(rawDescriptor);
        securityAttributes.lpSecurityDescriptor = securityDescriptor.get();

        wil::unique_hfile pipe(CreateNamedPipeW(
            pipeName.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 1024 * 1024, 1024 * 1024, options.commandTimeoutMs,
            &securityAttributes));
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
            cancel_and_drain(pipe.get(), connectOverlapped);
            DeleteFileW(sidecar.c_str());
            return nullptr;
        }

        const bool connected = wait_for_pipe_connection(
            pipe.get(), process.get(), connectOverlapped, connectError,
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
                std::string ignored;
                send_command_locked("DISCONNECT", "{}", 2000, ignored);
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

        std::string treeJson;
        if (!send_command_locked(
                "GET_TREE", "{}", m_options.commandTimeoutMs, treeJson)) {
            return false;
        }
        return m_applyTree && m_applyTree(root, treeJson);
    }

    std::vector<ConnectionEvent> poll_events() override {
        return {};
    }

    bool is_alive() const override {
        return connection_alive();
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

    bool send_command_locked(
        const std::string& command, const std::string& arguments,
        DWORD timeoutMs, std::string& payload) {
        if (!connection_alive() || !m_io)
            return false;

        const uint64_t commandId = m_nextCommandId++;
        const std::string id = std::to_string(commandId);
        if (!m_io->write_line(
                "REQUEST\t" + id + "\t" + command + "\t" + arguments, timeoutMs)) {
            m_alive.store(false);
            return false;
        }

        std::string response;
        if (!m_io->read_line(timeoutMs, response)) {
            m_alive.store(false);
            return false;
        }

        const std::string prefix = "RESPONSE\t" + id + "\t";
        if (response.rfind(prefix, 0) != 0) {
            fprintf(stderr, "lvt: %s TAP DLL returned an uncorrelated response\n",
                    m_options.frameworkLabel.c_str());
            m_alive.store(false);
            return false;
        }
        const size_t statusEnd = response.find('\t', prefix.size());
        if (statusEnd == std::string::npos) {
            m_alive.store(false);
            return false;
        }
        const std::string status =
            response.substr(prefix.size(), statusEnd - prefix.size());
        payload = response.substr(statusEnd + 1);
        if (status == "OK")
            return true;
        if (status == "ERROR") {
            if (g_debug)
                fprintf(stderr, "lvt: %s managed command %s failed: %s\n",
                        m_options.frameworkLabel.c_str(), command.c_str(), payload.c_str());
            return false;
        }

        m_alive.store(false);
        return false;
    }

    DWORD m_pid = 0;
    std::wstring m_tapModuleName;
    ManagedConnectionOptions m_options;
    wil::unique_handle m_process;
    wil::unique_hfile m_pipe;
    std::unique_ptr<DuplexPipeLineIo> m_io;
    ManagedTreeApplier m_applyTree;
    ManagedConnectionCapabilities m_capabilities;
    mutable std::atomic_bool m_alive = false;
    uint64_t m_nextCommandId = 1;
    std::mutex m_commandMutex;
};

} // namespace

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
