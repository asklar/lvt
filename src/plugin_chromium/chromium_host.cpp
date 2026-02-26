// lvt_chromium_host.cpp — Native messaging host for the LVT Chromium extension.
// Relays JSON messages between Chrome's native messaging protocol (stdin/stdout)
// and a named pipe that lvt.exe connects to.
//
// Usage:
//   lvt_chromium_host.exe              — Run as native messaging host (Chrome spawns this)
//   lvt_chromium_host.exe --register   — Register native messaging host for Chrome + Edge

#include <Windows.h>
#include <sddl.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <atomic>
#include <fstream>

static const char* PIPE_NAME = "\\\\.\\pipe\\lvt_chromium";
static const char* HOST_NAME = "com.lvt.chromium";

static std::atomic<bool> g_running{true};

// ---------- Native messaging protocol ----------
// Messages are length-prefixed: 4 bytes (uint32 LE) followed by JSON.

static bool read_native_message(std::string& out) {
    uint32_t len = 0;
    DWORD bytesRead = 0;
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);

    if (!ReadFile(hStdin, &len, 4, &bytesRead, nullptr) || bytesRead != 4)
        return false;

    if (len == 0 || len > 4 * 1024 * 1024) // 4MB max
        return false;

    out.resize(len);
    DWORD totalRead = 0;
    while (totalRead < len) {
        if (!ReadFile(hStdin, out.data() + totalRead, len - totalRead, &bytesRead, nullptr) || bytesRead == 0)
            return false;
        totalRead += bytesRead;
    }
    return true;
}

static bool write_native_message(const std::string& msg) {
    uint32_t len = static_cast<uint32_t>(msg.size());
    DWORD bytesWritten = 0;
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);

    if (!WriteFile(hStdout, &len, 4, &bytesWritten, nullptr) || bytesWritten != 4)
        return false;
    if (!WriteFile(hStdout, msg.data(), len, &bytesWritten, nullptr) || bytesWritten != len)
        return false;
    FlushFileBuffers(hStdout);
    return true;
}

// ---------- Named pipe server ----------

static HANDLE create_pipe() {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    // Allow all users to connect (lvt.exe may run as different user context)
    ConvertStringSecurityDescriptorToSecurityDescriptorA(
        "D:(A;;GRGW;;;WD)(A;;GRGW;;;AC)", SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr);

    HANDLE pipe = CreateNamedPipeA(
        PIPE_NAME,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,             // max instances
        64 * 1024,     // out buffer
        4 * 1024 * 1024, // in buffer (DOM trees can be large)
        0,
        &sa);

    LocalFree(sa.lpSecurityDescriptor);
    return pipe;
}

// Read a length-prefixed message from the named pipe
static bool read_pipe_message(HANDLE pipe, std::string& out) {
    // Read a length-prefixed message from the named pipe
    uint32_t len = 0;
    DWORD bytesRead = 0;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    BOOL ok = ReadFile(pipe, &len, 4, &bytesRead, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 30000) != WAIT_OBJECT_0) {
            CancelIo(pipe);
            CloseHandle(ov.hEvent);
            return false;
        }
        if (!GetOverlappedResult(pipe, &ov, &bytesRead, FALSE)) {
            CloseHandle(ov.hEvent);
            return false;
        }
    }
    CloseHandle(ov.hEvent);
    if (bytesRead != 4 || len == 0 || len > 4 * 1024 * 1024)
        return false;

    out.resize(len);
    DWORD totalRead = 0;
    while (totalRead < len) {
        ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ok = ReadFile(pipe, out.data() + totalRead, len - totalRead, &bytesRead, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent, 30000) != WAIT_OBJECT_0) {
                CancelIo(pipe);
                CloseHandle(ov.hEvent);
                return false;
            }
            if (!GetOverlappedResult(pipe, &ov, &bytesRead, FALSE)) {
                CloseHandle(ov.hEvent);
                return false;
            }
        }
        CloseHandle(ov.hEvent);
        if (bytesRead == 0) return false;
        totalRead += bytesRead;
    }
    return true;
}

// Write a length-prefixed message to the named pipe
static bool write_pipe_message(HANDLE pipe, const std::string& msg) {
    uint32_t len = static_cast<uint32_t>(msg.size());
    DWORD bytesWritten = 0;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // Write length prefix
    BOOL ok = WriteFile(pipe, &len, 4, &bytesWritten, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(ov.hEvent, 5000);
        if (!GetOverlappedResult(pipe, &ov, &bytesWritten, FALSE)) {
            CloseHandle(ov.hEvent);
            return false;
        }
    }
    if (bytesWritten != 4) { CloseHandle(ov.hEvent); return false; }

    // Write message body
    ResetEvent(ov.hEvent);
    ok = WriteFile(pipe, msg.data(), len, &bytesWritten, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        WaitForSingleObject(ov.hEvent, 30000);
        if (!GetOverlappedResult(pipe, &ov, &bytesWritten, FALSE)) {
            CloseHandle(ov.hEvent);
            return false;
        }
    }

    CloseHandle(ov.hEvent);
    return bytesWritten == len;
}

// ---------- Registration ----------

static std::wstring get_exe_path() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

static std::string wstring_to_utf8(const std::wstring& ws) {
    if (ws.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

static bool register_host() {
    auto exePath = get_exe_path();
    auto exeDir = exePath.substr(0, exePath.find_last_of(L"\\/"));

    // Create the native messaging host manifest JSON
    // Path needs escaped backslashes for JSON
    auto pathUtf8 = wstring_to_utf8(exePath);
    std::string escapedPath;
    for (char c : pathUtf8) {
        if (c == '\\') escapedPath += "\\\\";
        else escapedPath += c;
    }

    // The extension ID is deterministic because manifest.json contains a fixed "key".
    // ID: pgknpnjnhiflafcaeafgpjonadhbpfok
    std::string manifest =
        "{\n"
        "  \"name\": \"" + std::string(HOST_NAME) + "\",\n"
        "  \"description\": \"LVT Chromium DOM inspector bridge\",\n"
        "  \"path\": \"" + escapedPath + "\",\n"
        "  \"type\": \"stdio\",\n"
        "  \"allowed_origins\": [\"chrome-extension://pgknpnjnhiflafcaeafgpjonadhbpfok/\"]\n"
        "}\n";

    // Write manifest file next to the exe
    auto manifestPath = wstring_to_utf8(exeDir) + "\\com.lvt.chromium.json";
    {
        std::ofstream f(manifestPath, std::ios::binary | std::ios::trunc);
        if (!f) {
            fprintf(stderr, "Failed to write manifest to %s\n", manifestPath.c_str());
            return false;
        }
        f.write(manifest.data(), manifest.size());
    }

    auto manifestPathW = exeDir + L"\\com.lvt.chromium.json";

    // Register in Windows registry for both Chrome and Edge
    const wchar_t* regPaths[] = {
        L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.lvt.chromium",
        L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\com.lvt.chromium",
    };

    bool ok = true;
    for (auto regPath : regPaths) {
        HKEY key;
        LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, regPath, 0, nullptr,
                                          0, KEY_SET_VALUE, nullptr, &key, nullptr);
        if (status != ERROR_SUCCESS) {
            fprintf(stderr, "Failed to create registry key: %ls (error %ld)\n", regPath, status);
            ok = false;
            continue;
        }

        status = RegSetValueExW(key, nullptr, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(manifestPathW.c_str()),
                                static_cast<DWORD>((manifestPathW.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(key);

        if (status != ERROR_SUCCESS) {
            fprintf(stderr, "Failed to set registry value (error %ld)\n", status);
            ok = false;
        } else {
            fprintf(stderr, "Registered: %ls\n", regPath);
        }
    }

    if (ok) {
        fprintf(stderr, "Native messaging host registered successfully.\n");
        fprintf(stderr, "Manifest: %s\n", manifestPath.c_str());
    }
    return ok;
}

// ---------- Main relay loop ----------

static void run_relay() {
    // Set stdin/stdout to binary mode
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    HANDLE pipe = create_pipe();
    if (pipe == INVALID_HANDLE_VALUE) {
        // Pipe may already exist from another host instance — not fatal,
        // but we can't relay without it.
        write_native_message("{\"type\":\"error\",\"message\":\"Failed to create named pipe\"}");
        return;
    }

    // Tell the extension we're ready
    write_native_message("{\"type\":\"ready\"}");

    // Thread 1: Read from named pipe (lvt requests) → forward to extension (stdout)
    std::thread pipeReader([&]() {
        while (g_running) {
            // Wait for lvt to connect
            OVERLAPPED ov = {};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            ConnectNamedPipe(pipe, &ov);
            DWORD err = GetLastError();

            if (err == ERROR_IO_PENDING) {
                // Wait up to 60s for lvt to connect, then loop to check g_running
                while (g_running) {
                    if (WaitForSingleObject(ov.hEvent, 1000) == WAIT_OBJECT_0)
                        break;
                }
                if (!g_running) {
                    CancelIo(pipe);
                    CloseHandle(ov.hEvent);
                    break;
                }
            } else if (err != ERROR_PIPE_CONNECTED && err != 0) {
                CloseHandle(ov.hEvent);
                Sleep(1000);
                continue;
            }
            CloseHandle(ov.hEvent);

            // lvt is connected — relay messages
            while (g_running) {
                std::string msg;
                if (!read_pipe_message(pipe, msg))
                    break;
                // Forward lvt's request to the extension
                if (!write_native_message(msg))
                    break;
            }

            DisconnectNamedPipe(pipe);
        }
    });

    // Main thread: Read from extension (stdin) → forward to named pipe
    while (g_running) {
        std::string msg;
        if (!read_native_message(msg)) {
            g_running = false;
            break;
        }
        // Forward extension's response to lvt via pipe
        write_pipe_message(pipe, msg);
    }

    g_running = false;
    if (pipeReader.joinable())
        pipeReader.join();

    CloseHandle(pipe);
}

// ---------- Entry point ----------

int main(int argc, char* argv[]) {
    if (argc > 1 && (strcmp(argv[1], "--register") == 0 || strcmp(argv[1], "-r") == 0)) {
        return register_host() ? 0 : 1;
    }

    run_relay();
    return 0;
}
