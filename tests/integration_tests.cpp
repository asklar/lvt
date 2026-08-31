// Integration tests for LVT
// Run: ctest --test-dir build -R integration

#include <gtest/gtest.h>
#include <sstream>
#include <Windows.h>
#include <CommCtrl.h>
#include <TlHelp32.h>
#include <wil/resource.h>
#include <cstdio>
#include <cwctype>
#include <algorithm>
#include <string>
#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <vector>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <nlohmann/json.hpp>
#include <thread>
#include "element_key.h"
#include "lvt_config.h"
#if LVT_ENABLE_WPF || LVT_ENABLE_WINFORMS
#include "providers/managed_connection.h"
#endif
#if LVT_ENABLE_WINFORMS
#include "providers/winforms_provider.h"
#endif
#if LVT_ENABLE_WPF
#include "providers/wpf_provider.h"
#endif
#include "tree_builder.h"

#include "apps/NativeControlsFixture/native_controls_fixture_ids.h"
#include "exact_hwnd_recycle_test_support.h"
#include "element_key.h"
#include "framework_detector.h"
#include "input.h"
#include "providers/native_message.h"
#include "providers/native_property_connection.h"
#include "providers/native_win_event.h"
#include "providers/connection_registry.h"
#ifdef LVT_ENABLE_UIA
#include "providers/uia_actions.h"
#include "providers/uia_provider.h"
#endif
#include "tree_builder.h"

using json = nlohmann::json;
namespace fs = std::filesystem;
namespace native_fixture = lvt::native_fixture;

#if LVT_ENABLE_WPF || LVT_ENABLE_WINFORMS
TEST(ManagedConnectionSecurity, RejectsSpoofedPipeClientProcess) {
    const std::wstring pipeName =
        L"\\\\.\\pipe\\lvt_spoof_test_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(GetTickCount64());
    wil::unique_hfile pipe(CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 5000, nullptr));
    ASSERT_TRUE(pipe);

    wil::unique_event event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(event);
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    ASSERT_FALSE(ConnectNamedPipe(pipe.get(), &overlapped));
    ASSERT_EQ(GetLastError(), ERROR_IO_PENDING);

    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION processInfo{};
    const std::wstring spoofPath =
        fs::path(MANAGED_PIPE_SPOOF_EXE_PATH).wstring();
    std::wstring command =
        L"\"" + spoofPath + L"\" \"" +
        pipeName + L"\"";
    ASSERT_TRUE(CreateProcessW(
        nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, nullptr, &startup, &processInfo));
    wil::unique_handle spoofProcess(processInfo.hProcess);
    wil::unique_handle spoofThread(processInfo.hThread);

    ASSERT_EQ(WaitForSingleObject(event.get(), 5000), WAIT_OBJECT_0);
    DWORD connectedBytes = 0;
    ASSERT_TRUE(GetOverlappedResult(
        pipe.get(), &overlapped, &connectedBytes, FALSE));
    EXPECT_FALSE(lvt::detail::managed_pipe_client_matches_pid(
        pipe.get(), GetCurrentProcessId()));

    ASSERT_TRUE(DisconnectNamedPipe(pipe.get()));
    ASSERT_EQ(WaitForSingleObject(spoofProcess.get(), 5000), WAIT_OBJECT_0);

    ResetEvent(event.get());
    ZeroMemory(&overlapped, sizeof(overlapped));
    overlapped.hEvent = event.get();
    ASSERT_FALSE(ConnectNamedPipe(pipe.get(), &overlapped));
    ASSERT_EQ(GetLastError(), ERROR_IO_PENDING);

    std::atomic_bool expectedClientOpened = false;
    std::thread expectedClient([&pipeName, &expectedClientOpened] {
        wil::unique_hfile client(CreateFileW(
            pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, 0, nullptr));
        if (!client)
            return;
        expectedClientOpened = true;
        char byte = 0;
        DWORD read = 0;
        ReadFile(client.get(), &byte, 1, &read, nullptr);
    });
    const DWORD expectedWait = WaitForSingleObject(event.get(), 5000);
    if (expectedWait == WAIT_OBJECT_0) {
        EXPECT_TRUE(GetOverlappedResult(
            pipe.get(), &overlapped, &connectedBytes, FALSE));
        EXPECT_TRUE(lvt::detail::managed_pipe_client_matches_pid(
            pipe.get(), GetCurrentProcessId()));
    }
    DisconnectNamedPipe(pipe.get());
    expectedClient.join();
    ASSERT_EQ(expectedWait, WAIT_OBJECT_0);
    EXPECT_TRUE(expectedClientOpened);
}

TEST(ManagedConnectionSecurity, RepeatedSpoofsDrainFinalPendingRearm) {
    const std::wstring pipeName =
        L"\\\\.\\pipe\\lvt_spoof_deadline_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(GetTickCount64());
    wil::unique_hfile pipe(CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1, 4096, 4096, 5000, nullptr));
    ASSERT_TRUE(pipe);

    wil::unique_event event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(event);
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    ASSERT_FALSE(ConnectNamedPipe(pipe.get(), &overlapped));
    const DWORD connectError = GetLastError();
    ASSERT_EQ(connectError, ERROR_IO_PENDING);

    const std::wstring spoofPath =
        fs::path(MANAGED_PIPE_SPOOF_EXE_PATH).wstring();
    std::atomic_int clientsCompleted = 0;
    std::thread clients([&] {
        for (int index = 0; index < 2; ++index) {
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION info{};
            std::wstring command =
                L"\"" + spoofPath + L"\" \"" + pipeName + L"\"";
            if (!CreateProcessW(
                    nullptr, command.data(), nullptr, nullptr, FALSE,
                    CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info))
                return;
            wil::unique_handle process(info.hProcess);
            wil::unique_handle thread(info.hThread);
            if (WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0)
                return;
            ++clientsCompleted;
        }
    });

    EXPECT_FALSE(lvt::detail::wait_for_expected_pipe_client(
        pipe.get(), nullptr, GetCurrentProcessId(), overlapped,
        connectError, 1500));
    clients.join();
    EXPECT_EQ(clientsCompleted.load(), 2);

    DWORD transferred = 0;
    SetLastError(ERROR_SUCCESS);
    EXPECT_FALSE(GetOverlappedResult(
        pipe.get(), &overlapped, &transferred, FALSE));
    EXPECT_EQ(GetLastError(), ERROR_OPERATION_ABORTED)
        << "the final re-armed connect must be synchronously drained";

    ResetEvent(event.get());
    ZeroMemory(&overlapped, sizeof(overlapped));
    overlapped.hEvent = event.get();
    ASSERT_FALSE(ConnectNamedPipe(pipe.get(), &overlapped));
    ASSERT_EQ(GetLastError(), ERROR_IO_PENDING);
    EXPECT_FALSE(lvt::detail::wait_for_expected_pipe_client(
        pipe.get(), nullptr, GetCurrentProcessId(), overlapped,
        ERROR_IO_PENDING, 0));
    SetLastError(ERROR_SUCCESS);
    EXPECT_FALSE(GetOverlappedResult(
        pipe.get(), &overlapped, &transferred, FALSE));
    EXPECT_EQ(GetLastError(), ERROR_OPERATION_ABORTED);
}
#endif

// Locate lvt.exe relative to this test binary
static std::string get_lvt_path() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    fs::path dir = fs::path(buf).parent_path();
    auto lvt = dir / "lvt.exe";
    if (fs::exists(lvt)) return lvt.string();
    // Try sibling directories (Debug/Release)
    for (auto& entry : fs::directory_iterator(dir.parent_path())) {
        auto candidate = entry.path() / "lvt.exe";
        if (fs::exists(candidate)) return candidate.string();
    }
    return "lvt.exe";  // fallback
}

// Run a command and capture stdout (stderr is suppressed)
// Wraps in cmd /c "..." to handle multiple quoted arguments correctly.
static std::string run_command(const std::string& cmd) {
    std::string result;
    std::array<char, 4096> buffer;
    FILE* pipe = _popen(cmd.c_str(), "r");
    if (!pipe) return {};
    while (fgets(buffer.data(), (int)buffer.size(), pipe)) {
        result += buffer.data();
    }
    _pclose(pipe);
    return result;
}

struct FirstWatchEvent {
    json event;
    std::string output;
    bool started = false;
};

static FirstWatchEvent capture_first_watch_event(
    std::string command, DWORD timeoutMs = 10000) {
    FirstWatchEvent result;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    wil::unique_handle readEnd;
    wil::unique_handle writeEnd;
    if (!CreatePipe(
            readEnd.put(), writeEnd.put(), &security, 0) ||
        !SetHandleInformation(
            readEnd.get(), HANDLE_FLAG_INHERIT, 0)) {
        return result;
    }

    STARTUPINFOA startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd.get();
    startup.hStdError = writeEnd.get();
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessA(
            nullptr, command.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr,
            &startup, &processInfo)) {
        return result;
    }
    result.started = true;
    wil::unique_handle process(processInfo.hProcess);
    wil::unique_handle thread(processInfo.hThread);
    writeEnd.reset();

    const auto parseEvent = [&] {
        size_t cursor = 0;
        while (cursor < result.output.size()) {
            const auto newline =
                result.output.find('\n', cursor);
            if (newline == std::string::npos)
                break;
            auto parsed = json::parse(
                result.output.substr(
                    cursor, newline - cursor),
                nullptr, false);
            if (!parsed.is_discarded() &&
                parsed.contains("event")) {
                result.event = std::move(parsed);
                return true;
            }
            cursor = newline + 1;
        }
        return false;
    };
    const auto deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline &&
           !parseEvent()) {
        DWORD available = 0;
        if (!PeekNamedPipe(
                readEnd.get(), nullptr, 0, nullptr,
                &available, nullptr)) {
            break;
        }
        if (available == 0) {
            if (WaitForSingleObject(
                    process.get(), 0) == WAIT_OBJECT_0) {
                break;
            }
            Sleep(20);
            continue;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!ReadFile(
                readEnd.get(), chunk.data(), available,
                &read, nullptr) ||
            read == 0) {
            break;
        }
        result.output.append(chunk, 0, read);
    }

    TerminateProcess(process.get(), 0);
    WaitForSingleObject(process.get(), 5000);
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(
                readEnd.get(), nullptr, 0, nullptr,
                &available, nullptr) ||
            available == 0) {
            break;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!ReadFile(
                readEnd.get(), chunk.data(), available,
                &read, nullptr) ||
            read == 0) {
            break;
        }
        result.output.append(chunk, 0, read);
    }
    parseEvent();
    return result;
}

class LiveWatchCapture {
public:
    ~LiveWatchCapture() {
        stop();
    }

    bool start(std::string command) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        wil::unique_handle writeEnd;
        if (!CreatePipe(
                readEnd_.put(), writeEnd.put(), &security, 0) ||
            !SetHandleInformation(
                readEnd_.get(), HANDLE_FLAG_INHERIT, 0)) {
            return false;
        }
        STARTUPINFOA startup{sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = writeEnd.get();
        startup.hStdError = writeEnd.get();
        PROCESS_INFORMATION processInfo{};
        if (!CreateProcessA(
                nullptr, command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW, nullptr, nullptr,
                &startup, &processInfo)) {
            return false;
        }
        process_.reset(processInfo.hProcess);
        thread_.reset(processInfo.hThread);
        return true;
    }

    size_t mark() {
        read_available();
        return output_.size();
    }

    bool wait_for_event(
        size_t offset, const std::string& type,
        const std::string& key, DWORD timeoutMs) {
        const auto deadline = GetTickCount64() + timeoutMs;
        while (GetTickCount64() < deadline) {
            read_available();
            if (event_seen(offset, type, key))
                return true;
            if (process_ &&
                WaitForSingleObject(
                    process_.get(), 0) == WAIT_OBJECT_0) {
                break;
            }
            Sleep(20);
        }
        read_available();
        return event_seen(offset, type, key);
    }

    bool event_seen(
        size_t offset, const std::string& type,
        const std::string& key = {}) const {
        size_t cursor = offset;
        if (cursor > 0 && cursor <= output_.size() &&
            output_[cursor - 1] != '\n') {
            const auto next = output_.find('\n', cursor);
            if (next == std::string::npos)
                return false;
            cursor = next + 1;
        }
        while (cursor < output_.size()) {
            const auto newline = output_.find('\n', cursor);
            if (newline == std::string::npos)
                break;
            const auto event = json::parse(
                output_.substr(cursor, newline - cursor),
                nullptr, false);
            if (!event.is_discarded() &&
                event.value("event", "") == type &&
                (key.empty() ||
                 event.value("key", "") == key)) {
                return true;
            }
            cursor = newline + 1;
        }
        return false;
    }

    void drain() {
        read_available();
    }

    void stop() {
        if (!process_)
            return;
        if (WaitForSingleObject(
                process_.get(), 0) != WAIT_OBJECT_0) {
            TerminateProcess(process_.get(), 0);
            WaitForSingleObject(process_.get(), 5000);
        }
        read_available();
        process_.reset();
        thread_.reset();
        readEnd_.reset();
    }

    const std::string& output() const {
        return output_;
    }

    bool running() const {
        return process_ &&
               WaitForSingleObject(
                   process_.get(), 0) == WAIT_TIMEOUT;
    }

private:
    void read_available() {
        if (!readEnd_)
            return;
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(
                    readEnd_.get(), nullptr, 0, nullptr,
                    &available, nullptr) ||
                available == 0) {
                return;
            }
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!ReadFile(
                    readEnd_.get(), chunk.data(), available,
                    &read, nullptr) ||
                read == 0) {
                return;
            }
            output_.append(chunk, 0, read);
        }
    }

    wil::unique_handle readEnd_;
    wil::unique_handle process_;
    wil::unique_handle thread_;
    std::string output_;
};

// Build a command string
static std::string make_cmd(const std::string& lvt, const std::string& args) {
    return "\"" + lvt + "\" " + args;
}

static std::string cmd_escape_arg(std::string arg) {
    std::string escaped;
    escaped.reserve(arg.size());
    for (char c : arg) {
        if (c == '|' || c == '&' || c == '<' || c == '>' || c == '^')
            escaped += '^';
        escaped += c;
    }
    return escaped;
}

static std::string trim_crlf(const std::string& value) {
    auto end = value.find_last_not_of("\r\n");
    if (end == std::string::npos)
        return {};
    return value.substr(0, end + 1);
}

// Read a repository file for tests that assert on documentation. Line endings
// are normalised so a checkout with autocrlf does not look like a difference.
static std::string read_text_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name, const std::string& value)
        : name_(name) {
        const DWORD needed = GetEnvironmentVariableA(name, nullptr, 0);
        if (needed > 0) {
            previous_.resize(needed - 1);
            GetEnvironmentVariableA(name, previous_.data(), needed);
            hadPrevious_ = true;
        }
        SetEnvironmentVariableA(name, value.c_str());
    }

    ~ScopedEnvironmentVariable() {
        SetEnvironmentVariableA(
            name_.c_str(), hadPrevious_ ? previous_.c_str() : nullptr);
    }

private:
    std::string name_;
    std::string previous_;
    bool hadPrevious_ = false;
};

TEST(InputForeground, NullForegroundFallbackRestoresMinimizedTargetRepeatedly) {
    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW, L"STATIC", L"LVT foreground fallback",
        WS_OVERLAPPEDWINDOW,
        120, 120, 320, 180,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_TRUE(IsWindow(window));
    auto cleanup = wil::scope_exit([&] {
        if (IsWindow(window))
            DestroyWindow(window);
    });
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    SetForegroundWindow(window);

    const fs::path statsPath =
        fs::path(LVT_SOURCE_DIR) /
        ("foreground-null-" +
         std::to_string(GetCurrentProcessId()) + ".log");
    std::error_code ec;
    fs::remove(statsPath, ec);
    ScopedEnvironmentVariable firstFailure(
        "LVT_TEST_FOREGROUND_FIRST_SET_FAILURE", "1");
    ScopedEnvironmentVariable nullForeground(
        "LVT_TEST_NULL_CURRENT_FOREGROUND", "1");
    ScopedEnvironmentVariable assumeSuccess(
        "LVT_TEST_FOREGROUND_ASSUME_SUCCESS", "1");
    ScopedEnvironmentVariable stats(
        "LVT_TEST_FOREGROUND_STATS", statsPath.string());
    for (int attempt = 0; attempt < 3; ++attempt) {
        ShowWindow(window, SW_MINIMIZE);
        ASSERT_TRUE(IsIconic(window));
        EXPECT_TRUE(lvt::bring_to_foreground(window));
        EXPECT_FALSE(IsIconic(window));
    }
    std::ifstream stream(statsPath);
    const std::string contents{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    size_t retries = 0;
    for (size_t at = 0;
         (at = contents.find(
              "null-foreground-retry", at)) !=
         std::string::npos;
         at += 21) {
        ++retries;
    }
    EXPECT_EQ(retries, 3u);
    stream.close();
    fs::remove(statsPath, ec);
}

class PluginTargetProcess {
public:
    bool start() {
        const fs::path executable = NATIVE_CONTROLS_FIXTURE_EXE_PATH;
        if (!fs::exists(executable))
            return false;

        STARTUPINFOA startup{sizeof(startup)};
        PROCESS_INFORMATION info{};
        std::string command = "\"" + executable.string() + "\"";
        if (!CreateProcessA(
                nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                executable.parent_path().string().c_str(), &startup, &info)) {
            return false;
        }
        process_.reset(info.hProcess);
        thread_.reset(info.hThread);
        pid_ = info.dwProcessId;
        WaitForInputIdle(process_.get(), 5000);

        for (int attempt = 0; attempt < 50 && !hwnd_; ++attempt) {
            EnumWindows([](HWND hwnd, LPARAM parameter) -> BOOL {
                auto* self =
                    reinterpret_cast<PluginTargetProcess*>(parameter);
                DWORD owner = 0;
                GetWindowThreadProcessId(hwnd, &owner);
                if (owner == self->pid_ && IsWindowVisible(hwnd)) {
                    self->hwnd_ = hwnd;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(this));
            if (!hwnd_)
                Sleep(100);
        }
        return hwnd_ != nullptr;
    }

    void stop() {
        if (process_) {
            TerminateProcess(process_.get(), 0);
            WaitForSingleObject(process_.get(), 5000);
            process_.reset();
        }
    }

    ~PluginTargetProcess() {
        stop();
    }

    HWND hwnd() const { return hwnd_; }

    std::string hwnd_arg() const {
        char text[64]{};
        snprintf(text, sizeof(text), "--hwnd 0x%llX",
                 static_cast<unsigned long long>(
                     reinterpret_cast<uintptr_t>(hwnd_)));
        return text;
    }

private:
    wil::unique_process_handle process_;
    wil::unique_handle thread_;
    DWORD pid_ = 0;
    HWND hwnd_ = nullptr;
};

static fs::path plugin_stats_path(const std::string& testName) {
    return fs::path(get_lvt_path()).parent_path() /
           ("fake-plugin-" + testName + "-" +
            std::to_string(GetCurrentProcessId()) + ".log");
}

static std::vector<std::string> read_plugin_stats(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

static size_t count_plugin_stats(
    const std::vector<std::string>& lines, const std::string& prefix) {
    return static_cast<size_t>(std::count_if(
        lines.begin(), lines.end(), [&prefix](const std::string& line) {
            return line.rfind(prefix, 0) == 0;
        }));
}

struct WatchPluginResult {
    std::string output;
    std::vector<std::string> stats;
    DWORD exitCode = STILL_ACTIVE;
};

static WatchPluginResult run_plugin_watch(
    PluginTargetProcess& target, const fs::path& pluginDir,
    const fs::path& statsPath, const std::string& option,
    const std::string& failOpenAt, const std::string& failGetAt,
    size_t desiredRefreshes, const std::string& elementRef = {},
    const fs::path& detectionFile = {},
    const std::function<bool(
        const std::vector<std::string>&, ULONGLONG)>& driver = {},
    const std::string& malformedGetAt = "0") {
    std::error_code ec;
    fs::remove(statsPath, ec);

    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    wil::unique_handle readEnd;
    wil::unique_handle writeEnd;
    if (!CreatePipe(readEnd.put(), writeEnd.put(), &attributes, 0))
        return {};
    SetHandleInformation(readEnd.get(), HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd.get();
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION info{};
    std::string command =
        make_cmd(
            get_lvt_path(),
            target.hwnd_arg() + " --title \"" + option +
                "\" watch --interval 50" +
                (elementRef.empty()
                     ? std::string()
                     : " --element \"" + elementRef + "\""));
    {
        ScopedEnvironmentVariable pluginPath(
            "LVT_PLUGIN_DIR", pluginDir.string());
        ScopedEnvironmentVariable enabled("LVT_FAKE_PLUGIN_ENABLE", "1");
        ScopedEnvironmentVariable state(
            "LVT_FAKE_PLUGIN_STATE", statsPath.string());
        ScopedEnvironmentVariable fail(
            "LVT_FAKE_PLUGIN_FAIL_GET_AT", failGetAt);
        ScopedEnvironmentVariable malformed(
            "LVT_FAKE_PLUGIN_MALFORMED_GET_AT", malformedGetAt);
        ScopedEnvironmentVariable failOpen(
            "LVT_FAKE_PLUGIN_FAIL_OPEN_AT", failOpenAt);
        ScopedEnvironmentVariable events(
            "LVT_FAKE_PLUGIN_EMIT_EVENTS", "1");
        ScopedEnvironmentVariable delay(
            "LVT_FAKE_PLUGIN_GET_DELAY_MS", "0");
        ScopedEnvironmentVariable detectFile(
            "LVT_FAKE_PLUGIN_DETECT_FILE", detectionFile.string());
        ScopedEnvironmentVariable detectDelayAt(
            "LVT_FAKE_PLUGIN_DELAY_DETECT_AT", "0");
        ScopedEnvironmentVariable detectDelay(
            "LVT_FAKE_PLUGIN_DETECT_DELAY_MS", "0");
        if (!CreateProcessA(
                nullptr, command.data(), nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) {
            return {};
        }
    }
    wil::unique_process_handle process(info.hProcess);
    wil::unique_handle thread(info.hThread);
    writeEnd.reset();

    WatchPluginResult result;
    const auto startedAt = GetTickCount64();
    const auto deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
        DWORD available = 0;
        if (PeekNamedPipe(
                readEnd.get(), nullptr, 0, nullptr, &available, nullptr) &&
            available > 0) {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (ReadFile(
                    readEnd.get(), chunk.data(), available, &read, nullptr)) {
                result.output.append(chunk, 0, read);
            }
        }
        const auto stats = read_plugin_stats(statsPath);
        if (driver && driver(stats, GetTickCount64() - startedAt))
            break;
        const size_t refreshes =
            count_plugin_stats(stats, "get ") +
            count_plugin_stats(stats, "enrich ");
        if (!driver && refreshes >= desiredRefreshes)
            break;
        if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0)
            break;
        Sleep(25);
    }

    target.stop();
    if (WaitForSingleObject(process.get(), 10000) == WAIT_TIMEOUT)
        TerminateProcess(process.get(), 1);
    WaitForSingleObject(process.get(), 5000);
    GetExitCodeProcess(process.get(), &result.exitCode);

    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(
                readEnd.get(), nullptr, 0, nullptr, &available, nullptr) ||
            available == 0) {
            break;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!ReadFile(
                readEnd.get(), chunk.data(), available, &read, nullptr) ||
            read == 0) {
            break;
        }
        result.output.append(chunk, 0, read);
    }
    result.stats = read_plugin_stats(statsPath);
    fs::remove(statsPath, ec);
    return result;
}

static std::string discover_plugin_node_key(
    PluginTargetProcess& target, const fs::path& pluginDir,
    const fs::path& statsPath, const std::string& option) {
    ScopedEnvironmentVariable pluginPath(
        "LVT_PLUGIN_DIR", pluginDir.string());
    ScopedEnvironmentVariable enabled("LVT_FAKE_PLUGIN_ENABLE", "1");
    ScopedEnvironmentVariable state(
        "LVT_FAKE_PLUGIN_STATE", statsPath.string());
    ScopedEnvironmentVariable fail(
        "LVT_FAKE_PLUGIN_FAIL_GET_AT", "0");
    ScopedEnvironmentVariable malformed(
        "LVT_FAKE_PLUGIN_MALFORMED_GET_AT", "0");
    ScopedEnvironmentVariable failOpen(
        "LVT_FAKE_PLUGIN_FAIL_OPEN_AT", "0");

    const auto output = run_command(make_cmd(
        get_lvt_path(), target.hwnd_arg()));
    const auto tree = json::parse(output, nullptr, false);
    if (tree.is_discarded() || !tree.contains("root"))
        return {};

    std::function<const json*(const json&)> findPluginNode =
        [&](const json& element) -> const json* {
        if (element.value("type", "") == "FakePluginNode")
            return &element;
        if (element.contains("children") && element["children"].is_array()) {
            for (const auto& child : element["children"]) {
                if (const auto* found = findPluginNode(child))
                    return found;
            }
        }
        return nullptr;
    };
    const auto* pluginNode = findPluginNode(tree["root"]);
    return pluginNode ? pluginNode->value("key", "") : std::string();
}

TEST(PluginPersistentWatch, ReusesPollsReconnectsAndClosesExactlyOnce) {
    PluginTargetProcess target;
    ASSERT_TRUE(target.start());
    const auto statsPath = plugin_stats_path("watch-v2");
    const auto result = run_plugin_watch(
        target, LVT_FAKE_PLUGIN_V2_DIR, statsPath, "watch-filter",
        "1", "2", 5);

    EXPECT_EQ(result.exitCode, 0u);
    EXPECT_NE(result.output.find("FakePluginNode"), std::string::npos);
    EXPECT_EQ(count_plugin_stats(result.stats, "open "), 3u);
    EXPECT_EQ(count_plugin_stats(result.stats, "open_failed "), 1u);
    EXPECT_EQ(count_plugin_stats(result.stats, "close"), 2u);
    EXPECT_EQ(count_plugin_stats(result.stats, "enrich "), 0u);

    const auto gets = count_plugin_stats(result.stats, "get ");
    const auto failed = count_plugin_stats(result.stats, "get_failed ");
    const auto frees = count_plugin_stats(result.stats, "free");
    EXPECT_GE(gets, 5u);
    EXPECT_EQ(failed, 1u);
    EXPECT_EQ(frees, gets - failed);

    const auto polls = count_plugin_stats(result.stats, "poll");
    EXPECT_GT(polls, 0u);
    EXPECT_EQ(
        count_plugin_stats(result.stats, "events_free"), polls);

    for (const auto& line : result.stats) {
        if (line.rfind("get ", 0) == 0)
            EXPECT_NE(line.find("filter=watch-filter"), std::string::npos);
    }
}

TEST(PluginPersistentWatch, V1AndPartialV2RemainOneShot) {
    struct PluginCase {
        const char* directory;
        const char* name;
    };
    const PluginCase cases[] = {
        {LVT_FAKE_PLUGIN_V1_DIR, "v1"},
        {LVT_FAKE_PLUGIN_PARTIAL_DIR, "partial"},
    };

    for (const auto& pluginCase : cases) {
        SCOPED_TRACE(pluginCase.name);
        PluginTargetProcess target;
        ASSERT_TRUE(target.start());
        const auto statsPath =
            plugin_stats_path(std::string("watch-") + pluginCase.name);
        const auto result = run_plugin_watch(
            target, pluginCase.directory, statsPath, "legacy-filter",
            "0", "0", 3);

        EXPECT_EQ(result.exitCode, 0u);
        EXPECT_NE(result.output.find("FakePluginNode"), std::string::npos);
        const auto enriches =
            count_plugin_stats(result.stats, "enrich ");
        EXPECT_GE(enriches, 3u);
        EXPECT_EQ(count_plugin_stats(result.stats, "free"), enriches);
        EXPECT_EQ(count_plugin_stats(result.stats, "open "), 0u);
        EXPECT_EQ(count_plugin_stats(result.stats, "get "), 0u);
        EXPECT_EQ(count_plugin_stats(result.stats, "poll"), 0u);
        EXPECT_EQ(count_plugin_stats(result.stats, "close"), 0u);
        for (const auto& line : result.stats) {
            if (line.rfind("enrich ", 0) == 0)
                EXPECT_NE(
                    line.find("filter=legacy-filter"), std::string::npos);
        }
    }
}

TEST(PluginPersistentWatch, ReconcilesLateDetectionAndRemoval) {
    PluginTargetProcess target;
    ASSERT_TRUE(target.start());
    const auto statsPath = plugin_stats_path("watch-reconcile");
    const auto markerPath = plugin_stats_path("watch-detected");
    std::error_code ec;
    fs::remove(markerPath, ec);

    int phase = 0;
    const auto driver =
        [&](const std::vector<std::string>& stats, ULONGLONG elapsed) {
            if (phase == 0 && elapsed >= 300) {
                std::ofstream marker(markerPath, std::ios::binary);
                marker << "detected";
                marker.close();
                phase = 1;
            }
            if (phase == 1 &&
                count_plugin_stats(stats, "get ") >= 2) {
                fs::remove(markerPath, ec);
                phase = 2;
            }
            return phase == 2 &&
                   count_plugin_stats(stats, "close") >= 1;
        };
    const auto started = GetTickCount64();
    const auto result = run_plugin_watch(
        target, LVT_FAKE_PLUGIN_V2_DIR, statsPath, "late-filter",
        "0", "0", 0, {}, markerPath, driver);
    const auto elapsed = GetTickCount64() - started;

    EXPECT_EQ(result.exitCode, 0u);
    EXPECT_LT(elapsed, 5000u)
        << "obsolete plugin connection was not released during watch";
    EXPECT_EQ(phase, 2);
    EXPECT_EQ(count_plugin_stats(result.stats, "open "), 1u);
    EXPECT_EQ(count_plugin_stats(result.stats, "close"), 1u);
    EXPECT_GE(count_plugin_stats(result.stats, "get "), 2u);
    EXPECT_NE(result.output.find("FakePluginNode"), std::string::npos);
    EXPECT_NE(result.output.find("\"event\":\"added\""), std::string::npos);
    EXPECT_NE(result.output.find("\"event\":\"removed\""), std::string::npos);
    fs::remove(markerPath, ec);
}

TEST(PluginPersistentWatch, ScopedPluginFailurePreservesPreviousSnapshot) {
    PluginTargetProcess target;
    ASSERT_TRUE(target.start());
    const auto statsPath = plugin_stats_path("watch-scoped");
    const auto pluginKey = discover_plugin_node_key(
        target, LVT_FAKE_PLUGIN_V2_DIR, statsPath, "scoped-filter");
    ASSERT_FALSE(pluginKey.empty());
    const auto result = run_plugin_watch(
        target, LVT_FAKE_PLUGIN_V2_DIR, statsPath, "scoped-filter",
        "0", "0", 4, pluginKey, {}, {}, "2");

    EXPECT_EQ(result.exitCode, 0u);
    EXPECT_NE(result.output.find("FakePluginNode"), std::string::npos);
    EXPECT_NE(result.output.find(pluginKey), std::string::npos)
        << "the one-shot plugin key must remain the scoped watch root key";
    EXPECT_EQ(result.output.find("\"event\":\"removed\""), std::string::npos)
        << result.output;
    EXPECT_EQ(count_plugin_stats(result.stats, "get_malformed "), 1u);
    EXPECT_EQ(count_plugin_stats(result.stats, "enrich "), 0u);
    EXPECT_EQ(count_plugin_stats(result.stats, "open "), 2u);
    EXPECT_EQ(count_plugin_stats(result.stats, "close"), 2u);
}

// Launch a dedicated Notepad instance for testing
class NotepadFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Create a unique temp file so the Notepad title is predictable
        s_temp_file = (fs::temp_directory_path() / "lvt_integration_test.txt").string();
        {
            std::ofstream f(s_temp_file);
            f << "LVT integration test file\n";
        }

        STARTUPINFOA si = {sizeof(si)};
        s_pi = {};
        std::string cmd = "notepad.exe \"" + s_temp_file + "\"";
        CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                      0, nullptr, nullptr, &si, &s_pi);
        s_process.reset(s_pi.hProcess);
        s_thread.reset(s_pi.hThread);
        if (s_pi.hProcess) {
            WaitForInputIdle(s_pi.hProcess, 5000);
        }
        Sleep(5000);  // Wait for WinUI3 Notepad to fully initialize

        // Modern Notepad may launch through an App Execution Alias, so the
        // PID from CreateProcess may not be the actual window owner.
        // Find the window by title instead.
        s_hwnd = nullptr;
        for (int attempt = 0; attempt < 10 && !s_hwnd; attempt++) {
            EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                char title[256];
                GetWindowTextA(hwnd, title, sizeof(title));
                if (strstr(title, "lvt_integration_test") && IsWindowVisible(hwnd)) {
                    *reinterpret_cast<HWND*>(lParam) = hwnd;
                    return FALSE;
                }
                return TRUE;
            }, reinterpret_cast<LPARAM>(&s_hwnd));
            if (!s_hwnd) Sleep(1000);
        }

        if (s_hwnd) {
            GetWindowThreadProcessId(s_hwnd, &s_pid);
        } else {
            // Fallback to CreateProcess PID
            s_pid = s_pi.dwProcessId;
        }
    }

    static void TearDownTestSuite() {
        if (s_pi.hProcess) {
            TerminateProcess(s_pi.hProcess, 0);
            s_process.reset();
            s_thread.reset();
            s_pi.hProcess = nullptr;
            s_pi.hThread = nullptr;
        }
        fs::remove(s_temp_file);
    }

    static std::string get_pid_arg() {
        if (s_hwnd) {
            // Use HWND for reliable targeting
            char buf[32];
            sprintf_s(buf, "--hwnd 0x%llX", (unsigned long long)(uintptr_t)s_hwnd);
            return buf;
        }
        return "--pid " + std::to_string(s_pid);
    }

    static std::string s_temp_file;
    static PROCESS_INFORMATION s_pi;
    static wil::unique_handle s_process;
    static wil::unique_handle s_thread;
    static DWORD s_pid;
    static HWND s_hwnd;
};

std::string NotepadFixture::s_temp_file;
PROCESS_INFORMATION NotepadFixture::s_pi = {};
wil::unique_handle NotepadFixture::s_process;
wil::unique_handle NotepadFixture::s_thread;
DWORD NotepadFixture::s_pid = 0;
HWND NotepadFixture::s_hwnd = nullptr;

// ---- Basic functionality ----

TEST_F(NotepadFixture, CanDumpJsonTree) {
    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg()));
    ASSERT_FALSE(output.empty()) << "lvt produced no output";

    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << "Output is not valid JSON";
    EXPECT_TRUE(j.contains("target"));
    EXPECT_TRUE(j.contains("frameworks"));
    EXPECT_TRUE(j.contains("root"));
}

TEST_F(NotepadFixture, TargetInfo) {
    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg()));
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded());

    EXPECT_TRUE(j["target"]["pid"].is_number());
    EXPECT_GT(j["target"]["pid"].get<int>(), 0);
    auto proc = j["target"]["processName"].get<std::string>();
    // Notepad.exe or notepad.exe
    std::string lower_proc = proc;
    for (auto& c : lower_proc) c = (char)tolower(c);
    EXPECT_NE(lower_proc.find("notepad"), std::string::npos);
}

TEST_F(NotepadFixture, FrameworkDetection) {
    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg() + " frameworks"));
    ASSERT_FALSE(output.empty());
    // Notepad should at least have win32
    EXPECT_NE(output.find("win32"), std::string::npos);
}

TEST_F(NotepadFixture, TreeHasElements) {
    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg()));
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded());

    // Root should have an id
    EXPECT_EQ(j["root"]["id"], "e0");
    // Root should have children (Notepad has child windows)
    EXPECT_TRUE(j["root"].contains("children"));
    EXPECT_GT(j["root"]["children"].size(), 0u);
}

TEST_F(NotepadFixture, XmlOutput) {
    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg() + " --format xml"));
    ASSERT_FALSE(output.empty());

    EXPECT_NE(output.find("<LiveVisualTree"), std::string::npos);
    EXPECT_NE(output.find("</LiveVisualTree>"), std::string::npos);
    EXPECT_NE(output.find("id=\"e0\""), std::string::npos);
}

TEST_F(NotepadFixture, DepthLimit) {
    auto lvt = get_lvt_path();
    auto shallow = run_command(make_cmd(lvt, get_pid_arg() + " --depth 0"));
    auto full = run_command(make_cmd(lvt, get_pid_arg()));

    ASSERT_FALSE(shallow.empty());
    ASSERT_FALSE(full.empty());
    // Depth-limited output should be no longer than full output
    EXPECT_LE(shallow.size(), full.size());
}

TEST_F(NotepadFixture, ElementSubtree) {
    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg() + " --element e1"));
    if (output.empty()) {
        GTEST_SKIP() << "No output for --element e1 (element may not exist)";
    }
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded());
    // Root of output should be e1
    EXPECT_EQ(j["root"]["id"], "e1");
}

TEST_F(NotepadFixture, ScreenshotCapture) {
    auto lvt = get_lvt_path();
    // Use build directory (no spaces) to avoid cmd.exe quoting issues
    auto tmpFile = fs::path(lvt).parent_path() / "lvt_test_screenshot.png";
    fs::remove(tmpFile);

    auto output = run_command(make_cmd(lvt,
        get_pid_arg() + " screenshot --output " + tmpFile.string()));

    // the screenshot verb writes a PNG, not a tree, so stdout stays empty
    EXPECT_TRUE(output.empty()) << "the screenshot verb should not write a tree to stdout";
    // File should exist and be a valid PNG
    EXPECT_TRUE(fs::exists(tmpFile)) << "Screenshot file was not created";
    if (fs::exists(tmpFile)) {
        auto size = fs::file_size(tmpFile);
        EXPECT_GT(size, 100u) << "Screenshot file is too small to be a valid PNG";

        // Check PNG magic bytes
        std::ifstream f(tmpFile, std::ios::binary);
        char magic[8];
        f.read(magic, 8);
        EXPECT_EQ(magic[1], 'P');
        EXPECT_EQ(magic[2], 'N');
        EXPECT_EQ(magic[3], 'G');
    }
    fs::remove(tmpFile);
}

TEST_F(NotepadFixture, ScreenshotAndDumpAreSeparateInvocations) {
    // `--screenshot <file> --dump` used to do both at once. Splitting the verbs
    // makes that two commands, which is the trade for each verb having one job.
    auto lvt = get_lvt_path();
    auto tmpFile = fs::path(lvt).parent_path() / "lvt_test_both.png";
    fs::remove(tmpFile);

    auto shot = run_command(make_cmd(lvt,
        get_pid_arg() + " screenshot --output " + tmpFile.string()));
    EXPECT_TRUE(shot.empty()) << "the screenshot verb should not write a tree to stdout";
    ASSERT_TRUE(fs::exists(tmpFile)) << "Screenshot file was not created";
    fs::remove(tmpFile);

    auto dumped = run_command(make_cmd(lvt, get_pid_arg() + " dump"));
    ASSERT_FALSE(dumped.empty()) << "the dump verb should write a tree to stdout";
    auto j = json::parse(dumped, nullptr, false);
    EXPECT_FALSE(j.is_discarded()) << "stdout should be valid JSON";
}

TEST_F(NotepadFixture, OutputToFile) {
    auto lvt = get_lvt_path();
    auto tmpFile = fs::path(lvt).parent_path() / "lvt_test_output.json";
    fs::remove(tmpFile);

    run_command(make_cmd(lvt,
        get_pid_arg() + " --output " + tmpFile.string()));

    EXPECT_TRUE(fs::exists(tmpFile)) << "Output file was not created";
    if (fs::exists(tmpFile)) {
        std::ifstream f(tmpFile);
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        auto j = json::parse(content, nullptr, false);
        EXPECT_FALSE(j.is_discarded()) << "Output file is not valid JSON";
    }
    fs::remove(tmpFile);
}

// ---- Error handling ----

TEST(LvtCli, NoArgs) {
    auto lvt = get_lvt_path();
    auto ret = system(("\"" + lvt + "\" >nul").c_str());
    EXPECT_NE(ret, 0) << "Should return non-zero with no args";
}

TEST(LvtCli, InvalidHwnd) {
    auto lvt = get_lvt_path();
    auto ret = system(("\"" + lvt + "\" --hwnd 0xDEADBEEF >nul").c_str());
    EXPECT_NE(ret, 0) << "Should fail with invalid HWND";
}

TEST(LvtCli, UnknownArg) {
    auto lvt = get_lvt_path();
    auto ret = system(("\"" + lvt + "\" --bogus >nul").c_str());
    EXPECT_NE(ret, 0) << "Should fail with unknown argument";
}

// ---- Bounds validation (Win32) ----

// Reasonable screen coordinate range covering multi-monitor setups.
static constexpr int kMinReasonableCoord = -10000;
static constexpr int kMaxReasonableCoord = 50000;
static constexpr int kMaxReasonableSize  = 50000;

// Recursively collect all elements from JSON tree
static void collect_json_elements(const json& el, std::vector<const json*>& out) {
    out.push_back(&el);
    if (el.contains("children") && el["children"].is_array()) {
        for (auto& child : el["children"]) {
            collect_json_elements(child, out);
        }
    }
}

static void collect_stable_win32_map(const json& el, const std::string& path,
                                     std::map<std::string, std::string>& out) {
    if (el.value("framework", "") == "win32") {
        auto key = el.value("key", "");
        if (!key.empty()) {
            out[key] = el.value("framework", "") + "|" +
                       el.value("type", "") + "|" +
                       el.value("className", "") + "|" + path;
        }
    }

    if (el.contains("children") && el["children"].is_array()) {
        for (size_t i = 0; i < el["children"].size(); ++i) {
            collect_stable_win32_map(el["children"][i], path + "." + std::to_string(i), out);
        }
    }
}

static void collect_key_contract_map(const json& el, const std::string& path,
                                     std::map<std::string, std::string>& out) {
    auto key = el.value("key", "");
    if (!key.empty()) {
        out[key] = el.value("framework", "") + "|" +
                   el.value("type", "") + "|" +
                   el.value("className", "") + "|" + path;
    }

    if (el.contains("children") && el["children"].is_array()) {
        for (size_t i = 0; i < el["children"].size(); ++i) {
            collect_key_contract_map(el["children"][i], path + "." + std::to_string(i), out);
        }
    }
}

static bool json_tree_has_named_control(const json& el, const std::string& name) {
    if (el.contains("properties") && el["properties"].is_object() &&
        el["properties"].value("name", "") == name) {
        return true;
    }
    if (el.contains("children") && el["children"].is_array()) {
        for (auto& child : el["children"]) {
            if (json_tree_has_named_control(child, name))
                return true;
        }
    }
    return false;
}

static const json* find_named_control(const json& el, const std::string& name) {
    if (el.contains("properties") && el["properties"].is_object() &&
        el["properties"].value("name", "") == name) {
        return &el;
    }
    if (el.contains("children") && el["children"].is_array()) {
        for (auto& child : el["children"]) {
            if (auto* found = find_named_control(child, name))
                return found;
        }
    }
    return nullptr;
}

static bool frameworks_contain_wpf(const json& j) {
    if (!j.contains("frameworks") || !j["frameworks"].is_array())
        return false;
    for (auto& fw : j["frameworks"]) {
        if (fw.is_string() && fw.get<std::string>().starts_with("wpf"))
            return true;
    }
    return false;
}

static bool frameworks_contain_winui3(const json& j) {
    if (!j.contains("frameworks") || !j["frameworks"].is_array())
        return false;
    for (auto& fw : j["frameworks"]) {
        if (fw.is_string() && fw.get<std::string>().starts_with("winui3"))
            return true;
    }
    return false;
}

static bool frameworks_contain_avalonia(const json& j) {
    if (!j.contains("frameworks") || !j["frameworks"].is_array())
        return false;
    for (auto& fw : j["frameworks"]) {
        if (fw.is_string() && fw.get<std::string>().starts_with("avalonia"))
            return true;
    }
    return false;
}

static bool frameworks_contain_winforms(const json& j) {
    if (!j.contains("frameworks") || !j["frameworks"].is_array())
        return false;
    for (auto& fw : j["frameworks"]) {
        if (fw.is_string() && fw.get<std::string>().starts_with("winforms"))
            return true;
    }
    return false;
}

static bool has_winui3_descendant(const json& el) {
    if (el.value("framework", "") == "winui3")
        return true;

    if (el.contains("children") && el["children"].is_array()) {
        for (auto& child : el["children"]) {
            if (has_winui3_descendant(child))
                return true;
        }
    }
    return false;
}

static bool has_winui3_stitched_under_bridge(const json& el) {
    if (el.value("className", "") == "Microsoft.UI.Content.DesktopChildSiteBridge" &&
        el.contains("children") && el["children"].is_array()) {
        for (auto& child : el["children"]) {
            if (has_winui3_descendant(child))
                return true;
        }
    }

    if (el.contains("children") && el["children"].is_array()) {
        for (auto& child : el["children"]) {
            if (has_winui3_stitched_under_bridge(child))
                return true;
        }
    }
    return false;
}

static bool has_avalonia_control(const json& el) {
    if (el.value("framework", "") == "avalonia") {
        auto type = el.value("type", "");
        if (type == "Button" || type == "TextBlock" || type == "TextBox")
            return true;
        if (el.contains("properties") && el["properties"].is_object() &&
            el["properties"].contains("name"))
            return true;
    }

    if (el.contains("children") && el["children"].is_array()) {
        for (auto& child : el["children"]) {
            if (has_avalonia_control(child))
                return true;
        }
    }
    return false;
}

static bool has_avalonia_under_host(const json& el, bool underAvaloniaHost = false) {
    auto className = el.value("className", "");
    bool isAvaloniaHost = className.rfind("Avalonia-", 0) == 0;
    bool underHost = underAvaloniaHost || isAvaloniaHost;
    if (underHost && el.value("framework", "") == "avalonia")
        return true;

    if (el.contains("children") && el["children"].is_array()) {
        for (auto& child : el["children"]) {
            if (has_avalonia_under_host(child, underHost))
                return true;
        }
    }
    return false;
}

static const json* find_element_by_type_property(const json& el,
                                                 const std::string& type,
                                                 const std::string& property,
                                                 const std::string& value) {
    if (el.value("type", "") == type &&
        el.contains("properties") &&
        el["properties"].value(property, "") == value) {
        return &el;
    }

    if (el.contains("children") && el["children"].is_array()) {
        for (auto& child : el["children"]) {
            if (auto* found = find_element_by_type_property(child, type, property, value))
                return found;
        }
    }
    return nullptr;
}

static const json* find_element_by_hwnd(const json& el, HWND hwnd) {
    char buffer[32]{};
    snprintf(buffer, sizeof(buffer), "0x%p", hwnd);
    if (el.contains("properties") &&
        el["properties"].value("hwnd", "") == buffer) {
        return &el;
    }

    if (el.contains("children") && el["children"].is_array()) {
        for (auto& child : el["children"]) {
            if (auto* found = find_element_by_hwnd(child, hwnd))
                return found;
        }
    }
    return nullptr;
}

// Injected framework providers (WPF/WinUI3/XAML) collect their trees asynchronously,
// so an individual `lvt` dump can race and return an incomplete tree on slow/CI
// machines (see issue #27). These helpers retry until the framework subtree is
// present, making the injection-based fixtures deterministic.
template <typename Ready>
static json dump_ready_tree(const std::string& lvt, const std::string& pidArg,
                            Ready ready, int attempts = 40) {
    json j;
    for (int i = 0; i < attempts; ++i) {
        auto out = run_command(make_cmd(lvt, pidArg));
        j = json::parse(out, nullptr, false);
        if (!j.is_discarded() && j.contains("root") && ready(j))
            return j;
        Sleep(500);
    }
    return j;
}

// Retry `--query <ref>` (full element JSON) until it resolves to the expected key.
static json query_element_until(const std::string& lvt, const std::string& pidArg,
                                const std::string& ref, const std::string& expectedKey,
                                int attempts = 40) {
    json q;
    for (int i = 0; i < attempts; ++i) {
        auto out = run_command(make_cmd(lvt, pidArg + " query " + cmd_escape_arg(ref)));
        q = json::parse(out, nullptr, false);
        if (!q.is_discarded() && q.value("key", "") == expectedKey)
            return q;
        Sleep(500);
    }
    return q;
}

// Retry `--query <ref> <prop>` until it returns the expected value.
static std::string query_prop_until(const std::string& lvt, const std::string& pidArg,
                                    const std::string& ref, const std::string& prop,
                                    const std::string& expected, int attempts = 40) {
    std::string r;
    for (int i = 0; i < attempts; ++i) {
        r = trim_crlf(run_command(make_cmd(lvt, pidArg + " query " + cmd_escape_arg(ref) + " " + prop)));
        if (r == expected)
            return r;
        Sleep(500);
    }
    return r;
}

struct VisibleWindowSearch {
    DWORD pid;
    bool found;
    HWND hwnd;
};

static BOOL CALLBACK find_visible_window_for_pid(HWND hwnd, LPARAM lParam) {
    auto* search = reinterpret_cast<VisibleWindowSearch*>(lParam);
    DWORD windowPid = 0;
    GetWindowThreadProcessId(hwnd, &windowPid);
    if (windowPid == search->pid && IsWindowVisible(hwnd)) {
        search->found = true;
        search->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

static bool has_visible_window_for_pid(DWORD pid) {
    VisibleWindowSearch search{pid, false, nullptr};
    EnumWindows(find_visible_window_for_pid, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

static HWND visible_window_for_pid(DWORD pid) {
    VisibleWindowSearch search{pid, false, nullptr};
    EnumWindows(find_visible_window_for_pid, reinterpret_cast<LPARAM>(&search));
    return search.hwnd;
}

static std::wstring loaded_module_path(DWORD pid, const wchar_t* moduleName) {
    const HANDLE raw = CreateToolhelp32Snapshot(
        TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (raw == INVALID_HANDLE_VALUE)
        return {};
    wil::unique_handle snapshot(raw);
    MODULEENTRY32W module{sizeof(module)};
    if (!Module32FirstW(snapshot.get(), &module))
        return {};
    do {
        if (_wcsicmp(module.szModule, moduleName) == 0)
            return module.szExePath;
    } while (Module32NextW(snapshot.get(), &module));
    return {};
}

static std::wstring loaded_module_path_until(
    DWORD pid, const wchar_t* moduleName, int attempts = 20) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        auto path = loaded_module_path(pid, moduleName);
        if (!path.empty())
            return path;
        Sleep(100);
    }
    return {};
}

static bool wait_for_module_unloaded(
    DWORD pid, const wchar_t* moduleName, int attempts = 100) {
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (loaded_module_path(pid, moduleName).empty())
            return true;
        Sleep(50);
    }
    return false;
}

static int file_major_version(const std::wstring& path) {
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (bytes == 0)
        return 0;
    std::vector<unsigned char> version(bytes);
    if (!GetFileVersionInfoW(path.c_str(), 0, bytes, version.data()))
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

struct RuntimeDialogSearch {
    DWORD pid;
    bool found;
};

static BOOL CALLBACK find_runtime_dialog(HWND hwnd, LPARAM parameter) {
    auto* search = reinterpret_cast<RuntimeDialogSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid || !IsWindowVisible(hwnd))
        return TRUE;

    wchar_t className[64]{};
    wchar_t title[256]{};
    GetClassNameW(hwnd, className, static_cast<int>(_countof(className)));
    GetWindowTextW(hwnd, title, static_cast<int>(_countof(title)));
    if (wcscmp(className, L"#32770") == 0 ||
        wcsstr(title, L"Microsoft Visual C++ Runtime Library") != nullptr) {
        search->found = true;
        return FALSE;
    }
    return TRUE;
}

static bool has_runtime_error_dialog(DWORD pid) {
    RuntimeDialogSearch search{pid, false};
    EnumWindows(find_runtime_dialog, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

class PendingRemoteLoad {
public:
    bool start(DWORD pid, const fs::path& dllPath) {
        process_.reset(OpenProcess(
            PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
            FALSE, pid));
        if (!process_)
            return false;

        const std::wstring path = dllPath.wstring();
        const size_t pathBytes = (path.size() + 1) * sizeof(wchar_t);
        remotePath_ = VirtualAllocEx(
            process_.get(), nullptr, pathBytes, MEM_COMMIT, PAGE_READWRITE);
        if (!remotePath_)
            return false;
        if (!WriteProcessMemory(
                process_.get(), remotePath_, path.c_str(), pathBytes,
                nullptr)) {
            VirtualFreeEx(process_.get(), remotePath_, 0, MEM_RELEASE);
            remotePath_ = nullptr;
            return false;
        }

        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        if (!loadLibrary) {
            VirtualFreeEx(process_.get(), remotePath_, 0, MEM_RELEASE);
            remotePath_ = nullptr;
            return false;
        }
        thread_.reset(CreateRemoteThread(
            process_.get(), nullptr, 0, loadLibrary, remotePath_, 0, nullptr));
        if (thread_)
            return true;
        VirtualFreeEx(process_.get(), remotePath_, 0, MEM_RELEASE);
        remotePath_ = nullptr;
        return false;
    }

    bool complete(DWORD timeoutMs, DWORD& exitCode) {
        if (!thread_)
            return false;
        if (WaitForSingleObject(thread_.get(), timeoutMs) != WAIT_OBJECT_0)
            return false;

        const bool targetExited =
            WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0;
        const bool gotExitCode =
            GetExitCodeThread(thread_.get(), &exitCode) != FALSE;
        bool freed = true;
        if (!targetExited && remotePath_) {
            freed =
                VirtualFreeEx(
                    process_.get(), remotePath_, 0, MEM_RELEASE) != FALSE;
        }
        remotePath_ = nullptr;
        thread_.reset();
        return gotExitCode && freed;
    }

    ~PendingRemoteLoad() {
        if (!thread_)
            return;
        WaitForSingleObject(thread_.get(), INFINITE);
        if (WaitForSingleObject(process_.get(), 0) != WAIT_OBJECT_0 &&
            remotePath_) {
            VirtualFreeEx(process_.get(), remotePath_, 0, MEM_RELEASE);
        }
    }

private:
    wil::unique_handle process_;
    wil::unique_handle thread_;
    void* remotePath_ = nullptr;
};

#if LVT_ENABLE_WPF || LVT_ENABLE_WINFORMS
class ScopedSampleProcess {
public:
    bool start(const fs::path& executable) {
        STARTUPINFOA startup{sizeof(startup)};
        PROCESS_INFORMATION info{};
        std::string command = "\"" + executable.string() + "\"";
        if (!CreateProcessA(
                nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                executable.parent_path().string().c_str(), &startup, &info))
            return false;
        process_.reset(info.hProcess);
        thread_.reset(info.hThread);
        pid_ = info.dwProcessId;
        WaitForInputIdle(process_.get(), 5000);
        for (int attempt = 0; attempt < 30 && !hwnd_; ++attempt) {
            hwnd_ = visible_window_for_pid(pid_);
            if (!hwnd_)
                Sleep(100);
        }
        return hwnd_ != nullptr;
    }

    ~ScopedSampleProcess() {
        if (process_ && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT)
            TerminateProcess(process_.get(), 0);
    }

    DWORD pid() const { return pid_; }
    HWND hwnd() const { return hwnd_; }

private:
    wil::unique_handle process_;
    wil::unique_handle thread_;
    DWORD pid_ = 0;
    HWND hwnd_ = nullptr;
};

class ScopedUiBlock {
public:
    bool enter(const wchar_t* framework, DWORD pid) {
        const std::wstring prefix =
            L"Local\\Lvt" + std::wstring(framework) +
            L"SampleUiBlock_" + std::to_wstring(pid);
        trigger_.reset(OpenEventW(
            EVENT_MODIFY_STATE, FALSE, (prefix + L"_trigger").c_str()));
        entered_.reset(OpenEventW(
            SYNCHRONIZE, FALSE, (prefix + L"_entered").c_str()));
        release_.reset(OpenEventW(
            EVENT_MODIFY_STATE, FALSE, (prefix + L"_release").c_str()));
        if (!trigger_ || !entered_ || !release_)
            return false;
        if (!SetEvent(trigger_.get()))
            return false;
        active_ =
            WaitForSingleObject(entered_.get(), 5000) == WAIT_OBJECT_0;
        return active_;
    }

    void release() {
        if (active_) {
            SetEvent(release_.get());
            active_ = false;
        }
    }

    ~ScopedUiBlock() {
        release();
    }

private:
    wil::unique_handle trigger_;
    wil::unique_handle entered_;
    wil::unique_handle release_;
    bool active_ = false;
};

static const lvt::Element* find_named_element(
    const lvt::Element& element, const std::string& name) {
    auto found = element.properties.find("name");
    if (found != element.properties.end() && found->second == name)
        return &element;
    for (const auto& child : element.children) {
        if (const auto* match = find_named_element(child, name))
            return match;
    }
    return nullptr;
}

static const lvt::PropertyDescriptor* find_property_descriptor(
    const lvt::PropertySnapshotResult& snapshot, const std::string& name) {
    if (!snapshot.schema)
        return nullptr;
    for (const auto& descriptor : snapshot.schema->descriptors) {
        if (descriptor.name == name)
            return &descriptor;
    }
    return nullptr;
}

static const lvt::PropertyValue* find_property_value(
    const lvt::PropertySnapshotResult& snapshot,
    const std::string& descriptorId) {
    for (const auto& value : snapshot.values) {
        if (value.descriptorId == descriptorId)
            return &value;
    }
    return nullptr;
}
#endif

static bool deploy_plugins(const fs::path& source, const fs::path& dest, std::string& error) {
    std::error_code ec;
    fs::create_directories(dest, ec);
    if (ec) {
        error = "Failed to create " + dest.string() + ": " + ec.message();
        return false;
    }

    for (const auto& entry : fs::recursive_directory_iterator(source, ec)) {
        if (ec) {
            error = "Failed to enumerate " + source.string() + ": " + ec.message();
            return false;
        }

        auto relative = fs::relative(entry.path(), source, ec);
        if (ec) {
            error = "Failed to compute relative path for " + entry.path().string() + ": " + ec.message();
            return false;
        }

        auto target = dest / relative;
        if (entry.is_directory()) {
            fs::create_directories(target, ec);
            if (ec) {
                error = "Failed to create " + target.string() + ": " + ec.message();
                return false;
            }
            continue;
        }

        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            error = "Failed to create " + target.parent_path().string() + ": " + ec.message();
            return false;
        }

        if (!CopyFileW(entry.path().c_str(), target.c_str(), FALSE)) {
            DWORD err = GetLastError();
            if ((err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED) && fs::exists(target))
                continue;
            error = "Failed to copy " + entry.path().string() + " to " + target.string() +
                    " (error " + std::to_string(err) + ")";
            return false;
        }
    }
    return true;
}

class AvaloniaFixture : public ::testing::Test {
protected:
    void SetUp() override {
        auto lvt = get_lvt_path();
        fs::path buildDir = fs::path(lvt).parent_path();
        fs::path builtPlugins = buildDir / "plugins";
        if (!fs::exists(builtPlugins)) {
            GTEST_SKIP() << "Avalonia plugin output not found at " << builtPlugins.string()
                         << "; build lvt_avalonia_plugin first";
        }
        tapDir_ = builtPlugins / "avalonia";

        wchar_t profile[MAX_PATH]{};
        DWORD profileLen = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
        if (profileLen == 0 || profileLen >= MAX_PATH)
            GTEST_SKIP() << "USERPROFILE is not set; cannot deploy lvt plugins";

        fs::path userPlugins = fs::path(profile) / ".lvt" / "plugins";
        std::string deployError;
        if (!deploy_plugins(builtPlugins, userPlugins, deployError))
            GTEST_SKIP() << deployError;

        fs::path appExe = AVALONIA_SAMPLE_EXE_PATH;
        if (!fs::exists(appExe)) {
            GTEST_SKIP() << "Avalonia test app not built at " << appExe.string()
                         << "; build the avalonia_test_app target first";
        }

        STARTUPINFOA si = {sizeof(si)};
        pi_ = {};
        auto workdir = appExe.parent_path().string();
        std::string cmd = "\"" + appExe.string() + "\"";
        ASSERT_TRUE(CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                   0, nullptr, workdir.c_str(), &si, &pi_))
            << "Failed to launch " << appExe.string() << " (error " << GetLastError() << ")";
        process_.reset(pi_.hProcess);
        thread_.reset(pi_.hThread);
        if (pi_.hProcess)
            WaitForInputIdle(pi_.hProcess, 5000);

        ASSERT_EQ(
            lvt::detect_process_architecture(pi_.dwProcessId),
            lvt::get_host_architecture())
            << "the Avalonia fixture must match the integration test architecture";

        for (int attempt = 0; attempt < 10 && !has_visible_window_for_pid(pi_.dwProcessId); ++attempt)
            Sleep(500);

        const auto coreClr =
            loaded_module_path_until(pi_.dwProcessId, L"coreclr.dll");
        ASSERT_FALSE(coreClr.empty())
            << "the self-contained Avalonia fixture did not load CoreCLR";
        ASSERT_EQ(file_major_version(coreClr), 8)
            << "the Avalonia fixture must exercise the supported .NET 8 floor";

        std::string lastOutput;
        for (int attempt = 0; attempt < 30; ++attempt) {
            lastOutput = run_command(make_cmd(lvt, pid_arg()));
            auto j = json::parse(lastOutput, nullptr, false);
            if (!j.is_discarded() && frameworks_contain_avalonia(j) &&
                j.contains("root") && has_avalonia_control(j["root"])) {
                initialDump_ = std::move(j);
                return;
            }
            if (has_runtime_error_dialog(pi_.dwProcessId))
                break;
            Sleep(1000);
        }

        FAIL() << "Avalonia app never became ready with a nontrivial tree; "
               << "runtime dialog=" << has_runtime_error_dialog(pi_.dwProcessId)
               << ", last output=" << lastOutput;
    }

    void TearDown() override {
        if (pi_.hProcess) {
            TerminateProcess(pi_.hProcess, 0);
            process_.reset();
            thread_.reset();
            pi_.hProcess = nullptr;
            pi_.hThread = nullptr;
        }
    }

    std::string pid_arg() const {
        return "--pid " + std::to_string(pi_.dwProcessId);
    }

    const wchar_t* tap_name() const {
#if defined(_M_ARM64)
        return L"lvt_avalonia_tap_arm64.dll";
#elif defined(_M_IX86)
        return L"lvt_avalonia_tap_x86.dll";
#elif defined(_M_X64)
        return L"lvt_avalonia_tap_x64.dll";
#else
#error Unsupported Avalonia integration test architecture
#endif
    }

    fs::path sidecar_path() const {
        return tapDir_ /
            ("lvt_avalonia_pipe_" + std::to_string(pi_.dwProcessId) + ".txt");
    }

    PROCESS_INFORMATION pi_{};
    wil::unique_handle process_;
    wil::unique_handle thread_;
    fs::path tapDir_;
    json initialDump_;
};

TEST_F(AvaloniaFixture, HostfxrAbiReturnsTreeAndReconnectsCleanly) {
    ASSERT_TRUE(initialDump_.contains("root"));
    ASSERT_TRUE(has_avalonia_under_host(initialDump_["root"]))
        << "Avalonia elements should be stitched under the Avalonia-* HWND";
    EXPECT_EQ(WaitForSingleObject(process_.get(), 0), WAIT_TIMEOUT);
    EXPECT_FALSE(has_runtime_error_dialog(pi_.dwProcessId));
    EXPECT_TRUE(wait_for_module_unloaded(pi_.dwProcessId, tap_name()))
        << "the first one-shot Avalonia TAP remained pinned";
    EXPECT_FALSE(fs::exists(sidecar_path()))
        << "the first one-shot Avalonia connection left a stale sidecar";

    auto lvt = get_lvt_path();
    auto avaloniaReady = [](const json& j) {
        return frameworks_contain_avalonia(j) &&
               j.contains("root") &&
               has_avalonia_control(j["root"]);
    };
    auto secondDump = dump_ready_tree(lvt, pid_arg(), avaloniaReady);
    ASSERT_FALSE(secondDump.is_discarded());
    ASSERT_TRUE(avaloniaReady(secondDump));
    EXPECT_EQ(WaitForSingleObject(process_.get(), 0), WAIT_TIMEOUT);
    EXPECT_FALSE(has_runtime_error_dialog(pi_.dwProcessId));
    EXPECT_TRUE(wait_for_module_unloaded(pi_.dwProcessId, tap_name()))
        << "the reconnected Avalonia TAP remained pinned";
    EXPECT_FALSE(fs::exists(sidecar_path()))
        << "the reconnected Avalonia session left a stale sidecar";

    std::vector<const json*> elements;
    collect_json_elements(initialDump_["root"], elements);
    const auto avaloniaElements = std::count_if(
        elements.begin(), elements.end(), [](const json* element) {
            return element->value("framework", "") == "avalonia";
        });
    ASSERT_GE(avaloniaElements, 5u)
        << "the managed TAP must return a nontrivial Avalonia tree";
    EXPECT_NE(find_named_control(initialDump_["root"], "HelloText"), nullptr);
    EXPECT_NE(find_named_control(initialDump_["root"], "ClickButton"), nullptr);
    EXPECT_NE(find_named_control(initialDump_["root"], "InputBox"), nullptr);

    std::set<std::string> keys;
    for (auto* el : elements) {
        auto key = el->value("key", "");
        EXPECT_FALSE(key.empty()) << "Element " << el->value("id", "?") << " has empty durable key";
        EXPECT_TRUE(keys.insert(key).second) << "Duplicate durable key: " << key;
    }

    std::map<std::string, std::string> firstMap;
    std::map<std::string, std::string> secondMap;
    collect_key_contract_map(initialDump_["root"], "0", firstMap);
    collect_key_contract_map(secondDump["root"], "0", secondMap);
    EXPECT_EQ(firstMap, secondMap);

    auto* button = find_named_control(initialDump_["root"], "ClickButton");
    ASSERT_NE(button, nullptr) << "Expected x:Name'd Avalonia button in test tree";
    EXPECT_EQ(button->value("framework", ""), "avalonia");
    EXPECT_EQ(button->value("type", ""), "Button");

    auto buttonKey = button->value("key", "");
    ASSERT_FALSE(buttonKey.empty());
    auto queriedName = query_prop_until(lvt, pid_arg(), buttonKey, "name", "ClickButton");
    EXPECT_EQ(trim_crlf(queriedName), "ClickButton");
    EXPECT_EQ(WaitForSingleObject(process_.get(), 0), WAIT_TIMEOUT);
    EXPECT_FALSE(has_runtime_error_dialog(pi_.dwProcessId));
    EXPECT_TRUE(wait_for_module_unloaded(pi_.dwProcessId, tap_name()))
        << "the query reconnect left the Avalonia TAP pinned";
    EXPECT_FALSE(fs::exists(sidecar_path()))
        << "the query reconnect left a stale sidecar";
}

TEST_F(AvaloniaFixture, DelayedRemoteLoadRemainsSerializedAndCleansUp) {
    ASSERT_TRUE(wait_for_module_unloaded(pi_.dwProcessId, tap_name()));

    const std::wstring readyName =
        L"Local\\LvtLoaderLockBlocker_" +
        std::to_wstring(pi_.dwProcessId);
    wil::unique_event blockerReady(
        CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()));
    ASSERT_TRUE(blockerReady);

    PendingRemoteLoad blocker;
    ASSERT_TRUE(blocker.start(
        pi_.dwProcessId, fs::path(LOADER_LOCK_BLOCKER_DLL_PATH)));
    ASSERT_EQ(
        WaitForSingleObject(blockerReady.get(), 3000), WAIT_OBJECT_0)
        << "the test DLL did not acquire the target loader lock";

    const auto lvt = get_lvt_path();
    const auto target = pid_arg();
    auto first = std::async(std::launch::async, [lvt, target] {
        return run_command(make_cmd(lvt, target));
    });

    Sleep(5500);
    EXPECT_EQ(
        first.wait_for(std::chrono::milliseconds(0)),
        std::future_status::timeout)
        << "the first caller escaped while remote LoadLibraryW was pending";

    auto second = std::async(std::launch::async, [lvt, target] {
        return run_command(make_cmd(lvt, target));
    });
    EXPECT_EQ(
        second.wait_for(std::chrono::milliseconds(0)),
        std::future_status::timeout)
        << "the retry should wait for ownership of the injection transaction";

    DWORD blockerExitCode = STILL_ACTIVE;
    ASSERT_TRUE(blocker.complete(10000, blockerExitCode));
    EXPECT_EQ(blockerExitCode, 0u)
        << "the loader-lock blocker intentionally fails its LoadLibraryW";

    const auto firstOutput = first.get();
    const auto secondOutput = second.get();
    const auto firstTree = json::parse(firstOutput, nullptr, false);
    const auto secondTree = json::parse(secondOutput, nullptr, false);
    auto ready = [](const json& tree) {
        return !tree.is_discarded() &&
               frameworks_contain_avalonia(tree) &&
               tree.contains("root") &&
               has_avalonia_control(tree["root"]);
    };
    EXPECT_TRUE(ready(firstTree))
        << "the delayed initial injection did not return Avalonia content";
    EXPECT_TRUE(ready(secondTree))
        << "the serialized retry did not reconnect with Avalonia content";

    EXPECT_EQ(WaitForSingleObject(process_.get(), 0), WAIT_TIMEOUT);
    EXPECT_FALSE(has_runtime_error_dialog(pi_.dwProcessId));
    EXPECT_TRUE(wait_for_module_unloaded(pi_.dwProcessId, tap_name()))
        << "overlapping LoadLibraryW calls left an extra TAP module reference";
    EXPECT_FALSE(fs::exists(sidecar_path()))
        << "the delayed injection transaction left a stale sidecar";

    const std::wstring blockerName =
        fs::path(LOADER_LOCK_BLOCKER_DLL_PATH).filename().wstring();
    EXPECT_TRUE(wait_for_module_unloaded(
        pi_.dwProcessId, blockerName.c_str()))
        << "the loader-lock blocker should fail loading and leave no module";
}

class WinFormsSampleFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        s_sample_exe = WINFORMS_SAMPLE_EXE_PATH;
        if (!fs::exists(s_sample_exe)) {
            s_skip_reason = "WinForms sample app not found: " + s_sample_exe;
            return;
        }

        STARTUPINFOA si = {sizeof(si)};
        s_pi = {};
        auto workdir = fs::path(s_sample_exe).parent_path().string();
        std::string cmd = "\"" + s_sample_exe + "\"";
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, workdir.c_str(), &si, &s_pi)) {
            s_skip_reason = "Failed to launch WinForms sample app";
            return;
        }
        s_pid = s_pi.dwProcessId;
        s_process.reset(s_pi.hProcess);
        s_thread.reset(s_pi.hThread);
        if (s_pi.hProcess) {
            WaitForInputIdle(s_pi.hProcess, 5000);
        }

        auto lvt = get_lvt_path();
        auto winformsReady = [](const json& j) {
            return frameworks_contain_winforms(j) &&
                   j.contains("root") &&
                   json_tree_has_named_control(j["root"], "okButton") &&
                   json_tree_has_named_control(j["root"], "inputTextBox") &&
                   json_tree_has_named_control(j["root"], "messageLabel");
        };
        auto readyDump = dump_ready_tree(lvt, get_pid_arg(), winformsReady, 50);
        if (winformsReady(readyDump)) {
            s_ready = true;
            return;
        }

        s_skip_reason = "WinForms sample app never became ready with framework and named controls";
    }

    static void TearDownTestSuite() {
        if (s_pi.hProcess) {
            TerminateProcess(s_pi.hProcess, 0);
            s_process.reset();
            s_thread.reset();
            s_pi.hProcess = nullptr;
            s_pi.hThread = nullptr;
        }
    }

    static void SkipIfNotReady() {
        if (!s_ready)
            GTEST_SKIP() << s_skip_reason;
    }

    static std::string get_pid_arg() {
        return "--pid " + std::to_string(s_pid);
    }

    static PROCESS_INFORMATION s_pi;
    static wil::unique_handle s_process;
    static wil::unique_handle s_thread;
    static DWORD s_pid;
    static bool s_ready;
    static std::string s_sample_exe;
    static std::string s_skip_reason;
};

PROCESS_INFORMATION WinFormsSampleFixture::s_pi = {};
wil::unique_handle WinFormsSampleFixture::s_process;
wil::unique_handle WinFormsSampleFixture::s_thread;
DWORD WinFormsSampleFixture::s_pid = 0;
bool WinFormsSampleFixture::s_ready = false;
std::string WinFormsSampleFixture::s_sample_exe;
std::string WinFormsSampleFixture::s_skip_reason;

TEST_F(WinFormsSampleFixture, DetectsWinFormsFramework) {
    SkipIfNotReady();

    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg() + " frameworks"));
    ASSERT_FALSE(output.empty());
    EXPECT_NE(output.find("winforms"), std::string::npos);
}

TEST_F(WinFormsSampleFixture, EnrichesControlsWithManagedNameAndType) {
    SkipIfNotReady();

    auto lvt = get_lvt_path();
    auto winformsReady = [](const json& j) {
        return frameworks_contain_winforms(j) &&
               j.contains("root") &&
               json_tree_has_named_control(j["root"], "okButton") &&
               json_tree_has_named_control(j["root"], "inputTextBox") &&
               json_tree_has_named_control(j["root"], "messageLabel");
    };
    auto j = dump_ready_tree(lvt, get_pid_arg(), winformsReady, 50);
    ASSERT_TRUE(winformsReady(j)) << "WinForms tree never became ready";

    auto* okButton = find_named_control(j["root"], "okButton");
    ASSERT_NE(okButton, nullptr);
    EXPECT_EQ(okButton->value("framework", ""), "winforms");
    EXPECT_EQ(okButton->value("type", ""), "Button");
    EXPECT_EQ((*okButton)["properties"].value("winforms.type", ""), "System.Windows.Forms.Button");

    auto* textBox = find_named_control(j["root"], "inputTextBox");
    ASSERT_NE(textBox, nullptr);
    EXPECT_EQ(textBox->value("type", ""), "TextBox");
    EXPECT_EQ((*textBox)["properties"].value("winforms.type", ""), "System.Windows.Forms.TextBox");

    auto* label = find_named_control(j["root"], "messageLabel");
    ASSERT_NE(label, nullptr);
    EXPECT_EQ(label->value("type", ""), "Label");

    auto okKey = okButton->value("key", "");
    ASSERT_FALSE(okKey.empty());
    auto queried = query_element_until(lvt, get_pid_arg(), okKey, okKey);
    ASSERT_FALSE(queried.is_discarded()) << "query for okButton key never resolved";
    EXPECT_EQ(queried.value("framework", ""), "winforms");
    EXPECT_EQ(queried.value("type", ""), "Button");
    EXPECT_EQ(queried.value("name", ""), "okButton");

    auto queriedType = query_prop_until(lvt, get_pid_arg(), okKey, "winforms.type",
                                        "System.Windows.Forms.Button");
    EXPECT_EQ(queriedType, "System.Windows.Forms.Button");
}

TEST_F(WinFormsSampleFixture, DurableKeyContract) {
    SkipIfNotReady();

    auto lvt = get_lvt_path();
    auto winformsReady = [](const json& j) {
        return frameworks_contain_winforms(j) &&
               j.contains("root") &&
               json_tree_has_named_control(j["root"], "okButton") &&
               json_tree_has_named_control(j["root"], "inputTextBox") &&
               json_tree_has_named_control(j["root"], "messageLabel");
    };
    auto j1 = dump_ready_tree(lvt, get_pid_arg(), winformsReady, 50);
    auto j2 = dump_ready_tree(lvt, get_pid_arg(), winformsReady, 50);
    ASSERT_TRUE(winformsReady(j1)) << "WinForms tree never became ready (dump 1)";
    ASSERT_TRUE(winformsReady(j2)) << "WinForms tree never became ready (dump 2)";
    ASSERT_TRUE(frameworks_contain_winforms(j1));

    std::vector<const json*> elements;
    collect_json_elements(j1["root"], elements);
    ASSERT_GT(elements.size(), 0u);

    std::set<std::string> keys;
    for (auto* el : elements) {
        auto key = el->value("key", "");
        EXPECT_FALSE(key.empty()) << "Element " << el->value("id", "?") << " has empty durable key";
        EXPECT_TRUE(keys.insert(key).second) << "Duplicate durable key: " << key;
    }

    std::map<std::string, std::string> firstMap;
    std::map<std::string, std::string> secondMap;
    collect_key_contract_map(j1["root"], "0", firstMap);
    collect_key_contract_map(j2["root"], "0", secondMap);
    EXPECT_EQ(firstMap, secondMap);

    auto* okButton = find_named_control(j1["root"], "okButton");
    ASSERT_NE(okButton, nullptr);
    EXPECT_EQ(okButton->value("framework", ""), "winforms");
    EXPECT_EQ(okButton->value("type", ""), "Button");
    EXPECT_EQ((*okButton)["properties"].value("winforms.type", ""), "System.Windows.Forms.Button");

    auto okKey = okButton->value("key", "");
    ASSERT_FALSE(okKey.empty());
    auto queried = query_element_until(lvt, get_pid_arg(), okKey, okKey);
    ASSERT_FALSE(queried.is_discarded()) << "query for okButton key never resolved";
    EXPECT_EQ(queried.value("key", ""), okKey);
    EXPECT_EQ(queried.value("type", ""), "Button");
    EXPECT_EQ(queried.value("framework", ""), "winforms");
    EXPECT_EQ(queried.value("name", ""), "okButton");

    auto queriedType = query_prop_until(lvt, get_pid_arg(), okKey, "winforms.type",
                                        "System.Windows.Forms.Button");
    EXPECT_EQ(queriedType, "System.Windows.Forms.Button");
}

TEST_F(WinFormsSampleFixture, ConcurrentCollectorsDoNotPinTapModule) {
    SkipIfNotReady();
    const auto lvt = get_lvt_path();
    const auto command = make_cmd(lvt, get_pid_arg());
    std::promise<void> start;
    std::shared_future<void> startSignal(start.get_future());
    auto first = std::async(std::launch::async, [startSignal, command] {
        startSignal.wait();
        return run_command(command);
    });
    auto second = std::async(std::launch::async, [startSignal, command] {
        startSignal.wait();
        return run_command(command);
    });
    start.set_value();

    auto firstTree = json::parse(first.get(), nullptr, false);
    auto secondTree = json::parse(second.get(), nullptr, false);
    ASSERT_FALSE(firstTree.is_discarded());
    ASSERT_FALSE(secondTree.is_discarded());
    const bool firstManaged =
        firstTree.contains("root") &&
        json_tree_has_named_control(firstTree["root"], "okButton");
    const bool secondManaged =
        secondTree.contains("root") &&
        json_tree_has_named_control(secondTree["root"], "okButton");
    EXPECT_TRUE(firstManaged || secondManaged)
        << "at least one simultaneous collector must own the managed session";

    auto reconnected = dump_ready_tree(
        lvt, get_pid_arg(),
        [](const json& tree) {
            return tree.contains("root") &&
                   json_tree_has_named_control(tree["root"], "okButton");
        },
        20);
    EXPECT_TRUE(
        reconnected.contains("root") &&
        json_tree_has_named_control(reconnected["root"], "okButton"))
        << "concurrent LoadLibrary callers must not pin the TAP after both exit";
}

#if LVT_ENABLE_WINFORMS && LVT_WITH_MANAGED
TEST_F(WinFormsSampleFixture, PersistentConnectionReusesServerAndStableIdentity) {
    SkipIfNotReady();
    HWND hwnd = visible_window_for_pid(s_pid);
    ASSERT_NE(hwnd, nullptr);

    lvt::WinFormsProvider provider;
    auto connection = provider.open_connection(hwnd, s_pid);
    ASSERT_NE(connection, nullptr);
    auto capabilities = lvt::managed_connection_capabilities(*connection);
    ASSERT_TRUE(capabilities.has_value());
    EXPECT_NE(std::find(capabilities->commands.begin(), capabilities->commands.end(), "GET_TREE"),
              capabilities->commands.end());
    EXPECT_NE(std::find(capabilities->commands.begin(), capabilities->commands.end(), "DISCONNECT"),
              capabilities->commands.end());

    auto started = std::chrono::steady_clock::now();
    auto first = lvt::build_tree(hwnd, s_pid, {});
    ASSERT_TRUE(connection->get_tree(first, false));
    lvt::assign_element_ids(first);
    lvt::assign_element_keys(first);
    auto second = lvt::build_tree(hwnd, s_pid, {});
    ASSERT_TRUE(connection->get_tree(second, false));
    lvt::assign_element_ids(second);
    lvt::assign_element_keys(second);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(20));

    const auto* firstButton = find_named_element(first, "okButton");
    const auto* secondButton = find_named_element(second, "okButton");
    ASSERT_NE(firstButton, nullptr);
    ASSERT_NE(secondButton, nullptr);
    EXPECT_NE(firstButton->providerHandle, 0u);
    EXPECT_EQ(firstButton->providerHandle, secondButton->providerHandle);
    EXPECT_EQ(firstButton->key, secondButton->key);

    const auto* form = find_named_element(first, "MainForm");
    ASSERT_NE(form, nullptr);
    auto propertySnapshot =
        connection->get_property_snapshot(form->providerHandle);
    ASSERT_TRUE(propertySnapshot.ok) << propertySnapshot.error;
    const auto* editableText =
        find_property_descriptor(propertySnapshot, "EditableText");
    ASSERT_NE(editableText, nullptr);
    auto setText = connection->set_property(
        form->providerHandle, editableText->descriptorId, "integration value");
    ASSERT_TRUE(setText.ok) << setText.error;
    EXPECT_EQ(setText.value, "integration value");
    auto clearText = connection->clear_property(
        form->providerHandle, editableText->descriptorId);
    ASSERT_TRUE(clearText.ok) << clearText.error;
    EXPECT_TRUE(clearText.cleared);
    EXPECT_EQ(clearText.value, "Default text");

    auto afterReads = lvt::managed_connection_capabilities(*connection);
    ASSERT_TRUE(afterReads.has_value());
    EXPECT_EQ(afterReads->connectionId, capabilities->connectionId);
    EXPECT_EQ(afterReads->serverStartCount, capabilities->serverStartCount);

    const auto assemblyInstance = capabilities->assemblyInstanceId;
    const auto previousStartCount = capabilities->serverStartCount;
    connection.reset();

    auto reconnected = provider.open_connection(hwnd, s_pid);
    ASSERT_NE(reconnected, nullptr);
    auto reconnectedCapabilities = lvt::managed_connection_capabilities(*reconnected);
    ASSERT_TRUE(reconnectedCapabilities.has_value());
    EXPECT_NE(reconnectedCapabilities->connectionId, capabilities->connectionId);
    EXPECT_EQ(reconnectedCapabilities->assemblyInstanceId, assemblyInstance);
    EXPECT_EQ(reconnectedCapabilities->serverStartCount, previousStartCount + 1);

    auto refreshed = lvt::build_tree(hwnd, s_pid, {});
    ASSERT_TRUE(reconnected->get_tree(refreshed, false));
    lvt::assign_element_ids(refreshed);
    lvt::assign_element_keys(refreshed);
    const auto* refreshedButton = find_named_element(refreshed, "okButton");
    ASSERT_NE(refreshedButton, nullptr);
    EXPECT_EQ(refreshedButton->providerHandle, firstButton->providerHandle);
    EXPECT_EQ(refreshedButton->key, firstButton->key);
}

TEST_F(WinFormsSampleFixture, X86HostfxrAbiRunsTreeAndProperties) {
    if (sizeof(void*) != 4)
        GTEST_SKIP() << "x86 ABI regression";

    HWND hwnd = visible_window_for_pid(s_pid);
    ASSERT_NE(hwnd, nullptr);
    lvt::WinFormsProvider provider;
    auto connection = provider.open_connection(hwnd, s_pid);
    ASSERT_NE(connection, nullptr)
        << "the x86 TAP must enter managed RunServer without a CRT ABI failure";

    auto tree = lvt::build_tree(hwnd, s_pid, {});
    ASSERT_TRUE(connection->get_tree(tree, false));
    const auto* form = find_named_element(tree, "MainForm");
    ASSERT_NE(form, nullptr);
    auto snapshot =
        connection->get_property_snapshot(form->providerHandle);
    ASSERT_TRUE(snapshot.ok) << snapshot.error;
    const auto* property =
        find_property_descriptor(snapshot, "EditableText");
    ASSERT_NE(property, nullptr);
    auto set = connection->set_property(
        form->providerHandle, property->descriptorId, "x86 ABI");
    ASSERT_TRUE(set.ok) << set.error;
    EXPECT_EQ(set.value, "x86 ABI");
    auto clear = connection->clear_property(
        form->providerHandle, property->descriptorId);
    ASSERT_TRUE(clear.ok) << clear.error;
    EXPECT_EQ(clear.value, "Default text");
}

TEST_F(WinFormsSampleFixture, QueuedClearTimeoutCannotMutateLater) {
    SkipIfNotReady();
    HWND hwnd = visible_window_for_pid(s_pid);
    ASSERT_NE(hwnd, nullptr);
    lvt::WinFormsProvider provider;
    auto connection = provider.open_connection(hwnd, s_pid);
    ASSERT_NE(connection, nullptr);

    auto tree = lvt::build_tree(hwnd, s_pid, {});
    ASSERT_TRUE(connection->get_tree(tree, false));
    const auto* form = find_named_element(tree, "MainForm");
    ASSERT_NE(form, nullptr);
    auto snapshot =
        connection->get_property_snapshot(form->providerHandle);
    ASSERT_TRUE(snapshot.ok) << snapshot.error;
    const auto* property =
        find_property_descriptor(snapshot, "EditableText");
    ASSERT_NE(property, nullptr);
    auto set = connection->set_property(
        form->providerHandle, property->descriptorId, "blocked clear");
    ASSERT_TRUE(set.ok) << set.error;

    ScopedUiBlock block;
    ASSERT_TRUE(block.enter(L"WinForms", s_pid));
    auto clear = connection->clear_property(
        form->providerHandle, property->descriptorId);
    EXPECT_FALSE(clear.ok);
    EXPECT_NE(clear.error.find("before execution"), std::string::npos)
        << clear.error;
    block.release();

    auto after =
        connection->get_property_snapshot(form->providerHandle);
    ASSERT_TRUE(after.ok) << after.error;
    const auto* value =
        find_property_value(after, property->descriptorId);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(value->value, "blocked clear");
    EXPECT_TRUE(value->canClear);

    auto cleanup = connection->clear_property(
        form->providerHandle, property->descriptorId);
    ASSERT_TRUE(cleanup.ok) << cleanup.error;
}
#endif

class WpfSampleFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        s_sample_exe = WPF_SAMPLE_EXE_PATH;
        if (!fs::exists(s_sample_exe)) {
            s_skip_reason = "WPF sample app not found: " + s_sample_exe;
            return;
        }

        STARTUPINFOA si = {sizeof(si)};
        s_pi = {};
        auto workdir = fs::path(s_sample_exe).parent_path().string();
        std::string cmd = "\"" + s_sample_exe + "\"";
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, workdir.c_str(), &si, &s_pi)) {
            s_skip_reason = "Failed to launch WPF sample app";
            return;
        }
        s_pid = s_pi.dwProcessId;
        s_process.reset(s_pi.hProcess);
        s_thread.reset(s_pi.hThread);
        if (s_pi.hProcess) {
            WaitForInputIdle(s_pi.hProcess, 5000);
        }

        auto lvt = get_lvt_path();
        for (int attempt = 0; attempt < 30; ++attempt) {
            auto output = run_command(make_cmd(lvt, get_pid_arg()));
            auto j = json::parse(output, nullptr, false);
            if (!j.is_discarded() && frameworks_contain_wpf(j) &&
                j.contains("root") &&
                json_tree_has_named_control(j["root"], "OkButton") &&
                json_tree_has_named_control(j["root"], "NameBox") &&
                json_tree_has_named_control(j["root"], "AgreeCheck")) {
                s_ready = true;
                return;
            }
            Sleep(1000);
        }

        s_skip_reason = "WPF sample app never became ready with WPF framework and named controls";
    }

    static void TearDownTestSuite() {
        if (s_pi.hProcess) {
            TerminateProcess(s_pi.hProcess, 0);
            s_process.reset();
            s_thread.reset();
            s_pi.hProcess = nullptr;
            s_pi.hThread = nullptr;
        }
    }

    static void SkipIfNotReady() {
        if (!s_ready)
            GTEST_SKIP() << s_skip_reason;
    }

    static std::string get_pid_arg() {
        return "--pid " + std::to_string(s_pid);
    }

    static PROCESS_INFORMATION s_pi;
    static wil::unique_handle s_process;
    static wil::unique_handle s_thread;
    static DWORD s_pid;
    static bool s_ready;
    static std::string s_sample_exe;
    static std::string s_skip_reason;
};

PROCESS_INFORMATION WpfSampleFixture::s_pi = {};
wil::unique_handle WpfSampleFixture::s_process;
wil::unique_handle WpfSampleFixture::s_thread;
DWORD WpfSampleFixture::s_pid = 0;
bool WpfSampleFixture::s_ready = false;
std::string WpfSampleFixture::s_sample_exe;
std::string WpfSampleFixture::s_skip_reason;

TEST_F(WpfSampleFixture, ChildBoundsAreInTheSameCoordinateSpaceAsTheWindow) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    // The WPF walker read sizes from ActualWidth/Height, which are
    // device-independent units, but positions from PointToScreen, which are
    // device pixels. At 100% scaling those coincide and nothing looks wrong; at
    // 150% every child was reported 1.5x further from the origin than it really
    // is — outside the window rect lvt reads — so screenshot annotations landed
    // off-image and the bounds were unusable to any caller.
    //
    // Asserting containment rather than exact numbers keeps this meaningful at
    // any scaling, including the 100% where the bug is invisible.
    auto wpfReady = [](const json& j) {
        return frameworks_contain_wpf(j) &&
               json_tree_has_named_control(j["root"], "OkButton");
    };
    auto tree = dump_ready_tree(lvt, get_pid_arg(), wpfReady);
    ASSERT_TRUE(wpfReady(tree)) << "WPF tree never became ready";

    const auto& rootBounds = tree["root"]["bounds"];
    const int rx = rootBounds.value("x", 0);
    const int ry = rootBounds.value("y", 0);
    const int rw = rootBounds.value("width", 0);
    const int rh = rootBounds.value("height", 0);
    ASSERT_GT(rw, 0);
    ASSERT_GT(rh, 0);

    std::vector<const json*> elements;
    collect_json_elements(tree["root"], elements);
    ASSERT_GT(elements.size(), 1u);

    // Each sized element's centre should sit inside the window. A small margin
    // absorbs borders and shadows; it is deliberately far tighter than a DPI
    // factor, which displaces things by a third of the window or more.
    const int slackX = rw / 20 + 4;
    const int slackY = rh / 20 + 4;
    int sized = 0;
    int inside = 0;
    std::string worst;
    for (const auto* element : elements) {
        const auto& b = (*element)["bounds"];
        const int w = b.value("width", 0);
        const int h = b.value("height", 0);
        if (w <= 0 || h <= 0)
            continue;
        ++sized;
        const int cx = b.value("x", 0) + w / 2;
        const int cy = b.value("y", 0) + h / 2;
        if (cx >= rx - slackX && cx <= rx + rw + slackX && cy >= ry - slackY &&
            cy <= ry + rh + slackY) {
            ++inside;
        } else if (worst.empty()) {
            worst = element->value("id", "?") + " (" + element->value("type", "") + " \"" +
                    element->value("text", "") + "\") centre " + std::to_string(cx) + "," +
                    std::to_string(cy);
        }
    }

    ASSERT_GT(sized, 2) << "no sized WPF elements were checked";
    // A popup or adorner may legitimately fall outside, so this is a
    // proportion rather than an absolute — but a coordinate-space mismatch
    // moves nearly everything at once, which no amount of slack absorbs.
    EXPECT_GE(inside * 100 / sized, 90)
        << inside << " of " << sized << " WPF elements have their centre inside the window "
        << rx << "," << ry << " " << rw << "x" << rh << ". First outlier: " << worst
        << ". Element positions are probably in a different coordinate space than their sizes.";
}

TEST_F(WpfSampleFixture, DurableKeyContract) {
    SkipIfNotReady();

    auto lvt = get_lvt_path();
    auto wpfReady = [](const json& j) {
        return frameworks_contain_wpf(j) &&
               json_tree_has_named_control(j["root"], "OkButton");
    };
    auto j1 = dump_ready_tree(lvt, get_pid_arg(), wpfReady);
    auto j2 = dump_ready_tree(lvt, get_pid_arg(), wpfReady);
    ASSERT_TRUE(wpfReady(j1)) << "WPF tree never became ready (dump 1)";
    ASSERT_TRUE(wpfReady(j2)) << "WPF tree never became ready (dump 2)";
    ASSERT_TRUE(frameworks_contain_wpf(j1));

    std::vector<const json*> elements;
    collect_json_elements(j1["root"], elements);
    ASSERT_GT(elements.size(), 0u);

    std::set<std::string> keys;
    for (auto* el : elements) {
        auto key = el->value("key", "");
        EXPECT_FALSE(key.empty()) << "Element " << el->value("id", "?") << " has empty durable key";
        EXPECT_TRUE(keys.insert(key).second) << "Duplicate durable key: " << key;
    }

    std::map<std::string, std::string> firstMap;
    std::map<std::string, std::string> secondMap;
    collect_key_contract_map(j1["root"], "0", firstMap);
    collect_key_contract_map(j2["root"], "0", secondMap);
    EXPECT_EQ(firstMap, secondMap);

    auto* okButton = find_named_control(j1["root"], "OkButton");
    ASSERT_NE(okButton, nullptr);
    EXPECT_EQ(okButton->value("framework", ""), "wpf");
    EXPECT_EQ(okButton->value("type", ""), "Button");

    auto okKey = okButton->value("key", "");
    ASSERT_FALSE(okKey.empty());
    auto queried = query_element_until(lvt, get_pid_arg(), okKey, okKey);
    ASSERT_FALSE(queried.is_discarded()) << "query for OkButton key never resolved";
    EXPECT_EQ(queried.value("key", ""), okKey);
    EXPECT_EQ(queried.value("type", ""), "Button");
    EXPECT_EQ(queried.value("framework", ""), "wpf");
    EXPECT_EQ(queried.value("name", ""), "OkButton");
}

#if LVT_ENABLE_WPF && LVT_WITH_MANAGED
TEST_F(WpfSampleFixture, PersistentConnectionReusesServerAndStableIdentity) {
    SkipIfNotReady();
    HWND hwnd = visible_window_for_pid(s_pid);
    ASSERT_NE(hwnd, nullptr);

    lvt::WpfProvider provider;
    auto connection = provider.open_connection(hwnd, s_pid);
    ASSERT_NE(connection, nullptr);
    auto capabilities = lvt::managed_connection_capabilities(*connection);
    ASSERT_TRUE(capabilities.has_value());

    auto started = std::chrono::steady_clock::now();
    auto first = lvt::build_tree(hwnd, s_pid, {});
    ASSERT_TRUE(connection->get_tree(first, false));
    lvt::assign_element_ids(first);
    lvt::assign_element_keys(first);
    auto second = lvt::build_tree(hwnd, s_pid, {});
    ASSERT_TRUE(connection->get_tree(second, false));
    lvt::assign_element_ids(second);
    lvt::assign_element_keys(second);
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(20));

    const auto* firstButton = find_named_element(first, "OkButton");
    const auto* secondButton = find_named_element(second, "OkButton");
    ASSERT_NE(firstButton, nullptr);
    ASSERT_NE(secondButton, nullptr);
    EXPECT_NE(firstButton->providerHandle, 0u);
    EXPECT_EQ(firstButton->providerHandle, secondButton->providerHandle);
    EXPECT_EQ(firstButton->key, secondButton->key);

    auto propertySnapshot =
        connection->get_property_snapshot(firstButton->providerHandle);
    ASSERT_TRUE(propertySnapshot.ok) << propertySnapshot.error;
    const auto* opacity =
        find_property_descriptor(propertySnapshot, "Opacity");
    ASSERT_NE(opacity, nullptr);
    auto setOpacity = connection->set_property(
        firstButton->providerHandle, opacity->descriptorId, "0.4");
    ASSERT_TRUE(setOpacity.ok) << setOpacity.error;
    EXPECT_NEAR(std::stod(setOpacity.value), 0.4, 0.001);
    auto clearOpacity = connection->clear_property(
        firstButton->providerHandle, opacity->descriptorId);
    ASSERT_TRUE(clearOpacity.ok) << clearOpacity.error;
    EXPECT_TRUE(clearOpacity.cleared);
    EXPECT_NEAR(std::stod(clearOpacity.value), 0.75, 0.001);

    auto afterReads = lvt::managed_connection_capabilities(*connection);
    ASSERT_TRUE(afterReads.has_value());
    EXPECT_EQ(afterReads->connectionId, capabilities->connectionId);
    EXPECT_EQ(afterReads->serverStartCount, capabilities->serverStartCount);

    const auto assemblyInstance = capabilities->assemblyInstanceId;
    const auto previousStartCount = capabilities->serverStartCount;
    connection.reset();

    auto reconnected = provider.open_connection(hwnd, s_pid);
    ASSERT_NE(reconnected, nullptr);
    auto reconnectedCapabilities = lvt::managed_connection_capabilities(*reconnected);
    ASSERT_TRUE(reconnectedCapabilities.has_value());
    EXPECT_NE(reconnectedCapabilities->connectionId, capabilities->connectionId);
    EXPECT_EQ(reconnectedCapabilities->assemblyInstanceId, assemblyInstance);
    EXPECT_EQ(reconnectedCapabilities->serverStartCount, previousStartCount + 1);

    auto refreshed = lvt::build_tree(hwnd, s_pid, {});
    ASSERT_TRUE(reconnected->get_tree(refreshed, false));
    lvt::assign_element_ids(refreshed);
    lvt::assign_element_keys(refreshed);
    const auto* refreshedButton = find_named_element(refreshed, "OkButton");
    ASSERT_NE(refreshedButton, nullptr);
    EXPECT_EQ(refreshedButton->providerHandle, firstButton->providerHandle);
    EXPECT_EQ(refreshedButton->key, firstButton->key);
}

TEST_F(WpfSampleFixture, X86HostfxrAbiRunsTreeAndProperties) {
    if (sizeof(void*) != 4)
        GTEST_SKIP() << "x86 ABI regression";

    HWND hwnd = visible_window_for_pid(s_pid);
    ASSERT_NE(hwnd, nullptr);
    lvt::WpfProvider provider;
    auto connection = provider.open_connection(hwnd, s_pid);
    ASSERT_NE(connection, nullptr)
        << "the x86 TAP must enter managed RunServer without a CRT ABI failure";

    auto tree = lvt::build_tree(hwnd, s_pid, {});
    ASSERT_TRUE(connection->get_tree(tree, false));
    const auto* button = find_named_element(tree, "OkButton");
    ASSERT_NE(button, nullptr);
    auto snapshot =
        connection->get_property_snapshot(button->providerHandle);
    ASSERT_TRUE(snapshot.ok) << snapshot.error;
    const auto* property =
        find_property_descriptor(snapshot, "Opacity");
    ASSERT_NE(property, nullptr);
    auto set = connection->set_property(
        button->providerHandle, property->descriptorId, "0.4");
    ASSERT_TRUE(set.ok) << set.error;
    EXPECT_NEAR(std::stod(set.value), 0.4, 0.001);
    auto clear = connection->clear_property(
        button->providerHandle, property->descriptorId);
    ASSERT_TRUE(clear.ok) << clear.error;
    EXPECT_NEAR(std::stod(clear.value), 0.75, 0.001);
}

TEST_F(WpfSampleFixture, QueuedSetTimeoutCannotMutateLater) {
    SkipIfNotReady();
    HWND hwnd = visible_window_for_pid(s_pid);
    ASSERT_NE(hwnd, nullptr);
    lvt::WpfProvider provider;
    auto connection = provider.open_connection(hwnd, s_pid);
    ASSERT_NE(connection, nullptr);

    auto tree = lvt::build_tree(hwnd, s_pid, {});
    ASSERT_TRUE(connection->get_tree(tree, false));
    const auto* button = find_named_element(tree, "OkButton");
    ASSERT_NE(button, nullptr);
    auto snapshot =
        connection->get_property_snapshot(button->providerHandle);
    ASSERT_TRUE(snapshot.ok) << snapshot.error;
    const auto* property =
        find_property_descriptor(snapshot, "Opacity");
    ASSERT_NE(property, nullptr);

    ScopedUiBlock block;
    ASSERT_TRUE(block.enter(L"Wpf", s_pid));
    auto set = connection->set_property(
        button->providerHandle, property->descriptorId, "0.4");
    EXPECT_FALSE(set.ok);
    EXPECT_NE(set.error.find("before execution"), std::string::npos)
        << set.error;
    block.release();

    auto after =
        connection->get_property_snapshot(button->providerHandle);
    ASSERT_TRUE(after.ok) << after.error;
    const auto* value =
        find_property_value(after, property->descriptorId);
    ASSERT_NE(value, nullptr);
    EXPECT_NEAR(std::stod(value->value), 0.75, 0.001);
    EXPECT_FALSE(value->canClear);
}
#endif

#if LVT_ENABLE_WINFORMS && LVT_WITH_MANAGED
TEST(ManagedWinFormsConnection, TargetExitBreaksConnectionWithoutBlocking) {
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION processInfo{};
    std::string command = std::string("\"") + WINFORMS_SAMPLE_EXE_PATH + "\"";
    const auto workingDirectory = fs::path(WINFORMS_SAMPLE_EXE_PATH).parent_path().string();
    ASSERT_TRUE(CreateProcessA(
        nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
        workingDirectory.c_str(), &startup, &processInfo));
    wil::unique_handle process(processInfo.hProcess);
    wil::unique_handle thread(processInfo.hThread);
    WaitForInputIdle(process.get(), 5000);

    HWND hwnd = nullptr;
    for (int attempt = 0; attempt < 20 && !hwnd; ++attempt) {
        hwnd = visible_window_for_pid(processInfo.dwProcessId);
        if (!hwnd)
            Sleep(100);
    }
    ASSERT_NE(hwnd, nullptr);

    lvt::WinFormsProvider provider;
    auto connection = provider.open_connection(hwnd, processInfo.dwProcessId);
    ASSERT_NE(connection, nullptr);
    auto connectedTree = lvt::build_tree(hwnd, processInfo.dwProcessId, {});
    ASSERT_TRUE(connection->get_tree(connectedTree, false));
    const auto* connectedForm = find_named_element(connectedTree, "MainForm");
    ASSERT_NE(connectedForm, nullptr);
    const uint64_t providerHandle = connectedForm->providerHandle;
    ASSERT_TRUE(TerminateProcess(process.get(), 0));
    ASSERT_EQ(WaitForSingleObject(process.get(), 5000), WAIT_OBJECT_0);

    auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(connection->is_alive());
    lvt::Element root;
    EXPECT_FALSE(connection->get_tree(root, false));
    auto properties = connection->get_property_snapshot(providerHandle);
    EXPECT_FALSE(properties.ok);
    EXPECT_FALSE(properties.error.empty());
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
}
#endif

#if LVT_ENABLE_WPF && LVT_WITH_MANAGED
TEST(ManagedWpfConnection, TargetExitBreaksConnectionWithoutBlocking) {
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION processInfo{};
    std::string command = std::string("\"") + WPF_SAMPLE_EXE_PATH + "\"";
    const auto workingDirectory = fs::path(WPF_SAMPLE_EXE_PATH).parent_path().string();
    ASSERT_TRUE(CreateProcessA(
        nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
        workingDirectory.c_str(), &startup, &processInfo));
    wil::unique_handle process(processInfo.hProcess);
    wil::unique_handle thread(processInfo.hThread);
    WaitForInputIdle(process.get(), 5000);

    HWND hwnd = nullptr;
    for (int attempt = 0; attempt < 20 && !hwnd; ++attempt) {
        hwnd = visible_window_for_pid(processInfo.dwProcessId);
        if (!hwnd)
            Sleep(100);
    }
    ASSERT_NE(hwnd, nullptr);

    lvt::WpfProvider provider;
    auto connection = provider.open_connection(hwnd, processInfo.dwProcessId);
    ASSERT_NE(connection, nullptr);
    auto connectedTree = lvt::build_tree(hwnd, processInfo.dwProcessId, {});
    ASSERT_TRUE(connection->get_tree(connectedTree, false));
    const auto* connectedButton = find_named_element(connectedTree, "OkButton");
    ASSERT_NE(connectedButton, nullptr);
    const uint64_t providerHandle = connectedButton->providerHandle;
    ASSERT_TRUE(TerminateProcess(process.get(), 0));
    ASSERT_EQ(WaitForSingleObject(process.get(), 5000), WAIT_OBJECT_0);

    auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(connection->is_alive());
    lvt::Element root;
    EXPECT_FALSE(connection->get_tree(root, false));
    auto properties = connection->get_property_snapshot(providerHandle);
    EXPECT_FALSE(properties.ok);
    EXPECT_FALSE(properties.error.empty());
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
}
#endif

#if LVT_ENABLE_WINFORMS && LVT_WITH_MANAGED
TEST(ManagedCoreClrFloor, Net6WinFormsTreeAndProperties) {
    ScopedSampleProcess sample;
    ASSERT_TRUE(sample.start(WINFORMS_NET6_SAMPLE_EXE_PATH));
    auto coreClr = loaded_module_path_until(sample.pid(), L"coreclr.dll");
    ASSERT_FALSE(coreClr.empty());
    std::transform(
        coreClr.begin(), coreClr.end(), coreClr.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(towlower(value)); });
    EXPECT_NE(coreClr.find(L"\\6.0."), std::wstring::npos)
        << "the compatibility fixture must actually run on CoreCLR 6: "
        << fs::path(coreClr).string();

    lvt::WinFormsProvider provider;
    auto connection = provider.open_connection(sample.hwnd(), sample.pid());
    ASSERT_NE(connection, nullptr);
    auto tree = lvt::build_tree(sample.hwnd(), sample.pid(), {});
    ASSERT_TRUE(connection->get_tree(tree, false));
    const auto* form = find_named_element(tree, "MainForm");
    ASSERT_NE(form, nullptr);
    auto snapshot =
        connection->get_property_snapshot(form->providerHandle);
    ASSERT_TRUE(snapshot.ok) << snapshot.error;
    EXPECT_NE(find_property_descriptor(snapshot, "EditableText"), nullptr);
}
#endif

#if LVT_ENABLE_WPF && LVT_WITH_MANAGED
TEST(ManagedCoreClrFloor, Net6WpfTreeAndProperties) {
    ScopedSampleProcess sample;
    ASSERT_TRUE(sample.start(WPF_NET6_SAMPLE_EXE_PATH));
    auto coreClr = loaded_module_path_until(sample.pid(), L"coreclr.dll");
    ASSERT_FALSE(coreClr.empty());
    std::transform(
        coreClr.begin(), coreClr.end(), coreClr.begin(),
        [](wchar_t value) { return static_cast<wchar_t>(towlower(value)); });
    EXPECT_NE(coreClr.find(L"\\6.0."), std::wstring::npos)
        << "the compatibility fixture must actually run on CoreCLR 6: "
        << fs::path(coreClr).string();

    lvt::WpfProvider provider;
    auto connection = provider.open_connection(sample.hwnd(), sample.pid());
    ASSERT_NE(connection, nullptr);
    auto tree = lvt::build_tree(sample.hwnd(), sample.pid(), {});
    ASSERT_TRUE(connection->get_tree(tree, false));
    const auto* button = find_named_element(tree, "OkButton");
    ASSERT_NE(button, nullptr);
    auto snapshot =
        connection->get_property_snapshot(button->providerHandle);
    ASSERT_TRUE(snapshot.ok) << snapshot.error;
    EXPECT_NE(find_property_descriptor(snapshot, "Opacity"), nullptr);
}
#endif

#if LVT_ENABLE_WINFORMS && LVT_WITH_MANAGED
TEST(ManagedClrCompatibility, Net48WinFormsTreeAndProperties) {
    ScopedSampleProcess sample;
    ASSERT_TRUE(sample.start(WINFORMS_NET48_SAMPLE_EXE_PATH));
    EXPECT_FALSE(loaded_module_path_until(sample.pid(), L"clr.dll").empty());
    EXPECT_TRUE(loaded_module_path(sample.pid(), L"coreclr.dll").empty());

    lvt::WinFormsProvider provider;
    auto connection = provider.open_connection(sample.hwnd(), sample.pid());
    ASSERT_NE(connection, nullptr);
    auto tree = lvt::build_tree(sample.hwnd(), sample.pid(), {});
    ASSERT_TRUE(connection->get_tree(tree, false));
    const auto* form = find_named_element(tree, "MainForm");
    ASSERT_NE(form, nullptr);
    auto snapshot =
        connection->get_property_snapshot(form->providerHandle);
    ASSERT_TRUE(snapshot.ok) << snapshot.error;
    EXPECT_NE(find_property_descriptor(snapshot, "EditableText"), nullptr);
}
#endif

#if LVT_ENABLE_WPF && LVT_WITH_MANAGED
TEST(ManagedClrCompatibility, Net48WpfTreeAndProperties) {
    ScopedSampleProcess sample;
    ASSERT_TRUE(sample.start(WPF_NET48_SAMPLE_EXE_PATH));
    EXPECT_FALSE(loaded_module_path_until(sample.pid(), L"clr.dll").empty());
    EXPECT_TRUE(loaded_module_path(sample.pid(), L"coreclr.dll").empty());

    lvt::WpfProvider provider;
    auto connection = provider.open_connection(sample.hwnd(), sample.pid());
    ASSERT_NE(connection, nullptr);
    auto tree = lvt::build_tree(sample.hwnd(), sample.pid(), {});
    ASSERT_TRUE(connection->get_tree(tree, false));
    const auto* button = find_named_element(tree, "OkButton");
    ASSERT_NE(button, nullptr);
    auto snapshot =
        connection->get_property_snapshot(button->providerHandle);
    ASSERT_TRUE(snapshot.ok) << snapshot.error;
    EXPECT_NE(find_property_descriptor(snapshot, "Opacity"), nullptr);
}
#endif

#if LVT_ENABLE_WINFORMS && LVT_WITH_MANAGED
TEST(ManagedIdentity, NewProcessRejectsPriorAssemblyHandle) {
    uint64_t oldHandle = 0;
    {
        ScopedSampleProcess first;
        ASSERT_TRUE(first.start(WINFORMS_SAMPLE_EXE_PATH));
        lvt::WinFormsProvider provider;
        auto connection = provider.open_connection(first.hwnd(), first.pid());
        ASSERT_NE(connection, nullptr);
        auto tree = lvt::build_tree(first.hwnd(), first.pid(), {});
        ASSERT_TRUE(connection->get_tree(tree, false));
        const auto* form = find_named_element(tree, "MainForm");
        ASSERT_NE(form, nullptr);
        oldHandle = form->providerHandle;
        ASSERT_NE(oldHandle, 0u);
    }

    ScopedSampleProcess second;
    ASSERT_TRUE(second.start(WINFORMS_SAMPLE_EXE_PATH));
    lvt::WinFormsProvider provider;
    auto connection = provider.open_connection(second.hwnd(), second.pid());
    ASSERT_NE(connection, nullptr);
    auto tree = lvt::build_tree(second.hwnd(), second.pid(), {});
    ASSERT_TRUE(connection->get_tree(tree, false));
    const auto* form = find_named_element(tree, "MainForm");
    ASSERT_NE(form, nullptr);
    EXPECT_NE(form->providerHandle, oldHandle);
    auto stale = connection->get_property_snapshot(oldHandle);
    EXPECT_FALSE(stale.ok);
    EXPECT_NE(stale.error.find("stale"), std::string::npos);
}
#endif

class WinUI3SampleFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        s_sample_exe = WINUI3_SAMPLE_EXE_PATH;
        if (!fs::exists(s_sample_exe)) {
            s_skip_reason = "WinUI3 sample app not found: " + s_sample_exe;
            return;
        }

        STARTUPINFOA si = {sizeof(si)};
        s_pi = {};
        auto workdir = fs::path(s_sample_exe).parent_path().string();
        std::string cmd = "\"" + s_sample_exe + "\"";
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, workdir.c_str(), &si, &s_pi)) {
            s_skip_reason = "Failed to launch WinUI3 sample app";
            return;
        }
        s_pid = s_pi.dwProcessId;
        s_process.reset(s_pi.hProcess);
        s_thread.reset(s_pi.hThread);
        const auto fixtureArchitecture =
            lvt::detect_process_architecture(s_pid);
        const auto testArchitecture = lvt::get_host_architecture();
        if (fixtureArchitecture != lvt::Architecture::unknown &&
            testArchitecture != lvt::Architecture::unknown &&
            fixtureArchitecture != testArchitecture) {
            s_skip_reason =
                "WinUI3 fixture architecture mismatch: tests are " +
                std::string(lvt::architecture_name(testArchitecture)) +
                ", fixture is " +
                lvt::architecture_name(fixtureArchitecture);
            TerminateProcess(s_process.get(), 0);
            WaitForSingleObject(s_process.get(), 5000);
            s_process.reset();
            s_thread.reset();
            s_pi.hProcess = nullptr;
            s_pi.hThread = nullptr;
            return;
        }
        if (s_pi.hProcess) {
            WaitForInputIdle(s_pi.hProcess, 10000);
        }

        auto lvt = get_lvt_path();
        for (int attempt = 0; attempt < 60; ++attempt) {
            if (WaitForSingleObject(s_pi.hProcess, 0) == WAIT_OBJECT_0) {
                s_skip_reason = "WinUI3 sample app exited before it became ready";
                return;
            }

            auto output = run_command(make_cmd(lvt, get_pid_arg()));
            auto j = json::parse(output, nullptr, false);
            if (!j.is_discarded() && frameworks_contain_winui3(j) &&
                j.contains("root") &&
                json_tree_has_named_control(j["root"], "PrimaryButton") &&
                json_tree_has_named_control(j["root"], "InputBox") &&
                json_tree_has_named_control(j["root"], "ReadyCheckBox")) {
                s_ready = true;
                return;
            }
            Sleep(1000);
        }

        s_skip_reason = "WinUI3 sample app never became ready with WinUI3 framework and named controls";
    }

    static void TearDownTestSuite() {
        if (s_pi.hProcess) {
            TerminateProcess(s_pi.hProcess, 0);
            s_process.reset();
            s_thread.reset();
            s_pi.hProcess = nullptr;
            s_pi.hThread = nullptr;
        }
    }

    void SetUp() override {
        if (!s_ready)
            GTEST_SKIP() << s_skip_reason;
    }

    static void SkipIfNotReady() {
        if (!s_ready)
            GTEST_SKIP() << s_skip_reason;
    }

    static std::string get_pid_arg() {
        return "--pid " + std::to_string(s_pid);
    }

    static PROCESS_INFORMATION s_pi;
    static wil::unique_handle s_process;
    static wil::unique_handle s_thread;
    static DWORD s_pid;
    static bool s_ready;
    static std::string s_sample_exe;
    static std::string s_skip_reason;
};

PROCESS_INFORMATION WinUI3SampleFixture::s_pi = {};
wil::unique_handle WinUI3SampleFixture::s_process;
wil::unique_handle WinUI3SampleFixture::s_thread;
DWORD WinUI3SampleFixture::s_pid = 0;
bool WinUI3SampleFixture::s_ready = false;
std::string WinUI3SampleFixture::s_sample_exe;
std::string WinUI3SampleFixture::s_skip_reason;

TEST_F(WinUI3SampleFixture, DurableKeysDeterministicAndQueryable) {
    SkipIfNotReady();

    auto lvt = get_lvt_path();
    auto winui3Ready = [](const json& j) {
        return frameworks_contain_winui3(j) &&
               has_winui3_stitched_under_bridge(j["root"]) &&
               json_tree_has_named_control(j["root"], "PrimaryButton");
    };
    auto j1 = dump_ready_tree(lvt, get_pid_arg(), winui3Ready);
    auto j2 = dump_ready_tree(lvt, get_pid_arg(), winui3Ready);
    ASSERT_TRUE(winui3Ready(j1)) << "WinUI3 tree never became ready (dump 1)";
    ASSERT_TRUE(winui3Ready(j2)) << "WinUI3 tree never became ready (dump 2)";
    ASSERT_TRUE(frameworks_contain_winui3(j1));
    ASSERT_TRUE(has_winui3_stitched_under_bridge(j1["root"]))
        << "WinUI3 elements were not grafted under DesktopChildSiteBridge";

    std::vector<const json*> elements;
    collect_json_elements(j1["root"], elements);
    ASSERT_GT(elements.size(), 0u);

    std::set<std::string> keys;
    for (auto* el : elements) {
        auto key = el->value("key", "");
        EXPECT_FALSE(key.empty()) << "Element " << el->value("id", "?") << " has empty durable key";
        EXPECT_TRUE(keys.insert(key).second) << "Duplicate durable key: " << key;
    }

    std::map<std::string, std::string> firstMap;
    std::map<std::string, std::string> secondMap;
    collect_key_contract_map(j1["root"], "0", firstMap);
    collect_key_contract_map(j2["root"], "0", secondMap);
    EXPECT_EQ(firstMap, secondMap);

    auto* button = find_named_control(j1["root"], "PrimaryButton");
    ASSERT_NE(button, nullptr);
    EXPECT_EQ(button->value("framework", ""), "winui3");
    EXPECT_EQ(button->value("type", ""), "Button");

    auto buttonKey = button->value("key", "");
    ASSERT_FALSE(buttonKey.empty());
    auto byKey = query_prop_until(lvt, get_pid_arg(), buttonKey, "name", "PrimaryButton");
    EXPECT_EQ(byKey, "PrimaryButton");
}

// --fast skips IVisualTreeService::GetPropertyValuesChain (the dominant
// per-element cost of a rich WinUI3 tree, ~4.5ms/element measured live
// against Microsoft Store/Calculator) and collects bounds/Text/Content the
// cheaper way instead — see lvt_tap.cpp's CollectBounds/CollectPositionsAndText.
// This only checks the one thing that actually matters for a caller: the
// same named controls, with the same identity, still show up — not that
// every property matches (--fast is documented to report fewer of them).
TEST_F(WinUI3SampleFixture, FastModeStillFindsNamedControlsAndBounds) {
    SkipIfNotReady();

    auto lvt = get_lvt_path();
    auto winui3Ready = [](const json& j) {
        return frameworks_contain_winui3(j) &&
               has_winui3_stitched_under_bridge(j["root"]) &&
               json_tree_has_named_control(j["root"], "PrimaryButton");
    };
    auto j = dump_ready_tree(lvt, get_pid_arg() + " --fast", winui3Ready);
    ASSERT_TRUE(winui3Ready(j)) << "WinUI3 tree never became ready in --fast mode";

    auto* button = find_named_control(j["root"], "PrimaryButton");
    ASSERT_NE(button, nullptr);
    EXPECT_EQ(button->value("framework", ""), "winui3");
    EXPECT_EQ(button->value("type", ""), "Button");
    ASSERT_FALSE(button->value("key", "").empty());

    // Bounds still need to come from somewhere in fast mode — from the
    // direct FrameworkElement.ActualWidth/ActualHeight read in
    // CollectPositionsAndText, since CollectBounds (GetPropertyValuesChain)
    // is skipped entirely. At least some elements in a real, rendered
    // window must report non-zero size, or fast mode would be useless for
    // the highlight/hit-test use cases it exists to serve.
    std::vector<const json*> elements;
    collect_json_elements(j["root"], elements);
    bool anyNonZeroBounds = false;
    for (auto* el : elements) {
        if (!el->contains("bounds")) continue;
        auto& b = (*el)["bounds"];
        if (b.value("width", 0) > 0 && b.value("height", 0) > 0) {
            anyNonZeroBounds = true;
            break;
        }
    }
    EXPECT_TRUE(anyNonZeroBounds) << "no element reported non-zero bounds in --fast mode";
}

TEST_F(NotepadFixture, Win32BoundsReasonable) {
    // Every element in the Win32 tree should have reasonable (non-extreme) bounds
    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg()));
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded());

    std::vector<const json*> elements;
    collect_json_elements(j["root"], elements);
    ASSERT_GT(elements.size(), 0u);

    for (auto* el : elements) {
        if (!el->contains("bounds")) continue;
        auto& b = (*el)["bounds"];
        int x = b["x"].get<int>();
        int y = b["y"].get<int>();
        int w = b["width"].get<int>();
        int h = b["height"].get<int>();
        std::string id = el->value("id", "?");

        // No element should have bounds outside a generous screen range.
        EXPECT_GT(x, kMinReasonableCoord) << "Element " << id << " has extreme x=" << x;
        EXPECT_LT(x, kMaxReasonableCoord) << "Element " << id << " has extreme x=" << x;
        EXPECT_GT(y, kMinReasonableCoord) << "Element " << id << " has extreme y=" << y;
        EXPECT_LT(y, kMaxReasonableCoord) << "Element " << id << " has extreme y=" << y;
        EXPECT_GE(w, 0) << "Element " << id << " has negative width=" << w;
        EXPECT_GE(h, 0) << "Element " << id << " has negative height=" << h;
        EXPECT_LT(w, kMaxReasonableSize) << "Element " << id << " has extreme width=" << w;
        EXPECT_LT(h, kMaxReasonableSize) << "Element " << id << " has extreme height=" << h;
    }
}

TEST_F(NotepadFixture, RootBoundsNonZero) {
    // The root window should have meaningful non-zero bounds
    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg()));
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded());

    auto& b = j["root"]["bounds"];
    EXPECT_GT(b["width"].get<int>(), 0) << "Root width should be positive";
    EXPECT_GT(b["height"].get<int>(), 0) << "Root height should be positive";
}

TEST_F(NotepadFixture, DurableKeysDeterministicAcrossTwoRuns) {
    auto lvt = get_lvt_path();
    auto first = run_command(make_cmd(lvt, get_pid_arg()));
    auto second = run_command(make_cmd(lvt, get_pid_arg()));
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());

    auto j1 = json::parse(first, nullptr, false);
    auto j2 = json::parse(second, nullptr, false);
    ASSERT_FALSE(j1.is_discarded());
    ASSERT_FALSE(j2.is_discarded());

    std::map<std::string, std::string> firstMap;
    std::map<std::string, std::string> secondMap;
    collect_stable_win32_map(j1["root"], "0", firstMap);
    collect_stable_win32_map(j2["root"], "0", secondMap);
    if (firstMap.empty() || secondMap.empty())
        GTEST_SKIP() << "No stable Win32 elements found for deterministic key comparison";

    EXPECT_EQ(firstMap, secondMap);
}

TEST_F(NotepadFixture, QueryByIdAndDurableKey) {
    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg()));
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded());

    auto rootKey = j["root"].value("key", "");
    ASSERT_FALSE(rootKey.empty());

    auto byId = run_command(make_cmd(lvt, get_pid_arg() + " query e0 type"));
    EXPECT_EQ(trim_crlf(byId), j["root"].value("type", ""));

    auto byKey = run_command(make_cmd(lvt, get_pid_arg() + " query " + cmd_escape_arg(rootKey) + " id"));
    EXPECT_EQ(trim_crlf(byKey), "e0");
}

// ---- Annotation verification (DEBUG builds only) ----

#ifndef NDEBUG
TEST_F(NotepadFixture, AnnotationsJsonOutput) {
    // The --annotations-json flag should produce structured annotation data
    auto lvt = get_lvt_path();
    auto annFile = fs::path(lvt).parent_path() / "lvt_test_annotations.json";
    fs::remove(annFile);

    run_command(make_cmd(lvt,
        get_pid_arg() + " --annotations-json " + annFile.string()));

    ASSERT_TRUE(fs::exists(annFile)) << "Annotations file was not created";
    std::ifstream f(annFile);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto aj = json::parse(content, nullptr, false);
    ASSERT_FALSE(aj.is_discarded()) << "Annotations file is not valid JSON";
    ASSERT_TRUE(aj.is_array());

    // Notepad should have at least a few annotated elements (root + children)
    EXPECT_GT(aj.size(), 0u) << "Should annotate at least one element";

    // Every annotation should have an id and reasonable pixel-space bounds
    for (auto& a : aj) {
        EXPECT_TRUE(a.contains("id"));
        EXPECT_TRUE(a.contains("x"));
        EXPECT_TRUE(a.contains("y"));
        EXPECT_TRUE(a.contains("width"));
        EXPECT_TRUE(a.contains("height"));

        int w = a["width"].get<int>();
        int h = a["height"].get<int>();
        EXPECT_GT(w, 0) << "Annotation " << a["id"] << " has non-positive width";
        EXPECT_GT(h, 0) << "Annotation " << a["id"] << " has non-positive height";
    }

    f.close();
    fs::remove(annFile);
}

TEST_F(NotepadFixture, AnnotationsMatchTreeElements) {
    // Annotated element IDs should be a subset of the tree's element IDs
    auto lvt = get_lvt_path();
    auto annFile = fs::path(lvt).parent_path() / "lvt_test_ann_match.json";
    fs::remove(annFile);

    auto output = run_command(make_cmd(lvt,
        get_pid_arg() + " dump --annotations-json " + annFile.string()));

    auto tree = json::parse(output, nullptr, false);
    ASSERT_FALSE(tree.is_discarded());

    // Collect all element IDs from the tree
    std::vector<const json*> treeElements;
    collect_json_elements(tree["root"], treeElements);
    std::set<std::string> treeIds;
    for (auto* el : treeElements) {
        if (el->contains("id"))
            treeIds.insert((*el)["id"].get<std::string>());
    }

    // Read annotations
    ASSERT_TRUE(fs::exists(annFile));
    std::ifstream f(annFile);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto annotations = json::parse(content, nullptr, false);
    ASSERT_FALSE(annotations.is_discarded());

    // Every annotated ID must exist in the tree
    for (auto& a : annotations) {
        std::string id = a["id"].get<std::string>();
        EXPECT_TRUE(treeIds.count(id) > 0)
            << "Annotated element " << id << " not found in tree";
    }

    f.close();
    fs::remove(annFile);
}
#endif

// ---- Controlled Common Controls tests ----

class ComCtlWindowFixture : public ::testing::Test {
protected:
    void SetUp() override {
        INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES};
        ASSERT_TRUE(InitCommonControlsEx(&icc)) << "InitCommonControlsEx failed";

        readyEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        ASSERT_NE(readyEvent_, nullptr);

        thread_.reset(CreateThread(nullptr, 0, &ComCtlWindowFixture::thread_proc, this, 0, nullptr));
        ASSERT_NE(thread_, nullptr);

        ASSERT_EQ(WaitForSingleObject(readyEvent_.get(), 5000), WAIT_OBJECT_0)
            << "Timed out creating ComCtl test window";
        ASSERT_NE(parentHwnd_, nullptr);
        ASSERT_NE(listViewHwnd_, nullptr);
        ASSERT_NE(treeViewHwnd_, nullptr);
        ASSERT_TRUE(listTextOk_) << "ListView text was not populated";
        ASSERT_TRUE(treeTextOk_) << "TreeView text was not populated";
    }

    void TearDown() override {
        if (parentHwnd_)
            PostMessageW(parentHwnd_, WM_APP + 1, 0, 0);
        if (thread_) {
            WaitForSingleObject(thread_.get(), 5000);
            thread_.reset();
        }
        readyEvent_.reset();
    }

    std::string get_hwnd_arg() const {
        char buf[64];
        sprintf_s(buf, "--hwnd 0x%llX", (unsigned long long)(uintptr_t)parentHwnd_);
        return buf;
    }

private:
    static DWORD WINAPI thread_proc(void* param) {
        static_cast<ComCtlWindowFixture*>(param)->run_ui_thread();
        return 0;
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_APP + 1) {
            // Window proc owns the HWND lifetime; close on the UI thread.
            DestroyWindow(hwnd);
            return 0;
        }
        if (msg == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void run_ui_thread() {
        INITCOMMONCONTROLSEX icc{sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES};
        InitCommonControlsEx(&icc);

        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"LvtComCtlTestWindow";
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);

        parentHwnd_ = CreateWindowExW(0, L"LvtComCtlTestWindow", L"LVT ComCtl Test Window",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            120, 120, 520, 360,
            nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
        if (!parentHwnd_) {
            SetEvent(readyEvent_.get());
            return;
        }

        listViewHwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            10, 10, 480, 130,
            parentHwnd_, reinterpret_cast<HMENU>(1001), GetModuleHandle(nullptr), nullptr);

        treeViewHwnd_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
            WS_CHILD | WS_VISIBLE | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS,
            10, 155, 480, 130,
            parentHwnd_, reinterpret_cast<HMENU>(1002), GetModuleHandle(nullptr), nullptr);

        if (listViewHwnd_)
            populate_listview();
        if (treeViewHwnd_)
            populate_treeview();
        verify_control_text();

        UpdateWindow(parentHwnd_);
        pump_pending_messages();
        SetEvent(readyEvent_.get());

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        parentHwnd_ = nullptr;
        listViewHwnd_ = nullptr;
        treeViewHwnd_ = nullptr;
        UnregisterClassW(L"LvtComCtlTestWindow", GetModuleHandle(nullptr));
    }

    void populate_listview() {
        ListView_SetExtendedListViewStyle(listViewHwnd_, LVS_EX_FULLROWSELECT);

        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.cx = 160;
        col.pszText = const_cast<LPWSTR>(L"Name");
        col.iSubItem = 0;
        SendMessageW(listViewHwnd_, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));

        col.cx = 120;
        col.pszText = const_cast<LPWSTR>(L"Value");
        col.iSubItem = 1;
        SendMessageW(listViewHwnd_, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&col));

        insert_listview_item(0, L"Alpha", L"One");
        insert_listview_item(1, L"Beta", L"Two");
        insert_listview_item(2, L"Gamma", L"Three");
    }

    void insert_listview_item(int index, const wchar_t* name, const wchar_t* value) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = index;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(name);
        SendMessageW(listViewHwnd_, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));

        LVITEMW subitem{};
        subitem.iSubItem = 1;
        subitem.pszText = const_cast<LPWSTR>(value);
        SendMessageW(listViewHwnd_, LVM_SETITEMTEXTW, index, reinterpret_cast<LPARAM>(&subitem));
    }

    void populate_treeview() {
        TVINSERTSTRUCTW insert{};
        insert.hParent = TVI_ROOT;
        insert.hInsertAfter = TVI_LAST;
        insert.item.mask = TVIF_TEXT;
        insert.item.pszText = const_cast<LPWSTR>(L"Root Node");
        auto root = reinterpret_cast<HTREEITEM>(
            SendMessageW(treeViewHwnd_, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&insert)));

        insert.hParent = root;
        insert.item.pszText = const_cast<LPWSTR>(L"Child One");
        SendMessageW(treeViewHwnd_, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&insert));
        insert.item.pszText = const_cast<LPWSTR>(L"Child Two");
        SendMessageW(treeViewHwnd_, TVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&insert));

        SendMessageW(treeViewHwnd_, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(root));
    }

    static void pump_pending_messages() {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    void verify_control_text() {
        wchar_t listText[64]{};
        LVITEMW item{};
        item.iSubItem = 0;
        item.cchTextMax = static_cast<int>(_countof(listText));
        item.pszText = listText;
        SendMessageW(listViewHwnd_, LVM_GETITEMTEXTW, 0, reinterpret_cast<LPARAM>(&item));
        listTextOk_ = wcscmp(listText, L"Alpha") == 0;

        auto root = reinterpret_cast<HTREEITEM>(
            SendMessageW(treeViewHwnd_, TVM_GETNEXTITEM, TVGN_ROOT, 0));
        wchar_t treeText[64]{};
        TVITEMW treeItem{};
        treeItem.mask = TVIF_TEXT;
        treeItem.hItem = root;
        treeItem.pszText = treeText;
        treeItem.cchTextMax = static_cast<int>(_countof(treeText));
        SendMessageW(treeViewHwnd_, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&treeItem));
        treeTextOk_ = wcscmp(treeText, L"Root Node") == 0;
    }

protected:
    HWND parentHwnd_ = nullptr;
    HWND listViewHwnd_ = nullptr;
    HWND treeViewHwnd_ = nullptr;
    wil::unique_event readyEvent_;
    wil::unique_handle thread_;
    bool listTextOk_ = false;
    bool treeTextOk_ = false;
};

TEST_F(ComCtlWindowFixture, DetectsComCtlAndEnrichesItems) {
    auto lvt = get_lvt_path();
    auto fwOutput = run_command(make_cmd(lvt, get_hwnd_arg() + " frameworks"));
    ASSERT_FALSE(fwOutput.empty());
    EXPECT_NE(fwOutput.find("win32"), std::string::npos);
    EXPECT_NE(fwOutput.find("comctl"), std::string::npos);

    auto output = run_command(make_cmd(lvt, get_hwnd_arg()));
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << "Output is not valid JSON";

    std::vector<const json*> elements;
    collect_json_elements(j["root"], elements);

    const json* listView = nullptr;
    const json* treeView = nullptr;
    int listViewItemCount = 0;
    int treeViewItemCount = 0;
    for (auto* el : elements) {
        if (el->value("className", "") == "SysListView32") {
            listView = el;
            EXPECT_EQ(el->value("framework", ""), "comctl");
            EXPECT_EQ(el->value("type", ""), "ListView");
        }
        if (el->value("className", "") == "SysTreeView32") {
            treeView = el;
            EXPECT_EQ(el->value("framework", ""), "comctl");
            EXPECT_EQ(el->value("type", ""), "TreeView");
        }
        if (el->value("type", "") == "ListViewItem")
            listViewItemCount++;
        if (el->value("type", "") == "TreeViewItem")
            treeViewItemCount++;
    }

    ASSERT_NE(listView, nullptr);
    ASSERT_NE(treeView, nullptr);
    EXPECT_EQ((*listView)["properties"].value("itemCount", ""), "3");
    EXPECT_EQ((*listView)["properties"].value("columnCount", ""), "2");
    EXPECT_EQ((*listView)["properties"].value("viewMode", ""), "details");
    EXPECT_EQ((*treeView)["properties"].value("itemCount", ""), "3");
    EXPECT_EQ(listViewItemCount, 3);
    EXPECT_GE(treeViewItemCount, 1);
    ASSERT_NE(find_element_by_type_property(j["root"], "ListViewItem", "index", "0"), nullptr);
    ASSERT_NE(find_element_by_type_property(j["root"], "ListViewItem", "index", "1"), nullptr);
    ASSERT_NE(find_element_by_type_property(j["root"], "ListViewItem", "index", "2"), nullptr);
}

TEST_F(ComCtlWindowFixture, DurableKeysCoverFullStaticTreeAndQueryRoundTrips) {
    auto lvt = get_lvt_path();
    auto first = run_command(make_cmd(lvt, get_hwnd_arg()));
    auto second = run_command(make_cmd(lvt, get_hwnd_arg()));
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());

    auto j1 = json::parse(first, nullptr, false);
    auto j2 = json::parse(second, nullptr, false);
    ASSERT_FALSE(j1.is_discarded());
    ASSERT_FALSE(j2.is_discarded());

    std::vector<const json*> elements;
    collect_json_elements(j1["root"], elements);
    ASSERT_GT(elements.size(), 0u);

    std::set<std::string> keys;
    for (auto* el : elements) {
        auto key = el->value("key", "");
        EXPECT_FALSE(key.empty()) << el->value("id", "?") << " has empty key";
        EXPECT_TRUE(keys.insert(key).second) << "Duplicate key: " << key;
    }

    std::map<std::string, std::string> firstMap;
    std::map<std::string, std::string> secondMap;
    collect_key_contract_map(j1["root"], "0", firstMap);
    collect_key_contract_map(j2["root"], "0", secondMap);
    EXPECT_EQ(firstMap, secondMap);

    auto* item = find_element_by_type_property(j1["root"], "ListViewItem", "index", "0");
    ASSERT_NE(item, nullptr);
    auto key = item->value("key", "");
    ASSERT_FALSE(key.empty());

    auto byKey = run_command(make_cmd(lvt, get_hwnd_arg() + " query " + cmd_escape_arg(key) + " index"));
    EXPECT_EQ(trim_crlf(byKey), "0");
}

// ---- Standalone native Win32/Common Controls fixture ----

static lvt::Element* find_native_element_by_hwnd(
    lvt::Element& element, HWND hwnd) {
    if (element.nativeHandle == reinterpret_cast<uintptr_t>(hwnd))
        return &element;
    for (auto& child : element.children) {
        if (auto* found = find_native_element_by_hwnd(child, hwnd))
            return found;
    }
    return nullptr;
}

static lvt::Element* find_native_element_by_text(
    lvt::Element& element, const std::string& type,
    const std::string& text) {
    if (element.type == type && element.text == text)
        return &element;
    for (auto& child : element.children) {
        if (auto* found =
                find_native_element_by_text(child, type, text)) {
            return found;
        }
    }
    return nullptr;
}

static const lvt::PropertyDescriptor* native_descriptor(
    const lvt::PropertySnapshotResult& snapshot,
    const std::string& name) {
    if (!snapshot.schema)
        return nullptr;
    for (const auto& descriptor : snapshot.schema->descriptors) {
        if (descriptor.name == name)
            return &descriptor;
    }
    return nullptr;
}

static const lvt::PropertyValue* native_value(
    const lvt::PropertySnapshotResult& snapshot,
    const std::string& name) {
    const auto* descriptor = native_descriptor(snapshot, name);
    if (!descriptor)
        return nullptr;
    for (const auto& value : snapshot.values) {
        if (value.descriptorId == descriptor->descriptorId)
            return &value;
    }
    return nullptr;
}

static std::string utf8(const char8_t* value) {
    return reinterpret_cast<const char*>(value);
}

class ScopedNativeFixtureProcess {
public:
    bool start(const fs::path& fixturePath) {
        STARTUPINFOW startupInfo{sizeof(startupInfo)};
        PROCESS_INFORMATION processInfo{};
        std::wstring command = L"\"" + fixturePath.wstring() + L"\"";
        if (!CreateProcessW(
                nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                nullptr, nullptr, &startupInfo, &processInfo)) {
            return false;
        }
        process.reset(processInfo.hProcess);
        thread.reset(processInfo.hThread);
        pid = processInfo.dwProcessId;
        WaitForInputIdle(process.get(), 5000);

        for (int attempt = 0; attempt < 50 && !hwnd; ++attempt) {
            struct Search {
                DWORD pid;
                HWND hwnd;
            } search{pid, nullptr};
            EnumWindows(
                [](HWND candidate, LPARAM parameter) -> BOOL {
                    auto* search =
                        reinterpret_cast<Search*>(parameter);
                    DWORD owner = 0;
                    GetWindowThreadProcessId(candidate, &owner);
                    if (owner != search->pid)
                        return TRUE;
                    wchar_t className[128]{};
                    GetClassNameW(
                        candidate, className,
                        static_cast<int>(_countof(className)));
                    if (wcscmp(
                            className,
                            native_fixture::kWindowClass) == 0) {
                        search->hwnd = candidate;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&search));
            hwnd = search.hwnd;
            if (!hwnd)
                Sleep(100);
        }
        return hwnd != nullptr;
    }

    ~ScopedNativeFixtureProcess() {
        stop();
    }

    void stop() {
        if (hwnd && IsWindow(hwnd))
            PostMessageW(hwnd, native_fixture::kCloseMessage, 0, 0);
        if (process) {
            if (WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0) {
                TerminateProcess(process.get(), 1);
                WaitForSingleObject(process.get(), 5000);
            }
        }
        hwnd = nullptr;
        pid = 0;
        process.reset();
        thread.reset();
    }

    wil::unique_handle process;
    wil::unique_handle thread;
    DWORD pid = 0;
    HWND hwnd = nullptr;
};

class NativeControlsFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        const fs::path fixturePath = NATIVE_CONTROLS_FIXTURE_EXE_PATH;
        ASSERT_TRUE(fs::exists(fixturePath))
            << "Native controls fixture not built at " << fixturePath.string();

        STARTUPINFOW startupInfo{sizeof(startupInfo)};
        PROCESS_INFORMATION processInfo{};
        std::wstring command = L"\"" + fixturePath.wstring() + L"\"";
        ASSERT_TRUE(CreateProcessW(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            nullptr,
            &startupInfo,
            &processInfo))
            << "Failed to launch native controls fixture (error "
            << GetLastError() << ")";

        s_process.reset(processInfo.hProcess);
        s_thread.reset(processInfo.hThread);
        s_pid = processInfo.dwProcessId;
        WaitForInputIdle(s_process.get(), 5000);

        for (int attempt = 0; attempt < 50 && !s_hwnd; ++attempt) {
            EnumWindows(
                [](HWND hwnd, LPARAM parameter) -> BOOL {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(hwnd, &pid);
                    if (pid != s_pid)
                        return TRUE;

                    wchar_t className[128]{};
                    wchar_t title[128]{};
                    GetClassNameW(hwnd, className, static_cast<int>(_countof(className)));
                    GetWindowTextW(hwnd, title, static_cast<int>(_countof(title)));
                    if (wcscmp(className, native_fixture::kWindowClass) == 0 &&
                        wcscmp(title, native_fixture::kWindowTitle) == 0) {
                        *reinterpret_cast<HWND*>(parameter) = hwnd;
                        return FALSE;
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&s_hwnd));
            if (!s_hwnd)
                Sleep(100);
        }

        ASSERT_NE(s_hwnd, nullptr) << "Native controls fixture window was not created";
        ASSERT_TRUE(IsWindowVisible(s_hwnd));
    }

    static void TearDownTestSuite() {
        if (s_hwnd && IsWindow(s_hwnd))
            PostMessageW(s_hwnd, native_fixture::kCloseMessage, 0, 0);

        if (s_process) {
            const DWORD waitResult = WaitForSingleObject(s_process.get(), 5000);
            if (waitResult != WAIT_OBJECT_0) {
                ADD_FAILURE() << "Native controls fixture did not close cleanly";
                TerminateProcess(s_process.get(), 1);
                WaitForSingleObject(s_process.get(), 5000);
            } else {
                DWORD exitCode = 1;
                ASSERT_TRUE(GetExitCodeProcess(s_process.get(), &exitCode));
                EXPECT_EQ(exitCode, 0u);
            }
        }

        s_hwnd = nullptr;
        s_process.reset();
        s_thread.reset();
        s_pid = 0;
    }

    static HWND control(int id) {
        return GetDlgItem(s_hwnd, id);
    }

    static std::string get_hwnd_arg() {
        char buffer[64]{};
        sprintf_s(
            buffer,
            "--hwnd 0x%llX",
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s_hwnd)));
        return buffer;
    }

    static json dump_tree() {
        auto output = run_command(make_cmd(get_lvt_path(), get_hwnd_arg()));
        auto tree = json::parse(output, nullptr, false);
        EXPECT_FALSE(tree.is_discarded()) << "Output is not valid JSON:\n" << output;
        return tree;
    }

    struct NativeTree {
        std::shared_ptr<lvt::NativePropertyConnection> win32;
        std::shared_ptr<lvt::NativePropertyConnection> comctl;
        lvt::Element root;
    };

    static NativeTree native_tree() {
        NativeTree result;
        result.win32 = lvt::NativePropertyConnection::connect(
            s_hwnd, s_pid, "win32");
        std::string comctlVersion;
        const auto frameworks = lvt::detect_frameworks(s_hwnd, s_pid);
        for (const auto& framework : frameworks) {
            if (framework.type == lvt::Framework::ComCtl)
                comctlVersion = framework.version;
        }
        result.comctl = lvt::NativePropertyConnection::connect(
            s_hwnd, s_pid, "comctl", comctlVersion);
        auto lookup =
            [&result](const std::string& provider)
                -> lvt::IFrameworkConnection* {
            if (provider == "win32")
                return result.win32.get();
            if (provider == "comctl")
                return result.comctl.get();
            return nullptr;
        };
        result.root = lvt::build_tree(
            s_hwnd, s_pid, frameworks, -1, {}, false, lookup);
        lvt::assign_element_ids(result.root);
        lvt::assign_element_keys(result.root);
        return result;
    }

    static lvt::PropertySnapshotResult snapshot(
        NativeTree& tree, lvt::Element& element) {
        auto* connection = element.framework == "comctl"
            ? static_cast<lvt::IFrameworkConnection*>(tree.comctl.get())
            : static_cast<lvt::IFrameworkConnection*>(tree.win32.get());
        return connection->get_property_snapshot(element.providerHandle);
    }

    static lvt::PropertyMutationResult set(
        NativeTree& tree, lvt::Element& element,
        const lvt::PropertySnapshotResult& properties,
        const std::string& name, const std::string& value) {
        const auto* descriptor = native_descriptor(properties, name);
        if (!descriptor)
            return {};
        auto* connection = element.framework == "comctl"
            ? static_cast<lvt::IFrameworkConnection*>(tree.comctl.get())
            : static_cast<lvt::IFrameworkConnection*>(tree.win32.get());
        return connection->set_property(
            element.providerHandle, descriptor->descriptorId, value);
    }

    static lvt::PropertyMutationResult clear(
        NativeTree& tree, lvt::Element& element,
        const lvt::PropertySnapshotResult& properties,
        const std::string& name) {
        const auto* descriptor = native_descriptor(properties, name);
        if (!descriptor)
            return {};
        auto* connection = element.framework == "comctl"
            ? static_cast<lvt::IFrameworkConnection*>(tree.comctl.get())
            : static_cast<lvt::IFrameworkConnection*>(tree.win32.get());
        return connection->clear_property(
            element.providerHandle, descriptor->descriptorId);
    }

    static std::string state_summary() {
        DWORD_PTR protocolVersion = 0;
        EXPECT_NE(
            SendMessageTimeoutW(
                s_hwnd, native_fixture::kRefreshSummaryMessage, 0, 0,
                SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
                &protocolVersion),
            0);
        EXPECT_EQ(
            protocolVersion,
            static_cast<DWORD_PTR>(
                native_fixture::kSummaryProtocolVersion));
        auto tree = dump_tree();
        const json* summary = find_element_by_hwnd(
            tree["root"], control(native_fixture::kStateSummaryId));
        return summary ? summary->value("text", "") : std::string();
    }

    static wil::unique_handle s_process;
    static wil::unique_handle s_thread;
    static DWORD s_pid;
    static HWND s_hwnd;
};

wil::unique_handle NativeControlsFixture::s_process;
wil::unique_handle NativeControlsFixture::s_thread;
DWORD NativeControlsFixture::s_pid = 0;
HWND NativeControlsFixture::s_hwnd = nullptr;

static LRESULT send_native_fixture_message(
    HWND hwnd, UINT message, WPARAM wParam = 0, LPARAM lParam = 0) {
    DWORD_PTR result = 0;
    const LRESULT sent = SendMessageTimeoutW(
        hwnd, message, wParam, lParam,
        SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 5000, &result);
    EXPECT_NE(sent, 0);
    return sent ? static_cast<LRESULT>(result) : 0;
}

static bool wait_for_native_snapshot(
    lvt::IFrameworkConnection& connection,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        const auto events = connection.poll_events();
        if (std::any_of(
                events.begin(), events.end(),
                [](const lvt::ConnectionEvent& event) {
                    return event.mutation ==
                           lvt::ConnectionEvent::Mutation::snapshotRequired;
                })) {
            return true;
        }
        Sleep(20);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

static void drain_native_events(lvt::IFrameworkConnection& connection) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        Sleep(20);
        (void)connection.poll_events();
    }
}

static void publish_native_event_snapshot(
    const std::shared_ptr<lvt::NativePropertyConnection>& connection,
    HWND hwnd, DWORD pid) {
    auto lookup =
        [&connection](const std::string& provider)
            -> lvt::IFrameworkConnection* {
        return provider == "win32" ? connection.get() : nullptr;
    };
    auto tree = lvt::build_tree(
        hwnd, pid, lvt::detect_frameworks(hwnd, pid),
        -1, {}, false, lookup);
    connection->publish_targets(tree);
}

static bool wait_for_atomic_at_least(
    const std::atomic<uint32_t>& value, uint32_t expected,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load(std::memory_order_acquire) >= expected)
            return true;
        Sleep(20);
    }
    return value.load(std::memory_order_acquire) >= expected;
}

#ifdef LVT_ENABLE_UIA
static bool wait_for_uia_snapshot(
    lvt::UiaConnection& connection,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (!connection.refresh_events())
            return false;
        const auto events = connection.poll_events();
        if (std::any_of(
                events.begin(), events.end(),
                [](const lvt::ConnectionEvent& event) {
                    return event.mutation ==
                           lvt::ConnectionEvent::Mutation::snapshotRequired;
                })) {
            return true;
        }
        Sleep(20);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

static void drain_uia_events(lvt::UiaConnection& connection) {
    for (int attempt = 0; attempt < 10; ++attempt) {
        Sleep(20);
        ASSERT_TRUE(connection.refresh_events());
        (void)connection.poll_events();
    }
}

TEST_F(NativeControlsFixture, UiaHandlersRegisterOnceAndRemoveOnce) {
    lvt::uia_eventing_detail::reset_subscription_counters();
    {
        auto connection = lvt::UiaConnection::connect(s_hwnd, s_pid);
        ASSERT_NE(connection, nullptr);

        lvt::Element tree;
        const auto read_tree = [&] {
            for (int attempt = 0; attempt < 3; ++attempt) {
                if (connection->get_tree(tree, false))
                    return true;
                Sleep(static_cast<DWORD>(120 * (attempt + 1)));
            }
            return false;
        };
        ASSERT_TRUE(read_tree());
        ASSERT_TRUE(read_tree());

        const auto active =
            lvt::uia_eventing_detail::subscription_counters();
        EXPECT_EQ(active.connections, 1u);
        EXPECT_EQ(active.structureRegistrations, 1u);
        EXPECT_EQ(active.propertyRegistrations, 1u);
        EXPECT_EQ(
            active.automationRegistrations,
            lvt::uia_eventing_detail::
                subscribed_automation_event_ids().size());
        EXPECT_EQ(active.removeAllCalls, 0u);
    }

    const auto removed =
        lvt::uia_eventing_detail::subscription_counters();
    EXPECT_EQ(removed.removeAllCalls, 1u);
}

TEST_F(NativeControlsFixture, UiaEventsCoverValueStateAndStructureChanges) {
    send_native_fixture_message(
        s_hwnd, native_fixture::kDestroyEventChildMessage);
    auto connection = lvt::UiaConnection::connect(s_hwnd, s_pid);
    ASSERT_NE(connection, nullptr);
    drain_uia_events(*connection);

    HWND edit = control(native_fixture::kEditId);
    wchar_t originalText[256]{};
    GetWindowTextW(edit, originalText, static_cast<int>(_countof(originalText)));
    ASSERT_TRUE(SetWindowTextW(edit, L"UIA event value change"));
    NotifyWinEvent(
        EVENT_OBJECT_VALUECHANGE, edit, OBJID_CLIENT, CHILDID_SELF);
    EXPECT_TRUE(wait_for_uia_snapshot(*connection))
        << "a Value/Name-affecting update did not request a UIA snapshot";
    ASSERT_TRUE(SetWindowTextW(edit, originalText));

    drain_uia_events(*connection);
    HWND button = control(native_fixture::kButtonId);
    EnableWindow(button, FALSE);
    ASSERT_FALSE(IsWindowEnabled(button));
    NotifyWinEvent(
        EVENT_OBJECT_STATECHANGE, button, OBJID_CLIENT, CHILDID_SELF);
    EXPECT_TRUE(wait_for_uia_snapshot(*connection))
        << "an IsEnabled/state update did not request a UIA snapshot";
    EnableWindow(button, TRUE);
    ASSERT_TRUE(IsWindowEnabled(button));

    drain_uia_events(*connection);
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd, native_fixture::kCreateEventChildMessage),
        0);
    EXPECT_TRUE(wait_for_uia_snapshot(*connection))
        << "a UIA child addition did not request a snapshot";

    drain_uia_events(*connection);
    send_native_fixture_message(
        s_hwnd, native_fixture::kReorderEventChildrenMessage);
    EXPECT_TRUE(wait_for_uia_snapshot(*connection))
        << "a UIA child reorder did not request a snapshot";

    drain_uia_events(*connection);
    send_native_fixture_message(
        s_hwnd, native_fixture::kDestroyEventChildMessage);
    (void)connection->poll_events();
}

TEST_F(NativeControlsFixture, UiaSubscriptionsIsolateTwoWindowsInOneProcess) {
    const HWND other = reinterpret_cast<HWND>(
        send_native_fixture_message(
            s_hwnd, native_fixture::kGetOutOfTreeHwndMessage));
    ASSERT_TRUE(IsWindow(other));
    send_native_fixture_message(
        s_hwnd, native_fixture::kDestroyEventChildMessage, 0);
    send_native_fixture_message(
        s_hwnd, native_fixture::kDestroyEventChildMessage, 1);

    auto rootConnection = lvt::UiaConnection::connect(s_hwnd, s_pid);
    auto otherConnection = lvt::UiaConnection::connect(other, s_pid);
    ASSERT_NE(rootConnection, nullptr);
    ASSERT_NE(otherConnection, nullptr);
    drain_uia_events(*rootConnection);
    drain_uia_events(*otherConnection);

    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd, native_fixture::kCreateEventChildMessage, 0),
        0);
    EXPECT_TRUE(wait_for_uia_snapshot(*rootConnection));
    Sleep(250);
    EXPECT_TRUE(otherConnection->poll_events().empty())
        << "a root-subtree UIA event leaked to another top-level HWND";

    drain_uia_events(*rootConnection);
    drain_uia_events(*otherConnection);
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd, native_fixture::kCreateEventChildMessage, 1),
        0);
    EXPECT_TRUE(wait_for_uia_snapshot(*otherConnection));
    Sleep(250);
    EXPECT_TRUE(rootConnection->poll_events().empty())
        << "the second HWND's UIA event leaked back to the first root";

    send_native_fixture_message(
        s_hwnd, native_fixture::kDestroyEventChildMessage, 0);
    send_native_fixture_message(
        s_hwnd, native_fixture::kDestroyEventChildMessage, 1);
}

TEST_F(NativeControlsFixture, UiaSubscriptionsIsolateDifferentProcesses) {
    ScopedNativeFixtureProcess second;
    ASSERT_TRUE(second.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));

    auto firstConnection = lvt::UiaConnection::connect(s_hwnd, s_pid);
    auto secondConnection =
        lvt::UiaConnection::connect(second.hwnd, second.pid);
    ASSERT_NE(firstConnection, nullptr);
    ASSERT_NE(secondConnection, nullptr);
    drain_uia_events(*firstConnection);
    drain_uia_events(*secondConnection);

    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd, native_fixture::kCreateEventChildMessage),
        0);
    EXPECT_TRUE(wait_for_uia_snapshot(*firstConnection));
    Sleep(250);
    EXPECT_TRUE(secondConnection->poll_events().empty())
        << "a UIA event leaked across target processes";

    send_native_fixture_message(
        s_hwnd, native_fixture::kDestroyEventChildMessage);
}

TEST(UiaEventLifecycle, TargetExitRemovesHandlersAndStopsDelivery) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto before =
        lvt::uia_eventing_detail::subscription_counters();
    auto connection =
        lvt::UiaConnection::connect(fixture.hwnd, fixture.pid);
    ASSERT_NE(connection, nullptr);

    fixture.stop();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (connection->is_alive() &&
           std::chrono::steady_clock::now() < deadline) {
        Sleep(20);
    }
    EXPECT_FALSE(connection->is_alive());
    EXPECT_TRUE(connection->poll_events().empty());

    auto after = lvt::uia_eventing_detail::subscription_counters();
    while (after.removeAllCalls < before.removeAllCalls + 1 &&
           std::chrono::steady_clock::now() < deadline) {
        Sleep(20);
        after = lvt::uia_eventing_detail::subscription_counters();
    }
    EXPECT_GE(after.removeAllCalls, before.removeAllCalls + 1);
}

TEST(UiaEventLifecycle, RejectsReplacementProcessWithStaleExpectedPid) {
    DWORD stalePid = 0;
    {
        ScopedNativeFixtureProcess original;
        ASSERT_TRUE(original.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
        stalePid = original.pid;
    }

    ScopedNativeFixtureProcess replacement;
    ASSERT_TRUE(replacement.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    ASSERT_NE(replacement.pid, stalePid);

    EXPECT_EQ(
        lvt::UiaConnection::connect(replacement.hwnd, stalePid),
        nullptr)
        << "a valid replacement window must not be accepted under the stale "
           "registry PID, even if Windows reuses the numeric HWND";
    EXPECT_NE(
        lvt::UiaConnection::connect(
            replacement.hwnd, replacement.pid),
        nullptr);
}

TEST(
    UiaTargetIdentity,
    UnavailableExactRecycleRestoresDifferentValidReplacement) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const HWND original =
        GetDlgItem(fixture.hwnd, native_fixture::kEditId);
    ASSERT_TRUE(IsWindow(original));
    const auto identity = lvt::capture_uia_target_identity(
        original, fixture.pid,
        lvt::process_creation_identity(fixture.pid));
    ASSERT_TRUE(identity.has_value());

    lvt::test_support::ExactHwndRecycleOptions options;
    options.testMode =
        lvt::test_support::ExactHwndRecycleTestMode::
            forceUnavailable;
    options.rememberUnavailable = false;
    const auto recycled =
        lvt::test_support::recycle_event_child_exact(
            fixture.hwnd, original,
            native_fixture::kEditId, options);
    ASSERT_EQ(
        recycled.outcome,
        lvt::test_support::ExactHwndRecycleOutcome::
            forcedUnavailable)
        << recycled.reason;
    ASSERT_TRUE(IsWindow(recycled.replacement));
    EXPECT_NE(recycled.replacement, original);
    EXPECT_EQ(
        GetDlgItem(fixture.hwnd, native_fixture::kEditId),
        recycled.replacement);

    EXPECT_TRUE(FAILED(lvt::validate_uia_target_identity(*identity)));
    lvt::UiaProvider provider;
    bool ownershipLost = false;
    EXPECT_FALSE(provider.build(
        *identity, lvt::UiaOptions{}, nullptr,
        &ownershipLost));
    EXPECT_TRUE(ownershipLost);
}

TEST(
    UiaTargetIdentity,
    ExactRecycleGlobalHeldWindowCapIsEnforced) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const HWND original =
        GetDlgItem(fixture.hwnd, native_fixture::kEditId);
    ASSERT_TRUE(IsWindow(original));
    const bool suppressedBefore =
        lvt::test_support::exact_hwnd_recycle_search_suppressed();

    lvt::test_support::ExactHwndRecycleOptions options;
    options.testMode =
        lvt::test_support::ExactHwndRecycleTestMode::
            forceGlobalCap;
    options.rememberUnavailable = false;
    const auto recycled =
        lvt::test_support::recycle_event_child_exact(
            fixture.hwnd, original,
            native_fixture::kEditId, options);
    ASSERT_EQ(
        recycled.outcome,
        lvt::test_support::ExactHwndRecycleOutcome::
            searchUnavailable)
        << recycled.reason;
    EXPECT_EQ(
        recycled.peakHeldWindows,
        native_fixture::kExactHwndRecycleMaximumHeldWindows);
    EXPECT_EQ(recycled.remainingHeldWindows, 0u);
    ASSERT_TRUE(IsWindow(recycled.replacement));
    EXPECT_NE(recycled.replacement, original);
    EXPECT_EQ(
        lvt::test_support::exact_hwnd_recycle_search_suppressed(),
        suppressedBefore);
    EXPECT_TRUE(lvt::capture_uia_target_identity(
        recycled.replacement, fixture.pid,
        lvt::process_creation_identity(fixture.pid))
                    .has_value());
}

TEST(
    UiaTargetIdentity,
    ExactRecycleHardFailureIsNotSkippableOrCached) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const HWND original =
        GetDlgItem(fixture.hwnd, native_fixture::kEditId);
    ASSERT_TRUE(IsWindow(original));
    const bool suppressedBefore =
        lvt::test_support::exact_hwnd_recycle_search_suppressed();

    lvt::test_support::ExactHwndRecycleOptions options;
    options.testMode =
        lvt::test_support::ExactHwndRecycleTestMode::
            forceHardFailure;
    options.rememberUnavailable = true;
    const auto recycled =
        lvt::test_support::recycle_event_child_exact(
            fixture.hwnd, original,
            native_fixture::kEditId, options);
    ASSERT_EQ(
        recycled.outcome,
        lvt::test_support::ExactHwndRecycleOutcome::hardFailure)
        << recycled.reason;
    EXPECT_EQ(
        recycled.failureStage,
        native_fixture::ExactHwndRecycleFailureStage::
            createSearchCandidate);
    EXPECT_EQ(recycled.win32Error, ERROR_NOT_ENOUGH_MEMORY);
    EXPECT_EQ(recycled.peakHeldWindows, 0u);
    EXPECT_EQ(recycled.remainingHeldWindows, 0u);
    ASSERT_TRUE(IsWindow(recycled.replacement));
    EXPECT_NE(recycled.replacement, original);
    EXPECT_EQ(
        lvt::test_support::exact_hwnd_recycle_search_suppressed(),
        suppressedBefore);
    EXPECT_TRUE(lvt::capture_uia_target_identity(
        recycled.replacement, fixture.pid,
        lvt::process_creation_identity(fixture.pid))
                    .has_value());
}

TEST(
    UiaTargetIdentity,
    GuidWindowLifetimeSentinelRejectsRemovedProperty) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const HWND edit =
        GetDlgItem(fixture.hwnd, native_fixture::kEditId);
    ASSERT_TRUE(IsWindow(edit));
    const auto identity = lvt::capture_uia_target_identity(
        edit, fixture.pid,
        lvt::process_creation_identity(fixture.pid));
    ASSERT_TRUE(identity.has_value());

    struct PropertySearch {
        std::wstring name;
        HANDLE value = nullptr;
    } property;
    EnumPropsExW(
        edit,
        [](HWND, PWSTR name, HANDLE value, ULONG_PTR parameter) -> int {
            auto* property =
                reinterpret_cast<PropertySearch*>(parameter);
            constexpr wchar_t prefix[] = L"lvt.uia.window.";
            if (!IS_INTRESOURCE(name) &&
                wcsncmp(name, prefix, _countof(prefix) - 1) == 0) {
                property->name = name;
                property->value = value;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&property));
    ASSERT_FALSE(property.name.empty())
        << "the GUID window-lifetime property was not installed";
    ASSERT_NE(property.value, nullptr);
    EXPECT_EQ(
        RemovePropW(edit, property.name.c_str()),
        property.value);

    EXPECT_TRUE(FAILED(lvt::validate_uia_target_identity(*identity)));
    lvt::UiaProvider provider;
    bool ownershipLost = false;
    EXPECT_FALSE(provider.build(
        *identity, lvt::UiaOptions{}, nullptr,
        &ownershipLost));
    EXPECT_TRUE(ownershipLost);
}

TEST_F(
    NativeControlsFixture,
    ExactRecycledHwndIsRejectedEvenWhenRuntimeIdMatches) {
    if (lvt::test_support::exact_hwnd_recycle_search_suppressed()) {
        GTEST_SKIP()
            << lvt::test_support::exact_hwnd_recycle_unavailable_reason();
    }
    send_native_fixture_message(
        s_hwnd, native_fixture::kDestroyEventChildMessage);
    const HWND original = reinterpret_cast<HWND>(
        send_native_fixture_message(
            s_hwnd, native_fixture::kCreateEventChildMessage));
    ASSERT_TRUE(IsWindow(original));
    const auto identity = lvt::capture_uia_target_identity(
        original, s_pid, lvt::process_creation_identity(s_pid));
    ASSERT_TRUE(identity.has_value());
    auto connection = lvt::UiaConnection::connect(*identity);
    ASSERT_NE(connection, nullptr);

    const auto recycled =
        lvt::test_support::recycle_event_child_exact(
            s_hwnd, original);
    if (lvt::test_support::exact_hwnd_recycle_is_unavailable(
            recycled)) {
        GTEST_SKIP() << recycled.reason;
    }
    ASSERT_EQ(
        recycled.outcome,
        lvt::test_support::ExactHwndRecycleOutcome::achieved)
        << recycled.reason;
    ASSERT_EQ(recycled.replacement, original);
    ASSERT_TRUE(IsWindow(recycled.replacement));

    const auto replacement =
        lvt::capture_uia_target_identity(
            recycled.replacement, s_pid,
            identity->processCreationIdentity);
    ASSERT_TRUE(replacement.has_value());
    EXPECT_EQ(
        replacement->rootRuntimeId,
        identity->rootRuntimeId)
        << "this regression requires the HWND-derived RuntimeId collision";
    EXPECT_FALSE(connection->is_alive())
        << "the original per-window generation sentinel survived HWND reuse";

    lvt::UiaProvider provider;
    bool ownershipLost = false;
    EXPECT_FALSE(provider.build(
        *identity, lvt::UiaOptions{}, nullptr,
        &ownershipLost));
    EXPECT_TRUE(ownershipLost);

    lvt::ActionRequest request;
    request.kind = lvt::ActionKind::windowMinimize;
    request.elementRef =
        "uia:" + lvt::format_runtime_id(
            identity->rootRuntimeId);
    const auto action = lvt::perform_action(
        *identity, lvt::UiaOptions{}, request);
    EXPECT_FALSE(action.ok);
    EXPECT_EQ(action.errorCode, "ownershipLost");
    EXPECT_NE(
        action.message.find("ownershipLost"),
        std::string::npos);

    for (const auto kind : {
             lvt::ActionKind::typeText,
             lvt::ActionKind::pressKey}) {
        lvt::ActionRequest elementless;
        elementless.kind = kind;
        elementless.text =
            kind == lvt::ActionKind::typeText
                ? "must-not-type"
                : "A";
        const auto rejected = lvt::perform_action(
            *identity, lvt::UiaOptions{},
            elementless);
        EXPECT_FALSE(rejected.ok);
        EXPECT_EQ(rejected.errorCode, "ownershipLost");
    }
}

TEST_F(
    NativeControlsFixture,
    SyntheticInputRevalidatesAfterForegroundBeforeSendInput) {
    if (lvt::test_support::exact_hwnd_recycle_search_suppressed()) {
        GTEST_SKIP()
            << lvt::test_support::exact_hwnd_recycle_unavailable_reason();
    }
    send_native_fixture_message(
        s_hwnd, native_fixture::kDestroyEventChildMessage);
    const HWND original = reinterpret_cast<HWND>(
        send_native_fixture_message(
            s_hwnd, native_fixture::kCreateEventChildMessage));
    ASSERT_TRUE(IsWindow(original));

    const std::string base =
        "Local\\LvtUiaForeground_" +
        std::to_string(GetCurrentProcessId()) + "_" +
        std::to_string(GetTickCount64());
    wil::unique_event entered(CreateEventA(
        nullptr, TRUE, FALSE,
        (base + "-entered").c_str()));
    wil::unique_event release(CreateEventA(
        nullptr, TRUE, FALSE,
        (base + "-release").c_str()));
    ASSERT_TRUE(entered);
    ASSERT_TRUE(release);
    const fs::path statsPath =
        fs::path(get_lvt_path()).parent_path() /
        ("uia-send-input-" +
         std::to_string(GetCurrentProcessId()) + ".log");
    std::error_code ec;
    fs::remove(statsPath, ec);
    ScopedEnvironmentVariable foregroundGate(
        "LVT_TEST_UIA_AFTER_FOREGROUND_GATE", base);
    ScopedEnvironmentVariable sendInputStats(
        "LVT_TEST_UIA_SEND_INPUT_STATS",
        statsPath.string());
    ScopedEnvironmentVariable foregroundSuccess(
        "LVT_TEST_UIA_FOREGROUND_SUCCESS", "1");
    ScopedEnvironmentVariable suppressInput(
        "LVT_TEST_UIA_SUPPRESS_SEND_INPUT", "1");

    char hwndArgument[64]{};
    snprintf(
        hwndArgument, sizeof(hwndArgument),
        "--hwnd 0x%llX",
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(original)));
    auto pending = std::async(
        std::launch::async,
        [command = make_cmd(
             get_lvt_path(),
             std::string(hwndArgument) +
                 " type should-not-reach-replacement")] {
            return run_command(command);
        });
    lvt::test_support::ScopedEventSignal releaseOnExit(
        release.get());

    ASSERT_EQ(
        WaitForSingleObject(entered.get(), 15000),
        WAIT_OBJECT_0)
        << "the synthetic action did not reach foreground activation";
    const auto recycled =
        lvt::test_support::recycle_event_child_exact(
            s_hwnd, original);
    releaseOnExit.signal();

    const auto output = pending.get();
    const auto result =
        json::parse(output, nullptr, false);
    ASSERT_FALSE(result.is_discarded()) << output;
    if (lvt::test_support::exact_hwnd_recycle_is_unavailable(
            recycled)) {
        fs::remove(statsPath, ec);
        GTEST_SKIP() << recycled.reason;
    }
    ASSERT_EQ(
        recycled.outcome,
        lvt::test_support::ExactHwndRecycleOutcome::achieved)
        << recycled.reason;
    ASSERT_EQ(recycled.replacement, original);
    EXPECT_FALSE(result.value("ok", true));
    EXPECT_EQ(result.value("code", ""), "ownershipLost")
        << result.dump(2);
    EXPECT_TRUE(
        !fs::exists(statsPath) ||
        fs::file_size(statsPath, ec) == 0)
        << "SendInput was dispatched after target replacement";
    fs::remove(statsPath, ec);
}

TEST(UiaTargetIdentity, ProcessCreationMismatchRejectsPidReuse) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto captured = lvt::capture_uia_target_identity(
        fixture.hwnd, fixture.pid,
        lvt::process_creation_identity(fixture.pid));
    ASSERT_TRUE(captured.has_value());

    auto recycledPid = *captured;
    ++recycledPid.processCreationIdentity;
    lvt::UiaProvider provider;
    bool ownershipLost = false;
    EXPECT_FALSE(provider.build(
        recycledPid, lvt::UiaOptions{}, nullptr,
        &ownershipLost));
    EXPECT_TRUE(ownershipLost);
    EXPECT_EQ(
        lvt::UiaConnection::connect(recycledPid),
        nullptr);
}

TEST(UiaTargetIdentity, WinEventFallbackPreservesTargetsWhenSetPropIsUnavailable) {
    ScopedEnvironmentVariable forcePropertyFailure(
        "LVT_TEST_UIA_FORCE_WINDOW_PROP_FAILURE", "1");
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto identity = lvt::capture_uia_target_identity(
        fixture.hwnd, fixture.pid,
        lvt::process_creation_identity(fixture.pid));
    ASSERT_TRUE(identity.has_value())
        << "SetProp failure must fall back to the destroy subscription";
    auto connection = lvt::UiaConnection::connect(*identity);
    ASSERT_NE(connection, nullptr);

    fixture.stop();
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(5);
    while (connection->is_alive() &&
           std::chrono::steady_clock::now() < deadline) {
        Sleep(20);
    }
    EXPECT_FALSE(connection->is_alive());
}

TEST(UiaTargetIdentity, WaitClosureIsStructuredButWaitGoneStillSucceeds) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto identity = lvt::capture_uia_target_identity(
        fixture.hwnd, fixture.pid,
        lvt::process_creation_identity(fixture.pid));
    ASSERT_TRUE(identity.has_value());
    const auto reference =
        "uia:" + lvt::format_runtime_id(
            identity->rootRuntimeId);
    fixture.stop();

    lvt::ActionRequest waitFor;
    waitFor.kind = lvt::ActionKind::waitFor;
    waitFor.elementRef = reference;
    const auto closed = lvt::perform_action(
        *identity, lvt::UiaOptions{}, waitFor);
    EXPECT_FALSE(closed.ok);
    EXPECT_EQ(closed.errorCode, "ownershipLost");

    lvt::ActionRequest waitGone = waitFor;
    waitGone.kind = lvt::ActionKind::waitGone;
    const auto gone = lvt::perform_action(
        *identity, lvt::UiaOptions{}, waitGone);
    EXPECT_TRUE(gone.ok) << gone.message;
    EXPECT_EQ(gone.method, "wait-gone");
}

TEST(UiaTargetIdentity, CliWindowCloseReturnsSuccessAfterTargetDisappears) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    SECURITY_ATTRIBUTES security{
        sizeof(security), nullptr, TRUE};
    wil::unique_handle readEnd;
    wil::unique_handle writeEnd;
    ASSERT_TRUE(CreatePipe(
        readEnd.put(), writeEnd.put(), &security, 0));
    ASSERT_TRUE(SetHandleInformation(
        readEnd.get(), HANDLE_FLAG_INHERIT, 0));

    char hwndArgument[64]{};
    snprintf(
        hwndArgument, sizeof(hwndArgument),
        "--hwnd 0x%llX close",
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(fixture.hwnd)));
    std::string command =
        make_cmd(get_lvt_path(), hwndArgument);
    STARTUPINFOA startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd.get();
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION info{};
    ASSERT_TRUE(CreateProcessA(
        nullptr, command.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
        &startup, &info));
    wil::unique_handle process(info.hProcess);
    wil::unique_handle thread(info.hThread);
    writeEnd.reset();

    ASSERT_EQ(
        WaitForSingleObject(process.get(), 20000),
        WAIT_OBJECT_0);
    DWORD exitCode = 1;
    ASSERT_TRUE(GetExitCodeProcess(
        process.get(), &exitCode));
    EXPECT_EQ(exitCode, 0u);

    std::string output;
    char buffer[1024];
    DWORD read = 0;
    while (ReadFile(
               readEnd.get(), buffer,
               static_cast<DWORD>(sizeof(buffer)),
               &read, nullptr) &&
           read != 0) {
        output.append(buffer, read);
    }
    const auto result =
        json::parse(output, nullptr, false);
    ASSERT_FALSE(result.is_discarded()) << output;
    EXPECT_TRUE(result.value("ok", false)) << result.dump(2);
    EXPECT_EQ(
        result.value("method", ""),
        "WindowPattern.Close");
    EXPECT_FALSE(IsWindow(fixture.hwnd));
}

TEST(UiaTargetIdentity, WindowCloseReplacementRaceIsOwnershipLost) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto identity = lvt::capture_uia_target_identity(
        fixture.hwnd, fixture.pid,
        lvt::process_creation_identity(fixture.pid));
    ASSERT_TRUE(identity.has_value());
    const std::string base =
        "Local\\LvtClosePatternBoundary_" +
        std::to_string(GetCurrentProcessId()) + "_" +
        std::to_string(GetTickCount64());
    wil::unique_event entered(CreateEventA(
        nullptr, TRUE, FALSE,
        (base + "-entered").c_str()));
    wil::unique_event release(CreateEventA(
        nullptr, TRUE, FALSE,
        (base + "-release").c_str()));
    ASSERT_TRUE(entered);
    ASSERT_TRUE(release);
    ScopedEnvironmentVariable gate(
        "LVT_TEST_UIA_BEFORE_PATTERN_OPERATION_GATE", base);

    lvt::ActionRequest request;
    request.kind = lvt::ActionKind::windowClose;
    request.elementRef =
        "uia:" + lvt::format_runtime_id(
            identity->rootRuntimeId);
    auto pending = std::async(
        std::launch::async, [identity, request] {
            return lvt::perform_action(
                *identity, lvt::UiaOptions{}, request);
        });
    ASSERT_EQ(
        WaitForSingleObject(entered.get(), 10000),
        WAIT_OBJECT_0);
    fixture.stop();
    ScopedNativeFixtureProcess replacement;
    ASSERT_TRUE(replacement.start(
        NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    SetEvent(release.get());

    const auto result = pending.get();
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, "ownershipLost")
        << result.message;
}

TEST(UiaTargetIdentity, OneShotFallbackKeepsOriginalIdentity) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto identity = lvt::capture_uia_target_identity(
        fixture.hwnd, fixture.pid,
        lvt::process_creation_identity(fixture.pid));
    ASSERT_TRUE(identity.has_value());
    auto connection = lvt::UiaConnection::connect(*identity);
    ASSERT_NE(connection, nullptr);

    connection->fail_next_tree_for_testing();
    lvt::Element connectedTree;
    EXPECT_FALSE(connection->get_tree(
        connectedTree, false));

    lvt::UiaProvider provider;
    bool ownershipLost = false;
    const auto fallback = provider.build(
        *identity, lvt::UiaOptions{}, nullptr,
        &ownershipLost);
    EXPECT_TRUE(fallback.has_value())
        << "an unchanged target must retain ordinary transient fallback";
    EXPECT_FALSE(ownershipLost);
}

TEST(UiaTargetIdentity, ReplacementAfterElementFromHandleIsRejected) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    const auto identity = lvt::capture_uia_target_identity(
        fixture.hwnd, fixture.pid,
        lvt::process_creation_identity(fixture.pid));
    ASSERT_TRUE(identity.has_value());

    const std::string base =
        "Local\\LvtUiaAfterElement_" +
        std::to_string(GetCurrentProcessId()) + "_" +
        std::to_string(GetTickCount64());
    wil::unique_event entered(CreateEventA(
        nullptr, TRUE, FALSE,
        (base + "-entered").c_str()));
    wil::unique_event release(CreateEventA(
        nullptr, TRUE, FALSE,
        (base + "-release").c_str()));
    ASSERT_TRUE(entered);
    ASSERT_TRUE(release);
    ScopedEnvironmentVariable gate(
        "LVT_TEST_UIA_ONE_SHOT_AFTER_ELEMENT_GATE", base);

    auto pending = std::async(
        std::launch::async, [identity] {
            lvt::UiaProvider provider;
            bool ownershipLost = false;
            auto tree = provider.build(
                *identity, lvt::UiaOptions{}, nullptr,
                &ownershipLost);
            return std::make_pair(
                tree.has_value(), ownershipLost);
        });
    ASSERT_EQ(
        WaitForSingleObject(entered.get(), 5000),
        WAIT_OBJECT_0)
        << "the one-shot fallback did not reach ElementFromHandle";

    fixture.stop();
    SetEvent(release.get());
    const auto result = pending.get();
    EXPECT_FALSE(result.first);
    EXPECT_TRUE(result.second)
        << "replacement between precheck and ElementFromHandle validation "
           "must fail ownership";
}

TEST_F(
    NativeControlsFixture,
    CacheRefreshRevalidatesAfterBuildUpdatedCacheBoundary) {
    if (lvt::test_support::exact_hwnd_recycle_search_suppressed()) {
        GTEST_SKIP()
            << lvt::test_support::exact_hwnd_recycle_unavailable_reason();
    }
    send_native_fixture_message(
        s_hwnd, native_fixture::kDestroyEventChildMessage);
    const HWND original = reinterpret_cast<HWND>(
        send_native_fixture_message(
            s_hwnd, native_fixture::kCreateEventChildMessage));
    ASSERT_TRUE(IsWindow(original));
    const auto identity = lvt::capture_uia_target_identity(
        original, s_pid, lvt::process_creation_identity(s_pid));
    ASSERT_TRUE(identity.has_value());

    const std::string base =
        "Local\\LvtUiaCacheBoundary_" +
        std::to_string(GetCurrentProcessId()) + "_" +
        std::to_string(GetTickCount64());
    wil::unique_event entered(CreateEventA(
        nullptr, TRUE, FALSE,
        (base + "-entered").c_str()));
    wil::unique_event release(CreateEventA(
        nullptr, TRUE, FALSE,
        (base + "-release").c_str()));
    ASSERT_TRUE(entered);
    ASSERT_TRUE(release);
    ScopedEnvironmentVariable gate(
        "LVT_TEST_UIA_BEFORE_BUILD_CACHE_GATE", base);

    auto pending = std::async(
        std::launch::async, [identity] {
            lvt::UiaProvider provider;
            bool ownershipLost = false;
            auto tree = provider.build(
                *identity, lvt::UiaOptions{}, nullptr,
                &ownershipLost);
            return std::make_pair(
                tree.has_value(), ownershipLost);
        });
    lvt::test_support::ScopedEventSignal releaseOnExit(
        release.get());
    ASSERT_EQ(
        WaitForSingleObject(entered.get(), 10000),
        WAIT_OBJECT_0);
    const auto recycled =
        lvt::test_support::recycle_event_child_exact(
            s_hwnd, original);
    releaseOnExit.signal();

    const auto result = pending.get();
    if (lvt::test_support::exact_hwnd_recycle_is_unavailable(
            recycled)) {
        GTEST_SKIP() << recycled.reason;
    }
    ASSERT_EQ(
        recycled.outcome,
        lvt::test_support::ExactHwndRecycleOutcome::achieved)
        << recycled.reason;
    ASSERT_EQ(recycled.replacement, original);
    EXPECT_FALSE(result.first);
    EXPECT_TRUE(result.second)
        << "cache success/failure must be followed by identity validation";
}

TEST_F(
    NativeControlsFixture,
    PatternMutationRevalidatesAfterProviderBoundary) {
    if (lvt::test_support::exact_hwnd_recycle_search_suppressed()) {
        GTEST_SKIP()
            << lvt::test_support::exact_hwnd_recycle_unavailable_reason();
    }
    const HWND original = control(native_fixture::kEditId);
    ASSERT_TRUE(IsWindow(original));
    const auto identity = lvt::capture_uia_target_identity(
        original, s_pid, lvt::process_creation_identity(s_pid));
    ASSERT_TRUE(identity.has_value());

    const std::string base =
        "Local\\LvtUiaPatternBoundary_" +
        std::to_string(GetCurrentProcessId()) + "_" +
        std::to_string(GetTickCount64());
    wil::unique_event entered(CreateEventA(
        nullptr, TRUE, FALSE,
        (base + "-entered").c_str()));
    wil::unique_event release(CreateEventA(
        nullptr, TRUE, FALSE,
        (base + "-release").c_str()));
    ASSERT_TRUE(entered);
    ASSERT_TRUE(release);
    ScopedEnvironmentVariable gate(
        "LVT_TEST_UIA_BEFORE_PATTERN_OPERATION_GATE", base);

    lvt::ActionRequest request;
    request.kind = lvt::ActionKind::setValue;
    request.elementRef =
        "uia:" + lvt::format_runtime_id(
            identity->rootRuntimeId);
    request.text = "must-not-reach-replacement";
    auto pending = std::async(
        std::launch::async, [identity, request] {
            return lvt::perform_action(
                *identity, lvt::UiaOptions{}, request);
        });
    lvt::test_support::ScopedEventSignal releaseOnExit(
        release.get());
    ASSERT_EQ(
        WaitForSingleObject(entered.get(), 10000),
        WAIT_OBJECT_0);
    const auto recycled =
        lvt::test_support::recycle_event_child_exact(
            s_hwnd, original, native_fixture::kEditId);
    releaseOnExit.signal();

    const auto result = pending.get();
    if (lvt::test_support::exact_hwnd_recycle_is_unavailable(
            recycled)) {
        GTEST_SKIP() << recycled.reason;
    }
    ASSERT_EQ(
        recycled.outcome,
        lvt::test_support::ExactHwndRecycleOutcome::achieved)
        << recycled.reason;
    ASSERT_EQ(recycled.replacement, original);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.errorCode, "ownershipLost")
        << result.message;
    wchar_t text[128]{};
    GetWindowTextW(
        original, text, static_cast<int>(_countof(text)));
    EXPECT_NE(
        std::wstring(text),
        L"must-not-reach-replacement");
}
#endif

TEST_F(NativeControlsFixture, NativeWinEventsSignalCreateDestroyReorderAndBurst) {
    send_native_fixture_message(
        s_hwnd, native_fixture::kDestroyEventChildMessage);

    auto connection = lvt::NativePropertyConnection::connect(
        s_hwnd, s_pid, "win32");
    ASSERT_NE(connection, nullptr);
    ASSERT_TRUE(connection->event_hook_active_for_testing());
    auto diagnostics = connection->event_diagnostics_for_testing();
    ASSERT_NE(diagnostics, nullptr);

    publish_native_event_snapshot(connection, s_hwnd, s_pid);
    drain_native_events(*connection);

    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd, native_fixture::kCreateEventChildMessage),
        0);
    EXPECT_TRUE(wait_for_native_snapshot(*connection))
        << "EVENT_OBJECT_CREATE did not request a native snapshot";

    publish_native_event_snapshot(connection, s_hwnd, s_pid);
    drain_native_events(*connection);
    send_native_fixture_message(
        s_hwnd, native_fixture::kReorderEventChildrenMessage);
    EXPECT_TRUE(wait_for_native_snapshot(*connection))
        << "EVENT_OBJECT_REORDER did not request a native snapshot";

    drain_native_events(*connection);
    send_native_fixture_message(
        s_hwnd, native_fixture::kReparentGenericOutOfTreeMessage);
    EXPECT_TRUE(wait_for_native_snapshot(*connection))
        << "reparenting a published HWND out of the root was missed";
    drain_native_events(*connection);
    send_native_fixture_message(
        s_hwnd, native_fixture::kRestoreGenericParentMessage);
    EXPECT_TRUE(wait_for_native_snapshot(*connection))
        << "reparenting an HWND into the root was missed";

    drain_native_events(*connection);
    const uint32_t overflowBefore =
        diagnostics->overflows.load(std::memory_order_acquire);
    const uint32_t callbacksBefore =
        diagnostics->callbacks.load(std::memory_order_acquire);
    const uint32_t burstCount = static_cast<uint32_t>(
        lvt::native_eventing_detail::kNativeWinEventQueueCapacity * 4);
    send_native_fixture_message(
        s_hwnd, native_fixture::kBurstEventMessage,
        static_cast<WPARAM>(burstCount));
    ASSERT_TRUE(wait_for_atomic_at_least(
        diagnostics->callbacks, callbacksBefore + burstCount))
        << "the out-of-context hook did not drain the generated burst";
    ASSERT_TRUE(wait_for_atomic_at_least(
        diagnostics->overflows, overflowBefore + 1))
        << "the fixed native callback queue did not report overflow";
    const auto burst = connection->poll_events();
    ASSERT_EQ(burst.size(), 1u);
    EXPECT_EQ(
        burst[0].mutation,
        lvt::ConnectionEvent::Mutation::snapshotRequired);
    EXPECT_TRUE(connection->poll_events().empty())
        << "a burst must coalesce to one snapshotRequired notification";

    send_native_fixture_message(
        s_hwnd, native_fixture::kDestroyEventChildMessage);
    EXPECT_TRUE(wait_for_native_snapshot(*connection))
        << "EVENT_OBJECT_DESTROY did not request a native snapshot";
}

TEST_F(NativeControlsFixture, DumpKeyScopesWatchAndWatchKeyQueriesOneShot) {
    auto dump = dump_tree();
    auto* listView = find_element_by_hwnd(
        dump["root"], control(native_fixture::kListViewId));
    ASSERT_NE(listView, nullptr);
    auto* beta = find_element_by_type_property(
        *listView, "ListViewItem", "index", "1");
    ASSERT_NE(beta, nullptr);
    const auto dumpKey = beta->value("key", "");
    ASSERT_FALSE(dumpKey.empty());

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    wil::unique_handle readEnd;
    wil::unique_handle writeEnd;
    ASSERT_TRUE(CreatePipe(
        readEnd.put(), writeEnd.put(), &security, 0));
    ASSERT_TRUE(SetHandleInformation(
        readEnd.get(), HANDLE_FLAG_INHERIT, 0));

    STARTUPINFOA startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd.get();
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION processInfo{};
    std::string command = make_cmd(
        get_lvt_path(),
        get_hwnd_arg() + " watch --interval 50 --element \"" +
            dumpKey + "\"");
    ASSERT_TRUE(CreateProcessA(
        nullptr, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &processInfo));
    wil::unique_handle process(processInfo.hProcess);
    wil::unique_handle thread(processInfo.hThread);
    writeEnd.reset();

    std::string output;
    const auto deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline &&
           output.find('\n') == std::string::npos) {
        DWORD available = 0;
        if (!PeekNamedPipe(
                readEnd.get(), nullptr, 0, nullptr,
                &available, nullptr)) {
            break;
        }
        if (available == 0) {
            if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0)
                break;
            Sleep(20);
            continue;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!ReadFile(
                readEnd.get(), chunk.data(), available,
                &read, nullptr) ||
            read == 0) {
            break;
        }
        output.append(chunk, 0, read);
    }

    TerminateProcess(process.get(), 0);
    WaitForSingleObject(process.get(), 5000);

    const auto newline = output.find('\n');
    ASSERT_NE(newline, std::string::npos)
        << "scoped watch emitted no complete event";
    const auto event = json::parse(
        output.substr(0, newline), nullptr, false);
    ASSERT_FALSE(event.is_discarded()) << output;
    EXPECT_EQ(event.value("event", ""), "added");
    const auto watchKey = event.value("key", "");
    EXPECT_EQ(watchKey, dumpKey)
        << "the one-shot key must resolve in persistent watch";
    ASSERT_TRUE(event.contains("element"));
    EXPECT_EQ(event["element"].value("text", ""), "Beta row");

    const auto queried = run_command(make_cmd(
        get_lvt_path(),
        get_hwnd_arg() + " query " +
            cmd_escape_arg(watchKey) + " index"));
    EXPECT_EQ(trim_crlf(queried), "1")
        << "a key emitted by watch must resolve in one-shot query";
}

TEST_F(
    NativeControlsFixture,
    VisualScopedWatchInitialResolutionAcceptsPositionalRef) {
    auto dump = dump_tree();
    auto* listView = find_element_by_hwnd(
        dump["root"], control(native_fixture::kListViewId));
    ASSERT_NE(listView, nullptr);
    auto* beta = find_element_by_type_property(
        *listView, "ListViewItem", "index", "1");
    ASSERT_NE(beta, nullptr);
    const auto id = beta->value("id", "");
    const auto key = beta->value("key", "");
    ASSERT_FALSE(id.empty());
    ASSERT_FALSE(key.empty());

    auto result = capture_first_watch_event(make_cmd(
        get_lvt_path(),
        get_hwnd_arg() + " watch --interval 50 --element \"" +
            id + "\""));
    ASSERT_TRUE(result.started);
    ASSERT_FALSE(result.event.is_null()) << result.output;
    EXPECT_EQ(result.event.value("event", ""), "added");
    EXPECT_EQ(result.event.value("key", ""), key);
    ASSERT_TRUE(result.event.contains("element"));
    EXPECT_EQ(
        result.event["element"].value("text", ""),
        "Beta row");
    EXPECT_EQ(result.output.find("element '"), std::string::npos)
        << result.output;
}

TEST_F(
    NativeControlsFixture,
    UiaScopedWatchInitialResolutionAcceptsRuntimeId) {
    const auto output = run_command(make_cmd(
        get_lvt_path(), get_hwnd_arg() + " dump --uia"));
    auto tree = json::parse(output, nullptr, false);
    ASSERT_FALSE(tree.is_discarded()) << output;
    ASSERT_TRUE(tree.contains("root"));

    std::vector<const json*> elements;
    collect_json_elements(tree["root"], elements);
    const json* target = nullptr;
    for (const auto* element : elements) {
        const auto properties =
            element->value("properties", json::object());
        if (element->value("text", "") == "Fixture action" &&
            !properties.value("RuntimeId", "").empty()) {
            target = element;
            break;
        }
    }
    ASSERT_NE(target, nullptr) << output;
    const auto runtimeId =
        (*target)["properties"].value("RuntimeId", "");
    ASSERT_FALSE(runtimeId.empty());

    auto result = capture_first_watch_event(make_cmd(
        get_lvt_path(),
        get_hwnd_arg() +
            " watch --uia --interval 50 --element \"uia:" +
            runtimeId + "\""));
    ASSERT_TRUE(result.started);
    ASSERT_FALSE(result.event.is_null()) << result.output;
    EXPECT_EQ(result.event.value("event", ""), "added");
    ASSERT_TRUE(result.event.contains("element"));
    EXPECT_EQ(
        result.event["element"].value("text", ""),
        "Fixture action");
    EXPECT_EQ(
        result.event["element"]
            .value("properties", json::object())
            .value("RuntimeId", ""),
        runtimeId);
    EXPECT_EQ(result.output.find("element '"), std::string::npos)
        << result.output;
}

TEST_F(
    NativeControlsFixture,
    ScopedWatchFollowsIdentityReplacementButNotSlotOccupants) {
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd, native_fixture::kRestoreDefaultListMessage),
        0);
    auto restoreList = wil::scope_exit([&] {
        send_native_fixture_message(
            s_hwnd, native_fixture::kRestoreDefaultListMessage);
    });

    const auto findListItem =
        [&](const json& tree, const std::string& text,
            const std::string& index) -> const json* {
        auto* listView = find_element_by_hwnd(
            tree["root"], control(native_fixture::kListViewId));
        if (!listView || !listView->contains("children"))
            return nullptr;
        for (const auto& child : (*listView)["children"]) {
            if (child.value("type", "") == "ListViewItem" &&
                child.value("text", "") == text &&
                child.value("properties", json::object())
                        .value("index", "") == index) {
                return &child;
            }
        }
        return nullptr;
    };

    auto initial = dump_tree();
    const auto* alpha =
        findListItem(initial, "Alpha row", "0");
    ASSERT_NE(alpha, nullptr);
    const auto alphaKey = alpha->value("key", "");
    ASSERT_FALSE(alphaKey.empty());

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    wil::unique_handle readEnd;
    wil::unique_handle writeEnd;
    ASSERT_TRUE(CreatePipe(
        readEnd.put(), writeEnd.put(), &security, 0));
    ASSERT_TRUE(SetHandleInformation(
        readEnd.get(), HANDLE_FLAG_INHERIT, 0));

    STARTUPINFOA startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd.get();
    startup.hStdError = writeEnd.get();
    PROCESS_INFORMATION processInfo{};
    std::string command = make_cmd(
        get_lvt_path(),
        get_hwnd_arg() + " watch --interval 50 --element \"" +
            alphaKey + "\"");
    ASSERT_TRUE(CreateProcessA(
        nullptr, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &processInfo));
    wil::unique_handle process(processInfo.hProcess);
    wil::unique_handle thread(processInfo.hThread);
    writeEnd.reset();

    std::string output;
    const auto readAvailable = [&] {
        DWORD available = 0;
        if (!PeekNamedPipe(
                readEnd.get(), nullptr, 0, nullptr,
                &available, nullptr) ||
            available == 0) {
            return;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (ReadFile(
                readEnd.get(), chunk.data(), available,
                &read, nullptr) &&
            read != 0) {
            output.append(chunk, 0, read);
        }
    };
    const auto eventSeen =
        [&](size_t offset, const std::string& type,
            const std::string& key) {
        size_t cursor = offset;
        if (cursor > 0 && cursor <= output.size() &&
            output[cursor - 1] != '\n') {
            const auto next = output.find('\n', cursor);
            if (next == std::string::npos)
                return false;
            cursor = next + 1;
        }
        while (cursor < output.size()) {
            const auto newline = output.find('\n', cursor);
            if (newline == std::string::npos)
                break;
            const auto event = json::parse(
                output.substr(cursor, newline - cursor),
                nullptr, false);
            if (!event.is_discarded() &&
                event.value("event", "") == type &&
                (key.empty() ||
                 event.value("key", "") == key)) {
                return true;
            }
            cursor = newline + 1;
        }
        return false;
    };
    const auto waitFor =
        [&](const std::function<bool()>& ready,
            DWORD timeoutMs) {
        const auto deadline = GetTickCount64() + timeoutMs;
        while (GetTickCount64() < deadline) {
            readAvailable();
            if (ready())
                return true;
            if (WaitForSingleObject(
                    process.get(), 0) == WAIT_OBJECT_0) {
                break;
            }
            Sleep(20);
        }
        readAvailable();
        return ready();
    };

    ASSERT_TRUE(waitFor(
        [&] { return eventSeen(0, "added", alphaKey); },
        10000)) << output;

    size_t phaseStart = output.size();
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kDeleteFirstListItemMessage),
        0);
    ASSERT_TRUE(waitFor(
        [&] {
            return eventSeen(
                phaseStart, "removed", alphaKey);
        },
        10000)) << output;
    Sleep(250);
    readAvailable();
    EXPECT_FALSE(eventSeen(
        phaseStart, "added", std::string()));

    phaseStart = output.size();
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kInsertAlphaFirstListItemMessage),
        0);
    ASSERT_TRUE(waitFor(
        [&] {
            return eventSeen(
                phaseStart, "added", alphaKey);
        },
        10000)) << output;

    phaseStart = output.size();
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kMutateListViewIdentityMessage),
        0);
    auto changed = dump_tree();
    const auto* replacement = findListItem(
        changed, "External replacement", "0");
    ASSERT_NE(replacement, nullptr);
    const auto replacementKey =
        replacement->value("key", "");
    ASSERT_NE(replacementKey, alphaKey);
    ASSERT_TRUE(waitFor(
        [&] {
            return eventSeen(
                       phaseStart, "removed", alphaKey) &&
                   eventSeen(
                       phaseStart, "added", replacementKey);
        },
        10000)) << output;

    phaseStart = output.size();
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kDuplicateSecondListIdentityMessage),
        0);
    auto ambiguous = dump_tree();
    const auto* ambiguousRoot = findListItem(
        ambiguous, "External replacement", "0");
    ASSERT_NE(ambiguousRoot, nullptr);
    const auto ambiguousKey =
        ambiguousRoot->value("key", "");
    ASSERT_NE(ambiguousKey, replacementKey);
    ASSERT_TRUE(waitFor(
        [&] {
            return eventSeen(
                       phaseStart, "removed", replacementKey) &&
                   eventSeen(
                       phaseStart, "added", ambiguousKey);
        },
        10000)) << output;

    TerminateProcess(process.get(), 0);
    WaitForSingleObject(process.get(), 5000);
    readAvailable();
    EXPECT_EQ(output.find("element '"), std::string::npos)
        << output;
}

TEST_F(
    NativeControlsFixture,
    AmbiguousPositionalScopeDoesNotAdoptShiftedSibling) {
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd, native_fixture::kRestoreDefaultListMessage),
        0);
    auto restore = wil::scope_exit([&] {
        send_native_fixture_message(
            s_hwnd, native_fixture::kRestoreDefaultListMessage);
    });
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kMutateListViewIdentityMessage),
        0);
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kDuplicateSecondListIdentityMessage),
        0);

    auto tree = dump_tree();
    auto* listView = find_element_by_hwnd(
        tree["root"], control(native_fixture::kListViewId));
    ASSERT_NE(listView, nullptr);
    auto* item0 = find_element_by_type_property(
        *listView, "ListViewItem", "index", "0");
    ASSERT_NE(item0, nullptr);
    const auto ambiguousKey = item0->value("key", "");
    ASSERT_FALSE(ambiguousKey.empty());
    EXPECT_EQ(
        ambiguousKey.find("identity:"),
        std::string::npos);

    LiveWatchCapture watch;
    ASSERT_TRUE(watch.start(make_cmd(
        get_lvt_path(),
        get_hwnd_arg() + " watch --interval 50 --element \"" +
            ambiguousKey + "\"")));
    ASSERT_TRUE(watch.wait_for_event(
        0, "added", ambiguousKey, 10000))
        << watch.output();

    const auto removedAt = watch.mark();
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kDeleteFirstListItemMessage),
        0);
    ASSERT_TRUE(watch.wait_for_event(
        removedAt, "removed", ambiguousKey, 10000))
        << watch.output();
    Sleep(250);
    watch.drain();
    EXPECT_FALSE(watch.event_seen(
        removedAt, "added"))
        << "the identical sibling that shifted into slot 0 was adopted:\n"
        << watch.output();
    EXPECT_TRUE(watch.running());

    const auto reinsertedAt = watch.mark();
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kInsertExternalFirstListItemMessage),
        0);
    Sleep(350);
    watch.drain();
    EXPECT_FALSE(watch.event_seen(
        reinsertedAt, "added"))
        << "an ambiguous scope without a continuity token reappeared:\n"
        << watch.output();
    EXPECT_TRUE(watch.running());

    const auto restoredAt = watch.mark();
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kRestoreSecondListIdentityMessage),
        0);
    Sleep(350);
    watch.drain();
    EXPECT_FALSE(watch.event_seen(
        restoredAt, "added"))
        << "restoring uniqueness silently resuscitated an absent positional scope:\n"
        << watch.output();
    EXPECT_EQ(
        watch.output().find("element '"),
        std::string::npos)
        << watch.output();
}

TEST_F(
    NativeControlsFixture,
    ToolbarSeparatorScopeDoesNotAdoptShiftedSeparator) {
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kPopulateAdjacentToolbarSeparatorsMessage),
        0);
    auto restore = wil::scope_exit([&] {
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kRestoreDefaultToolbarMessage);
    });

    auto tree = dump_tree();
    auto* toolbar = find_element_by_hwnd(
        tree["root"], control(native_fixture::kToolbarId));
    ASSERT_NE(toolbar, nullptr);
    std::vector<const json*> separators;
    for (const auto& child : (*toolbar)["children"]) {
        if (child.value("type", "") == "ToolbarSeparator")
            separators.push_back(&child);
    }
    ASSERT_EQ(separators.size(), 2u);
    const auto firstKey = separators[0]->value("key", "");
    ASSERT_FALSE(firstKey.empty());
    EXPECT_EQ(firstKey.find("identity:"), std::string::npos);
    EXPECT_EQ(separators[1]->value("key", "").find("identity:"),
              std::string::npos);

    auto persistent = native_tree();
    auto* persistentToolbar = find_native_element_by_hwnd(
        persistent.root, control(native_fixture::kToolbarId));
    ASSERT_NE(persistentToolbar, nullptr);
    int persistentSeparators = 0;
    for (const auto& child : persistentToolbar->children) {
        if (child.type != "ToolbarSeparator")
            continue;
        ++persistentSeparators;
        EXPECT_EQ(child.providerHandle, 0u)
            << "separators must not expose a mutation target";
        EXPECT_TRUE(child.durableIdentity.empty())
            << "positional separator indices are not durable identity";
    }
    EXPECT_EQ(persistentSeparators, 2);

    LiveWatchCapture watch;
    ASSERT_TRUE(watch.start(make_cmd(
        get_lvt_path(),
        get_hwnd_arg() + " watch --interval 50 --element \"" +
            firstKey + "\"")));
    ASSERT_TRUE(watch.wait_for_event(
        0, "added", firstKey, 10000))
        << watch.output();

    const auto removedAt = watch.mark();
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kDeleteFirstToolbarSeparatorMessage),
        0);
    ASSERT_TRUE(watch.wait_for_event(
        removedAt, "removed", firstKey, 10000))
        << watch.output();
    Sleep(250);
    watch.drain();
    EXPECT_FALSE(watch.event_seen(removedAt, "added"))
        << "the second separator shifted into the deleted separator's scope:\n"
        << watch.output();
    EXPECT_TRUE(watch.running());

    auto fresh = dump_tree();
    toolbar = find_element_by_hwnd(
        fresh["root"], control(native_fixture::kToolbarId));
    ASSERT_NE(toolbar, nullptr);
    const json* remaining = nullptr;
    for (const auto& child : (*toolbar)["children"]) {
        if (child.value("type", "") == "ToolbarSeparator") {
            remaining = &child;
            break;
        }
    }
    ASSERT_NE(remaining, nullptr);
    const auto freshKey = remaining->value("key", "");
    EXPECT_NE(freshKey, firstKey);
    EXPECT_EQ(freshKey.find("identity:"), std::string::npos);
    EXPECT_EQ(
        trim_crlf(run_command(make_cmd(
            get_lvt_path(),
            get_hwnd_arg() + " query " +
                cmd_escape_arg(freshKey) + " index"))),
        "0");
}

TEST_F(NativeControlsFixture, NativeWatchDoesNotDuplicateStructuralDiffs) {
    send_native_fixture_message(
        s_hwnd, native_fixture::kDestroyEventChildMessage);

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    wil::unique_handle readEnd;
    wil::unique_handle writeEnd;
    ASSERT_TRUE(CreatePipe(readEnd.put(), writeEnd.put(), &security, 0));
    ASSERT_TRUE(SetHandleInformation(
        readEnd.get(), HANDLE_FLAG_INHERIT, 0));

    STARTUPINFOA startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd.get();
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION processInfo{};
    std::string command = make_cmd(
        get_lvt_path(), get_hwnd_arg() + " watch --interval 50");
    ASSERT_TRUE(CreateProcessA(
        nullptr, command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &processInfo));
    wil::unique_handle process(processInfo.hProcess);
    wil::unique_handle thread(processInfo.hThread);
    writeEnd.reset();

    std::string output;
    const auto read_until =
        [&](const std::function<bool(const std::string&)>& ready,
            DWORD timeoutMs) {
        const auto deadline = GetTickCount64() + timeoutMs;
        while (GetTickCount64() < deadline && !ready(output)) {
            DWORD available = 0;
            if (!PeekNamedPipe(
                    readEnd.get(), nullptr, 0, nullptr,
                    &available, nullptr)) {
                break;
            }
            if (available == 0) {
                Sleep(20);
                continue;
            }
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!ReadFile(
                    readEnd.get(), chunk.data(), available,
                    &read, nullptr) ||
                read == 0) {
                break;
            }
            output.append(chunk, 0, read);
        }
        return ready(output);
    };
    const auto has_complete = [](const std::string& value,
                                 const std::string& needle) {
        const auto at = value.find(needle);
        return at != std::string::npos &&
               value.find('\n', at) != std::string::npos;
    };

    ASSERT_TRUE(read_until(
        [&](const std::string& value) {
            return has_complete(value, "protocol=2");
        },
        10000))
        << "watch did not finish its initial native snapshot";

    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd, native_fixture::kCreateEventChildMessage),
        0);
    ASSERT_TRUE(read_until(
        [&](const std::string& value) {
            return has_complete(value, "Event child");
        },
        5000))
        << "watch did not report the created HWND";

    send_native_fixture_message(
        s_hwnd, native_fixture::kDestroyEventChildMessage);
    const bool sawRemoved = read_until(
        [&](const std::string& value) {
            std::istringstream lines(value);
            std::string line;
            while (std::getline(lines, line)) {
                auto event = json::parse(line, nullptr, false);
                if (!event.is_discarded() &&
                    event.value("event", "") == "removed" &&
                    event.contains("element") &&
                    event["element"].value("text", "") ==
                        "Event child") {
                    return true;
                }
            }
            return false;
        },
        5000);

    Sleep(250);
    (void)read_until(
        [](const std::string&) { return false; }, 100);
    TerminateProcess(process.get(), 0);
    WaitForSingleObject(process.get(), 5000);
    ASSERT_TRUE(sawRemoved)
        << "watch did not report the destroyed HWND:\n"
        << output;

    size_t added = 0;
    size_t removed = 0;
    std::istringstream lines(output);
    std::string line;
    while (std::getline(lines, line)) {
        auto event = json::parse(line, nullptr, false);
        if (event.is_discarded() || !event.contains("element") ||
            event["element"].value("text", "") != "Event child") {
            continue;
        }
        added += event.value("event", "") == "added" ? 1u : 0u;
        removed += event.value("event", "") == "removed" ? 1u : 0u;
    }
    EXPECT_EQ(added, 1u)
        << "WinEvent hints were emitted beside the authoritative add diff";
    EXPECT_EQ(removed, 1u)
        << "WinEvent hints were emitted beside the authoritative remove diff";
}

TEST(NativeWinEventing, LogicalChildAndTabDeletionKeepRootConnectionsAlive) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));

    auto rootConnection = lvt::NativePropertyConnection::connect(
        fixture.hwnd, fixture.pid, "win32");
    ASSERT_NE(rootConnection, nullptr);
    ASSERT_TRUE(rootConnection->event_hook_active_for_testing());
    auto rootDiagnostics =
        rootConnection->event_diagnostics_for_testing();
    ASSERT_NE(rootDiagnostics, nullptr);
    publish_native_event_snapshot(
        rootConnection, fixture.hwnd, fixture.pid);
    drain_native_events(*rootConnection);

    ASSERT_NE(
        send_native_fixture_message(
            fixture.hwnd,
            native_fixture::kNotifyRootClientChildDestroyMessage,
            0, 7),
        0);
    EXPECT_TRUE(wait_for_native_snapshot(*rootConnection))
        << "a root HWND logical-child destroy did not request a snapshot";
    EXPECT_TRUE(IsWindow(fixture.hwnd));
    EXPECT_TRUE(rootConnection->is_alive())
        << "a client logical-child destroy killed the root connection";
    EXPECT_TRUE(rootConnection->event_hook_active_for_testing());
    EXPECT_EQ(
        rootDiagnostics->unhooks.load(std::memory_order_acquire), 0u);

    const HWND tab = GetDlgItem(
        fixture.hwnd, native_fixture::kTabControlId);
    ASSERT_NE(tab, nullptr);
    auto tabConnection = lvt::NativePropertyConnection::connect(
        tab, fixture.pid, "win32");
    ASSERT_NE(tabConnection, nullptr);
    ASSERT_TRUE(tabConnection->event_hook_active_for_testing());
    auto tabDiagnostics =
        tabConnection->event_diagnostics_for_testing();
    ASSERT_NE(tabDiagnostics, nullptr);
    publish_native_event_snapshot(tabConnection, tab, fixture.pid);
    drain_native_events(*tabConnection);

    const LRESULT tabCountBefore =
        SendMessageW(tab, TCM_GETITEMCOUNT, 0, 0);
    ASSERT_GT(tabCountBefore, 0);
    ASSERT_NE(
        send_native_fixture_message(
            fixture.hwnd, native_fixture::kDeleteFirstTabMessage),
        0);
    EXPECT_EQ(
        SendMessageW(tab, TCM_GETITEMCOUNT, 0, 0),
        tabCountBefore - 1);
    EXPECT_TRUE(wait_for_native_snapshot(*tabConnection))
        << "deleting a tab item did not request a native snapshot";
    EXPECT_TRUE(IsWindow(tab));
    EXPECT_TRUE(tabConnection->is_alive())
        << "deleting a tab item killed the tab HWND connection";
    EXPECT_TRUE(tabConnection->event_hook_active_for_testing());
    EXPECT_EQ(
        tabDiagnostics->unhooks.load(std::memory_order_acquire), 0u);

    send_native_fixture_message(
        fixture.hwnd, native_fixture::kRestoreTabsMessage);
}

TEST(NativeWinEventing, ActualRootWindowDestructionMarksDeadAndUnhooks) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));

    const HWND sibling = reinterpret_cast<HWND>(
        send_native_fixture_message(
            fixture.hwnd,
            native_fixture::kGetOutOfTreeHwndMessage));
    ASSERT_NE(sibling, nullptr);
    ASSERT_TRUE(IsWindow(sibling));

    auto connection = lvt::NativePropertyConnection::connect(
        sibling, fixture.pid, "win32");
    ASSERT_NE(connection, nullptr);
    ASSERT_TRUE(connection->event_hook_active_for_testing());
    auto diagnostics = connection->event_diagnostics_for_testing();
    ASSERT_NE(diagnostics, nullptr);
    publish_native_event_snapshot(
        connection, sibling, fixture.pid);
    drain_native_events(*connection);

    ASSERT_NE(
        send_native_fixture_message(
            fixture.hwnd,
            native_fixture::kDestroyOutOfTreeWindowMessage),
        0);
    ASSERT_TRUE(wait_for_atomic_at_least(diagnostics->unhooks, 1))
        << "root HWND destruction did not promptly stop the event hook";
    EXPECT_FALSE(IsWindow(sibling));
    EXPECT_FALSE(connection->is_alive());
    EXPECT_EQ(
        WaitForSingleObject(fixture.process.get(), 0),
        WAIT_TIMEOUT)
        << "destroying one root HWND unexpectedly exited the target process";

    connection.reset();
    EXPECT_EQ(
        diagnostics->unhooks.load(std::memory_order_acquire), 1u)
        << "root destruction and destructor unhooked the same hook twice";
}

TEST(NativeWinEventing, SimultaneousRootsAndProcessesDoNotLeakEvents) {
    ScopedNativeFixtureProcess first;
    ScopedNativeFixtureProcess second;
    ASSERT_TRUE(first.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));
    ASSERT_TRUE(second.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));

    const HWND sibling = reinterpret_cast<HWND>(
        send_native_fixture_message(
            first.hwnd, native_fixture::kGetOutOfTreeHwndMessage));
    ASSERT_NE(sibling, nullptr);

    auto firstRoot = lvt::NativePropertyConnection::connect(
        first.hwnd, first.pid, "win32");
    auto siblingRoot = lvt::NativePropertyConnection::connect(
        sibling, first.pid, "win32");
    auto secondRoot = lvt::NativePropertyConnection::connect(
        second.hwnd, second.pid, "win32");
    ASSERT_NE(firstRoot, nullptr);
    ASSERT_NE(siblingRoot, nullptr);
    ASSERT_NE(secondRoot, nullptr);
    ASSERT_TRUE(firstRoot->event_hook_active_for_testing());
    ASSERT_TRUE(siblingRoot->event_hook_active_for_testing());
    ASSERT_TRUE(secondRoot->event_hook_active_for_testing());

    publish_native_event_snapshot(firstRoot, first.hwnd, first.pid);
    publish_native_event_snapshot(siblingRoot, sibling, first.pid);
    publish_native_event_snapshot(secondRoot, second.hwnd, second.pid);
    drain_native_events(*firstRoot);
    drain_native_events(*siblingRoot);
    drain_native_events(*secondRoot);

    ASSERT_NE(
        send_native_fixture_message(
            first.hwnd, native_fixture::kCreateEventChildMessage),
        0);
    EXPECT_TRUE(wait_for_native_snapshot(*firstRoot));
    Sleep(150);
    EXPECT_TRUE(siblingRoot->poll_events().empty())
        << "another root in the same process received this root's event";
    EXPECT_TRUE(secondRoot->poll_events().empty())
        << "another process received this process's event";

    ASSERT_NE(
        send_native_fixture_message(
            first.hwnd, native_fixture::kCreateEventChildMessage, 1),
        0);
    EXPECT_TRUE(wait_for_native_snapshot(*siblingRoot));
    Sleep(150);
    EXPECT_TRUE(firstRoot->poll_events().empty())
        << "the main root received its sibling top-level window's event";
    EXPECT_TRUE(secondRoot->poll_events().empty());

    send_native_fixture_message(
        first.hwnd, native_fixture::kDestroyEventChildMessage);
    send_native_fixture_message(
        first.hwnd, native_fixture::kDestroyEventChildMessage, 1);
}

TEST(NativeWinEventing, RefcountedTeardownTargetExitAndPostDisconnectAreSafe) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));

    const std::string key =
        "native-event-lifecycle-" +
        std::to_string(GetTickCount64());
    auto created = lvt::NativePropertyConnection::connect(
        fixture.hwnd, fixture.pid, "win32");
    ASSERT_NE(created, nullptr);
    auto first = lvt::ConnectionRegistry::instance().acquire(
        fixture.pid, fixture.hwnd, key,
        [&created](HWND, DWORD)
            -> std::shared_ptr<lvt::IFrameworkConnection> {
            return created;
        });
    auto second = lvt::ConnectionRegistry::instance().acquire(
        fixture.pid, fixture.hwnd, key,
        [](HWND, DWORD) -> std::shared_ptr<lvt::IFrameworkConnection> {
            ADD_FAILURE() << "the second holder must reuse the same generation";
            return nullptr;
        });
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    auto diagnostics = created->event_diagnostics_for_testing();
    ASSERT_NE(diagnostics, nullptr);
    ASSERT_TRUE(created->event_hook_active_for_testing());
    created.reset();

    first.reset();
    Sleep(100);
    EXPECT_EQ(
        diagnostics->unhooks.load(std::memory_order_acquire), 0u)
        << "the first refcount release tore down a shared generation";
    second.reset();
    ASSERT_TRUE(wait_for_atomic_at_least(diagnostics->unhooks, 1));
    EXPECT_EQ(
        diagnostics->unhooks.load(std::memory_order_acquire), 1u);

    const uint32_t callbacksAfterDisconnect =
        diagnostics->callbacks.load(std::memory_order_acquire);
    ASSERT_NE(
        send_native_fixture_message(
            fixture.hwnd, native_fixture::kCreateEventChildMessage),
        0);
    Sleep(200);
    EXPECT_EQ(
        diagnostics->callbacks.load(std::memory_order_acquire),
        callbacksAfterDisconnect)
        << "a callback ran after UnhookWinEvent returned";
    send_native_fixture_message(
        fixture.hwnd, native_fixture::kDestroyEventChildMessage);

    auto exitConnection = lvt::NativePropertyConnection::connect(
        fixture.hwnd, fixture.pid, "win32");
    ASSERT_NE(exitConnection, nullptr);
    auto exitDiagnostics =
        exitConnection->event_diagnostics_for_testing();
    ASSERT_NE(exitDiagnostics, nullptr);
    fixture.stop();
    ASSERT_TRUE(wait_for_atomic_at_least(exitDiagnostics->unhooks, 1));
    EXPECT_FALSE(exitConnection->is_alive());
    exitConnection.reset();
    EXPECT_EQ(
        exitDiagnostics->unhooks.load(std::memory_order_acquire), 1u)
        << "target exit and destructor unhooked the same hook twice";
}

TEST_F(NativeControlsFixture, ProviderDumpExposesAllControlsAndBaselineValues) {
    auto frameworkOutput =
        run_command(make_cmd(get_lvt_path(), get_hwnd_arg() + " frameworks"));
    EXPECT_NE(frameworkOutput.find("win32"), std::string::npos);
    EXPECT_NE(frameworkOutput.find("comctl"), std::string::npos);

    auto tree = dump_tree();
    ASSERT_FALSE(tree.is_discarded());
    ASSERT_TRUE(tree.contains("root"));
    EXPECT_EQ(tree["root"].value("className", ""), "LvtNativePropertyFixtureWindow");
    EXPECT_EQ(tree["root"].value("text", ""), "LVT Native Property Fixture");

    struct ExpectedControl {
        int id;
        const char* className;
        const char* type;
        const char* text;
        const char* framework;
    };
    const ExpectedControl expectedControls[] = {
        {native_fixture::kCheckboxId, "Button", "Button", "Tri-state check", "win32"},
        {native_fixture::kRadioId, "Button", "Button", "Primary radio", "win32"},
        {native_fixture::kButtonId, "Button", "Button", "Fixture action", "win32"},
        {native_fixture::kEditId, "Edit", "Edit", "Editable seed", "win32"},
        {native_fixture::kReadOnlyEditId, "Edit", "Edit", "Read-only seed", "win32"},
        {native_fixture::kComboBoxId, "ComboBox", "ComboBox", "", "win32"},
        {native_fixture::kListBoxId, "ListBox", "ListBox", "", "win32"},
        {native_fixture::kScrollBarId, "ScrollBar", "ScrollBar", "", "win32"},
        {native_fixture::kListViewId, "SysListView32", "ListView", "", "comctl"},
        {native_fixture::kTreeViewId, "SysTreeView32", "TreeView", "", "comctl"},
        {native_fixture::kToolbarId, "ToolbarWindow32", "Toolbar", "", "comctl"},
        {native_fixture::kStatusBarId, "msctls_statusbar32", "StatusBar", "", "comctl"},
        {native_fixture::kTabControlId, "SysTabControl32", "TabControl", "", "comctl"},
        {native_fixture::kGenericTextId, "LvtNativePropertyFixtureText", "Window",
         "Generic child seed", "win32"},
    };

    for (const auto& expected : expectedControls) {
        const HWND hwnd = control(expected.id);
        ASSERT_NE(hwnd, nullptr) << "Control ID " << expected.id << " was not created";
        const json* element = find_element_by_hwnd(tree["root"], hwnd);
        ASSERT_NE(element, nullptr)
            << "Control ID " << expected.id << " was not present in the provider dump";
        EXPECT_EQ(element->value("className", ""), expected.className)
            << "Control ID " << expected.id;
        EXPECT_EQ(element->value("type", ""), expected.type)
            << "Control ID " << expected.id;
        EXPECT_EQ(element->value("text", ""), expected.text)
            << "Control ID " << expected.id;
        EXPECT_EQ(element->value("framework", ""), expected.framework)
            << "Control ID " << expected.id;
    }

    const json* listView =
        find_element_by_hwnd(tree["root"], control(native_fixture::kListViewId));
    ASSERT_NE(listView, nullptr);
    EXPECT_EQ((*listView)["properties"].value("itemCount", ""), "3");
    EXPECT_EQ((*listView)["properties"].value("columnCount", ""), "2");
    EXPECT_EQ((*listView)["properties"].value("viewMode", ""), "details");
    const json* alpha =
        find_element_by_type_property(*listView, "ListViewItem", "index", "0");
    const json* beta =
        find_element_by_type_property(*listView, "ListViewItem", "index", "1");
    const json* gamma =
        find_element_by_type_property(*listView, "ListViewItem", "index", "2");
    ASSERT_NE(alpha, nullptr);
    ASSERT_NE(beta, nullptr);
    ASSERT_NE(gamma, nullptr);
    EXPECT_EQ((*alpha)["properties"].value("index", ""), "0");
    EXPECT_EQ((*beta)["properties"].value("index", ""), "1");
    EXPECT_EQ((*beta)["properties"].value("selected", ""), "true");
    EXPECT_EQ((*gamma)["properties"].value("index", ""), "2");

    const json* treeView =
        find_element_by_hwnd(tree["root"], control(native_fixture::kTreeViewId));
    ASSERT_NE(treeView, nullptr);
    EXPECT_EQ((*treeView)["properties"].value("itemCount", ""), "3");
    ASSERT_FALSE((*treeView)["children"].empty());
    EXPECT_EQ(
        (*treeView)["children"][0]["properties"].value("expanded", ""), "true");
    EXPECT_EQ(
        (*treeView)["children"][0]["properties"].value("hasChildren", ""), "true");

    const json* toolbar =
        find_element_by_hwnd(tree["root"], control(native_fixture::kToolbarId));
    ASSERT_NE(toolbar, nullptr);
    EXPECT_EQ((*toolbar)["properties"].value("buttonCount", ""), "3");
    const json* apply = find_element_by_type_property(
        *toolbar, "ToolbarButton", "commandId", "2001");
    const json* pin = find_element_by_type_property(
        *toolbar, "ToolbarButton", "commandId", "2002");
    const json* disabled = find_element_by_type_property(
        *toolbar, "ToolbarButton", "commandId", "2003");
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(pin, nullptr);
    ASSERT_NE(disabled, nullptr);
    EXPECT_EQ(apply->value("text", ""), "Apply");
    EXPECT_EQ(pin->value("text", ""), "Pinned");
    EXPECT_EQ((*pin)["properties"].value("checked", ""), "true");
    EXPECT_EQ(disabled->value("text", ""), "Disabled");
    EXPECT_EQ((*disabled)["properties"].value("enabled", ""), "false");

    const json* status =
        find_element_by_hwnd(tree["root"], control(native_fixture::kStatusBarId));
    ASSERT_NE(status, nullptr);
    EXPECT_EQ((*status)["properties"].value("partCount", ""), "4");
    ASSERT_EQ((*status)["children"].size(), 4u);
    EXPECT_EQ((*status)["children"][0].value("text", ""), "Ready");
    EXPECT_EQ((*status)["children"][1].value("text", ""), "3 items");
    EXPECT_EQ((*status)["children"][2].value("text", ""), "Idle");
    EXPECT_EQ((*status)["children"][3].value("text", ""), "");
    EXPECT_EQ(
        (*status)["children"][3]["properties"].value("ownerDraw", ""),
        "true");

    const json* tabs =
        find_element_by_hwnd(tree["root"], control(native_fixture::kTabControlId));
    ASSERT_NE(tabs, nullptr);
    EXPECT_EQ((*tabs)["properties"].value("tabCount", ""), "3");
    EXPECT_EQ((*tabs)["properties"].value("selectedIndex", ""), "1");
    ASSERT_EQ((*tabs)["children"].size(), 3u);
    EXPECT_EQ((*tabs)["children"][0]["properties"].value("index", ""), "0");
    EXPECT_EQ((*tabs)["children"][1]["properties"].value("index", ""), "1");
    EXPECT_EQ((*tabs)["children"][1]["properties"].value("selected", ""), "true");
    EXPECT_EQ((*tabs)["children"][2]["properties"].value("index", ""), "2");
}

TEST_F(NativeControlsFixture, NativeDurableKeysMatchOneShotAndPersistentTrees) {
    const auto frameworks = lvt::detect_frameworks(s_hwnd, s_pid);
    auto oneShot = lvt::build_tree(s_hwnd, s_pid, frameworks);
    auto persistent = native_tree();

    EXPECT_EQ(oneShot.key, persistent.root.key);
    EXPECT_EQ(oneShot.key.rfind("win32:0x", 0), 0u);
    EXPECT_EQ(oneShot.providerHandle, 0u);
    EXPECT_NE(persistent.root.providerHandle, 0u);

    for (const int id : {
             native_fixture::kCheckboxId,
             native_fixture::kComboBoxId,
             native_fixture::kListViewId,
             native_fixture::kTreeViewId,
             native_fixture::kToolbarId,
             native_fixture::kStatusBarId,
             native_fixture::kTabControlId,
             native_fixture::kGenericTextId,
         }) {
        const HWND hwnd = control(id);
        auto* oneShotElement =
            find_native_element_by_hwnd(oneShot, hwnd);
        auto* persistentElement =
            find_native_element_by_hwnd(persistent.root, hwnd);
        ASSERT_NE(oneShotElement, nullptr) << "control " << id;
        ASSERT_NE(persistentElement, nullptr) << "control " << id;
        EXPECT_EQ(oneShotElement->key, persistentElement->key)
            << "control " << id;
        EXPECT_EQ(oneShotElement->providerHandle, 0u)
            << "one-shot controls do not acquire mutation handles";
        EXPECT_NE(persistentElement->providerHandle, 0u)
            << "persistent controls retain mutation handles";
        EXPECT_EQ(
            oneShotElement->key.rfind(
                oneShotElement->framework + ":0x", 0),
            0u);
    }

    const auto compareLogical =
        [](const lvt::Element* oneShotElement,
           const lvt::Element* persistentElement,
           const char* label) {
        ASSERT_NE(oneShotElement, nullptr) << label;
        ASSERT_NE(persistentElement, nullptr) << label;
        EXPECT_EQ(oneShotElement->key, persistentElement->key)
            << label;
        EXPECT_EQ(oneShotElement->providerHandle, 0u)
            << label;
        EXPECT_NE(persistentElement->providerHandle, 0u)
            << label;
        EXPECT_EQ(
            persistentElement->key.find("800000000000"),
            std::string::npos)
            << "the public key must not expose a session mutation handle";
    };

    auto* oneShotList = find_native_element_by_hwnd(
        oneShot, control(native_fixture::kListViewId));
    auto* persistentList = find_native_element_by_hwnd(
        persistent.root, control(native_fixture::kListViewId));
    compareLogical(
        find_native_element_by_text(
            *oneShotList, "ListViewItem", "Beta row"),
        find_native_element_by_text(
            *persistentList, "ListViewItem", "Beta row"),
        "list-view item");

    auto* oneShotTree = find_native_element_by_hwnd(
        oneShot, control(native_fixture::kTreeViewId));
    auto* persistentTree = find_native_element_by_hwnd(
        persistent.root, control(native_fixture::kTreeViewId));
    compareLogical(
        find_native_element_by_text(
            *oneShotTree, "TreeViewItem", "Fixture Grandchild"),
        find_native_element_by_text(
            *persistentTree, "TreeViewItem", "Fixture Grandchild"),
        "tree-view item");

    auto* oneShotToolbar = find_native_element_by_hwnd(
        oneShot, control(native_fixture::kToolbarId));
    auto* persistentToolbar = find_native_element_by_hwnd(
        persistent.root, control(native_fixture::kToolbarId));
    compareLogical(
        find_native_element_by_text(
            *oneShotToolbar, "ToolbarButton", "Apply"),
        find_native_element_by_text(
            *persistentToolbar, "ToolbarButton", "Apply"),
        "toolbar button");

    auto* oneShotStatus = find_native_element_by_hwnd(
        oneShot, control(native_fixture::kStatusBarId));
    auto* persistentStatus = find_native_element_by_hwnd(
        persistent.root, control(native_fixture::kStatusBarId));
    ASSERT_GE(oneShotStatus->children.size(), 2u);
    ASSERT_GE(persistentStatus->children.size(), 2u);
    compareLogical(
        &oneShotStatus->children[1],
        &persistentStatus->children[1],
        "status-bar part");
    EXPECT_TRUE(
        oneShotStatus->children[1].durableIdentity.empty());
    EXPECT_TRUE(
        persistentStatus->children[1].durableIdentity.empty())
        << "mutable status-part indices must remain structural";

    auto* oneShotTabs = find_native_element_by_hwnd(
        oneShot, control(native_fixture::kTabControlId));
    auto* persistentTabs = find_native_element_by_hwnd(
        persistent.root, control(native_fixture::kTabControlId));
    compareLogical(
        find_native_element_by_text(
            *oneShotTabs, "Tab", "Details"),
        find_native_element_by_text(
            *persistentTabs, "Tab", "Details"),
        "tab item");
}

TEST_F(NativeControlsFixture, LogicalNativeKeysTrackSafeIdentityRules) {
    auto before = dump_tree();
    auto* beforeToolbar = find_element_by_hwnd(
        before["root"], control(native_fixture::kToolbarId));
    ASSERT_NE(beforeToolbar, nullptr);
    auto* beforeApply = find_element_by_type_property(
        *beforeToolbar, "ToolbarButton", "commandId", "2001");
    ASSERT_NE(beforeApply, nullptr);
    const auto applyKey = beforeApply->value("key", "");
    ASSERT_FALSE(applyKey.empty());

    DWORD_PTR changed = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kMoveToolbarApplyMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            2000, &changed),
        0);
    auto restoreToolbar = wil::scope_exit([&] {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kRestoreToolbarOrderMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            2000, &ignored);
    });
    auto reordered = dump_tree();
    auto* reorderedToolbar = find_element_by_hwnd(
        reordered["root"], control(native_fixture::kToolbarId));
    ASSERT_NE(reorderedToolbar, nullptr);
    auto* reorderedApply = find_element_by_type_property(
        *reorderedToolbar, "ToolbarButton", "commandId", "2001");
    ASSERT_NE(reorderedApply, nullptr);
    EXPECT_EQ(reorderedApply->value("key", ""), applyKey)
        << "a unique documented toolbar command survives reordering";
    restoreToolbar.reset();

    before = dump_tree();
    auto* beforeList = find_element_by_hwnd(
        before["root"], control(native_fixture::kListViewId));
    ASSERT_NE(beforeList, nullptr);
    auto* beforeAlpha = find_element_by_type_property(
        *beforeList, "ListViewItem", "index", "0");
    ASSERT_NE(beforeAlpha, nullptr);
    const auto alphaKey = beforeAlpha->value("key", "");
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kMutateListViewIdentityMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            2000, &changed),
        0);
    auto restoreList = wil::scope_exit([&] {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kRestoreListViewIdentityMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            2000, &ignored);
    });
    auto replaced = dump_tree();
    auto* replacedList = find_element_by_hwnd(
        replaced["root"], control(native_fixture::kListViewId));
    ASSERT_NE(replacedList, nullptr);
    auto* replacement = find_element_by_type_property(
        *replacedList, "ListViewItem", "index", "0");
    ASSERT_NE(replacement, nullptr);
    EXPECT_NE(replacement->value("key", ""), alphaKey)
        << "text is only a safe item identity while that identity remains";
    restoreList.reset();

    before = dump_tree();
    auto* beforeTabs = find_element_by_hwnd(
        before["root"], control(native_fixture::kTabControlId));
    ASSERT_NE(beforeTabs, nullptr);
    const json* overview = nullptr;
    for (const auto& child : (*beforeTabs)["children"]) {
        if (child.value("text", "") == "Overview") {
            overview = &child;
            break;
        }
    }
    ASSERT_NE(overview, nullptr);
    const auto overviewKey = overview->value("key", "");
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kMakeDuplicateTabsMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            2000, &changed),
        0);
    auto restoreTabs = wil::scope_exit([&] {
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kRestoreTabsMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            2000, &ignored);
    });
    auto duplicated = dump_tree();
    auto* duplicatedTabs = find_element_by_hwnd(
        duplicated["root"], control(native_fixture::kTabControlId));
    ASSERT_NE(duplicatedTabs, nullptr);
    int overviewCount = 0;
    for (const auto& child : (*duplicatedTabs)["children"]) {
        if (child.value("text", "") != "Overview")
            continue;
        ++overviewCount;
        EXPECT_NE(child.value("key", ""), overviewKey)
            << "duplicate labels must fall back to positional identity";
    }
    EXPECT_EQ(overviewCount, 2);
}

TEST_F(
    NativeControlsFixture,
    LogicalIdentityScansIncludeHiddenItemsAndRespectSafetyBound) {
    auto restore = wil::scope_exit([&] {
        send_native_fixture_message(
            s_hwnd, native_fixture::kRestoreDefaultListMessage);
        send_native_fixture_message(
            s_hwnd, native_fixture::kRestoreDefaultToolbarMessage);
    });
    const auto findText =
        [](const json& root, const std::string& type,
           const std::string& text) -> const json* {
        std::vector<const json*> elements;
        collect_json_elements(root, elements);
        for (const auto* element : elements) {
            if (element->value("type", "") == type &&
                element->value("text", "") == text) {
                return element;
            }
        }
        return nullptr;
    };
    const auto countType =
        [](const json& root, const std::string& type) {
        std::vector<const json*> elements;
        collect_json_elements(root, elements);
        return std::count_if(
            elements.begin(), elements.end(),
            [&](const json* element) {
                return element->value("type", "") == type;
            });
    };

    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kPopulateLargeListHiddenDuplicateMessage),
        0);
    auto tree = dump_tree();
    auto* listView = find_element_by_hwnd(
        tree["root"], control(native_fixture::kListViewId));
    ASSERT_NE(listView, nullptr);
    EXPECT_EQ((*listView)["properties"].value("itemCount", ""), "52");
    ASSERT_EQ(countType(*listView, "ListViewItem"), 50);
    auto* shared = findText(
        *listView, "ListViewItem", "Shared row");
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->value("key", "").find("identity:"),
              std::string::npos)
        << "an off-screen duplicate must prevent a durable text identity";

    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kDeleteHiddenListDuplicateMessage),
        0);
    tree = dump_tree();
    listView = find_element_by_hwnd(
        tree["root"], control(native_fixture::kListViewId));
    shared = findText(*listView, "ListViewItem", "Shared row");
    ASSERT_NE(shared, nullptr);
    const auto uniqueListKey = shared->value("key", "");
    EXPECT_NE(uniqueListKey.find("identity:"), std::string::npos);

    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd, native_fixture::kReorderLargeListMessage),
        0);
    tree = dump_tree();
    listView = find_element_by_hwnd(
        tree["root"], control(native_fixture::kListViewId));
    shared = findText(*listView, "ListViewItem", "Shared row");
    ASSERT_NE(shared, nullptr);
    EXPECT_EQ(shared->value("key", ""), uniqueListKey);
    EXPECT_EQ(
        (*shared)["properties"].value("index", ""), "1");
    EXPECT_EQ(
        trim_crlf(run_command(make_cmd(
            get_lvt_path(),
            get_hwnd_arg() + " query " +
                cmd_escape_arg(uniqueListKey) + " index"))),
        "1");

    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd, native_fixture::kRestoreDefaultListMessage),
        0);
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kPopulateLargeToolbarHiddenDuplicateMessage),
        0);
    tree = dump_tree();
    auto* toolbar = find_element_by_hwnd(
        tree["root"], control(native_fixture::kToolbarId));
    ASSERT_NE(toolbar, nullptr);
    EXPECT_EQ((*toolbar)["properties"].value("buttonCount", ""), "52");
    ASSERT_EQ(countType(*toolbar, "ToolbarButton"), 50);
    auto* command = find_element_by_type_property(
        *toolbar, "ToolbarButton", "commandId", "3000");
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->value("key", "").find("identity:"),
              std::string::npos)
        << "a duplicate beyond the display limit must be observed";
    EXPECT_EQ(
        (*command)["properties"].value(
            "ambiguousCommandId", ""),
        "true");

    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd,
            native_fixture::kDeleteHiddenToolbarDuplicateMessage),
        0);
    tree = dump_tree();
    toolbar = find_element_by_hwnd(
        tree["root"], control(native_fixture::kToolbarId));
    command = find_element_by_type_property(
        *toolbar, "ToolbarButton", "commandId", "3000");
    ASSERT_NE(command, nullptr);
    EXPECT_NE(command->value("key", "").find("identity:"),
              std::string::npos);

    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd, native_fixture::kRestoreDefaultToolbarMessage),
        0);
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd, native_fixture::kPopulateOversizedListMessage),
        0);
    tree = dump_tree();
    listView = find_element_by_hwnd(
        tree["root"], control(native_fixture::kListViewId));
    auto* oversizedListItem = find_element_by_type_property(
        *listView, "ListViewItem", "index", "0");
    ASSERT_NE(oversizedListItem, nullptr);
    EXPECT_EQ(
        oversizedListItem->value("key", "").find("identity:"),
        std::string::npos);

    auto persistent = native_tree();
    auto* persistentList = find_native_element_by_hwnd(
        persistent.root, control(native_fixture::kListViewId));
    ASSERT_NE(persistentList, nullptr);
    auto* persistentListItem = find_native_element_by_text(
        *persistentList, "ListViewItem", "Oversized row 0");
    ASSERT_NE(persistentListItem, nullptr);
    auto listProperties =
        snapshot(persistent, *persistentListItem);
    ASSERT_TRUE(listProperties.ok) << listProperties.error;
    for (const auto& descriptor : listProperties.schema->descriptors)
        EXPECT_FALSE(descriptor.writable);

    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd, native_fixture::kRestoreDefaultListMessage),
        0);
    ASSERT_NE(
        send_native_fixture_message(
            s_hwnd, native_fixture::kPopulateOversizedToolbarMessage),
        0);
    tree = dump_tree();
    toolbar = find_element_by_hwnd(
        tree["root"], control(native_fixture::kToolbarId));
    command = find_element_by_type_property(
        *toolbar, "ToolbarButton", "commandId", "4000");
    ASSERT_NE(command, nullptr);
    EXPECT_EQ(command->value("key", "").find("identity:"),
              std::string::npos);
    EXPECT_EQ(
        (*command)["properties"].value(
            "commandIdentityUnverified", ""),
        "true");

    persistent = native_tree();
    auto* persistentToolbar = find_native_element_by_hwnd(
        persistent.root, control(native_fixture::kToolbarId));
    ASSERT_NE(persistentToolbar, nullptr);
    ASSERT_FALSE(persistentToolbar->children.empty());
    auto toolbarProperties =
        snapshot(persistent, persistentToolbar->children[0]);
    ASSERT_TRUE(toolbarProperties.ok) << toolbarProperties.error;
    for (const auto& descriptor :
         toolbarProperties.schema->descriptors) {
        EXPECT_FALSE(descriptor.writable);
    }
}

TEST_F(NativeControlsFixture, ReadOnlySummaryReportsCompleteNativeState) {
    const LONG_PTR windowExStyle = GetWindowLongPtrW(s_hwnd, GWL_EXSTYLE);
    EXPECT_NE(windowExStyle & WS_EX_NOACTIVATE, 0);
    const LONG_PTR checkboxStyle =
        GetWindowLongPtrW(control(native_fixture::kCheckboxId), GWL_STYLE);
    EXPECT_EQ(checkboxStyle & BS_TYPEMASK, BS_AUTO3STATE);
    const LONG_PTR radioStyle =
        GetWindowLongPtrW(control(native_fixture::kRadioId), GWL_STYLE);
    EXPECT_EQ(radioStyle & BS_TYPEMASK, BS_AUTORADIOBUTTON);
    const LONG_PTR readOnlyStyle =
        GetWindowLongPtrW(control(native_fixture::kReadOnlyEditId), GWL_STYLE);
    EXPECT_NE(readOnlyStyle & ES_READONLY, 0);
    EXPECT_EQ(
        SendMessageW(control(native_fixture::kCheckboxId), BM_GETCHECK, 0, 0),
        BST_INDETERMINATE);
    EXPECT_EQ(
        SendMessageW(control(native_fixture::kRadioId), BM_GETCHECK, 0, 0),
        BST_CHECKED);
    EXPECT_EQ(
        SendMessageW(control(native_fixture::kComboBoxId), CB_GETCOUNT, 0, 0), 3);
    EXPECT_EQ(
        SendMessageW(control(native_fixture::kComboBoxId), CB_GETCURSEL, 0, 0), 1);
    EXPECT_EQ(
        SendMessageW(control(native_fixture::kListBoxId), LB_GETCOUNT, 0, 0), 4);
    EXPECT_EQ(
        SendMessageW(control(native_fixture::kListBoxId), LB_GETCURSEL, 0, 0), 2);

    DWORD_PTR protocolVersion = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd,
            native_fixture::kRefreshSummaryMessage,
            0,
            0,
            SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
            2000,
            &protocolVersion),
        0);
    ASSERT_EQ(
        protocolVersion,
        static_cast<DWORD_PTR>(native_fixture::kSummaryProtocolVersion));

    auto tree = dump_tree();
    ASSERT_FALSE(tree.is_discarded());
    const json* summary =
        find_element_by_hwnd(tree["root"], control(native_fixture::kStateSummaryId));
    ASSERT_NE(summary, nullptr);
    EXPECT_EQ(summary->value("className", ""), "Static");
    const std::string text = summary->value("text", "");
    EXPECT_NE(text.find("protocol=2"), std::string::npos);
    EXPECT_NE(text.find("checkbox(id=1001)=2"), std::string::npos);
    EXPECT_NE(text.find("radio(id=1002)=1"), std::string::npos);
    EXPECT_NE(text.find("button(id=1003)=Fixture action"), std::string::npos);
    EXPECT_NE(text.find("edit(id=1004)=Editable seed"), std::string::npos);
    EXPECT_NE(text.find("readonly(id=1005)=Read-only seed"), std::string::npos);
    EXPECT_NE(
        text.find("combo(id=1006)=1:Green;items=Red|Green|Blue"),
        std::string::npos);
    EXPECT_NE(
        text.find("listbox(id=1007)=2:Gamma;items=Alpha|Beta|Gamma|Delta"),
        std::string::npos);
    EXPECT_NE(text.find("scrollbar(id=1008)=10,110,10,42"), std::string::npos);
    EXPECT_NE(
        text.find(
            "listview(id=1009)=1:Beta row;focus=-1;items=Alpha row|Beta row|Gamma row"),
        std::string::npos);
    EXPECT_NE(
        text.find(
            "tree(id=1010)=selected:Fixture Child;Fixture Root[expanded]/"
            "Fixture Child[expanded]/"
            "Fixture Grandchild"),
        std::string::npos);
    EXPECT_NE(
        text.find(
            "toolbar(id=1011)=2001:Apply:enabled,2002:Pinned:enabled+checked,"
            "2003:Disabled:disabled"),
        std::string::npos);
    EXPECT_NE(
        text.find("status(id=1012)=Ready|3 items|Idle|<ownerdraw>"),
        std::string::npos);
    EXPECT_NE(
        text.find("tab(id=1013)=1:Details;items=Overview|Details|Advanced"),
        std::string::npos);
    EXPECT_NE(text.find("generic(id=1014)=Generic child seed"), std::string::npos);
}

TEST_F(NativeControlsFixture, Win32TypedPropertiesRoundTripAndValidate) {
    auto native = native_tree();
    ASSERT_NE(native.win32, nullptr);
    ASSERT_NE(native.comctl, nullptr);

    auto* generic = find_native_element_by_hwnd(
        native.root, control(native_fixture::kGenericTextId));
    ASSERT_NE(generic, nullptr);
    EXPECT_EQ(generic->framework, "win32");
    EXPECT_NE(generic->providerHandle, 0u);
    EXPECT_EQ(generic->key.rfind("win32:0x", 0), 0u);

    auto genericProperties = snapshot(native, *generic);
    ASSERT_TRUE(genericProperties.ok) << genericProperties.error;
    const auto* textDescriptor =
        native_descriptor(genericProperties, "Text");
    const auto* enabledDescriptor =
        native_descriptor(genericProperties, "Enabled");
    ASSERT_NE(textDescriptor, nullptr);
    ASSERT_NE(enabledDescriptor, nullptr);
    EXPECT_EQ(textDescriptor->kind, lvt::PropertyEditorKind::string);
    EXPECT_EQ(enabledDescriptor->kind, lvt::PropertyEditorKind::boolean);
    EXPECT_TRUE(textDescriptor->writable);
    EXPECT_TRUE(enabledDescriptor->writable);

    auto changedText = set(
        native, *generic, genericProperties, "Text",
        utf8(u8"Unicode ✓ 東京"));
    ASSERT_TRUE(changedText.ok) << changedText.error;
    EXPECT_EQ(changedText.value, utf8(u8"Unicode ✓ 東京"));
    auto emptyText =
        set(native, *generic, genericProperties, "Text", "");
    ASSERT_TRUE(emptyText.ok) << emptyText.error;
    auto restoredText = set(
        native, *generic, genericProperties, "Text",
        "Generic child seed");
    ASSERT_TRUE(restoredText.ok) << restoredText.error;

    auto disabled =
        set(native, *generic, genericProperties, "Enabled", "false");
    ASSERT_TRUE(disabled.ok) << disabled.error;
    EXPECT_FALSE(IsWindowEnabled(
        control(native_fixture::kGenericTextId)));
    auto enabled =
        set(native, *generic, genericProperties, "Enabled", "true");
    ASSERT_TRUE(enabled.ok) << enabled.error;
    EXPECT_TRUE(IsWindowEnabled(
        control(native_fixture::kGenericTextId)));

    auto* checkbox = find_native_element_by_hwnd(
        native.root, control(native_fixture::kCheckboxId));
    auto* pushButton = find_native_element_by_hwnd(
        native.root, control(native_fixture::kButtonId));
    ASSERT_NE(checkbox, nullptr);
    ASSERT_NE(pushButton, nullptr);
    auto checkboxProperties = snapshot(native, *checkbox);
    ASSERT_TRUE(checkboxProperties.ok) << checkboxProperties.error;
    const auto* checkState =
        native_descriptor(checkboxProperties, "CheckState");
    ASSERT_NE(checkState, nullptr);
    EXPECT_TRUE(checkState->writable);
    EXPECT_EQ(checkState->choices.size(), 3u);
    for (const auto& value :
         {"unchecked", "checked", "indeterminate"}) {
        auto changed =
            set(native, *checkbox, checkboxProperties, "CheckState", value);
        ASSERT_TRUE(changed.ok) << value << ": " << changed.error;
        EXPECT_EQ(changed.value, value);
    }
    auto pushProperties = snapshot(native, *pushButton);
    ASSERT_TRUE(pushProperties.ok) << pushProperties.error;
    const auto* pushState =
        native_descriptor(pushProperties, "CheckState");
    ASSERT_NE(pushState, nullptr);
    EXPECT_FALSE(pushState->writable);
    const auto* pushStateValue =
        native_value(pushProperties, "CheckState");
    ASSERT_NE(pushStateValue, nullptr);
    EXPECT_NE(
        pushStateValue->readOnlyReason.find("not a checkbox"),
        std::string::npos);

    auto* edit = find_native_element_by_hwnd(
        native.root, control(native_fixture::kEditId));
    ASSERT_NE(edit, nullptr);
    auto editProperties = snapshot(native, *edit);
    ASSERT_TRUE(editProperties.ok) << editProperties.error;
    for (const char* property :
         {"Text", "SelectionStart", "SelectionEnd", "ReadOnly"}) {
        const auto* descriptor =
            native_descriptor(editProperties, property);
        ASSERT_NE(descriptor, nullptr) << property;
        EXPECT_TRUE(descriptor->writable) << property;
    }
    ASSERT_TRUE(set(
        native, *edit, editProperties, "Text",
        utf8(u8"Grüße 東京")).ok);
    ASSERT_TRUE(set(
        native, *edit, editProperties, "SelectionEnd", "8").ok);
    ASSERT_TRUE(set(
        native, *edit, editProperties, "SelectionStart", "2").ok);
    ASSERT_TRUE(set(
        native, *edit, editProperties, "ReadOnly", "true").ok);
    auto invalidSelection = set(
        native, *edit, editProperties, "SelectionStart", "9");
    EXPECT_FALSE(invalidSelection.ok);
    EXPECT_EQ(invalidSelection.hresult, E_INVALIDARG);
    ASSERT_TRUE(set(
        native, *edit, editProperties, "ReadOnly", "false").ok);
    ASSERT_TRUE(set(
        native, *edit, editProperties, "SelectionStart", "0").ok);
    ASSERT_TRUE(set(
        native, *edit, editProperties, "SelectionEnd", "0").ok);
    ASSERT_TRUE(set(
        native, *edit, editProperties, "Text", "Editable seed").ok);

    auto* combo = find_native_element_by_hwnd(
        native.root, control(native_fixture::kComboBoxId));
    auto* listBox = find_native_element_by_hwnd(
        native.root, control(native_fixture::kListBoxId));
    ASSERT_NE(combo, nullptr);
    ASSERT_NE(listBox, nullptr);
    auto comboProperties = snapshot(native, *combo);
    auto listProperties = snapshot(native, *listBox);
    ASSERT_TRUE(comboProperties.ok) << comboProperties.error;
    ASSERT_TRUE(listProperties.ok) << listProperties.error;
    EXPECT_TRUE(
        native_descriptor(comboProperties, "SelectedIndex")
            ->supportsClear);
    EXPECT_TRUE(
        native_descriptor(listProperties, "SelectedIndex")
            ->supportsClear);
    ASSERT_TRUE(set(
        native, *combo, comboProperties, "SelectedIndex", "2").ok);
    auto clearedCombo =
        clear(native, *combo, comboProperties, "SelectedIndex");
    ASSERT_TRUE(clearedCombo.ok) << clearedCombo.error;
    EXPECT_TRUE(clearedCombo.cleared);
    ASSERT_TRUE(set(
        native, *combo, comboProperties, "SelectedIndex", "1").ok);
    auto invalidCombo = set(
        native, *combo, comboProperties, "SelectedIndex", "3");
    EXPECT_FALSE(invalidCombo.ok);
    ASSERT_TRUE(set(
        native, *listBox, listProperties, "SelectedIndex", "0").ok);
    ASSERT_TRUE(clear(
        native, *listBox, listProperties, "SelectedIndex").ok);
    ASSERT_TRUE(set(
        native, *listBox, listProperties, "SelectedIndex", "2").ok);

    auto* scrollBar = find_native_element_by_hwnd(
        native.root, control(native_fixture::kScrollBarId));
    ASSERT_NE(scrollBar, nullptr);
    auto scrollProperties = snapshot(native, *scrollBar);
    ASSERT_TRUE(scrollProperties.ok) << scrollProperties.error;
    EXPECT_TRUE(
        native_descriptor(scrollProperties, "Minimum")->writable);
    EXPECT_TRUE(
        native_descriptor(scrollProperties, "Maximum")->writable);
    EXPECT_TRUE(
        native_descriptor(scrollProperties, "Position")->writable);
    EXPECT_FALSE(
        native_descriptor(scrollProperties, "PageSize")->writable);
    ASSERT_TRUE(set(
        native, *scrollBar, scrollProperties, "Position", "80").ok);
    ASSERT_TRUE(set(
        native, *scrollBar, scrollProperties, "Minimum", "5").ok);
    ASSERT_TRUE(set(
        native, *scrollBar, scrollProperties, "Maximum", "120").ok);
    auto invalidRange = set(
        native, *scrollBar, scrollProperties, "Maximum", "40");
    EXPECT_FALSE(invalidRange.ok);
    EXPECT_EQ(invalidRange.hresult, E_INVALIDARG);
    ASSERT_TRUE(set(
        native, *scrollBar, scrollProperties, "Minimum", "10").ok);
    ASSERT_TRUE(set(
        native, *scrollBar, scrollProperties, "Maximum", "110").ok);
    ASSERT_TRUE(set(
        native, *scrollBar, scrollProperties, "Position", "42").ok);

    const auto summary = state_summary();
    EXPECT_NE(
        summary.find("checkbox(id=1001)=2"), std::string::npos);
    EXPECT_NE(
        summary.find(
            "edit(id=1004)=Editable seed;selection=0,0;readonly=0"),
        std::string::npos);
    EXPECT_NE(
        summary.find("combo(id=1006)=1:Green"),
        std::string::npos);
    EXPECT_NE(
        summary.find("listbox(id=1007)=2:Gamma"),
        std::string::npos);
    EXPECT_NE(
        summary.find("scrollbar(id=1008)=10,110,10,42"),
        std::string::npos);
}

TEST_F(NativeControlsFixture, ComCtlTypedPropertiesRoundTripAndRejectStaleItems) {
    auto native = native_tree();
    ASSERT_NE(native.comctl, nullptr);

    auto* listView = find_native_element_by_hwnd(
        native.root, control(native_fixture::kListViewId));
    ASSERT_NE(listView, nullptr);
    EXPECT_EQ(listView->key.rfind("comctl:0x", 0), 0u);
    auto listViewProperties = snapshot(native, *listView);
    ASSERT_TRUE(listViewProperties.ok) << listViewProperties.error;
    ASSERT_TRUE(set(
        native, *listView, listViewProperties, "ViewMode", "list").ok);
    ASSERT_TRUE(set(
        native, *listView, listViewProperties, "ViewMode", "details").ok);

    auto* beta = find_native_element_by_text(
        *listView, "ListViewItem", "Beta row");
    auto* alpha = find_native_element_by_text(
        *listView, "ListViewItem", "Alpha row");
    ASSERT_NE(beta, nullptr);
    ASSERT_NE(alpha, nullptr);
    auto betaProperties = snapshot(native, *beta);
    ASSERT_TRUE(betaProperties.ok) << betaProperties.error;
    for (const char* property : {"Selected", "Focused", "Text"}) {
        ASSERT_TRUE(native_descriptor(betaProperties, property)->writable)
            << property;
    }
    ASSERT_TRUE(set(
        native, *beta, betaProperties, "Selected", "false").ok);
    ASSERT_TRUE(set(
        native, *beta, betaProperties, "Focused", "true").ok);
    ASSERT_TRUE(set(
        native, *beta, betaProperties, "Text",
        utf8(u8"Beta ✓ 東京")).ok);
    ASSERT_TRUE(set(
        native, *beta, betaProperties, "Text", "").ok);
    ASSERT_TRUE(set(
        native, *beta, betaProperties, "Text", "Beta row").ok);
    auto duplicateText = set(
        native, *beta, betaProperties, "Text", "Alpha row");
    EXPECT_FALSE(duplicateText.ok);
    EXPECT_NE(
        duplicateText.error.find("ambiguous"), std::string::npos);
    ASSERT_TRUE(set(
        native, *beta, betaProperties, "Focused", "false").ok);
    ASSERT_TRUE(set(
        native, *beta, betaProperties, "Selected", "true").ok);

    auto alphaProperties = snapshot(native, *alpha);
    ASSERT_TRUE(alphaProperties.ok) << alphaProperties.error;
    DWORD_PTR changed = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kMutateListViewIdentityMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000, &changed),
        0);
    auto stale = set(
        native, *alpha, alphaProperties, "Selected", "true");
    EXPECT_FALSE(stale.ok);
    EXPECT_NE(
        stale.error.find("changed since"), std::string::npos);
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kRestoreListViewIdentityMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000, &changed),
        0);

    auto* treeView = find_native_element_by_hwnd(
        native.root, control(native_fixture::kTreeViewId));
    ASSERT_NE(treeView, nullptr);
    auto* child = find_native_element_by_text(
        *treeView, "TreeViewItem", "Fixture Child");
    auto* grandchild = find_native_element_by_text(
        *treeView, "TreeViewItem", "Fixture Grandchild");
    ASSERT_NE(child, nullptr);
    ASSERT_NE(grandchild, nullptr)
        << "tree provider must preserve nested item identity";
    auto childProperties = snapshot(native, *child);
    auto grandchildProperties = snapshot(native, *grandchild);
    ASSERT_TRUE(childProperties.ok) << childProperties.error;
    ASSERT_TRUE(grandchildProperties.ok) << grandchildProperties.error;
    ASSERT_TRUE(set(
        native, *child, childProperties, "Selected", "false").ok);
    ASSERT_TRUE(set(
        native, *child, childProperties, "Selected", "true").ok);
    ASSERT_TRUE(set(
        native, *child, childProperties, "Expanded", "false").ok);
    ASSERT_TRUE(set(
        native, *child, childProperties, "Expanded", "true").ok);
    ASSERT_TRUE(set(
        native, *grandchild, grandchildProperties, "Selected", "true").ok);
    ASSERT_TRUE(set(
        native, *grandchild, grandchildProperties, "Text",
        utf8(u8"Grandchild ✓")).ok);
    ASSERT_TRUE(set(
        native, *grandchild, grandchildProperties, "Text", "").ok);
    ASSERT_TRUE(set(
        native, *grandchild, grandchildProperties, "Text",
        "Fixture Grandchild").ok);
    ASSERT_TRUE(set(
        native, *child, childProperties, "Selected", "true").ok);

    auto* toolbar = find_native_element_by_hwnd(
        native.root, control(native_fixture::kToolbarId));
    ASSERT_NE(toolbar, nullptr);
    auto* apply = find_native_element_by_text(
        *toolbar, "ToolbarButton", "Apply");
    auto* pin = find_native_element_by_text(
        *toolbar, "ToolbarButton", "Pinned");
    auto* disabled = find_native_element_by_text(
        *toolbar, "ToolbarButton", "Disabled");
    ASSERT_NE(apply, nullptr);
    ASSERT_NE(pin, nullptr);
    ASSERT_NE(disabled, nullptr);
    auto applyProperties = snapshot(native, *apply);
    auto pinProperties = snapshot(native, *pin);
    auto disabledProperties = snapshot(native, *disabled);
    ASSERT_TRUE(applyProperties.ok) << applyProperties.error;
    ASSERT_TRUE(pinProperties.ok) << pinProperties.error;
    ASSERT_TRUE(disabledProperties.ok) << disabledProperties.error;
    EXPECT_FALSE(
        native_descriptor(applyProperties, "Checked")->writable);
    EXPECT_TRUE(
        native_descriptor(pinProperties, "Checked")->writable);
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kMutateToolbarIdentityMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000, &changed),
        0);
    auto staleCommand = set(
        native, *apply, applyProperties, "Text", "wrong command");
    EXPECT_FALSE(staleCommand.ok);
    EXPECT_NE(
        staleCommand.error.find("different command"),
        std::string::npos);
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kRestoreToolbarIdentityMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000, &changed),
        0);
    ASSERT_TRUE(set(
        native, *pin, pinProperties, "Checked", "false").ok);
    ASSERT_TRUE(set(
        native, *pin, pinProperties, "Checked", "true").ok);
    ASSERT_TRUE(set(
        native, *disabled, disabledProperties, "Enabled", "true").ok);
    ASSERT_TRUE(set(
        native, *disabled, disabledProperties, "Enabled", "false").ok);
    ASSERT_TRUE(set(
        native, *apply, applyProperties, "Text",
        utf8(u8"Apply ✓")).ok);
    ASSERT_TRUE(set(
        native, *apply, applyProperties, "Text", "").ok);
    ASSERT_TRUE(set(
        native, *apply, applyProperties, "Text", "Apply").ok);

    auto* status = find_native_element_by_hwnd(
        native.root, control(native_fixture::kStatusBarId));
    ASSERT_NE(status, nullptr);
    ASSERT_GE(status->children.size(), 2u);
    auto firstPartProperties = snapshot(native, status->children[0]);
    const auto schemasAfterFirstPart =
        native.comctl->cached_schema_count_for_testing();
    auto secondPartProperties = snapshot(native, status->children[1]);
    ASSERT_TRUE(firstPartProperties.ok) << firstPartProperties.error;
    ASSERT_TRUE(secondPartProperties.ok) << secondPartProperties.error;
    EXPECT_EQ(
        firstPartProperties.schema->schemaId,
        secondPartProperties.schema->schemaId);
    EXPECT_EQ(
        native.comctl->cached_schema_count_for_testing(),
        schemasAfterFirstPart)
        << "schemas are shared by class/style/capability rather than HWND item";
    ASSERT_TRUE(set(
        native, status->children[1], secondPartProperties, "Text",
        utf8(u8"三 items ✓")).ok);
    ASSERT_TRUE(set(
        native, status->children[1], secondPartProperties, "Text", "").ok);
    ASSERT_TRUE(set(
        native, status->children[1], secondPartProperties, "Text",
        "3 items").ok);

    auto* tabs = find_native_element_by_hwnd(
        native.root, control(native_fixture::kTabControlId));
    ASSERT_NE(tabs, nullptr);
    auto tabProperties = snapshot(native, *tabs);
    ASSERT_TRUE(tabProperties.ok) << tabProperties.error;
    ASSERT_TRUE(set(
        native, *tabs, tabProperties, "SelectedIndex", "-1").ok);
    ASSERT_TRUE(set(
        native, *tabs, tabProperties, "SelectedIndex", "2").ok);
    auto invalidTab = set(
        native, *tabs, tabProperties, "SelectedIndex", "3");
    EXPECT_FALSE(invalidTab.ok);
    ASSERT_TRUE(set(
        native, *tabs, tabProperties, "SelectedIndex", "1").ok);
    ASSERT_GE(tabs->children.size(), 1u);
    auto tabItemProperties = snapshot(native, tabs->children[0]);
    ASSERT_TRUE(tabItemProperties.ok) << tabItemProperties.error;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kDeleteFirstTabMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000, &changed),
        0);
    auto staleTab = set(
        native, tabs->children[0], tabItemProperties, "Text",
        "stale tab write");
    EXPECT_FALSE(staleTab.ok);
    EXPECT_NE(
        staleTab.error.find("changed since"),
        std::string::npos);
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kRestoreTabsMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000, &changed),
        0);
    ASSERT_TRUE(set(
        native, tabs->children[0], tabItemProperties, "Text",
        utf8(u8"Overview ✓")).ok);
    ASSERT_TRUE(set(
        native, tabs->children[0], tabItemProperties, "Text", "").ok);
    ASSERT_TRUE(set(
        native, tabs->children[0], tabItemProperties, "Text",
        "Overview").ok);
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kMakeDuplicateTabsMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000, &changed),
        0);
    auto duplicateTabs = snapshot(native, tabs->children[0]);
    ASSERT_TRUE(duplicateTabs.ok) << duplicateTabs.error;
    const auto* duplicateTabText =
        native_descriptor(duplicateTabs, "Text");
    const auto* duplicateTabValue =
        native_value(duplicateTabs, "Text");
    ASSERT_NE(duplicateTabText, nullptr);
    ASSERT_NE(duplicateTabValue, nullptr);
    EXPECT_FALSE(duplicateTabText->writable);
    EXPECT_NE(
        duplicateTabValue->readOnlyReason.find("not unique"),
        std::string::npos);
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kRestoreTabsMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000, &changed),
        0);

    const auto summary = state_summary();
    EXPECT_NE(
        summary.find(
            "listview(id=1009)=1:Beta row;focus=-1;"
            "items=Alpha row|Beta row|Gamma row"),
        std::string::npos);
    EXPECT_NE(
        summary.find(
            "tree(id=1010)=selected:Fixture Child;"
            "Fixture Root[expanded]/Fixture Child[expanded]/"
            "Fixture Grandchild"),
        std::string::npos);
    EXPECT_NE(
        summary.find(
            "toolbar(id=1011)=2001:Apply:enabled,"
            "2002:Pinned:enabled+checked,2003:Disabled:disabled"),
        std::string::npos);
    EXPECT_NE(
        summary.find("status(id=1012)=Ready|3 items|Idle|<ownerdraw>"),
        std::string::npos);
    EXPECT_NE(
        summary.find(
            "tab(id=1013)=1:Details;items=Overview|Details|Advanced"),
        std::string::npos);
}

TEST_F(NativeControlsFixture, ToolbarLongTextIsReadWithoutFixedBufferOverflow) {
    auto native = native_tree();
    auto* toolbar = find_native_element_by_hwnd(
        native.root, control(native_fixture::kToolbarId));
    ASSERT_NE(toolbar, nullptr);
    auto* apply = find_native_element_by_text(
        *toolbar, "ToolbarButton", "Apply");
    ASSERT_NE(apply, nullptr);

    DWORD_PTR changed = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kSetLongToolbarTextMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000, &changed),
        0);
    ASSERT_NE(changed, 0u);

    auto properties = snapshot(native, *apply);
    ASSERT_TRUE(properties.ok) << properties.error;
    const auto* text = native_value(properties, "Text");
    ASSERT_NE(text, nullptr);
    EXPECT_EQ(
        text->value.size(),
        native_fixture::kLongToolbarTextLength);
    EXPECT_TRUE(std::all_of(
        text->value.begin(), text->value.end(),
        [](char value) { return value == 'X'; }));

    auto tree = dump_tree();
    const json* toolbarJson = find_element_by_hwnd(
        tree["root"], control(native_fixture::kToolbarId));
    ASSERT_NE(toolbarJson, nullptr);
    const json* applyJson = find_element_by_type_property(
        *toolbarJson, "ToolbarButton", "commandId", "2001");
    ASSERT_NE(applyJson, nullptr);
    EXPECT_EQ(
        applyJson->value("text", "").size(),
        native_fixture::kLongToolbarTextLength);
    EXPECT_TRUE(IsWindow(s_hwnd))
        << "the oversized toolbar read killed the target process";

    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kRestoreToolbarTextMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000, &changed),
        0);
}

TEST_F(NativeControlsFixture, OwnerDrawStatusPartNeverTreatsItemDataAsText) {
    auto native = native_tree();
    auto* status = find_native_element_by_hwnd(
        native.root, control(native_fixture::kStatusBarId));
    ASSERT_NE(status, nullptr);
    ASSERT_EQ(status->children.size(), 4u);
    auto properties = snapshot(native, status->children[3]);
    ASSERT_TRUE(properties.ok) << properties.error;
    const auto* descriptor =
        native_descriptor(properties, "Text");
    const auto* value =
        native_value(properties, "Text");
    ASSERT_NE(descriptor, nullptr);
    ASSERT_NE(value, nullptr);
    EXPECT_FALSE(descriptor->writable);
    EXPECT_NE(
        value->readOnlyReason.find("Owner-drawn"),
        std::string::npos);
    EXPECT_NE(
        value->unavailableReason.find("Owner-drawn"),
        std::string::npos);

    auto refused = set(
        native, status->children[3], properties, "Text",
        "must not replace item data");
    EXPECT_FALSE(refused.ok);
    EXPECT_TRUE(IsWindow(s_hwnd));

    DWORD_PTR painted = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kValidateOwnerDrawStatusMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
            &painted),
        0);
    EXPECT_EQ(painted, 1u)
        << "owner-draw item data or painting was corrupted";
}

TEST_F(NativeControlsFixture, TabCustomExtraIsNotReadAsGenericItemIdentity) {
    DWORD_PTR changed = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kSetTabItemExtraMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
            &changed),
        0);

    auto native = native_tree();
    auto* tabs = find_native_element_by_hwnd(
        native.root, control(native_fixture::kTabControlId));
    ASSERT_NE(tabs, nullptr);
    ASSERT_EQ(tabs->children.size(), 3u);
    EXPECT_EQ(tabs->children[0].text, "Overview");
    EXPECT_EQ(tabs->children[1].text, "Overview");
    auto properties = snapshot(native, tabs->children[0]);
    ASSERT_TRUE(properties.ok) << properties.error;
    const auto* descriptor =
        native_descriptor(properties, "Text");
    const auto* value =
        native_value(properties, "Text");
    ASSERT_NE(descriptor, nullptr);
    ASSERT_NE(value, nullptr);
    EXPECT_FALSE(descriptor->writable);
    EXPECT_NE(
        value->readOnlyReason.find("not unique"),
        std::string::npos);

    DWORD_PTR validExtra = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kValidateTabItemExtraMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
            &validExtra),
        0);
    EXPECT_EQ(validExtra, 1u)
        << "generic TCITEM reads overwrote custom item-extra bytes";
    EXPECT_TRUE(IsWindow(s_hwnd));

    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kRestoreTabsMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
            &changed),
        0);
}

TEST_F(NativeControlsFixture, DuplicateToolbarCommandsAreReadOnlyAndReordersAreStale) {
    auto native = native_tree();
    auto* toolbar = find_native_element_by_hwnd(
        native.root, control(native_fixture::kToolbarId));
    ASSERT_NE(toolbar, nullptr);
    auto* apply = find_native_element_by_text(
        *toolbar, "ToolbarButton", "Apply");
    ASSERT_NE(apply, nullptr);
    auto baseline = snapshot(native, *apply);
    ASSERT_TRUE(baseline.ok) << baseline.error;

    DWORD_PTR changed = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kDuplicateToolbarCommandsMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
            &changed),
        0);
    auto duplicate = snapshot(native, *apply);
    ASSERT_TRUE(duplicate.ok) << duplicate.error;
    for (const auto& descriptor : duplicate.schema->descriptors) {
        EXPECT_FALSE(descriptor.writable);
        const auto* value =
            native_value(duplicate, descriptor.name);
        ASSERT_NE(value, nullptr);
        EXPECT_NE(
            value->readOnlyReason.find("duplicated"),
            std::string::npos);
    }

    auto dump = dump_tree();
    const json* toolbarJson = find_element_by_hwnd(
        dump["root"], control(native_fixture::kToolbarId));
    ASSERT_NE(toolbarJson, nullptr);
    int ambiguous = 0;
    for (const auto& child :
         toolbarJson->value("children", json::array())) {
        ambiguous += child.value("properties", json::object())
                         .value("ambiguousCommandId", "") == "true";
    }
    EXPECT_EQ(ambiguous, 2);

    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kRestoreToolbarCommandsMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
            &changed),
        0);
    baseline = snapshot(native, *apply);
    ASSERT_TRUE(baseline.ok) << baseline.error;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kMoveToolbarApplyMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
            &changed),
        0);
    auto stale = set(
        native, *apply, baseline, "Text", "stale reorder");
    EXPECT_FALSE(stale.ok);
    EXPECT_NE(
        stale.error.find("different command"),
        std::string::npos);
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kRestoreToolbarOrderMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
            &changed),
        0);
}

TEST_F(NativeControlsFixture, LongListAndTreeTextRoundTripsExactly) {
    auto native = native_tree();
    auto* listView = find_native_element_by_hwnd(
        native.root, control(native_fixture::kListViewId));
    auto* treeView = find_native_element_by_hwnd(
        native.root, control(native_fixture::kTreeViewId));
    ASSERT_NE(listView, nullptr);
    ASSERT_NE(treeView, nullptr);
    auto* beta = find_native_element_by_text(
        *listView, "ListViewItem", "Beta row");
    auto* grandchild = find_native_element_by_text(
        *treeView, "TreeViewItem", "Fixture Grandchild");
    ASSERT_NE(beta, nullptr);
    ASSERT_NE(grandchild, nullptr);
    auto betaProperties = snapshot(native, *beta);
    auto treeProperties = snapshot(native, *grandchild);
    ASSERT_TRUE(betaProperties.ok) << betaProperties.error;
    ASSERT_TRUE(treeProperties.ok) << treeProperties.error;

    const std::string longText(
        native_fixture::kLongItemTextLength, 'L');
    auto setList = set(
        native, *beta, betaProperties, "Text", longText);
    ASSERT_TRUE(setList.ok) << setList.error;
    EXPECT_EQ(setList.value, longText);
    auto readList = snapshot(native, *beta);
    ASSERT_TRUE(readList.ok) << readList.error;
    EXPECT_EQ(native_value(readList, "Text")->value, longText);
    auto listDump = dump_tree();
    const json* listJson = find_element_by_hwnd(
        listDump["root"], control(native_fixture::kListViewId));
    ASSERT_NE(listJson, nullptr);
    const json* longListItem = find_element_by_type_property(
        *listJson, "ListViewItem", "index", "1");
    ASSERT_NE(longListItem, nullptr);
    EXPECT_EQ(longListItem->value("text", ""), longText);
    ASSERT_TRUE(set(
        native, *beta, readList, "Text", "Beta row").ok);

    auto setTree = set(
        native, *grandchild, treeProperties, "Text", longText);
    ASSERT_TRUE(setTree.ok) << setTree.error;
    EXPECT_EQ(setTree.value, longText);
    auto readTree = snapshot(native, *grandchild);
    ASSERT_TRUE(readTree.ok) << readTree.error;
    EXPECT_EQ(native_value(readTree, "Text")->value, longText);
    auto treeDump = dump_tree();
    const json* treeJson = find_element_by_hwnd(
        treeDump["root"], control(native_fixture::kTreeViewId));
    ASSERT_NE(treeJson, nullptr);
    std::vector<const json*> treeItems;
    collect_json_elements(*treeJson, treeItems);
    const json* longTreeItem = nullptr;
    for (const auto* candidate : treeItems) {
        if (candidate->value("type", "") == "TreeViewItem" &&
            candidate->value("text", "") == longText) {
            longTreeItem = candidate;
            break;
        }
    }
    ASSERT_NE(longTreeItem, nullptr);
    ASSERT_TRUE(set(
        native, *grandchild, readTree, "Text",
        "Fixture Grandchild").ok);

    const std::string tooLong(
        lvt::kMaximumNativePropertyTextChars + 1, 'Z');
    auto beforeFailure = snapshot(native, *beta);
    ASSERT_TRUE(beforeFailure.ok) << beforeFailure.error;
    auto rejected = set(
        native, *beta, beforeFailure, "Text", tooLong);
    EXPECT_FALSE(rejected.ok);
    auto afterFailure = snapshot(native, *beta);
    ASSERT_TRUE(afterFailure.ok) << afterFailure.error;
    EXPECT_EQ(native_value(afterFailure, "Text")->value, "Beta row")
        << "a rejected oversized value still mutated the target";
}

TEST_F(NativeControlsFixture, NativeTargetsMustBelongToTheBuiltRootTree) {
    auto native = native_tree();
    DWORD_PTR siblingValue = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kGetOutOfTreeHwndMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
            &siblingValue),
        0);
    HWND sibling = reinterpret_cast<HWND>(siblingValue);
    ASSERT_NE(sibling, nullptr);
    DWORD siblingPid = 0;
    GetWindowThreadProcessId(sibling, &siblingPid);
    EXPECT_EQ(siblingPid, s_pid);
    EXPECT_FALSE(IsChild(s_hwnd, sibling));

    EXPECT_EQ(native.win32->register_hwnd(sibling), 0u);
    auto rejected = native.win32->get_property_snapshot(
        reinterpret_cast<uintptr_t>(sibling));
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.hresult, HRESULT_FROM_WIN32(ERROR_NOT_FOUND));

    auto* generic = find_native_element_by_hwnd(
        native.root, control(native_fixture::kGenericTextId));
    ASSERT_NE(generic, nullptr);
    auto baseline = snapshot(native, *generic);
    ASSERT_TRUE(baseline.ok) << baseline.error;
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kReparentGenericOutOfTreeMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
            &siblingValue),
        0);
    auto reparented =
        native.win32->get_property_snapshot(generic->providerHandle);
    EXPECT_FALSE(reparented.ok);
    EXPECT_NE(
        reparented.error.find("no longer in"),
        std::string::npos);
    ASSERT_NE(
        SendMessageTimeoutW(
            s_hwnd, native_fixture::kRestoreGenericParentMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
            &siblingValue),
        0);
}

TEST_F(NativeControlsFixture, NativePropertySafetyRejectsHungClosedAndOwnerDataTargets) {
    auto native = native_tree();
    ASSERT_NE(native.win32, nullptr);

    DWORD_PTR ignored = 0;
    ASSERT_TRUE(PostMessageW(
        s_hwnd, native_fixture::kHangMessage, 0, 0));
    Sleep(100);
    const auto started = std::chrono::steady_clock::now();
    auto hung = native.win32->get_property_snapshot(
        native.root.providerHandle);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_FALSE(hung.ok);
    EXPECT_EQ(hung.hresult, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_NE(hung.error.find("timeout"), std::string::npos);
    EXPECT_LT(
        elapsed, std::chrono::seconds(2))
        << "SendMessageTimeoutW must bound an unresponsive target";
    Sleep(1600);

    HWND local = CreateWindowExW(
        0, WC_STATICW, L"closed", WS_OVERLAPPED,
        0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(local, nullptr);
    auto closedConnection = lvt::NativePropertyConnection::connect(
        local, GetCurrentProcessId(), "win32");
    ASSERT_NE(closedConnection, nullptr);
    const auto closedHandle = closedConnection->register_hwnd(local);
    DestroyWindow(local);
    auto closed = closedConnection->get_property_snapshot(closedHandle);
    EXPECT_FALSE(closed.ok);
    EXPECT_EQ(
        closed.hresult,
        HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE));

    HWND owner = CreateWindowExW(
        0, WC_STATICW, L"owner", WS_OVERLAPPED,
        0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(owner, nullptr);
    HWND ownerData = CreateWindowExW(
        0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_OWNERDATA,
        0, 0, 100, 100, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(ownerData, nullptr);
    SendMessageW(ownerData, LVM_SETITEMCOUNT, 1, 0);
    auto ownerDataConnection = lvt::NativePropertyConnection::connect(
        owner, GetCurrentProcessId(), "comctl", "test");
    ASSERT_NE(ownerDataConnection, nullptr);
    const auto ownerDataHandle =
        ownerDataConnection->register_listview_item(ownerData, 0);
    auto ownerDataProperties =
        ownerDataConnection->get_property_snapshot(ownerDataHandle);
    ASSERT_TRUE(ownerDataProperties.ok) << ownerDataProperties.error;
    for (const auto& descriptor :
         ownerDataProperties.schema->descriptors) {
        EXPECT_FALSE(descriptor.writable);
        const auto* value =
            native_value(ownerDataProperties, descriptor.name);
        ASSERT_NE(value, nullptr);
        EXPECT_NE(
            value->readOnlyReason.find("Owner-data"),
            std::string::npos);
    }
    DestroyWindow(owner);
}

TEST(NativePointerMessageSafety, TimedOutRemoteBufferLivesUntilTargetExit) {
    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(NATIVE_CONTROLS_FIXTURE_EXE_PATH));

    auto win32 = lvt::NativePropertyConnection::connect(
        fixture.hwnd, fixture.pid, "win32");
    auto comctl = lvt::NativePropertyConnection::connect(
        fixture.hwnd, fixture.pid, "comctl", "test");
    ASSERT_NE(win32, nullptr);
    ASSERT_NE(comctl, nullptr);
    auto frameworks =
        lvt::detect_frameworks(fixture.hwnd, fixture.pid);
    auto lookup =
        [&win32, &comctl](const std::string& provider)
            -> lvt::IFrameworkConnection* {
        if (provider == "win32")
            return win32.get();
        if (provider == "comctl")
            return comctl.get();
        return nullptr;
    };
    auto tree = lvt::build_tree(
        fixture.hwnd, fixture.pid, frameworks, -1, {}, false, lookup);
    auto* toolbar = find_native_element_by_hwnd(
        tree, GetDlgItem(fixture.hwnd, native_fixture::kToolbarId));
    ASSERT_NE(toolbar, nullptr);
    auto* apply = find_native_element_by_text(
        *toolbar, "ToolbarButton", "Apply");
    ASSERT_NE(apply, nullptr);
    auto baseline =
        comctl->get_property_snapshot(apply->providerHandle);
    ASSERT_TRUE(baseline.ok) << baseline.error;

    DWORD_PTR armed = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            fixture.hwnd, native_fixture::kArmDelayedPointerMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000, &armed),
        0);
    const auto deferredBefore =
        lvt::deferred_native_pointer_message_count_for_testing();
    const auto started = std::chrono::steady_clock::now();
    auto delayed =
        comctl->get_property_snapshot(apply->providerHandle);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_FALSE(delayed.ok);
    EXPECT_EQ(delayed.hresult, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_LT(elapsed, std::chrono::seconds(2));
    EXPECT_GT(
        lvt::deferred_native_pointer_message_count_for_testing(),
        deferredBefore);

    Sleep(700);
    DWORD_PTR pointerState = 0;
    ASSERT_NE(
        SendMessageTimeoutW(
            fixture.hwnd, native_fixture::kGetDelayedPointerStateMessage,
            0, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 2000,
            &pointerState),
        0);
    EXPECT_EQ(pointerState, 4u)
        << "the target observed freed pointer memory after caller timeout";
    EXPECT_TRUE(IsWindow(fixture.hwnd));

    fixture.stop();
    for (int attempt = 0;
         attempt < 100 &&
         lvt::deferred_native_pointer_message_count_for_testing() !=
             deferredBefore;
         ++attempt) {
        Sleep(20);
    }
    EXPECT_EQ(
        lvt::deferred_native_pointer_message_count_for_testing(),
        deferredBefore)
        << "deferred pointer buffers must be released after target exit";
}

TEST(NativeCrossBitness, WinEventSnapshotsRemainAvailable) {
    const fs::path source = LVT_SOURCE_DIR;
    const fs::path otherFixture =
        sizeof(void*) == 8
            ? source / "build-x86" / "lvt_native_controls_fixture.exe"
            : source / "build" / "lvt_native_controls_fixture.exe";
    if (!fs::exists(otherFixture))
        GTEST_SKIP() << "opposite-architecture fixture is not built";

    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(otherFixture));
    ASSERT_NE(
        lvt::detect_process_architecture(fixture.pid),
        lvt::get_host_architecture());

    auto connection = lvt::NativePropertyConnection::connect(
        fixture.hwnd, fixture.pid, "win32");
    ASSERT_NE(connection, nullptr);
    ASSERT_TRUE(connection->event_hook_active_for_testing());
    publish_native_event_snapshot(
        connection, fixture.hwnd, fixture.pid);
    drain_native_events(*connection);

    ASSERT_NE(
        send_native_fixture_message(
            fixture.hwnd,
            native_fixture::kCreateEventChildMessage),
        0);
    EXPECT_TRUE(wait_for_native_snapshot(*connection));
    send_native_fixture_message(
        fixture.hwnd,
        native_fixture::kDestroyEventChildMessage);
}

#ifdef LVT_ENABLE_UIA
TEST(NativeCrossBitness, UiaPersistentEventsRemainAvailable) {
    const fs::path source = LVT_SOURCE_DIR;
    const fs::path otherFixture =
        sizeof(void*) == 8
            ? source / "build-x86" / "lvt_native_controls_fixture.exe"
            : source / "build" / "lvt_native_controls_fixture.exe";
    if (!fs::exists(otherFixture))
        GTEST_SKIP() << "opposite-architecture fixture is not built";

    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(otherFixture));
    ASSERT_NE(
        lvt::detect_process_architecture(fixture.pid),
        lvt::get_host_architecture());

    auto connection =
        lvt::UiaConnection::connect(fixture.hwnd, fixture.pid);
    ASSERT_NE(connection, nullptr);
    drain_uia_events(*connection);

    ASSERT_NE(
        send_native_fixture_message(
            fixture.hwnd,
            native_fixture::kCreateEventChildMessage),
        0);
    EXPECT_TRUE(wait_for_uia_snapshot(*connection))
        << "the persistent UIA callback did not cross the architecture boundary";
}
#endif

TEST(NativeCrossBitness, PublicBuildTreeSkipsAbiSensitiveComCtlMessages) {
    const fs::path source = LVT_SOURCE_DIR;
    const fs::path otherFixture =
        sizeof(void*) == 8
            ? source / "build-x86" / "lvt_native_controls_fixture.exe"
            : source / "build" / "lvt_native_controls_fixture.exe";
    if (!fs::exists(otherFixture))
        GTEST_SKIP() << "opposite-architecture fixture is not built";

    ScopedNativeFixtureProcess fixture;
    ASSERT_TRUE(fixture.start(otherFixture));
    ASSERT_NE(
        lvt::detect_process_architecture(fixture.pid),
        lvt::get_host_architecture());

    const auto frameworks =
        lvt::detect_frameworks(fixture.hwnd, fixture.pid);
    auto tree = lvt::build_tree(
        fixture.hwnd, fixture.pid, frameworks);

    auto no_logical_child_type =
        [](const lvt::Element& element, const std::string& type) {
        return std::none_of(
            element.children.begin(), element.children.end(),
            [&type](const lvt::Element& child) {
                return child.type == type;
            });
    };

    auto* listView = find_native_element_by_hwnd(
        tree, GetDlgItem(fixture.hwnd, native_fixture::kListViewId));
    auto* treeView = find_native_element_by_hwnd(
        tree, GetDlgItem(fixture.hwnd, native_fixture::kTreeViewId));
    auto* toolbar = find_native_element_by_hwnd(
        tree, GetDlgItem(fixture.hwnd, native_fixture::kToolbarId));
    auto* status = find_native_element_by_hwnd(
        tree, GetDlgItem(fixture.hwnd, native_fixture::kStatusBarId));
    auto* tabs = find_native_element_by_hwnd(
        tree, GetDlgItem(fixture.hwnd, native_fixture::kTabControlId));
    ASSERT_NE(listView, nullptr);
    ASSERT_NE(treeView, nullptr);
    ASSERT_NE(toolbar, nullptr);
    ASSERT_NE(status, nullptr);
    ASSERT_NE(tabs, nullptr);
    EXPECT_EQ(listView->properties["itemCount"], "3");
    EXPECT_EQ(listView->properties["viewMode"], "details");
    EXPECT_EQ(treeView->properties["itemCount"], "3");
    EXPECT_EQ(toolbar->properties["buttonCount"], "3");
    EXPECT_EQ(status->properties["partCount"], "4");
    EXPECT_EQ(tabs->properties["selectedIndex"], "1");
    EXPECT_TRUE(no_logical_child_type(*listView, "ListViewItem"));
    EXPECT_TRUE(no_logical_child_type(*treeView, "TreeViewItem"));
    EXPECT_TRUE(no_logical_child_type(*toolbar, "ToolbarButton"));
    EXPECT_TRUE(no_logical_child_type(*status, "StatusBarPart"));
    EXPECT_TRUE(no_logical_child_type(*tabs, "Tab"));
    EXPECT_TRUE(IsWindow(fixture.hwnd));
}

// ---- Framework-specific bounds (WinUI3/XAML) ----

TEST_F(NotepadFixture, WinUI3BoundsIfDetected) {
    // If WinUI3 framework is detected, verify XAML element bounds are reasonable
    auto lvt = get_lvt_path();
    auto fwOutput = run_command(make_cmd(lvt, get_pid_arg() + " frameworks"));
    if (fwOutput.find("winui3") == std::string::npos) {
        GTEST_SKIP() << "WinUI3 not detected for this Notepad instance";
    }

    auto output = run_command(make_cmd(lvt, get_pid_arg()));
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded());

    std::vector<const json*> elements;
    collect_json_elements(j["root"], elements);

    int winui3Count = 0;
    for (auto* el : elements) {
        if (!el->contains("framework")) continue;
        if ((*el)["framework"].get<std::string>() != "winui3") continue;
        winui3Count++;

        if (!el->contains("bounds")) continue;
        auto& b = (*el)["bounds"];
        int x = b["x"].get<int>();
        int y = b["y"].get<int>();
        int w = b["width"].get<int>();
        int h = b["height"].get<int>();
        std::string id = el->value("id", "?");

        // WinUI3 elements should never have INT_MIN/INT_MAX bounds
        // (the bug this PR fixes)
        EXPECT_GT(x, kMinReasonableCoord) << "WinUI3 element " << id << " has extreme x";
        EXPECT_LT(x, kMaxReasonableCoord) << "WinUI3 element " << id << " has extreme x";
        EXPECT_GT(y, kMinReasonableCoord) << "WinUI3 element " << id << " has extreme y";
        EXPECT_LT(y, kMaxReasonableCoord) << "WinUI3 element " << id << " has extreme y";
        EXPECT_GE(w, 0) << "WinUI3 element " << id << " has negative width";
        EXPECT_GE(h, 0) << "WinUI3 element " << id << " has negative height";
    }

    EXPECT_GT(winui3Count, 0) << "WinUI3 detected but no WinUI3 elements in tree";
}

TEST_F(NotepadFixture, XamlBoundsIfDetected) {
    // If XAML framework is detected (UWP), verify element bounds are reasonable
    auto lvt = get_lvt_path();
    auto fwOutput = run_command(make_cmd(lvt, get_pid_arg() + " frameworks"));
    if (fwOutput.find("xaml") == std::string::npos) {
        GTEST_SKIP() << "XAML (UWP) not detected for this Notepad instance";
    }

    auto output = run_command(make_cmd(lvt, get_pid_arg()));
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded());

    std::vector<const json*> elements;
    collect_json_elements(j["root"], elements);

    for (auto* el : elements) {
        if (!el->contains("framework")) continue;
        if ((*el)["framework"].get<std::string>() != "xaml") continue;

        if (!el->contains("bounds")) continue;
        auto& b = (*el)["bounds"];
        int x = b["x"].get<int>();
        int y = b["y"].get<int>();
        int w = b["width"].get<int>();
        int h = b["height"].get<int>();
        std::string id = el->value("id", "?");

        EXPECT_GT(x, kMinReasonableCoord) << "XAML element " << id << " has extreme x";
        EXPECT_LT(x, kMaxReasonableCoord) << "XAML element " << id << " has extreme x";
        EXPECT_GT(y, kMinReasonableCoord) << "XAML element " << id << " has extreme y";
        EXPECT_LT(y, kMaxReasonableCoord) << "XAML element " << id << " has extreme y";
        EXPECT_GE(w, 0) << "XAML element " << id << " has negative width";
        EXPECT_GE(h, 0) << "XAML element " << id << " has negative height";
    }
}

// ---- Controlled Win32 window tests ----
// Creates a window with known child controls and verifies structure, bounds, and annotations.

class KnownWindowFixture : public ::testing::Test {
protected:
    void SetUp() override {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"LvtTestWindow";
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassExW(&wc);

        // Create parent window
        parentWindow_.reset(CreateWindowExW(0, L"LvtTestWindow", L"LVT Test Window",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            100, 100, 400, 300,
            nullptr, nullptr, GetModuleHandle(nullptr), nullptr));
        parentHwnd_ = parentWindow_.get();
        ASSERT_NE(parentHwnd_, nullptr) << "Failed to create test window";

        // Child controls at known client-area positions
        buttonHwnd_ = CreateWindowExW(0, L"Button", L"Click Me",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 10, 100, 30,
            parentHwnd_, nullptr, GetModuleHandle(nullptr), nullptr);
        ASSERT_NE(buttonHwnd_, nullptr);

        editHwnd_ = CreateWindowExW(0, L"Edit", L"Hello",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT,
            10, 50, 200, 25,
            parentHwnd_, nullptr, GetModuleHandle(nullptr), nullptr);
        ASSERT_NE(editHwnd_, nullptr);

        staticHwnd_ = CreateWindowExW(0, L"Static", L"Label Text",
            WS_CHILD | WS_VISIBLE,
            10, 90, 150, 20,
            parentHwnd_, nullptr, GetModuleHandle(nullptr), nullptr);
        ASSERT_NE(staticHwnd_, nullptr);

        UpdateWindow(parentHwnd_);
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    void TearDown() override {
        parentWindow_.reset();
        parentHwnd_ = nullptr;
        UnregisterClassW(L"LvtTestWindow", GetModuleHandle(nullptr));
    }

    std::string get_hwnd_arg() const {
        char buf[64];
        sprintf_s(buf, "--hwnd 0x%llX", (unsigned long long)(uintptr_t)parentHwnd_);
        return buf;
    }

    wil::unique_hwnd parentWindow_;
    HWND parentHwnd_ = nullptr;
    HWND buttonHwnd_ = nullptr;
    HWND editHwnd_ = nullptr;
    HWND staticHwnd_ = nullptr;
};

TEST_F(KnownWindowFixture, TreeStructure) {
    // Verify root and 3 children with correct class names, types, and text
    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_hwnd_arg()));
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << "Output is not valid JSON";

    auto& root = j["root"];
    EXPECT_EQ(root["id"], "e0");
    EXPECT_EQ(root["framework"], "win32");
    EXPECT_EQ(root["className"], "LvtTestWindow");
    EXPECT_EQ(root["text"], "LVT Test Window");
    EXPECT_EQ(root["type"], "Window");

    ASSERT_TRUE(root.contains("children"));
    ASSERT_EQ(root["children"].size(), 3u) << "Expected exactly 3 child controls";

    // Match children by className (order may vary)
    std::map<std::string, const json*> byClass;
    for (auto& child : root["children"]) {
        byClass[child["className"].get<std::string>()] = &child;
    }

    ASSERT_TRUE(byClass.count("Button"));
    EXPECT_EQ((*byClass["Button"])["type"], "Button");
    EXPECT_EQ((*byClass["Button"])["text"], "Click Me");
    EXPECT_EQ((*byClass["Button"])["framework"], "win32");

    ASSERT_TRUE(byClass.count("Edit"));
    EXPECT_EQ((*byClass["Edit"])["type"], "Edit");
    EXPECT_EQ((*byClass["Edit"])["text"], "Hello");
    EXPECT_EQ((*byClass["Edit"])["framework"], "win32");

    ASSERT_TRUE(byClass.count("Static"));
    EXPECT_EQ((*byClass["Static"])["type"], "Static");
    EXPECT_EQ((*byClass["Static"])["text"], "Label Text");
    EXPECT_EQ((*byClass["Static"])["framework"], "win32");
}

TEST_F(KnownWindowFixture, ChildBoundsMatchWin32) {
    // Verify each control's bounds match the Win32 GetWindowRect values
    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_hwnd_arg()));
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded());

    struct ControlCheck {
        HWND hwnd;
        std::string className;
    };
    ControlCheck controls[] = {
        {parentHwnd_, "LvtTestWindow"},
        {buttonHwnd_, "Button"},
        {editHwnd_,   "Edit"},
        {staticHwnd_, "Static"},
    };

    std::vector<const json*> elements;
    collect_json_elements(j["root"], elements);

    for (auto& ctrl : controls) {
        RECT expected{};
        GetWindowRect(ctrl.hwnd, &expected);
        int ex = expected.left;
        int ey = expected.top;
        int ew = expected.right - expected.left;
        int eh = expected.bottom - expected.top;

        // Find element by className
        const json* found = nullptr;
        for (auto* el : elements) {
            if (el->value("className", "") == ctrl.className) {
                found = el;
                break;
            }
        }
        ASSERT_NE(found, nullptr) << ctrl.className << " not found in tree";

        auto& b = (*found)["bounds"];
        EXPECT_EQ(b["x"].get<int>(), ex) << ctrl.className << " x mismatch";
        EXPECT_EQ(b["y"].get<int>(), ey) << ctrl.className << " y mismatch";
        EXPECT_EQ(b["width"].get<int>(), ew) << ctrl.className << " width mismatch";
        EXPECT_EQ(b["height"].get<int>(), eh) << ctrl.className << " height mismatch";
    }
}

#ifndef NDEBUG
TEST_F(KnownWindowFixture, AnnotationsIncludeKnownControls) {
    // Verify that annotations include each known control with positive bounds
    auto lvt = get_lvt_path();
    auto annFile = fs::path(lvt).parent_path() / "lvt_test_known_ann.json";
    fs::remove(annFile);

    auto output = run_command(make_cmd(lvt,
        get_hwnd_arg() + " dump --annotations-json " + annFile.string()));
    auto tree = json::parse(output, nullptr, false);
    ASSERT_FALSE(tree.is_discarded());

    ASSERT_TRUE(fs::exists(annFile));
    std::ifstream f(annFile);
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto annotations = json::parse(content, nullptr, false);
    ASSERT_FALSE(annotations.is_discarded());
    ASSERT_TRUE(annotations.is_array());

    // Build set of annotated IDs
    std::set<std::string> annotatedIds;
    for (auto& a : annotations) {
        annotatedIds.insert(a["id"].get<std::string>());
    }

    // Map tree element IDs to classNames
    std::vector<const json*> elements;
    collect_json_elements(tree["root"], elements);

    for (auto* el : elements) {
        std::string cn = el->value("className", "");
        if (cn == "Button" || cn == "Edit" || cn == "Static" || cn == "LvtTestWindow") {
            std::string id = (*el)["id"].get<std::string>();
            EXPECT_TRUE(annotatedIds.count(id) > 0)
                << cn << " (id=" << id << ") should be annotated";
        }
    }

    // Every annotation must have positive dimensions
    for (auto& a : annotations) {
        EXPECT_GT(a["width"].get<int>(), 0)
            << "Annotation " << a["id"] << " has non-positive width";
        EXPECT_GT(a["height"].get<int>(), 0)
            << "Annotation " << a["id"] << " has non-positive height";
    }

    f.close();
    fs::remove(annFile);
}
#endif

// --- UIA tree (--uia) ---
// These assert the properties that make the UIA tree useful for automation:
// AutomationIds, control types, and pattern support. They reuse the WinUI3
// sample because it has known, stable AutomationIds.

namespace {

// Tree JSON nests dynamic properties under "properties" (unlike --query output,
// which flattens them), so reach through that object.
std::string uia_prop(const json& node, const std::string& name) {
    if (!node.contains("properties"))
        return {};
    return node["properties"].value(name, "");
}

bool uia_has_prop(const json& node, const std::string& name) {
    return node.contains("properties") && node["properties"].contains(name);
}

const json* find_by_automation_id(const json& node, const std::string& automationId) {
    if (uia_prop(node, "AutomationId") == automationId)
        return &node;
    if (node.contains("children")) {
        for (const auto& child : node["children"]) {
            if (auto* found = find_by_automation_id(child, automationId))
                return found;
        }
    }
    return nullptr;
}

size_t count_json_nodes(const json& node) {
    size_t count = 1;
    if (node.contains("children")) {
        for (const auto& child : node["children"])
            count += count_json_nodes(child);
    }
    return count;
}

const lvt::Element* find_uia_element_by_automation_id(
    const lvt::Element& element, const std::string& automationId) {
    const auto found = element.properties.find("AutomationId");
    if (found != element.properties.end() &&
        found->second == automationId) {
        return &element;
    }
    for (const auto& child : element.children) {
        if (const auto* match =
                find_uia_element_by_automation_id(child, automationId)) {
            return match;
        }
    }
    return nullptr;
}

} // namespace

TEST_F(WinUI3SampleFixture, UiaTreeExposesAutomationIdsAndControlTypes) {
    SkipIfNotReady();

    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg() + " --uia"));
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded()) << "--uia did not produce JSON:\n" << output;
    ASSERT_TRUE(j.contains("root"));

    auto* button = find_by_automation_id(j["root"], "PrimaryButton");
    ASSERT_NE(button, nullptr) << "PrimaryButton not found in the UIA tree";
    EXPECT_EQ(button->value("type", ""), "Button");  // ControlType is promoted to "type"
    EXPECT_EQ(uia_prop(*button, "framework"), "");
    EXPECT_EQ(button->value("framework", ""), "uia");
    EXPECT_EQ(uia_prop(*button, "FrameworkId"), "XAML");
    // Invoke is what makes the button actionable; it must be advertised.
    EXPECT_NE(uia_prop(*button, "SupportedPatterns").find("Invoke"), std::string::npos)
        << "SupportedPatterns=" << uia_prop(*button, "SupportedPatterns");

    auto* box = find_by_automation_id(j["root"], "InputBox");
    ASSERT_NE(box, nullptr);
    EXPECT_EQ(box->value("type", ""), "Edit");
    EXPECT_NE(uia_prop(*box, "SupportedPatterns").find("Value"), std::string::npos);

    auto* check = find_by_automation_id(j["root"], "ReadyCheckBox");
    ASSERT_NE(check, nullptr);
    EXPECT_EQ(check->value("type", ""), "CheckBox");
    EXPECT_NE(uia_prop(*check, "SupportedPatterns").find("Toggle"), std::string::npos);
}

TEST_F(WinUI3SampleFixture, UiaTreeGatesPatternPropertiesByPatternSupport) {
    SkipIfNotReady();

    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg() + " --uia"));
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded());

    // UIA answers pattern-backed properties on every element regardless of
    // support. Without gating, a Button reports Toggle.ToggleState and the
    // output becomes mostly noise.
    auto* button = find_by_automation_id(j["root"], "PrimaryButton");
    ASSERT_NE(button, nullptr);
    EXPECT_FALSE(uia_has_prop(*button, "Toggle.ToggleState"));
    EXPECT_FALSE(uia_has_prop(*button, "Grid.RowCount"));
    EXPECT_FALSE(uia_has_prop(*button, "RangeValue.Value"));

    // The checkbox supports Toggle, so there it is real data.
    auto* check = find_by_automation_id(j["root"], "ReadyCheckBox");
    ASSERT_NE(check, nullptr);
    ASSERT_TRUE(uia_has_prop(*check, "Toggle.ToggleState"));
    EXPECT_EQ(uia_prop(*check, "Toggle.ToggleState"), "On");

    auto* box = find_by_automation_id(j["root"], "InputBox");
    ASSERT_NE(box, nullptr);
    ASSERT_TRUE(uia_has_prop(*box, "Value.Value"));
    EXPECT_FALSE(uia_has_prop(*box, "Toggle.ToggleState"));
}

TEST_F(WinUI3SampleFixture, UiaViewsNarrowFromRawToContent) {
    SkipIfNotReady();

    auto lvt = get_lvt_path();
    auto nodesFor = [&](const std::string& view) -> size_t {
        auto output = run_command(make_cmd(lvt, get_pid_arg() + " --uia --uia-view " + view));
        auto j = json::parse(output, nullptr, false);
        EXPECT_FALSE(j.is_discarded()) << "--uia-view " << view << " produced no JSON";
        if (j.is_discarded() || !j.contains("root"))
            return 0;
        return count_json_nodes(j["root"]);
    };

    const size_t raw = nodesFor("raw");
    const size_t control = nodesFor("control");
    const size_t content = nodesFor("content");

    ASSERT_GT(content, 0u);
    EXPECT_GE(raw, control) << "raw view should be at least as large as control";
    EXPECT_GE(control, content) << "control view should be at least as large as content";
    EXPECT_GT(raw, content) << "raw and content should differ on a real XAML tree";
}

TEST_F(WinUI3SampleFixture, UiaElementsCarryDurableKeysAndResolveByRuntimeId) {
    SkipIfNotReady();

    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg() + " --uia"));
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded());

    auto* button = find_by_automation_id(j["root"], "PrimaryButton");
    ASSERT_NE(button, nullptr);

    // element_key.cpp prefers a property named "AutomationId", so a UIA element
    // with one gets a durable key derived from it for free.
    auto key = button->value("key", "");
    ASSERT_FALSE(key.empty());
    EXPECT_NE(key.find("AutomationId:PrimaryButton"), std::string::npos) << "key=" << key;

    auto runtimeId = uia_prop(*button, "RuntimeId");
    ASSERT_FALSE(runtimeId.empty());

    auto queried = run_command(make_cmd(
        lvt, get_pid_arg() + " query uia:" + runtimeId + " AutomationId"));
    // Trim the trailing newline the CLI writes.
    while (!queried.empty() && (queried.back() == '\n' || queried.back() == '\r'))
        queried.pop_back();
    EXPECT_EQ(queried, "PrimaryButton");
}

TEST_F(WinUI3SampleFixture, UiaExtraPropertiesAreOptInAndReportUnknownNames) {
    SkipIfNotReady();

    auto lvt = get_lvt_path();

    // ProviderDescription is verbose, so it is not in the default set.
    auto defaultOut = run_command(make_cmd(lvt, get_pid_arg() + " --uia"));
    auto defaultJson = json::parse(defaultOut, nullptr, false);
    ASSERT_FALSE(defaultJson.is_discarded());
    auto* defaultButton = find_by_automation_id(defaultJson["root"], "PrimaryButton");
    ASSERT_NE(defaultButton, nullptr);
    EXPECT_FALSE(uia_has_prop(*defaultButton, "ProviderDescription"));

    // Asking for it by name brings it back.
    auto withProps = run_command(make_cmd(
        lvt, get_pid_arg() + " --uia --uia-props ProviderDescription"));
    auto withPropsJson = json::parse(withProps, nullptr, false);
    ASSERT_FALSE(withPropsJson.is_discarded());
    auto* richButton = find_by_automation_id(withPropsJson["root"], "PrimaryButton");
    ASSERT_NE(richButton, nullptr);
    EXPECT_TRUE(uia_has_prop(*richButton, "ProviderDescription"));

    // An explicitly requested property also bypasses the unset-value
    // suppression: naming one means wanting it even at its default.
    auto forced = json::parse(
        run_command(make_cmd(lvt, get_pid_arg() + " --uia --uia-props LandmarkType")),
        nullptr, false);
    ASSERT_FALSE(forced.is_discarded());
    EXPECT_TRUE(uia_has_prop(forced["root"], "LandmarkType"))
        << "--uia-props should override sentinel suppression";

    // And an unknown name is reported on stderr without failing the walk —
    // the other half of what this test's name promises.
    auto unknown = run_command(make_cmd(
        lvt, get_pid_arg() + " --uia --uia-props NotARealProperty") + " 2>&1");
    EXPECT_NE(unknown.find("unknown UIA property 'NotARealProperty'"), std::string::npos)
        << "an unknown property name should be reported";
    EXPECT_NE(unknown.find("\"root\""), std::string::npos)
        << "an unknown property name must not abort the walk";
}

TEST_F(WinUI3SampleFixture, UiaWorksWithElementScopingAndXmlFormat) {
    SkipIfNotReady();

    auto lvt = get_lvt_path();
    auto full = json::parse(run_command(make_cmd(lvt, get_pid_arg() + " --uia")), nullptr, false);
    ASSERT_FALSE(full.is_discarded());
    auto* box = find_by_automation_id(full["root"], "InputBox");
    ASSERT_NE(box, nullptr);
    const auto boxId = box->value("id", "");
    ASSERT_FALSE(boxId.empty());

    // --element scopes the UIA tree the same way it scopes the visual tree.
    auto scoped = json::parse(
        run_command(make_cmd(lvt, get_pid_arg() + " --uia --element " + boxId)), nullptr, false);
    ASSERT_FALSE(scoped.is_discarded()) << "--uia --element produced no JSON";
    ASSERT_TRUE(scoped.contains("root"));
    EXPECT_EQ(uia_prop(scoped["root"], "AutomationId"), "InputBox");
    EXPECT_LT(count_json_nodes(scoped["root"]), count_json_nodes(full["root"]));

    // XML is a supported output format for the UIA tree too.
    auto xml = run_command(make_cmd(lvt, get_pid_arg() + " --uia --format xml"));
    EXPECT_NE(xml.find("<LiveVisualTree"), std::string::npos) << xml.substr(0, 200);
    EXPECT_NE(xml.find("AutomationId=\"InputBox\""), std::string::npos);
    EXPECT_NE(xml.find("frameworks=\"uia (control view)\""), std::string::npos);
}

#ifndef NDEBUG
TEST_F(WinUI3SampleFixture, UiaTreeDrivesScreenshotAnnotations) {
    SkipIfNotReady();

    // UIA BoundingRectangle is already in screen coordinates, which is why
    // annotation works unchanged. Assert that rather than assuming it.
    auto lvt = get_lvt_path();
    auto annFile = fs::path(lvt).parent_path() / "lvt_uia_annotations.json";
    fs::remove(annFile);

    run_command(make_cmd(lvt,
        get_pid_arg() + " dump --uia --annotations-json " + annFile.string()));

    ASSERT_TRUE(fs::exists(annFile)) << "no annotations produced for a UIA tree";
    std::string content;
    {
        std::ifstream f(annFile);
        content.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }  // close before removing
    auto aj = json::parse(content, nullptr, false);
    fs::remove(annFile);

    ASSERT_FALSE(aj.is_discarded());
    ASSERT_TRUE(aj.is_array());
    EXPECT_GT(aj.size(), 0u) << "UIA elements produced no annotation rectangles";
    for (const auto& a : aj) {
        EXPECT_FALSE(a.value("id", "").empty());
        EXPECT_GT(a.value("width", 0), 0);
        EXPECT_GT(a.value("height", 0), 0);
    }
}
#endif

TEST_F(WinUI3SampleFixture, UiaWalkDegradesGracefullyOnDeadline) {
    SkipIfNotReady();

    // A cross-process UIA call can block indefinitely on a wedged target, so
    // the walk is bounded. Assert the bound actually bites and is *visible*:
    // for a machine consumer, a silently shortened tree that parses fine is
    // worse than an error.
    auto lvt = get_lvt_path();

    auto full = json::parse(run_command(make_cmd(lvt, get_pid_arg() + " --uia")), nullptr, false);
    ASSERT_FALSE(full.is_discarded());
    const size_t fullNodes = count_json_nodes(full["root"]);
    ASSERT_GT(fullNodes, 1u);
    EXPECT_FALSE(uia_has_prop(full["root"], "Truncated"))
        << "a complete walk must not be marked truncated";

    auto clipped = json::parse(
        run_command(make_cmd(lvt, get_pid_arg() + " --uia --uia-timeout 1")), nullptr, false);
    ASSERT_FALSE(clipped.is_discarded())
        << "a hit deadline must not corrupt the emitted tree";
    ASSERT_TRUE(clipped.contains("root"));

    // The deadline must have had an effect, and said so in the document.
    EXPECT_LT(count_json_nodes(clipped["root"]), fullNodes)
        << "--uia-timeout 1 produced a full tree, so the deadline is not bounding anything";
    EXPECT_EQ(uia_prop(clipped["root"], "Truncated"), "true")
        << "a truncated tree must be self-describing, not silently short";
}

TEST_F(WinUI3SampleFixture, UiaWatchEmitsAddedEvents) {
    SkipIfNotReady();

    // --watch runs until Ctrl+C, so it must be driven with an explicit pipe and
    // terminated: _popen/_pclose would block forever waiting for a process that
    // never exits. The first tick emits the current tree as "added" events,
    // which is enough to prove --uia is wired into the watch path and not only
    // the one-shot dump path.
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    wil::unique_handle readEnd, writeEnd;
    ASSERT_TRUE(CreatePipe(readEnd.put(), writeEnd.put(), &sa, 0));
    ASSERT_TRUE(SetHandleInformation(readEnd.get(), HANDLE_FLAG_INHERIT, 0));

    STARTUPINFOA si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd.get();
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};

    auto lvt = get_lvt_path();
    std::string cmd = make_cmd(lvt, get_pid_arg() + " watch --uia --interval 500");
    ASSERT_TRUE(CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    wil::unique_handle process(pi.hProcess);
    wil::unique_handle thread(pi.hThread);
    writeEnd.reset();  // so ReadFile sees EOF if the child ever exits

    // Read whatever the first tick produces, bounded by a deadline. Stop once a
    // *complete* line mentioning the button has arrived, so the parse below is
    // never handed a half-read line.
    auto have_complete_button_line = [](const std::string& s) {
        const auto at = s.find("PrimaryButton");
        return at != std::string::npos && s.find('\n', at) != std::string::npos;
    };

    std::string output;
    const auto deadline = GetTickCount64() + 20000;
    while (GetTickCount64() < deadline && !have_complete_button_line(output)) {
        DWORD available = 0;
        if (!PeekNamedPipe(readEnd.get(), nullptr, 0, nullptr, &available, nullptr))
            break;
        if (available == 0) {
            Sleep(50);
            continue;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!ReadFile(readEnd.get(), chunk.data(), available, &read, nullptr) || read == 0)
            break;
        output.append(chunk, 0, read);
    }

    TerminateProcess(process.get(), 0);
    WaitForSingleObject(process.get(), 5000);

    ASSERT_FALSE(output.empty()) << "--uia --watch emitted nothing";
    EXPECT_TRUE(have_complete_button_line(output))
        << "watch events did not include the UIA tree's controls";

    // Every complete line must be a well-formed change event. Drop anything
    // after the final newline: that is a partially read line, not malformed output.
    const auto lastNewline = output.rfind('\n');
    ASSERT_NE(lastNewline, std::string::npos) << "no complete line was read";
    std::istringstream lines(output.substr(0, lastNewline));
    std::string line;
    int events = 0;
    while (std::getline(lines, line)) {
        if (line.empty())
            continue;
        auto ev = json::parse(line, nullptr, false);
        ASSERT_FALSE(ev.is_discarded())
            << "watch emitted non-JSON: " << line.substr(0, 200);
        EXPECT_EQ(ev.value("event", ""), "added");
        ASSERT_TRUE(ev.contains("element"));
        ++events;
    }
    EXPECT_GT(events, 0) << "no complete watch events parsed";
}

#ifdef LVT_ENABLE_UIA
TEST_F(
    WinUI3SampleFixture,
    UiaStructureEventsCoverChildRemovalAdditionAndReorder) {
    SkipIfNotReady();
    const HWND hwnd = visible_window_for_pid(s_pid);
    ASSERT_NE(hwnd, nullptr);
    auto connection = lvt::UiaConnection::connect(hwnd, s_pid);
    ASSERT_NE(connection, nullptr);
    drain_uia_events(*connection);

    const auto read_list = [&]() {
        lvt::Element tree;
        EXPECT_TRUE(connection->get_tree(tree, false));
        const auto* list =
            find_uia_element_by_automation_id(tree, "ItemsList");
        EXPECT_NE(list, nullptr);
        return list ? *list : lvt::Element{};
    };
    const auto invoke = [&](const std::string& automationId) {
        auto tree = json::parse(
            run_command(make_cmd(
                get_lvt_path(), get_pid_arg() + " dump --uia")),
            nullptr, false);
        EXPECT_FALSE(tree.is_discarded());
        const auto* button =
            tree.is_discarded()
                ? nullptr
                : find_by_automation_id(tree["root"], automationId);
        EXPECT_NE(button, nullptr);
        if (!button)
            return false;
        const auto runtimeId = uia_prop(*button, "RuntimeId");
        auto result = json::parse(
            run_command(make_cmd(
                get_lvt_path(),
                get_pid_arg() + " invoke uia:" + runtimeId)),
            nullptr, false);
        EXPECT_FALSE(result.is_discarded());
        EXPECT_TRUE(result.value("ok", false)) << result.dump(2);
        return result.value("ok", false);
    };

    const auto large = read_list();
    ASSERT_GT(large.children.size(), 1u);

    ASSERT_TRUE(invoke("ToggleListSizeButton"));
    EXPECT_TRUE(wait_for_uia_snapshot(*connection))
        << "removing UIA list children did not request a snapshot";
    const auto small = read_list();
    EXPECT_LT(small.children.size(), large.children.size());

    drain_uia_events(*connection);
    ASSERT_TRUE(invoke("ToggleListSizeButton"));
    EXPECT_TRUE(wait_for_uia_snapshot(*connection))
        << "adding UIA list children did not request a snapshot";
    const auto restored = read_list();
    EXPECT_GT(restored.children.size(), small.children.size());

    const std::string firstBefore =
        restored.children.empty()
            ? std::string()
            : restored.children.front().text;
    drain_uia_events(*connection);
    ASSERT_TRUE(invoke("ReorderListButton"));
    EXPECT_TRUE(wait_for_uia_snapshot(*connection))
        << "reordering UIA list children did not request a snapshot";
    const auto reordered = read_list();
    ASSERT_FALSE(reordered.children.empty());
    EXPECT_NE(reordered.children.front().text, firstBefore);

    ASSERT_TRUE(invoke("ReorderListButton"));
}
#endif

// Regression test for the persistent-connection redesign (see
// connection_registry.h / xaml_diag_common.cpp's XamlDiagConnection): watch
// must inject and subscribe (InitializeXamlDiagnosticsEx + AdviseVisualTreeChange)
// exactly ONCE for the life of a session, not once per tick. That per-tick
// re-injection was the confirmed root cause of an unbounded resource leak
// (a message-only window created and never destroyed every tick) and the
// "tree refreshes/resets" bug diagnosed live against Microsoft Store.
TEST_F(WinUI3SampleFixture, WatchReusesOnePersistentConnectionAcrossManyTicks) {
    SkipIfNotReady();

    // The sample app here is unpackaged (plain CreateProcessA, no AppContainer),
    // so its TAP DLL logs to the simple, global %TEMP%\lvt_tap.log - counting
    // "SetSite called" lines before and after running several ticks is a
    // direct, unambiguous measure of how many times the TAP DLL was actually
    // (re)injected, which nothing observable from outside the process
    // (stdout, the CLI's own exit code) can distinguish otherwise.
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring logPath = std::wstring(tempPath) + L"lvt_tap.log";

    auto count_set_site_calls = [&]() -> int {
        std::ifstream log(logPath, std::ios::binary);
        if (!log)
            return 0;
        int count = 0;
        std::string line;
        while (std::getline(log, line)) {
            if (line.find("SetSite called") != std::string::npos)
                count++;
        }
        return count;
    };

    const int before = count_set_site_calls();

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    wil::unique_handle readEnd, writeEnd;
    ASSERT_TRUE(CreatePipe(readEnd.put(), writeEnd.put(), &sa, 0));
    ASSERT_TRUE(SetHandleInformation(readEnd.get(), HANDLE_FLAG_INHERIT, 0));

    STARTUPINFOA si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd.get();
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};

    auto lvt = get_lvt_path();
    std::string cmd = make_cmd(lvt, get_pid_arg() + " watch --fast --interval 300");
    ASSERT_TRUE(CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                               CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    wil::unique_handle process(pi.hProcess);
    wil::unique_handle thread(pi.hThread);
    writeEnd.reset();  // so ReadFile sees EOF if the child ever exits

    // Drain output for several seconds so multiple ticks genuinely happen -
    // event content/correctness is already covered elsewhere; only that
    // real time passes while the connection stays open and keeps serving
    // requests matters for this test.
    std::string output;
    const auto deadline = GetTickCount64() + 6000;
    while (GetTickCount64() < deadline) {
        DWORD available = 0;
        if (!PeekNamedPipe(readEnd.get(), nullptr, 0, nullptr, &available, nullptr))
            break;
        if (available == 0) {
            Sleep(50);
            continue;
        }
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!ReadFile(readEnd.get(), chunk.data(), available, &read, nullptr) || read == 0)
            break;
        output.append(chunk, 0, read);
    }

    TerminateProcess(process.get(), 0);
    WaitForSingleObject(process.get(), 5000);

    ASSERT_FALSE(output.empty()) << "watch emitted nothing";

    const int after = count_set_site_calls();
    // Exactly one new connection is expected for this whole session. This
    // used to allow two as a retry margin, which let a real startup bug
    // escape: acquire_watch_connections built its CoreWindow probe using
    // the full detected framework list, causing one complete throwaway
    // injection before opening the persistent connection. The probe is now
    // deliberately Win32-only; any second SetSite here is a regression.
    EXPECT_EQ(after - before, 1)
        << "watch must inject exactly once (SetSite called " << before << " times before, "
        << after << " times after)";
}

// Proof-of-safety test for the non-unload policy described in
// docs/tap-dll-design.md § "Module lifetime and why DllCanUnloadNow returns
// S_FALSE".  Each lvt dump is a separate process: it injects (or re-activates
// an existing session via SetSite), collects the tree, and exits — closing the
// pipe and triggering CleanupUIResources in the TAP DLL. The test verifies:
//   (a) the target process survives every round,
//   (b) each round returns a valid WinUI3 tree with the expected controls, and
//   (c) every message window created by the TAP DLL produces a matching
//       "Cleanup: message window destroyed" log entry, proving teardown
//       properly releases active resources without leaking windows.
//
// Log filtering is scoped to s_pi.dwProcessId via the "][<pid>]["
// process marker that LogMsg emits. Concurrent TAP sessions in other
// processes do not affect the counts.
//
// To run in isolation:
//   ctest --test-dir build -R RepeatedDumpCycles
// or:
//   lvt_integration_tests --gtest_filter=WinUI3SampleFixture.RepeatedDumpCyclesPreserveTargetAndCleanUpResources
TEST_F(WinUI3SampleFixture, RepeatedDumpCyclesPreserveTargetAndCleanUpResources) {
    SkipIfNotReady();

    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    const std::wstring logPath = std::wstring(tempPath) + L"lvt_tap.log";

    // The TAP log process marker for this target: "][<pid>][". Any other
    // process's TAP entries are excluded from all counts.
    const std::string process_marker =
        "][" + std::to_string(s_pi.dwProcessId) + "][";

    // Count log lines that match both the PID marker and a substring pattern.
    const auto count_pid_log = [&](const char* pattern) -> int {
        std::ifstream log(logPath, std::ios::binary);
        if (!log) return 0;
        int n = 0;
        std::string line;
        while (std::getline(log, line)) {
            if (line.find(process_marker) != std::string::npos &&
                line.find(pattern) != std::string::npos)
                ++n;
        }
        return n;
    };

    // Baseline before this test.  The fixture's SetUpTestSuite already ran
    // one or more dumps to verify readiness; those may have left entries for
    // this PID in the log.  We compare deltas, not absolute values.
    const int created_before = count_pid_log("Created message window");
    const int cleanup_before = count_pid_log("Cleanup: message window destroyed");

    // Poll until the PID-filtered cleanup count reaches `expected_total`, or
    // until `timeout_ms` elapses.  Returns true if the count was reached.
    const auto wait_for_cleanup = [&](int expected_total, int timeout_ms) -> bool {
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        while (true) {
            if (count_pid_log("Cleanup: message window destroyed") >= expected_total)
                return true;
            if (GetTickCount64() >= deadline)
                return false;
            Sleep(100);
        }
    };

    const auto lvt = get_lvt_path();
    constexpr int kRounds = 5;

    for (int round = 0; round < kRounds; ++round) {
        // Run a complete dump: inject (or reuse the session), collect the
        // tree, then exit — which breaks the pipe and triggers CleanupUIResources.
        const auto output = run_command(make_cmd(lvt, get_pid_arg()));

        ASSERT_EQ(WaitForSingleObject(s_pi.hProcess, 0), WAIT_TIMEOUT)
            << "target process crashed on dump cycle " << round + 1;

        auto j = json::parse(output, nullptr, false);
        ASSERT_FALSE(j.is_discarded())
            << "dump on cycle " << round + 1 << " returned non-JSON output";
        ASSERT_TRUE(j.contains("root"))
            << "dump on cycle " << round + 1 << " is missing the root element";
        EXPECT_TRUE(frameworks_contain_winui3(j))
            << "dump on cycle " << round + 1 << " lost WinUI3 framework detection";
        EXPECT_TRUE(json_tree_has_named_control(j["root"], "PrimaryButton"))
            << "dump on cycle " << round + 1 << " lost PrimaryButton control";

        // Wait for CleanupUIResources to complete for this round.
        // The TAP DLL's SendMessageTimeout has a 2 s budget; allow 3 s total.
        // EXPECT (not ASSERT) so we still run the target-survival check below.
        EXPECT_TRUE(wait_for_cleanup(cleanup_before + round + 1, 3000))
            << "cleanup did not complete within 3 s for cycle " << round + 1
            << " (target PID " << s_pi.dwProcessId << ")";
    }

    // Final resource-balance check: every message window that was created in
    // this test run must have been destroyed exactly once.  A deficit means
    // a leaked window; a surplus is impossible.
    const int new_created = count_pid_log("Created message window")  - created_before;
    const int new_cleaned = count_pid_log("Cleanup: message window destroyed") - cleanup_before;

    EXPECT_GE(new_created, 1)
        << "expected at least one message window across " << kRounds << " dump cycles "
           "(target PID " << s_pi.dwProcessId << ")";
    EXPECT_EQ(new_created, new_cleaned)
        << "every created message window must be destroyed exactly once; "
           "mismatch indicates a leak ("
        << new_created << " created, " << new_cleaned << " destroyed, "
           "target PID " << s_pi.dwProcessId << ")";

    // Target must still be alive after all cycles.
    EXPECT_EQ(WaitForSingleObject(s_pi.hProcess, 0), WAIT_TIMEOUT)
        << "target process crashed during or after " << kRounds << " dump cycles";
}

// --- Cross-architecture behaviour ---
// --uia injects nothing, so it is exempt from the architecture-match check that
// the visual-tree providers need. Both halves of that claim are asserted here,
// against a real 32-bit process.

namespace {

// Returns a 32-bit system executable with a UI, or empty if none is available.
std::string find_wow64_app() {
    for (const char* name : {"charmap.exe", "notepad.exe", "mspaint.exe"}) {
        auto candidate = fs::path("C:\\Windows\\SysWOW64") / name;
        if (fs::exists(candidate))
            return candidate.string();
    }
    return {};
}

bool is_wow64(HANDLE process) {
    BOOL wow64 = FALSE;
    return IsWow64Process(process, &wow64) && wow64;
}

} // namespace

class Wow64TargetFixture : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        auto exe = find_wow64_app();
        if (exe.empty()) {
            s_skip_reason = "no 32-bit system app available on this machine";
            return;
        }
        if (sizeof(void*) == 4) {
            // These tests are built for the same architecture as lvt.exe, so a
            // 32-bit build has no mismatch to observe against a 32-bit target.
            s_skip_reason = "host is 32-bit, so a 32-bit target is not a mismatch";
            return;
        }

        STARTUPINFOA si = {sizeof(si)};
        PROCESS_INFORMATION pi = {};
        std::string cmd = "\"" + exe + "\"";
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, nullptr, &si, &pi)) {
            s_skip_reason = "failed to launch " + exe;
            return;
        }
        s_process.reset(pi.hProcess);
        s_thread.reset(pi.hThread);
        s_pid = pi.dwProcessId;
        WaitForInputIdle(s_process.get(), 10000);

        if (!is_wow64(s_process.get())) {
            s_skip_reason = exe + " did not start as a 32-bit process";
            return;
        }

        // Wait for a top-level window so lvt has something to resolve.
        auto lvt = get_lvt_path();
        for (int attempt = 0; attempt < 30; ++attempt) {
            auto probe = run_command(make_cmd(lvt, get_pid_arg() + " --uia --depth 0"));
            auto j = json::parse(probe, nullptr, false);
            if (!j.is_discarded() && j.contains("root")) {
                s_ready = true;
                return;
            }
            Sleep(500);
        }
        s_skip_reason = "32-bit target never exposed a resolvable window";
    }

    static void TearDownTestSuite() {
        if (s_process) {
            TerminateProcess(s_process.get(), 0);
            s_process.reset();
            s_thread.reset();
        }
    }

    static void SkipIfNotReady() {
        if (!s_ready)
            GTEST_SKIP() << s_skip_reason;
    }

    static std::string get_pid_arg() { return "--pid " + std::to_string(s_pid); }

    static wil::unique_handle s_process;
    static wil::unique_handle s_thread;
    static DWORD s_pid;
    static bool s_ready;
    static std::string s_skip_reason;
};

wil::unique_handle Wow64TargetFixture::s_process;
wil::unique_handle Wow64TargetFixture::s_thread;
DWORD Wow64TargetFixture::s_pid = 0;
bool Wow64TargetFixture::s_ready = false;
std::string Wow64TargetFixture::s_skip_reason;

TEST_F(Wow64TargetFixture, VisualTreeReportsArchitectureMismatch) {
    SkipIfNotReady();

    // IMAGE_FILE_MACHINE_I386 used to be unhandled, so a 32-bit target was
    // classified as the host architecture and this check never fired at all.
    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg() + " --depth 0") + " 2>&1");

    EXPECT_NE(output.find("architecture mismatch"), std::string::npos)
        << "expected a mismatch against a 32-bit target, got:\n" << output;
    EXPECT_NE(output.find("x86"), std::string::npos)
        << "the target architecture should be named:\n" << output;
    // The message should point at the way forward.
    EXPECT_NE(output.find("--uia"), std::string::npos)
        << "the mismatch message should mention the cross-architecture option";
}

TEST_F(Wow64TargetFixture, UiaReadsAcrossArchitectures) {
    SkipIfNotReady();

    auto lvt = get_lvt_path();
    auto output = run_command(make_cmd(lvt, get_pid_arg() + " --uia"));
    auto j = json::parse(output, nullptr, false);
    ASSERT_FALSE(j.is_discarded())
        << "--uia should work against a 32-bit target:\n" << output;
    ASSERT_TRUE(j.contains("root"));

    // Not just a bare root: real automation data must come across the boundary.
    const auto& root = j["root"];
    EXPECT_FALSE(root.value("id", "").empty());
    EXPECT_FALSE(root.value("type", "").empty());  // ControlType is promoted to "type"
    EXPECT_FALSE(uia_prop(root, "RuntimeId").empty());
    EXPECT_GT(count_json_nodes(root), 1u) << "expected child elements from the 32-bit target";
}

TEST_F(WinUI3SampleFixture, UiaEmitsEnumNamesNotRawNumbers) {
    SkipIfNotReady();

    // The unit tests pin each mapping; this asserts the mappings are actually
    // reached on a live tree, and that no enum-valued property leaks an integer.
    auto lvt = get_lvt_path();
    auto j = json::parse(run_command(make_cmd(lvt, get_pid_arg() + " --uia")), nullptr, false);
    ASSERT_FALSE(j.is_discarded());

    auto* window = &j["root"];
    ASSERT_TRUE(window->contains("properties"));

    // Used to be emitted raw as "2".
    EXPECT_EQ(uia_prop(*window, "Window.WindowInteractionState"), "ReadyForUserInteraction");

    auto* check = find_by_automation_id(j["root"], "ReadyCheckBox");
    ASSERT_NE(check, nullptr);
    EXPECT_EQ(uia_prop(*check, "Toggle.ToggleState"), "On");
    // Culture is an LCID, previously emitted as "1033". Assert it resolved to a
    // BCP-47 tag rather than a specific locale: the value depends on the
    // machine's language, and the exact LCID mapping is pinned by unit tests.
    const auto culture = uia_prop(*check, "Culture");
    ASSERT_FALSE(culture.empty());
    EXPECT_EQ(culture.find_first_of("0123456789"), std::string::npos)
        << "Culture should be a locale name, not a raw LCID: " << culture;
    EXPECT_NE(culture.find('-'), std::string::npos) << culture;

    // Properties that are unset must be absent, not rendered as their default.
    // This is the seam where LandmarkType shipped as "LandmarkType(0)" and
    // LiveSetting as "Off" on every single element.
    EXPECT_FALSE(uia_has_prop(*check, "LandmarkType"))
        << "an unset LandmarkType must be omitted, not rendered";
    EXPECT_FALSE(uia_has_prop(*check, "LiveSetting"))
        << "an unset LiveSetting must be omitted, not rendered";
    EXPECT_FALSE(uia_has_prop(*check, "Orientation"))
        << "an unset Orientation must be omitted, not rendered";

    // ControlType is promoted to Element::type, so it must not also appear in
    // the property map.
    EXPECT_FALSE(uia_has_prop(*check, "ControlType"))
        << "ControlType is promoted to \"type\" and must not be duplicated";
    EXPECT_EQ(check->value("type", ""), "CheckBox");

    // Sweep the whole tree: no enum-valued property may render as a bare number.
    static const char* enumProps[] = {
        "Toggle.ToggleState", "ExpandCollapse.State", "Orientation",
        "Window.WindowVisualState", "Window.WindowInteractionState",
        "Table.RowOrColumnMajor", "LiveSetting", "LandmarkType",
    };
    std::vector<const json*> stack{&j["root"]};
    int checked = 0;
    while (!stack.empty()) {
        const json* node = stack.back();
        stack.pop_back();
        for (const char* prop : enumProps) {
            const auto value = uia_prop(*node, prop);
            if (value.empty())
                continue;
            ++checked;
            const bool numeric = value.find_first_not_of("-0123456789") == std::string::npos;
            EXPECT_FALSE(numeric) << prop << " rendered as a raw number: " << value;
            // "EnumName(n)" means lvt did not recognise the value. On a stock
            // control that is a mapping gap, not a legitimate rendering — this
            // is what let LandmarkType="LandmarkType(0)" ship green.
            EXPECT_EQ(value.find('('), std::string::npos)
                << prop << " fell back to the unrecognised-value form: " << value;
        }
        if (node->contains("children")) {
            for (const auto& child : (*node)["children"])
                stack.push_back(&child);
        }
    }
    EXPECT_GT(checked, 0) << "no enum-valued properties were present to check";
}

// --- Interaction verbs ---
// These assert that lvt actually drives the app, not merely that a command
// returned success: each checks an observable change in the target's own tree.

namespace {

json run_action_json(const std::string& lvt, const std::string& args) {
    auto output = run_command(make_cmd(lvt, args));
    auto j = json::parse(output, nullptr, false);
    EXPECT_FALSE(j.is_discarded()) << "action produced no JSON:\n" << output;
    return j;
}

// Read one AutomationId's element out of a fresh UIA walk.
json uia_element(const std::string& lvt, const std::string& pidArg,
                 const std::string& automationId) {
    auto tree = json::parse(run_command(make_cmd(lvt, pidArg + " dump --uia")), nullptr, false);
    if (tree.is_discarded() || !tree.contains("root"))
        return json();
    const json* found = find_by_automation_id(tree["root"], automationId);
    return found ? *found : json();
}

} // namespace

TEST_F(WinUI3SampleFixture, ClickInvokesThroughAPatternAndChangesTheApp) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    const auto before = uia_element(lvt, get_pid_arg(), "StatusText");
    ASSERT_FALSE(before.is_null());
    const auto beforeText = before.value("text", "");
    ASSERT_FALSE(beforeText.empty());

    auto button = uia_element(lvt, get_pid_arg(), "PrimaryButton");
    ASSERT_FALSE(button.is_null());
    const auto ref = "uia:" + uia_prop(button, "RuntimeId");

    auto result = run_action_json(lvt, get_pid_arg() + " click " + ref);
    EXPECT_TRUE(result.value("ok", false)) << result.dump(2);
    // A button exposes Invoke, so this must not fall back to synthetic input:
    // the pattern route neither steals focus nor moves the cursor.
    EXPECT_EQ(result.value("method", ""), "InvokePattern");

    const auto after = uia_element(lvt, get_pid_arg(), "StatusText");
    ASSERT_FALSE(after.is_null());
    EXPECT_NE(after.value("text", ""), beforeText)
        << "the click reported success but the app did not react";
}

TEST_F(WinUI3SampleFixture, ToggleFlipsCheckboxState) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    auto before = uia_element(lvt, get_pid_arg(), "ReadyCheckBox");
    ASSERT_FALSE(before.is_null());
    const auto beforeState = uia_prop(before, "Toggle.ToggleState");
    ASSERT_FALSE(beforeState.empty());
    const auto ref = "uia:" + uia_prop(before, "RuntimeId");

    auto result = run_action_json(lvt, get_pid_arg() + " toggle " + ref);
    EXPECT_TRUE(result.value("ok", false)) << result.dump(2);
    EXPECT_EQ(result.value("method", ""), "TogglePattern");

    auto after = uia_element(lvt, get_pid_arg(), "ReadyCheckBox");
    ASSERT_FALSE(after.is_null());
    EXPECT_NE(uia_prop(after, "Toggle.ToggleState"), beforeState);

    // Put it back, so the ordering of tests in this fixture does not matter.
    run_command(make_cmd(lvt, get_pid_arg() + " toggle " + ref));
}

TEST_F(WinUI3SampleFixture, SetValueWritesThroughTheValuePattern) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    auto box = uia_element(lvt, get_pid_arg(), "InputBox");
    ASSERT_FALSE(box.is_null());
    const auto ref = "uia:" + uia_prop(box, "RuntimeId");
    const auto original = uia_prop(box, "Value.Value");

    auto result = run_action_json(lvt,
        get_pid_arg() + " set-value " + ref + " written-by-a-test");
    EXPECT_TRUE(result.value("ok", false)) << result.dump(2);
    EXPECT_EQ(result.value("method", ""), "ValuePattern");

    auto after = uia_element(lvt, get_pid_arg(), "InputBox");
    ASSERT_FALSE(after.is_null());
    EXPECT_EQ(uia_prop(after, "Value.Value"), "written-by-a-test");

    // Restore, so this fixture's tests do not depend on running order.
    if (!original.empty())
        run_command(make_cmd(lvt, get_pid_arg() + " set-value " + ref + " " +
                                      cmd_escape_arg(original)));
}

TEST_F(WinUI3SampleFixture, ActionResultReportsHowItWasPerformed) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    // A caller needs to distinguish a quiet pattern call from synthetic input,
    // because only the latter steals focus and depends on window ordering.
    auto button = uia_element(lvt, get_pid_arg(), "PrimaryButton");
    ASSERT_FALSE(button.is_null());
    const auto ref = "uia:" + uia_prop(button, "RuntimeId");

    const auto readStatus = [&] {
        auto status = uia_element(lvt, get_pid_arg(), "StatusText");
        return status.is_null() ? std::string() : status.value("text", "");
    };

    const auto beforePattern = readStatus();
    ASSERT_FALSE(beforePattern.empty());

    auto viaPattern = run_action_json(lvt, get_pid_arg() + " click " + ref);
    EXPECT_EQ(viaPattern.value("method", ""), "InvokePattern");
    EXPECT_NE(readStatus(), beforePattern) << "the pattern click did not reach the app";

    // double-click has no pattern equivalent, so it must say it used SendInput
    // rather than quietly invoking once and claiming success.
    const auto beforeInput = readStatus();
    auto viaInput = run_action_json(lvt, get_pid_arg() + " double-click " + ref);

    if (!viaInput.value("ok", false) &&
        viaInput.value("error", "").find("foreground") != std::string::npos) {
        // SetForegroundWindow is advisory: the shell refuses it when another
        // process owns the foreground, which on a shared machine can be
        // anything. Refusing is the correct behaviour — a synthetic click that
        // proceeded anyway would land on the wrong window — so this is an
        // environment limitation rather than a defect.
        GTEST_SKIP() << "another window held the foreground, so synthetic input was "
                        "correctly refused: " << viaInput.dump(2);
    }

    EXPECT_TRUE(viaInput.value("ok", false)) << viaInput.dump(2);
    EXPECT_EQ(viaInput.value("method", ""), "SendInput");
    // The label alone proves nothing: assert the app actually saw the click.
    // This is the only synthetic-input path in the suite, and so the one most
    // worth verifying by effect rather than by what it claims to have done.
    EXPECT_NE(readStatus(), beforeInput)
        << "the synthetic click reported success but the app did not react";
}

TEST_F(WinUI3SampleFixture, InvokeRefusesElementsWithoutThePattern) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    auto check = uia_element(lvt, get_pid_arg(), "ReadyCheckBox");
    ASSERT_FALSE(check.is_null());
    const auto ref = "uia:" + uia_prop(check, "RuntimeId");

    // A checkbox toggles, it does not invoke. The strict verb must say so
    // rather than falling back to a click that happens to work.
    auto result = run_action_json(lvt, get_pid_arg() + " invoke " + ref);
    EXPECT_FALSE(result.value("ok", true));
    EXPECT_NE(result.value("error", "").find("Invoke"), std::string::npos)
        << result.dump(2);
}

TEST_F(WinUI3SampleFixture, WindowVerbsChangeVisualState) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    auto minimized = run_action_json(lvt, get_pid_arg() + " minimize");
    EXPECT_TRUE(minimized.value("ok", false)) << minimized.dump(2);
    EXPECT_NE(minimized.value("method", "").find("WindowPattern"), std::string::npos);

    auto restored = run_action_json(lvt, get_pid_arg() + " restore");
    EXPECT_TRUE(restored.value("ok", false)) << restored.dump(2);

    auto tree = json::parse(run_command(make_cmd(lvt, get_pid_arg() + " dump --uia")),
                            nullptr, false);
    ASSERT_FALSE(tree.is_discarded());
    EXPECT_EQ(uia_prop(tree["root"], "Window.WindowVisualState"), "Normal");
}

TEST_F(WinUI3SampleFixture, WaitForReturnsImmediatelyWhenSatisfied) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    auto check = uia_element(lvt, get_pid_arg(), "ReadyCheckBox");
    ASSERT_FALSE(check.is_null());
    const auto ref = "uia:" + uia_prop(check, "RuntimeId");
    const auto state = uia_prop(check, "Toggle.ToggleState");

    auto present = run_action_json(lvt, get_pid_arg() + " wait-for " + ref);
    EXPECT_TRUE(present.value("ok", false)) << present.dump(2);

    auto withProp = run_action_json(lvt,
        get_pid_arg() + " wait-for " + ref + " --wait-prop Toggle.ToggleState=" + state);
    EXPECT_TRUE(withProp.value("ok", false)) << withProp.dump(2);
}

TEST_F(WinUI3SampleFixture, WaitForTimesOutWithAnActionableMessage) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    auto check = uia_element(lvt, get_pid_arg(), "ReadyCheckBox");
    ASSERT_FALSE(check.is_null());
    const auto ref = "uia:" + uia_prop(check, "RuntimeId");

    const auto start = GetTickCount64();
    auto result = run_action_json(lvt, get_pid_arg() + " wait-for " + ref +
        " --wait-prop Toggle.ToggleState=Indeterminate --wait-timeout 900");
    const auto elapsed = GetTickCount64() - start;

    EXPECT_FALSE(result.value("ok", true));
    EXPECT_NE(result.value("error", "").find("timed out"), std::string::npos)
        << result.dump(2);
    // It must actually wait rather than returning at once, and must not hang.
    EXPECT_GE(elapsed, 800u);
    EXPECT_LT(elapsed, 20000u);
}

TEST_F(WinUI3SampleFixture, ActionsReportFailureForUnknownElements) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    auto result = run_action_json(lvt, get_pid_arg() + " click uia:99.99.99");
    EXPECT_FALSE(result.value("ok", true));
    EXPECT_FALSE(result.value("error", "").empty());
}

TEST_F(WinUI3SampleFixture, SetValueFallsBackToRangeValueForNumericControls) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    // A slider carries its value through RangeValue, not Value, so set-value
    // has to try both before giving up and typing.
    auto slider = uia_element(lvt, get_pid_arg(), "LevelSlider");
    ASSERT_FALSE(slider.is_null()) << "sample app has no LevelSlider";
    const auto ref = "uia:" + uia_prop(slider, "RuntimeId");

    auto result = run_action_json(lvt, get_pid_arg() + " set-value " + ref + " 75");
    EXPECT_TRUE(result.value("ok", false)) << result.dump(2);
    EXPECT_EQ(result.value("method", ""), "RangeValuePattern");

    auto after = uia_element(lvt, get_pid_arg(), "LevelSlider");
    ASSERT_FALSE(after.is_null());
    EXPECT_EQ(uia_prop(after, "RangeValue.Value"), "75");
}

TEST_F(WinUI3SampleFixture, ExpandAndCollapseDriveTheExpandCollapsePattern) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    auto combo = uia_element(lvt, get_pid_arg(), "ChoiceCombo");
    ASSERT_FALSE(combo.is_null()) << "sample app has no ChoiceCombo";

    // Deliberately the durable key, not uia:<RuntimeId>. Expanding a combo box
    // reparents it into a popup, which changes both its eN id and the
    // HWND-derived part of its RuntimeId; only the durable key survives. That
    // is the whole reason durable keys exist.
    const auto ref = cmd_escape_arg(combo.value("key", ""));
    ASSERT_FALSE(ref.empty());

    auto expanded = run_action_json(lvt, get_pid_arg() + " --uia expand " + ref);
    EXPECT_TRUE(expanded.value("ok", false)) << expanded.dump(2);
    EXPECT_EQ(expanded.value("method", ""), "ExpandCollapsePattern");
    EXPECT_EQ(uia_prop(uia_element(lvt, get_pid_arg(), "ChoiceCombo"),
                       "ExpandCollapse.State"), "Expanded");

    auto collapsed = run_action_json(lvt, get_pid_arg() + " --uia collapse " + ref);
    EXPECT_TRUE(collapsed.value("ok", false)) << collapsed.dump(2);
    EXPECT_EQ(uia_prop(uia_element(lvt, get_pid_arg(), "ChoiceCombo"),
                       "ExpandCollapse.State"), "Collapsed");
}

TEST_F(WinUI3SampleFixture, DurableKeysOutliveReferencesThatStructureBreaks) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    // Pins the identity guidance the docs give, because it is not obvious.
    // Expanding a combo box reparents it into a popup. A durable key captured
    // beforehand still resolves to something actionable afterwards; the
    // RuntimeId captured at the same moment does not, because its host-window
    // component changed.
    auto before = uia_element(lvt, get_pid_arg(), "ChoiceCombo");
    ASSERT_FALSE(before.is_null());
    const auto key = before.value("key", "");
    const auto runtimeRef = "uia:" + uia_prop(before, "RuntimeId");
    ASSERT_FALSE(key.empty());

    auto expanded = run_action_json(lvt,
        get_pid_arg() + " --uia expand " + cmd_escape_arg(key));
    ASSERT_TRUE(expanded.value("ok", false)) << expanded.dump(2);

    // The pre-expand RuntimeId is now stale.
    auto viaRuntimeId = run_action_json(lvt,
        get_pid_arg() + " --uia collapse " + runtimeRef);
    EXPECT_FALSE(viaRuntimeId.value("ok", true))
        << "if RuntimeId survived reparenting here, the docs should stop warning about it";

    // The durable key captured before the change still works.
    auto viaKey = run_action_json(lvt,
        get_pid_arg() + " --uia collapse " + cmd_escape_arg(key));
    EXPECT_TRUE(viaKey.value("ok", false)) << viaKey.dump(2);
    EXPECT_EQ(uia_prop(uia_element(lvt, get_pid_arg(), "ChoiceCombo"),
                       "ExpandCollapse.State"), "Collapsed");
}

TEST_F(WinUI3SampleFixture, ScrollDrivesTheScrollPattern) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    auto list = uia_element(lvt, get_pid_arg(), "ItemsList");
    ASSERT_FALSE(list.is_null()) << "sample app has no ItemsList";
    const auto ref = "uia:" + uia_prop(list, "RuntimeId");
    const auto before = uia_prop(list, "Scroll.VerticalPercent");

    auto result = run_action_json(lvt, get_pid_arg() + " scroll " + ref + " down");
    EXPECT_TRUE(result.value("ok", false)) << result.dump(2);
    // A scrollable list must use the pattern, not the wheel: the wheel needs
    // the window in front and lands wherever the cursor is.
    EXPECT_EQ(result.value("method", ""), "ScrollPattern");

    auto after = uia_element(lvt, get_pid_arg(), "ItemsList");
    ASSERT_FALSE(after.is_null());
    EXPECT_NE(uia_prop(after, "Scroll.VerticalPercent"), before)
        << "scroll reported success but the list did not move";
}

TEST_F(WinUI3SampleFixture, SelectionVerbsRealizeVirtualizedItems) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    // A long list virtualizes, so its items are placeholders until realized.
    // This is the case that separates working on a toy UI from a real one.
    auto tree = json::parse(run_command(make_cmd(lvt, get_pid_arg() + " dump --uia")),
                            nullptr, false);
    ASSERT_FALSE(tree.is_discarded());

    std::vector<const json*> stack{&tree["root"]};
    std::vector<std::string> virtualizedRefs;
    while (!stack.empty()) {
        const json* node = stack.back();
        stack.pop_back();
        if (node->value("type", "") == "ListItem" &&
            uia_prop(*node, "SupportedPatterns").find("VirtualizedItem") != std::string::npos) {
            virtualizedRefs.push_back("uia:" + uia_prop(*node, "RuntimeId"));
        }
        if (node->contains("children")) {
            for (const auto& child : (*node)["children"])
                stack.push_back(&child);
        }
    }
    ASSERT_GE(virtualizedRefs.size(), 2u) << "expected a virtualized list to act on";

    auto selected = run_action_json(lvt, get_pid_arg() + " select " + virtualizedRefs[0]);
    EXPECT_TRUE(selected.value("ok", false)) << selected.dump(2);
    // The method must admit the realize step happened.
    EXPECT_NE(selected.value("method", "").find("VirtualizedItem.Realize"), std::string::npos)
        << selected.dump(2);
    EXPECT_NE(selected.value("method", "").find("SelectionItemPattern"), std::string::npos);

    auto added = run_action_json(lvt,
        get_pid_arg() + " add-to-selection " + virtualizedRefs[1]);
    EXPECT_TRUE(added.value("ok", false)) << added.dump(2);
    EXPECT_NE(added.value("method", "").find("AddToSelection"), std::string::npos);

    auto removed = run_action_json(lvt,
        get_pid_arg() + " remove-from-selection " + virtualizedRefs[1]);
    EXPECT_TRUE(removed.value("ok", false)) << removed.dump(2);
}

TEST_F(WinUI3SampleFixture, FocusAndSelectTextActOnTheTextBox) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    auto box = uia_element(lvt, get_pid_arg(), "InputBox");
    ASSERT_FALSE(box.is_null());
    const auto ref = "uia:" + uia_prop(box, "RuntimeId");

    auto focused = run_action_json(lvt, get_pid_arg() + " focus " + ref);
    EXPECT_TRUE(focused.value("ok", false)) << focused.dump(2);
    EXPECT_EQ(focused.value("method", ""), "SetFocus");
    EXPECT_EQ(uia_prop(uia_element(lvt, get_pid_arg(), "InputBox"), "HasKeyboardFocus"),
              "true");

    // With no argument, select-text selects everything.
    auto selectedAll = run_action_json(lvt, get_pid_arg() + " select-text " + ref);
    EXPECT_TRUE(selectedAll.value("ok", false)) << selectedAll.dump(2);
    EXPECT_NE(selectedAll.value("method", "").find("TextPattern"), std::string::npos);
}

// --- Regressions from the PR review ---
// Each of these pins a bug that shipped and was caught in review, so the test
// names describe the wrong behaviour they prevent rather than the API surface.

TEST_F(NotepadFixture, ActionOnANonWindowElementDoesNotActOnTheWindow) {
    auto lvt = get_lvt_path();

    // Re-finding an element used to fall back to the window root whenever the
    // lookup missed, without checking the id matched. `minimize` aimed at a
    // TextBlock therefore minimized the whole application and reported success.
    // A raw-view element is the case that triggered it: its RuntimeId names a
    // child HWND, which is exactly what the lookup could not reach.
    auto tree = json::parse(
        run_command(make_cmd(lvt, get_pid_arg() + " dump --uia --uia-view raw")), nullptr, false);
    ASSERT_FALSE(tree.is_discarded()) << "raw UIA dump produced no JSON";

    std::vector<const json*> all;
    collect_json_elements(tree["root"], all);
    const json* target = nullptr;
    const auto rootId = uia_prop(tree["root"], "RuntimeId");
    for (const auto* element : all) {
        const auto rid = uia_prop(*element, "RuntimeId");
        if (rid.empty() || rid == rootId)
            continue;
        // It has to be an element FindFirst cannot resolve, or the buggy
        // fallback is never reached and the test proves nothing. Raw-view-only
        // elements are exactly that case, and a Text element additionally
        // guarantees the Window pattern is absent, so a correct implementation
        // must refuse.
        if (uia_prop(*element, "IsControlElement") != "false")
            continue;
        if (element->value("type", "") != "Text")
            continue;
        target = element;
        break;
    }
    if (!target)
        GTEST_SKIP() << "no raw-view-only Text element in this Notepad build to test with";

    const auto ref = "uia:" + uia_prop(*target, "RuntimeId");
    auto result = run_action_json(lvt, get_pid_arg() + " minimize " + ref + " --uia-view raw");

    EXPECT_FALSE(result.value("ok", true))
        << "minimizing a Text element must fail, not minimize the window: " << result.dump(2);
    // The specific message matters: it proves the element was found and the
    // pattern was checked, rather than the lookup silently redirecting to the
    // window (which would have succeeded).
    EXPECT_NE(result.value("error", "").find("does not support"), std::string::npos)
        << result.dump(2);
    EXPECT_FALSE(IsIconic(s_hwnd)) << "the window was minimized by an action aimed elsewhere";
}

TEST_F(NotepadFixture, RawViewOnlyElementsCanStillBeResolvedForActions) {
    auto lvt = get_lvt_path();

    // The counterpart to the test above: refusing must come from the pattern
    // check, not from the element being unreachable. If re-finding regressed to
    // control-view-only lookup this would report "could not be located".
    auto tree = json::parse(
        run_command(make_cmd(lvt, get_pid_arg() + " dump --uia --uia-view raw")), nullptr, false);
    ASSERT_FALSE(tree.is_discarded());

    std::vector<const json*> all;
    collect_json_elements(tree["root"], all);
    const json* target = nullptr;
    for (const auto* element : all) {
        if (uia_prop(*element, "IsControlElement") == "false" &&
            !uia_prop(*element, "RuntimeId").empty()) {
            target = element;
            break;
        }
    }
    if (!target)
        GTEST_SKIP() << "this Notepad build exposes no raw-view-only elements";

    const auto ref = "uia:" + uia_prop(*target, "RuntimeId");
    auto result = run_action_json(lvt, get_pid_arg() + " focus " + ref + " --uia-view raw");
    EXPECT_EQ(result.value("error", "").find("could not be located"), std::string::npos)
        << "a raw-view element the walk emitted must be resolvable for actions: " << result.dump(2);
}

TEST_F(NotepadFixture, WindowVerbsStillWorkOnTheWindowElement) {
    // The guard added above must not break the legitimate case it guards.
    auto lvt = get_lvt_path();

    auto result = run_action_json(lvt, get_pid_arg() + " minimize");
    ASSERT_TRUE(result.value("ok", false)) << result.dump(2);
    Sleep(600);
    EXPECT_TRUE(IsIconic(s_hwnd));

    auto restored = run_action_json(lvt, get_pid_arg() + " restore");
    EXPECT_TRUE(restored.value("ok", false)) << restored.dump(2);
    Sleep(600);
    EXPECT_FALSE(IsIconic(s_hwnd));
}

TEST_F(NotepadFixture, EveryActionReportsItsOwnNameNotUnknown) {
    auto lvt = get_lvt_path();

    // Nine of the twenty action kinds had no name and reported themselves as
    // "unknown" in the result JSON, which made results impossible to correlate
    // with the command that produced them. These four were among them, and are
    // reachable without needing a particular control to exist.
    const std::pair<std::string, std::string> cases[] = {
        {"restore", "restore"},
        {"maximize", "maximize"},
        {"minimize", "minimize"},
        {"wait-gone uia:99.99.99 --wait-timeout 500", "wait-gone"},
    };
    for (const auto& [command, expected] : cases) {
        auto result = run_action_json(lvt, get_pid_arg() + " " + command);
        EXPECT_EQ(result.value("action", ""), expected)
            << "command '" << command << "' reported the wrong action: " << result.dump(2);
    }
    run_action_json(lvt, get_pid_arg() + " restore");
}

TEST_F(NotepadFixture, WaitGoneSucceedsWhenTheWindowItselfCloses) {
    auto lvt = get_lvt_path();

    // `close` then `wait-gone` is the natural pairing, and it used to time out:
    // once the window was destroyed the tree could not be built, so the
    // satisfied-check never ran and the wait reported failure for exactly the
    // outcome it was waiting for.
    //
    // The window under test is one this test owns and destroys. Launching a
    // second Notepad and killing it is not an option: modern Notepad serves
    // every window from one process, so terminating it would take the shared
    // fixture window with it.
    static constexpr wchar_t kClassName[] = L"LvtWaitGoneProbe";
    wil::unique_event ready(wil::EventOptions::ManualReset);
    wil::unique_event destroy(wil::EventOptions::ManualReset);
    HWND probe = nullptr;

    std::thread ui([&] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);

        probe = CreateWindowExW(0, kClassName, L"lvt wait-gone probe",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 400, 300,
                                nullptr, nullptr, wc.hInstance, nullptr);
        ready.SetEvent();
        if (!probe)
            return;

        // UIA reads this window cross-process, so it has to be pumping messages
        // the whole time lvt is looking at it.
        for (;;) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (destroy.wait(20))
                break;
        }
        DestroyWindow(probe);
        // Let the destruction actually complete before the thread exits.
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    });
    auto joinUi = wil::scope_exit([&] {
        destroy.SetEvent();
        if (ui.joinable())
            ui.join();
    });

    ASSERT_TRUE(ready.wait(5000)) << "the probe window thread never started";
    ASSERT_NE(probe, nullptr) << "could not create the probe window";

    char hwndArg[64];
    snprintf(hwndArg, sizeof(hwndArg), "--hwnd 0x%llX",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(probe)));

    auto probeTree = json::parse(
        run_command(make_cmd(lvt, std::string(hwndArg) + " dump --uia")), nullptr, false);
    ASSERT_FALSE(probeTree.is_discarded()) << "could not read the probe window's UIA tree";
    const auto probeRef = "uia:" + uia_prop(probeTree["root"], "RuntimeId");
    ASSERT_NE(probeRef, "uia:");

    destroy.SetEvent();
    ui.join();
    for (int i = 0; i < 50 && IsWindow(probe); ++i)
        Sleep(100);
    ASSERT_FALSE(IsWindow(probe)) << "the probe window did not go away";

    const auto start = GetTickCount64();
    auto result = run_action_json(
        lvt, std::string(hwndArg) + " wait-gone " + probeRef + " --wait-timeout 8000");
    const auto elapsed = GetTickCount64() - start;

    EXPECT_TRUE(result.value("ok", false))
        << "wait-gone must succeed once the window is destroyed: " << result.dump(2);
    EXPECT_LT(elapsed, 7000u) << "wait-gone burned its timeout instead of noticing the closure";
}

TEST_F(NotepadFixture, SyntheticInputRefusesOffscreenTargets) {
    auto lvt = get_lvt_path();

    // Offscreen elements report coordinates far outside the desktop. The mouse
    // APIs clamp such a point to the nearest edge and report success, so a
    // click aimed at one used to be injected at the desktop corner — on some
    // other window entirely — and reported as having worked.
    auto tree = json::parse(
        run_command(make_cmd(lvt, get_pid_arg() + " dump --uia --uia-view raw")), nullptr, false);
    ASSERT_FALSE(tree.is_discarded());

    std::vector<const json*> all;
    collect_json_elements(tree["root"], all);
    const json* offscreen = nullptr;
    for (const auto* element : all) {
        if (uia_prop(*element, "IsOffscreen") != "true")
            continue;
        if (uia_prop(*element, "RuntimeId").empty())
            continue;
        // Only elements with no quiet route reach the synthetic path.
        const auto patterns = uia_prop(*element, "SupportedPatterns");
        if (patterns.find("Invoke") != std::string::npos)
            continue;
        offscreen = element;
        break;
    }
    if (!offscreen)
        GTEST_SKIP() << "no offscreen element without an Invoke pattern to test with";

    const auto ref = "uia:" + uia_prop(*offscreen, "RuntimeId");
    auto result = run_action_json(lvt, get_pid_arg() + " click " + ref + " --uia-view raw");
    EXPECT_FALSE(result.value("ok", true))
        << "clicking an offscreen element must fail rather than click the desktop corner: "
        << result.dump(2);
}

TEST_F(NotepadFixture, PackagedSkillMatchesTheRepositorySkill) {
    // skills/lvt/SKILL.md is what plugin.json ships to consumers, while
    // .github/skills/lvt/SKILL.md is what this repo's own agent reads. They had
    // drifted far enough that the shipped copy documented flags which now exit
    // 1, so the packaged skill actively told its reader to run broken commands.
    const std::string root = LVT_SOURCE_DIR;
    auto packaged = read_text_file(root + "\\skills\\lvt\\SKILL.md");
    auto repository = read_text_file(root + "\\.github\\skills\\lvt\\SKILL.md");
    ASSERT_FALSE(packaged.empty()) << "skills/lvt/SKILL.md is missing or empty";
    ASSERT_FALSE(repository.empty()) << ".github/skills/lvt/SKILL.md is missing or empty";
    EXPECT_EQ(packaged, repository)
        << "the packaged skill has drifted from the repository skill; copy "
           ".github/skills/lvt/SKILL.md over skills/lvt/SKILL.md";
}

TEST_F(NotepadFixture, NoSkillDocumentsRemovedFlags) {
    // The flags below became verbs, so a doc that still shows them tells the
    // reader to run something that exits 1.
    const std::string root = LVT_SOURCE_DIR;
    for (const char* relative : {"\\skills\\lvt\\SKILL.md", "\\.github\\skills\\lvt\\SKILL.md"}) {
        const auto text = read_text_file(root + relative);
        ASSERT_FALSE(text.empty()) << relative;
        for (const char* removed : {"--dump", "--frameworks", "--click ", "--invoke "}) {
            EXPECT_EQ(text.find(removed), std::string::npos)
                << relative << " still documents the removed flag " << removed;
        }
    }
}

TEST_F(WinUI3SampleFixture, WaitGoneSucceedsForAnElementThatNeverExisted) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    // An absent element is already "gone", so this must return rather than
    // burn the whole timeout.
    const auto start = GetTickCount64();
    auto result = run_action_json(lvt,
        get_pid_arg() + " wait-gone uia:99.99.99 --wait-timeout 5000");
    const auto elapsed = GetTickCount64() - start;

    EXPECT_TRUE(result.value("ok", false)) << result.dump(2);
    EXPECT_LT(elapsed, 4000u) << "wait-gone should return as soon as the element is absent";
}

// --- Verb parsing ---
// The CLI restructure moved every mode from a flag to a positional verb. That
// parsing has no unit-test seam (it lives in main.cpp), so it is covered here
// through the binary, which is also how users hit it.

TEST_F(NotepadFixture, DumpIsTheDefaultVerb) {
    auto lvt = get_lvt_path();
    // Omitting the verb has to keep working, or every existing invocation and
    // every published example breaks.
    auto implied = run_command(make_cmd(lvt, get_pid_arg()));
    auto explicitDump = run_command(make_cmd(lvt, get_pid_arg() + " dump"));

    auto a = json::parse(implied, nullptr, false);
    auto b = json::parse(explicitDump, nullptr, false);
    ASSERT_FALSE(a.is_discarded()) << "implied dump produced no JSON";
    ASSERT_FALSE(b.is_discarded()) << "explicit dump produced no JSON";
    EXPECT_TRUE(a.contains("root"));
    EXPECT_TRUE(b.contains("root"));
}

TEST_F(NotepadFixture, VerbCanFollowOptions) {
    // Options are order-independent, so the verb must be found wherever it sits.
    auto lvt = get_lvt_path();
    auto before = run_command(make_cmd(lvt, "frameworks " + get_pid_arg()));
    auto after = run_command(make_cmd(lvt, get_pid_arg() + " frameworks"));
    EXPECT_FALSE(before.empty());
    EXPECT_EQ(before, after);
}

TEST_F(NotepadFixture, LegacyFlagsReportTheirReplacement) {
    auto lvt = get_lvt_path();
    // "unknown argument" would be unhelpful here: the shape of the command
    // changed, not the spelling of a flag.
    struct Case { const char* flag; const char* expect; };
    const Case cases[] = {
        {"--dump", "dump"},
        {"--frameworks", "frameworks"},
        {"--watch", "watch"},
        {"--query", "query"},
        {"--screenshot", "screenshot"},
    };
    for (const auto& c : cases) {
        auto output = run_command(make_cmd(lvt, get_pid_arg() + " " + c.flag) + " 2>&1");
        EXPECT_NE(output.find("is now"), std::string::npos)
            << c.flag << " should report its replacement, got:\n" << output;
        // Asserting the verb name alone is tautological — every one of these is
        // a substring of the flag being echoed back, so it would pass for any
        // message that merely repeated the input. Requiring the verb to appear
        // *after* "is now" is what actually pins the replacement being named.
        const auto isNow = output.find("is now");
        ASSERT_NE(isNow, std::string::npos);
        EXPECT_NE(output.find(c.expect, isNow), std::string::npos)
            << c.flag << " should name the '" << c.expect << "' verb as its replacement, got:\n"
            << output;
    }
}

TEST_F(NotepadFixture, UnknownVerbAndBadArityAreRejected) {
    auto lvt = get_lvt_path();

    auto unknown = run_command(make_cmd(lvt, get_pid_arg() + " frobnicate") + " 2>&1");
    EXPECT_NE(unknown.find("unknown verb"), std::string::npos) << unknown;

    // set-value needs two arguments; one must not be silently accepted.
    auto tooFew = run_command(make_cmd(lvt, get_pid_arg() + " set-value e0") + " 2>&1");
    EXPECT_NE(tooFew.find("usage"), std::string::npos) << tooFew;

    // query takes at most two.
    auto tooMany = run_command(make_cmd(lvt, get_pid_arg() + " query e0 type extra") + " 2>&1");
    EXPECT_NE(tooMany.find("usage"), std::string::npos) << tooMany;
}

TEST_F(NotepadFixture, ScreenshotVerbDefaultsAndHonoursOutput) {
    auto lvt = get_lvt_path();
    auto tmpFile = fs::path(lvt).parent_path() / "lvt_verb_shot.png";
    fs::remove(tmpFile);

    run_command(make_cmd(lvt, get_pid_arg() + " screenshot --output " + tmpFile.string()));
    EXPECT_TRUE(fs::exists(tmpFile)) << "--output should name the PNG for the screenshot verb";
    fs::remove(tmpFile);

    // The other half of this test's name: with no --output the verb writes
    // lvt-screenshot.png in the working directory. Run from a scratch directory
    // so the default cannot be confused with a leftover file.
    const auto scratch = fs::temp_directory_path() / "lvt_screenshot_default";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch, ec);
    const auto defaultShot = scratch / "lvt-screenshot.png";

    run_command("cd /d \"" + scratch.string() + "\" && " +
                make_cmd(lvt, get_pid_arg() + " screenshot"));
    EXPECT_TRUE(fs::exists(defaultShot))
        << "screenshot with no --output should write lvt-screenshot.png";
    fs::remove_all(scratch, ec);
}

TEST_F(WinUI3SampleFixture, UiaReferenceImpliesTheUiaTree) {
    SkipIfNotReady();
    auto lvt = get_lvt_path();

    auto button = uia_element(lvt, get_pid_arg(), "PrimaryButton");
    ASSERT_FALSE(button.is_null());
    const auto runtimeId = uia_prop(button, "RuntimeId");
    ASSERT_FALSE(runtimeId.empty());

    // A uia: reference cannot resolve against the visual tree, so asking for
    // one without --uia would otherwise fail confusingly.
    auto queried = trim_crlf(run_command(make_cmd(
        lvt, get_pid_arg() + " query uia:" + runtimeId + " AutomationId")));
    EXPECT_EQ(queried, "PrimaryButton");
}
